/**
 * @file chameleon.c
 * @brief NimBLE GATT-client transport + ChameleonUltra NUS protocol.
 *
 * Central role: stop the shared BT service -> bring up the ESP BLE controller
 * (unchanged) + the NimBLE host in BLE mode -> GAP scan by name -> connect ->
 * discover the Nordic UART Service and its TX/RX characteristics -> subscribe
 * to notifications (CCCD) -> exchange ChameleonUltra frames. The controller
 * init is untouched from the Bluedroid version; only the host + GAP/GATT
 * client layer was ported from Bluedroid to NimBLE.
 */
#include "chameleon.h"

#include <esp_bt.h>
#include <esp_nimble_hci.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <host/ble_hs.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_att.h>
#include <services/gap/ble_svc_gap.h>
#include <os/os_mbuf.h>
#include <esp_log.h>
#include <furi.h>
#include <btshim.h>
#include <string.h>

#define TAG           "Chameleon"
#define CHAM_DEV_NAME "ChameleonUltra"

/* Nordic UART Service UUIDs, stored little-endian (matches NimBLE ble_uuid128_t
 * .value byte order): 6E400001-B5A3-F393-E0A9-E50E24DCCA9E (service),
 * -0002 TX(write), -0003 RX(notify) */
static const uint8_t NUS_SVC[16] =
    {0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
     0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E};
static const uint8_t NUS_TX[16] =
    {0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
     0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E};
static const uint8_t NUS_RX[16] =
    {0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
     0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E};
static const uint16_t CCCD_UUID = 0x2902;

/* HF14A_RAW option bits: b7 activateRfField, b6 waitResponse, b5 appendCrc,
 * b4 autoSelect, b3 keepRfField, b2 checkResponseCrc. Default = a normal
 * ISO14443-3A exchange with auto-anticollision and CRC handling. */
#define CHAMELEON_RAW_OPT_DEFAULT ((1 << 6) | (1 << 5) | (1 << 4) | (1 << 2)) /* 0x74 */

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_connected = false;
static bool s_hal_started = false;
static bool s_host_inited = false;
static volatile bool s_host_synced = false;
/* True if BT was disabled in settings when we connected: we force-started the
 * stack so the ESP controller gets initialized, and must disable it again on
 * disconnect to honor the user's setting. */
static bool s_bt_was_disabled = false;

static volatile bool s_scanning = false;
static volatile bool s_dev_found = false;
static ble_addr_t s_peer_addr;

/* Discovery progress + resolved NUS handles */
static volatile bool s_search_done = false; /* service discovery complete */
static volatile bool s_chr_done = false;    /* characteristic discovery complete */
static volatile bool s_dsc_done = false;    /* descriptor discovery complete */
static uint16_t s_svc_start = 0;
static uint16_t s_svc_end = 0;
static uint16_t s_write_handle = 0;
static uint16_t s_notify_handle = 0;
static uint16_t s_cccd_handle = 0;
static volatile bool s_cccd_written = false;

static int s_device_mode = -1; /* cache to skip redundant CHANGE_DEVICE_MODE */

/* RX frame reassembly + last parsed response (commands are serialized) */
static uint8_t s_acc[CHAMELEON_RESP_DATA_MAX + 32];
static size_t s_acc_len = 0;
static ChameleonResp s_last_resp;
static volatile bool s_resp_ready = false;

static bool uuid128_eq(const ble_uuid_t* u, const uint8_t ref[16]) {
    if(u->type != BLE_UUID_TYPE_128) return false;
    const ble_uuid128_t* u128 = (const ble_uuid128_t*)u;
    return memcmp(u128->value, ref, 16) == 0;
}

static uint8_t cham_lrc(const uint8_t* d, size_t n) {
    uint8_t s = 0;
    for(size_t i = 0; i < n; i++) s += d[i];
    return (uint8_t)((0x100 - (s & 0xff)) & 0xff);
}

/* Try to extract one complete frame from the accumulation buffer. */
static void cham_try_parse(void) {
    /* Resync to SOF 0x11 0xEF */
    while(s_acc_len >= 2 && !(s_acc[0] == 0x11 && s_acc[1] == 0xEF)) {
        memmove(s_acc, s_acc + 1, --s_acc_len);
    }
    if(s_acc_len < 10) return;

    uint16_t data_len = ((uint16_t)s_acc[6] << 8) | s_acc[7];
    if(data_len > CHAMELEON_RESP_DATA_MAX) {
        /* Bogus length — drop SOF and resync */
        memmove(s_acc, s_acc + 2, s_acc_len -= 2);
        return;
    }
    size_t frame_len = 10u + data_len;
    if(s_acc_len < frame_len) return;

    s_last_resp.command = ((uint16_t)s_acc[2] << 8) | s_acc[3];
    s_last_resp.status = s_acc[5];
    s_last_resp.data_len = data_len;
    if(data_len) memcpy(s_last_resp.data, &s_acc[9], data_len);
    s_resp_ready = true;

    /* Consume the frame; keep any trailing bytes for the next parse */
    size_t rest = s_acc_len - frame_len;
    if(rest) memmove(s_acc, s_acc + frame_len, rest);
    s_acc_len = rest;
}

/* ------------------------------------------------- GAP / GATT events ----- */

/* Single NimBLE event callback for both scanning (ble_gap_disc) and the
 * connection (ble_gap_connect): DISC results, connect/disconnect, MTU and
 * incoming notifications all arrive here. */
static int cham_gap_event(struct ble_gap_event* event, void* arg) {
    (void)arg;
    switch(event->type) {
    case BLE_GAP_EVENT_DISC: {
        if(s_dev_found) return 0;
        struct ble_hs_adv_fields fields;
        if(ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0)
            return 0;
        /* ble_hs_adv_parse_fields fills .name for both complete (0x09) and
         * shortened (0x08) local name AD types. */
        if(fields.name != NULL && fields.name_len == (sizeof(CHAM_DEV_NAME) - 1) &&
           memcmp(fields.name, CHAM_DEV_NAME, fields.name_len) == 0) {
            s_peer_addr = event->disc.addr;
            s_dev_found = true;
            ble_gap_disc_cancel();
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        if(event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_connected = true;
            ble_gattc_exchange_mtu(s_conn_handle, NULL, NULL);
        } else {
            s_connected = false;
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_connected = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        return 0;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGD(TAG, "MTU=%u", event->mtu.value);
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        struct os_mbuf* om = event->notify_rx.om;
        uint16_t vlen = OS_MBUF_PKTLEN(om);
        ESP_LOGD(TAG, "NOTIFY len=%u h=%u", vlen, event->notify_rx.attr_handle);
        if(vlen > 0) {
            size_t space = sizeof(s_acc) - s_acc_len;
            size_t n = vlen;
            if(n > space) {
                /* Overflow — reset accumulator and keep latest chunk */
                s_acc_len = 0;
                n = (vlen > sizeof(s_acc)) ? sizeof(s_acc) : vlen;
            }
            os_mbuf_copydata(om, 0, n, s_acc + s_acc_len);
            s_acc_len += n;
            cham_try_parse();
        }
        return 0;
    }
    default:
        return 0;
    }
}

/* -------------------------------------------- GATT discovery callbacks --- */

static int cham_disc_svc_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error* error,
    const struct ble_gatt_svc* service,
    void* arg) {
    (void)conn_handle;
    (void)arg;
    if(error->status == 0) {
        if(uuid128_eq(&service->uuid.u, NUS_SVC)) {
            s_svc_start = service->start_handle;
            s_svc_end = service->end_handle;
        }
    } else if(error->status == BLE_HS_EDONE) {
        s_search_done = true;
    }
    return 0;
}

static int cham_disc_chr_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error* error,
    const struct ble_gatt_chr* chr,
    void* arg) {
    (void)conn_handle;
    (void)arg;
    if(error->status == 0) {
        if(uuid128_eq(&chr->uuid.u, NUS_TX))
            s_write_handle = chr->val_handle;
        else if(uuid128_eq(&chr->uuid.u, NUS_RX))
            s_notify_handle = chr->val_handle;
    } else if(error->status == BLE_HS_EDONE) {
        s_chr_done = true;
    }
    return 0;
}

static int cham_disc_dsc_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error* error,
    uint16_t chr_val_handle,
    const struct ble_gatt_dsc* dsc,
    void* arg) {
    (void)conn_handle;
    (void)chr_val_handle;
    (void)arg;
    if(error->status == 0) {
        if(dsc->uuid.u.type == BLE_UUID_TYPE_16 && ble_uuid_u16(&dsc->uuid.u) == CCCD_UUID)
            s_cccd_handle = dsc->handle;
    } else if(error->status == BLE_HS_EDONE) {
        s_dsc_done = true;
    }
    return 0;
}

static int cham_cccd_write_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error* error,
    struct ble_gatt_attr* attr,
    void* arg) {
    (void)conn_handle;
    (void)attr;
    (void)arg;
    s_cccd_written = (error->status == 0);
    return 0;
}

static int cham_cmd_write_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error* error,
    struct ble_gatt_attr* attr,
    void* arg) {
    (void)conn_handle;
    (void)error;
    (void)attr;
    (void)arg;
    /* Command responses arrive via notifications; nothing to do on write ACK. */
    return 0;
}

/* ------------------------------------------------- NimBLE host plumbing -- */

static void cham_on_sync(void) {
    s_host_synced = true;
}

static void cham_on_reset(int reason) {
    ESP_LOGW(TAG, "NimBLE host reset, reason=%d", reason);
    s_host_synced = false;
}

static void cham_host_task(void* param) {
    (void)param;
    /* Returns only after nimble_port_stop() is called. */
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* --------------------------------------------------- HAL start / stop ---- */

static bool cham_hal_start(void) {
    if(s_hal_started) return true;

    Bt* bt = furi_record_open(RECORD_BT);
    /* If BT is disabled in settings the radio stack was never started, so the
     * ESP BT controller is uninitialized. Bring it up through the normal
     * service path first; otherwise the controller takeover below dereferences
     * an uninitialized controller and crashes. */
    s_bt_was_disabled = !bt_is_enabled(bt);
    if(s_bt_was_disabled) {
        ESP_LOGI(TAG, "BT disabled in settings, force-starting stack first");
        bt_start_stack(bt);
        furi_delay_ms(300);
    }
    bt_stop_stack(bt);
    furi_record_close(RECORD_BT);
    furi_delay_ms(100);

    /* Controller init — unchanged from the Bluedroid version. */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&bt_cfg);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;

    /* NimBLE host bring-up (controller already enabled above).
     * TODO(idf): esp_nimble_init() is the ESP-IDF host init entry point; on
     * some IDF versions this symbol is nimble_port_init() (nimble/nimble_port.h).
     * Match whatever the firmware-wide migration standardizes on. */
    err = esp_nimble_init();
    if(err != ESP_OK) return false;

    ble_hs_cfg.sync_cb = cham_on_sync;
    ble_hs_cfg.reset_cb = cham_on_reset;
    ble_svc_gap_init();
    ble_att_set_preferred_mtu(247);

    s_host_synced = false;
    s_connected = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

    nimble_port_freertos_init(cham_host_task);
    s_host_inited = true;

    /* Wait for the host to sync (controller ready). */
    for(int i = 0; i < 100 && !s_host_synced; i++) furi_delay_ms(10);
    if(!s_host_synced) {
        ESP_LOGW(TAG, "NimBLE host sync timeout");
        return false;
    }

    s_hal_started = true;
    return true;
}

static void cham_hal_stop(void) {
    if(s_connected && s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        furi_delay_ms(100);
    }
    if(s_scanning) {
        ble_gap_disc_cancel();
        furi_delay_ms(50);
        s_scanning = false;
    }

    /* Tear the NimBLE host down; the controller is disabled/deinited below. */
    if(s_host_inited) {
        nimble_port_stop();
        furi_delay_ms(50);
        nimble_port_deinit();
        furi_delay_ms(50);
        s_host_inited = false;
    }

    if(esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_disable();
        furi_delay_ms(50);
    }
    if(esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_deinit();
        furi_delay_ms(50);
    }

    Bt* bt = furi_record_open(RECORD_BT);
    bt_start_stack(bt);
    if(s_bt_was_disabled) {
        /* BT was off in settings before we connected — put it back off so we
         * don't leave the device advertising against the user's choice. */
        furi_delay_ms(200);
        bt_stop_stack(bt);
        s_bt_was_disabled = false;
    }
    furi_record_close(RECORD_BT);

    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_connected = false;
    s_host_synced = false;
    s_hal_started = false;
    s_device_mode = -1;
}

/* ----------------------------------------------------- public API -------- */

bool chameleon_connect(volatile bool* abort_flag) {
    if(s_connected) return true;
    if(!cham_hal_start()) {
        ESP_LOGE(TAG, "HAL start failed");
        cham_hal_stop();
        return false;
    }

    /* Reset per-connection state */
    s_dev_found = false;
    s_connected = false;
    s_search_done = false;
    s_chr_done = false;
    s_dsc_done = false;
    s_cccd_written = false;
    s_svc_start = s_svc_end = s_write_handle = s_notify_handle = s_cccd_handle = 0;
    s_acc_len = 0;
    s_resp_ready = false;

    /* Scan for the device by name (active scan; results in cham_gap_event) */
    struct ble_gap_disc_params dp = {
        .itvl = 0x50,
        .window = 0x30,
        .filter_policy = 0,
        .limited = 0,
        .passive = 0,
        .filter_duplicates = 0,
    };
    s_scanning = true;
    if(ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &dp, cham_gap_event, NULL) != 0) {
        s_scanning = false;
        cham_hal_stop();
        return false;
    }

    for(int i = 0; i < 300 && !s_dev_found; i++) { /* up to ~15 s */
        if(abort_flag && *abort_flag) {
            ble_gap_disc_cancel();
            cham_hal_stop();
            return false;
        }
        furi_delay_ms(50);
    }
    if(!s_dev_found) {
        ESP_LOGW(TAG, "device not found");
        ble_gap_disc_cancel();
        cham_hal_stop();
        return false;
    }
    ble_gap_disc_cancel();
    s_scanning = false;
    furi_delay_ms(100);

    /* Connect (30 s timeout) */
    if(ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &s_peer_addr, 30000, NULL, cham_gap_event, NULL) !=
       0) {
        cham_hal_stop();
        return false;
    }
    for(int i = 0; i < 100 && !s_connected; i++) {
        if(abort_flag && *abort_flag) {
            if(s_conn_handle != BLE_HS_CONN_HANDLE_NONE)
                ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            cham_hal_stop();
            return false;
        }
        furi_delay_ms(50);
    }
    if(!s_connected) {
        ESP_LOGW(TAG, "connect timeout");
        cham_hal_stop();
        return false;
    }

    /* Discover all services; cham_disc_svc_cb latches the NUS handles. */
    s_search_done = false;
    if(ble_gattc_disc_all_svcs(s_conn_handle, cham_disc_svc_cb, NULL) != 0) {
        cham_hal_stop();
        return false;
    }
    for(int i = 0; i < 150 && !s_search_done; i++) furi_delay_ms(20);
    if(!s_search_done) {
        ESP_LOGW(TAG, "service discovery timeout");
        cham_hal_stop();
        return false;
    }
    if(s_svc_start == 0) {
        ESP_LOGW(TAG, "NUS service not found");
        cham_hal_stop();
        return false;
    }
    ESP_LOGD(TAG, "NUS svc handles %u..%u", s_svc_start, s_svc_end);

    /* Find TX (write) + RX (notify) characteristic handles */
    s_chr_done = false;
    if(ble_gattc_disc_all_chrs(s_conn_handle, s_svc_start, s_svc_end, cham_disc_chr_cb, NULL) !=
       0) {
        cham_hal_stop();
        return false;
    }
    for(int i = 0; i < 150 && !s_chr_done; i++) furi_delay_ms(20);
    if(s_write_handle == 0 || s_notify_handle == 0) {
        ESP_LOGW(TAG, "NUS chars missing (w=%u n=%u)", s_write_handle, s_notify_handle);
        cham_hal_stop();
        return false;
    }

    /* Discover the RX characteristic's CCCD descriptor */
    s_dsc_done = false;
    if(ble_gattc_disc_all_dscs(s_conn_handle, s_notify_handle, s_svc_end, cham_disc_dsc_cb, NULL) !=
       0) {
        cham_hal_stop();
        return false;
    }
    for(int i = 0; i < 100 && !s_dsc_done; i++) furi_delay_ms(20);

    /* Subscribe to notifications: write CCCD = 0x0001 */
    if(s_cccd_handle != 0) {
        uint8_t v[2] = {0x01, 0x00};
        s_cccd_written = false;
        if(ble_gattc_write_flat(
               s_conn_handle, s_cccd_handle, v, sizeof(v), cham_cccd_write_cb, NULL) == 0) {
            for(int i = 0; i < 60 && !s_cccd_written; i++) furi_delay_ms(20);
        }
        ESP_LOGD(TAG, "CCCD handle=%u written=%d", s_cccd_handle, (int)s_cccd_written);
    } else {
        ESP_LOGW(TAG, "CCCD descr not found");
    }

    ESP_LOGD(TAG, "connected & subscribed");
    return true;
}

void chameleon_disconnect(void) {
    if(!s_hal_started) return;
    cham_hal_stop();
    s_connected = false;
}

bool chameleon_is_connected(void) {
    return s_connected;
}

bool chameleon_cmd(
    uint16_t cmd,
    const uint8_t* data,
    uint16_t len,
    ChameleonResp* out,
    uint32_t timeout_ms) {
    if(!s_connected || s_write_handle == 0) return false;
    if(len > CHAMELEON_RESP_DATA_MAX) return false;

    uint8_t frame[CHAMELEON_RESP_DATA_MAX + 16];
    frame[0] = 0x11;
    frame[1] = 0xEF;
    frame[2] = (cmd >> 8) & 0xFF;
    frame[3] = cmd & 0xFF;
    frame[4] = 0x00;
    frame[5] = 0x00;
    frame[6] = (len >> 8) & 0xFF;
    frame[7] = len & 0xFF;
    frame[8] = cham_lrc(&frame[2], 6);
    if(len) memcpy(&frame[9], data, len);
    frame[9 + len] = cham_lrc(&frame[9], len);

    s_resp_ready = false;
    s_acc_len = 0;

    int e = ble_gattc_write_flat(
        s_conn_handle, s_write_handle, frame, 10 + len, cham_cmd_write_cb, NULL);
    ESP_LOGD(TAG, "cmd %u write -> rc=%d (wh=%u)", cmd, e, s_write_handle);
    if(e != 0) {
        ESP_LOGW(TAG, "write_char rc=%d", e);
        return false;
    }

    uint32_t waited = 0;
    while(!s_resp_ready && waited < timeout_ms) {
        furi_delay_ms(10);
        waited += 10;
    }
    if(!s_resp_ready) {
        ESP_LOGW(TAG, "cmd %u TIMEOUT (no notify in %lums)", cmd, (unsigned long)timeout_ms);
        return false;
    }
    ESP_LOGD(
        TAG,
        "cmd %u resp: rxcmd=%u status=%02X len=%u",
        cmd,
        s_last_resp.command,
        s_last_resp.status,
        s_last_resp.data_len);
    if(out) *out = s_last_resp;
    return true;
}

bool chameleon_mf1_write_block(
    uint8_t block,
    uint8_t key_type,
    const uint8_t key[6],
    const uint8_t data16[16]) {
    uint8_t payload[2 + 6 + 16];
    payload[0] = key_type; /* 0x60 = key A, 0x61 = key B */
    payload[1] = block;
    memcpy(&payload[2], key, 6);
    memcpy(&payload[8], data16, 16);

    ChameleonResp r;
    if(!chameleon_cmd(ChameleonCmdMf1WriteOneBlock, payload, sizeof(payload), &r, 1500))
        return false;
    return r.status == CHAMELEON_STATUS_HF_TAG_OK;
}

bool chameleon_mfu_write_page(uint8_t page, const uint8_t data4[4]) {
    /* HF14A_RAW WRITE (0xA2 page d0 d1 d2 d3); the card answers a 4-bit ACK,
     * so accept HF_TAG_OK regardless of returned data length. */
    uint8_t frame[6] = {0xA2, page, data4[0], data4[1], data4[2], data4[3]};
    uint8_t payload[5 + sizeof(frame)];
    payload[0] = CHAMELEON_RAW_OPT_DEFAULT;
    payload[1] = 0x00;
    payload[2] = 200;
    payload[3] = 0x00;
    payload[4] = (uint8_t)(sizeof(frame) * 8);
    memcpy(&payload[5], frame, sizeof(frame));

    ChameleonResp r;
    if(!chameleon_cmd(ChameleonCmdHf14aRaw, payload, sizeof(payload), &r, 1500))
        return false;
    return r.status == CHAMELEON_STATUS_HF_TAG_OK;
}

static bool chameleon_cmd_ok(uint16_t cmd, const uint8_t* d, uint16_t n) {
    ChameleonResp r;
    if(!chameleon_cmd(cmd, d, n, &r, 1500)) return false;
    return r.status == CHAMELEON_STATUS_SUCCESS;
}

bool chameleon_slot_select(uint8_t slot, uint16_t tag_type) {
    if(slot < 1 || slot > 8) return false;
    uint8_t s = slot - 1;

    uint8_t en[3] = {s, 0x02 /* HF */, 0x01};
    if(!chameleon_cmd_ok(ChameleonCmdSetSlotEnable, en, sizeof(en))) return false;

    uint8_t act[1] = {s};
    if(!chameleon_cmd_ok(ChameleonCmdSetActiveSlot, act, sizeof(act))) return false;

    uint8_t ty[3] = {s, (uint8_t)(tag_type >> 8), (uint8_t)(tag_type & 0xFF)};
    if(!chameleon_cmd_ok(ChameleonCmdSetSlotTagType, ty, sizeof(ty))) return false;

    /* Initialise the slot to defaults for this tag type — required before
     * loading emulation data, otherwise the emulated card stays inert. */
    if(!chameleon_cmd_ok(ChameleonCmdSetSlotDataDefault, ty, sizeof(ty))) return false;

    return true;
}

bool chameleon_slot_config_save(void) {
    return chameleon_cmd_ok(ChameleonCmdSlotDataConfigSave, NULL, 0);
}

bool chameleon_mf1_eload(uint8_t start_block, const uint8_t* data, uint16_t nblocks) {
    /* Keep each BLE write well under the ATT MTU: 8 blocks (128 B) per chunk */
    const uint16_t chunk_blocks = 8;
    uint8_t buf[1 + chunk_blocks * 16];
    for(uint16_t b = 0; b < nblocks; b += chunk_blocks) {
        uint16_t cnt = (nblocks - b < chunk_blocks) ? (nblocks - b) : chunk_blocks;
        buf[0] = (uint8_t)(start_block + b);
        memcpy(&buf[1], &data[(size_t)b * 16], (size_t)cnt * 16);
        if(!chameleon_cmd_ok(ChameleonCmdMf1WriteEmuBlockData, buf, 1 + cnt * 16))
            return false;
    }
    return true;
}

bool chameleon_mfu_eload(uint8_t start_page, const uint8_t* data, uint16_t npages) {
    /* MF0_NTAG_WRITE_EMU_PAGE_DATA payload is [page_start, page_count, data]
     * (unlike MF1_WRITE_EMU_BLOCK_DATA which is just [block, data]). Omitting
     * the count byte shifts all pages by one and the clone reads back wrong. */
    const uint16_t chunk_pages = 30; /* 2 + 120 B */
    uint8_t buf[2 + chunk_pages * 4];
    for(uint16_t p = 0; p < npages; p += chunk_pages) {
        uint16_t cnt = (npages - p < chunk_pages) ? (npages - p) : chunk_pages;
        buf[0] = (uint8_t)(start_page + p);
        buf[1] = (uint8_t)cnt;
        memcpy(&buf[2], &data[(size_t)p * 4], (size_t)cnt * 4);
        if(!chameleon_cmd_ok(ChameleonCmdMf0NtagWriteEmuPageData, buf, 2 + cnt * 4))
            return false;
    }
    return true;
}

bool chameleon_set_anticoll(
    const uint8_t* uid,
    uint8_t uid_len,
    const uint8_t atqa[2],
    uint8_t sak) {
    if(uid_len != 4 && uid_len != 7) return false;
    uint8_t p[1 + 10 + 4];
    uint8_t i = 0;
    p[i++] = uid_len;
    memcpy(&p[i], uid, uid_len);
    i += uid_len;
    p[i++] = atqa[1];
    p[i++] = atqa[0];
    p[i++] = sak;
    p[i++] = 0x00; /* ATS length */
    return chameleon_cmd_ok(ChameleonCmdHf14aSetAntiCollData, p, i);
}

bool chameleon_get_app_version(uint8_t* major, uint8_t* minor) {
    ChameleonResp r;
    if(!chameleon_cmd(ChameleonCmdGetAppVersion, NULL, 0, &r, 1000)) return false;
    if(r.status != CHAMELEON_STATUS_SUCCESS || r.data_len < 2) return false;
    if(major) *major = r.data[1]; /* device sends [minor, major] */
    if(minor) *minor = r.data[0];
    return true;
}

bool chameleon_get_battery(uint16_t* millivolt, uint8_t* percent) {
    ChameleonResp r;
    if(!chameleon_cmd(ChameleonCmdGetBatteryInfo, NULL, 0, &r, 1000)) return false;
    if(r.status != CHAMELEON_STATUS_SUCCESS || r.data_len < 3) return false;
    if(millivolt) *millivolt = ((uint16_t)r.data[0] << 8) | r.data[1];
    if(percent) *percent = r.data[2];
    return true;
}

bool chameleon_set_device_mode(uint8_t mode) {
    if(s_device_mode == (int)mode) return true;
    ChameleonResp r;
    if(!chameleon_cmd(ChameleonCmdChangeMode, &mode, 1, &r, 1000)) return false;
    if(r.status != CHAMELEON_STATUS_SUCCESS) return false;
    s_device_mode = (int)mode;
    return true;
}

bool chameleon_hf14a_scan(uint8_t* uid, uint8_t* uid_len, uint8_t atqa[2], uint8_t* sak) {
    ChameleonResp r;
    if(!chameleon_cmd(ChameleonCmdHf14aScan, NULL, 0, &r, 1000)) return false;
    if(r.status != CHAMELEON_STATUS_HF_TAG_OK || r.data_len < 5) return false;
    /* Response data: [uid_len][uid...][atqa1][atqa0][sak] */
    uint8_t ul = r.data[0];
    if(ul == 0 || ul > 10 || (size_t)(4 + ul) > r.data_len) return false;
    if(uid) memcpy(uid, &r.data[1], ul);
    if(uid_len) *uid_len = ul;
    if(atqa) {
        atqa[1] = r.data[1 + ul];
        atqa[0] = r.data[2 + ul];
    }
    if(sak) *sak = r.data[3 + ul];
    return true;
}

bool chameleon_mf1_read_block(
    uint8_t block,
    uint8_t key_type,
    const uint8_t key[6],
    uint8_t out16[16]) {
    uint8_t payload[8];
    payload[0] = key_type; /* 0x60 = key A, 0x61 = key B */
    payload[1] = block;
    memcpy(&payload[2], key, 6);

    ChameleonResp r;
    if(!chameleon_cmd(ChameleonCmdMf1ReadOneBlock, payload, sizeof(payload), &r, 1200))
        return false;
    if(r.status != CHAMELEON_STATUS_HF_TAG_OK || r.data_len < 16) return false;
    memcpy(out16, r.data, 16);
    return true;
}

bool chameleon_hf14a_raw(
    const uint8_t* data,
    uint8_t len,
    uint8_t* out,
    uint16_t out_cap,
    uint16_t* out_len) {
    if(len == 0 || len > 240) return false;

    uint8_t payload[5 + 240];
    payload[0] = CHAMELEON_RAW_OPT_DEFAULT;
    payload[1] = 0x00;
    payload[2] = 200; /* timeout */
    payload[3] = 0x00;
    payload[4] = (uint8_t)(len * 8); /* bitlen */
    memcpy(&payload[5], data, len);

    ChameleonResp r;
    if(!chameleon_cmd(ChameleonCmdHf14aRaw, payload, 5 + len, &r, 1200)) return false;
    if(r.status != CHAMELEON_STATUS_HF_TAG_OK || r.data_len == 0) return false;
    uint16_t n = r.data_len;
    if(n > out_cap) n = out_cap;
    memcpy(out, r.data, n);
    if(out_len) *out_len = n;
    return true;
}
