/**
 * @file ble_serial.c
 * BLE Serial Service — ESP32 implementation (NimBLE host)
 *
 * Creates a custom GATT service with the same UUIDs as the STM32 Flipper Zero
 * serial service so the Flipper mobile app / qFlipper can connect.
 *
 * MIGRATION NOTE: This module was ported from the ESP-IDF Bluedroid host API to
 * the NimBLE host API. The BLE *controller* init is unchanged
 * (esp_bt_controller_init/enable in BLE mode); only the host stack changed.
 * The exported API in ble_serial.h is UNCHANGED — only internals differ.
 */

#include "ble_serial.h"
#include <protobuf_version.h>

#include <string.h>
#include <stdlib.h>

#define BLE_SERIAL_STR_HELPER(x) #x
#define BLE_SERIAL_STR(x)        BLE_SERIAL_STR_HELPER(x)

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <esp_bt.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <nvs_flash.h>

/* ---- NimBLE host headers (replace the Bluedroid esp_gap/esp_gatts/esp_bt_main) ---- */
#include <esp_nimble_hci.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <host/ble_hs.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/util/util.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>
#include <store/config/ble_store_config.h>

#include <furi_ble/gap.h>

/* Defined in furi_hal_bt.c — bridges BLE events to the BT service */
extern void furi_hal_bt_emit_gap_event(GapEvent event);

/* ble_store_config_init() is provided by the NimBLE store/config module but is
 * not always declared in a public header on ESP-IDF. */
extern void ble_store_config_init(void);

#define TAG "ble_serial"

/* ---- UUIDs ----
 * NimBLE ble_uuid128_t stores bytes little-endian (LSB first), which is exactly
 * the byte order the original Bluedroid arrays used, so the byte sequences below
 * are copied verbatim from the Bluedroid version. */

/* Service: 8fe5b3d5-2e7f-4a98-2a48-7acc60fe0000 */
static const ble_uuid128_t serial_svc_uuid = BLE_UUID128_INIT(
    0x00, 0x00, 0xFE, 0x60, 0xCC, 0x7A, 0x48, 0x2A,
    0x98, 0x4A, 0x7F, 0x2E, 0xD5, 0xB3, 0xE5, 0x8F);

/* TX (Flipper→Client): 19ed82ae-ed21-4c9d-4145-228e61fe0000 */
static const ble_uuid128_t serial_tx_uuid = BLE_UUID128_INIT(
    0x00, 0x00, 0xFE, 0x61, 0x8E, 0x22, 0x45, 0x41,
    0x9D, 0x4C, 0x21, 0xED, 0xAE, 0x82, 0xED, 0x19);

/* RX (Client→Flipper): 19ed82ae-ed21-4c9d-4145-228e62fe0000 */
static const ble_uuid128_t serial_rx_uuid = BLE_UUID128_INIT(
    0x00, 0x00, 0xFE, 0x62, 0x8E, 0x22, 0x45, 0x41,
    0x9D, 0x4C, 0x21, 0xED, 0xAE, 0x82, 0xED, 0x19);

/* Flow Control: 19ed82ae-ed21-4c9d-4145-228e63fe0000 */
static const ble_uuid128_t serial_flow_uuid = BLE_UUID128_INIT(
    0x00, 0x00, 0xFE, 0x63, 0x8E, 0x22, 0x45, 0x41,
    0x9D, 0x4C, 0x21, 0xED, 0xAE, 0x82, 0xED, 0x19);

/* RPC Status: 19ed82ae-ed21-4c9d-4145-228e64fe0000 */
static const ble_uuid128_t serial_rpc_uuid = BLE_UUID128_INIT(
    0x00, 0x00, 0xFE, 0x64, 0x8E, 0x22, 0x45, 0x41,
    0x9D, 0x4C, 0x21, 0xED, 0xAE, 0x82, 0xED, 0x19);

/* Custom Flipper API Version characteristic: 03f6666d-ae5e-47c8-8e1a-5d873eb5a933 */
static const ble_uuid128_t dis_api_version_uuid = BLE_UUID128_INIT(
    0x33, 0xa9, 0xb5, 0x3e, 0x87, 0x5d, 0x1a, 0x8e,
    0xc8, 0x47, 0x5e, 0xae, 0x6d, 0x66, 0xf6, 0x03);

/* ---- Characteristic value handles (filled by NimBLE at service registration) ---- */
static uint16_t tx_val_handle;
static uint16_t rx_val_handle;
static uint16_t flow_val_handle;
static uint16_t rpc_val_handle;
static uint16_t bas_level_handle;

/* ---- Device Information Service (0x180A) constant values ---- */
static const char dis_manufacturer[] = "Flipper Devices Inc.";
static const char dis_hw_revision[] = "ESP32-C6 1.0";
static const char dis_sw_revision[] = "1.4.3";
/* API version format: "major.minor" — pulled from the protobuf header so it
 * stays in sync with rpc_system. Anything < 0.6 makes the Flipper iOS app
 * report "Outdated Firmware Version"; anything >= 1.0 makes it report
 * "Outdated Mobile App Version". Current value: 0.25. */
static const char dis_api_version[] =
    BLE_SERIAL_STR(PROTOBUF_MAJOR_VERSION) "." BLE_SERIAL_STR(PROTOBUF_MINOR_VERSION);

/* Battery level (no real battery on the ESP32 dev board) */
static uint8_t bas_battery_level = 100;

/* ---- Instance ---- */

struct BleSerial {
    SemaphoreHandle_t mutex;
    BleSerialConfig config;
    bool connected;
    uint16_t conn_handle;

    /* Pinned by host-task callbacks via serial_acquire_active(); drained to 0 by
     * ble_serial_free() before the instance is deleted. Guarded by the GLOBAL
     * mutex (serial_state.mutex), not this instance's mutex. */
    uint32_t refs;

    /* RPC Status value store. NimBLE app-defined characteristics have no
     * built-in attribute value store (unlike Bluedroid's set/get_attr_value),
     * so we keep the last-written value here to serve reads and re-notifies. */
    uint32_t rpc_status;

    SerialServiceEventCallback event_callback;
    void* event_context;
    uint16_t buff_size;
    uint16_t bytes_ready;

    BleSerialStateCallback state_callback;
    void* state_context;
};

/* ---- Global state ---- */
static struct {
    SemaphoreHandle_t mutex;
    /* ready_sem is created once and never deleted: it is signaled from the
     * NimBLE host task (sync callback), so deleting it on a timeout path would
     * leave a late give hitting a dead handle. */
    SemaphoreHandle_t ready_sem;   /* signaled on host sync (stack ready) */
    bool initialized;              /* controller + host brought up */
    bool svcs_added;               /* GATT services registered with the host */
    bool synced;                   /* NimBLE host<->controller sync complete */
    bool advertising;
    bool advertising_requested;
    uint8_t own_addr_type;         /* BLE_OWN_ADDR_PUBLIC / _RANDOM */
    BleSerial* active;
} serial_state = {0};

/* ---- Advertising parameters ---- */
static struct ble_gap_adv_params serial_adv_params = {
    .conn_mode = BLE_GAP_CONN_MODE_UND,   /* connectable undirected (was ADV_TYPE_IND) */
    .disc_mode = BLE_GAP_DISC_MODE_GEN,
    .itvl_min = 0x20,
    .itvl_max = 0x40,
};

/* ---- Forward declarations ---- */
static int ble_gap_event(struct ble_gap_event* event, void* arg);
static void serial_try_start_advertising_locked(void);

/* ---- Helpers ---- */

static void serial_lock(BleSerial* s) {
    xSemaphoreTake(s->mutex, portMAX_DELAY);
}
static void serial_unlock(BleSerial* s) {
    xSemaphoreGive(s->mutex);
}
static void serial_lock_global(void) {
    xSemaphoreTake(serial_state.mutex, portMAX_DELAY);
}
static void serial_unlock_global(void) {
    xSemaphoreGive(serial_state.mutex);
}

/*
 * Host-task GATT/GAP callbacks capture serial_state.active, then dereference /
 * lock the instance. ble_serial_free() can null active and delete the instance in
 * that gap -> use-after-free. Pin the active instance with a refcount taken under
 * the global lock (where active is valid), which ble_serial_free() drains to 0
 * before freeing. Every acquire that returns non-NULL MUST be released.
 */
static BleSerial* serial_acquire_active(void) {
    serial_lock_global();
    BleSerial* a = serial_state.active;
    if(a) a->refs++;
    serial_unlock_global();
    return a;
}
static void serial_release_active(BleSerial* a) {
    if(!a) return;
    serial_lock_global();
    if(a->refs > 0) a->refs--;
    serial_unlock_global();
}

static void serial_update_connection(BleSerial* s, bool connected) {
    BleSerialStateCallback cb = NULL;
    void* ctx = NULL;

    serial_lock(s);
    s->connected = connected;
    cb = s->state_callback;
    ctx = s->state_context;
    serial_unlock(s);

    if(cb) cb(connected, ctx);
}

static uint8_t serial_io_cap(BleSerialPairingMode mode) {
    switch(mode) {
    case BleSerialPairingDisplayOnly:
        return BLE_HS_IO_DISPLAY_ONLY;
    case BleSerialPairingInputOnly:
        return BLE_HS_IO_KEYBOARD_ONLY;
    case BleSerialPairingPinCodeVerifyYesNo:
    default:
        return BLE_HS_IO_DISPLAY_YESNO;
    }
}

static void serial_make_random_static_addr(uint8_t mac[6]) {
    mac[5] |= 0xC0; /* Set top 2 bits for random static address */
}

static bool serial_has_custom_mac(const BleSerialConfig* config) {
    static const uint8_t zero[6] = {0};
    return memcmp(config->mac, zero, 6) != 0;
}

/* Send a notification (need_confirm == false path) on the given value handle. */
static void serial_notify(uint16_t conn_handle, uint16_t val_handle, const void* data, uint16_t len) {
    struct os_mbuf* om = ble_hs_mbuf_from_flat(data, len);
    if(!om) {
        ESP_LOGE(TAG, "notify: mbuf alloc failed");
        return;
    }
    int rc = ble_gatts_notify_custom(conn_handle, val_handle, om);
    if(rc != 0) {
        ESP_LOGW(TAG, "notify: ble_gatts_notify_custom rc=%d", rc);
    }
}

/* ---- GATT access callbacks ----
 * NimBLE returns characteristic data directly from the access callback; there
 * is NO explicit send_response call (that was Bluedroid's model). */

static int serial_tx_access(
    uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt* ctxt, void* arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    /* TX is a notify/indicate source. A raw READ just returns empty (matches
     * the original, which sent a zero-length response). An encrypted read still
     * triggers client-initiated pairing via the char's read permission. */
    if(ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return 0; /* empty value */
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int serial_rx_access(
    uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt* ctxt, void* arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if(ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return 0; /* empty value */
    }
    if(ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    BleSerial* serial = serial_acquire_active();
    if(!serial) return 0;

    /* Flatten the incoming write into a local buffer. */
    static uint8_t rx_buf[BLE_SVC_SERIAL_DATA_LEN_MAX];
    uint16_t rx_len = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, rx_buf, sizeof(rx_buf), &rx_len);
    if(rc != 0) {
        ESP_LOGW(TAG, "rx: mbuf_to_flat rc=%d", rc);
        serial_release_active(serial);
        return BLE_ATT_ERR_UNLIKELY;
    }
    ESP_LOGD(TAG, "gatts: RX data %d bytes", rx_len);

    SerialServiceEventCallback cb = NULL;
    void* cbctx = NULL;
    serial_lock(serial);
    cb = serial->event_callback;
    cbctx = serial->event_context;
    if(serial->bytes_ready >= rx_len) {
        serial->bytes_ready -= rx_len;
    } else {
        serial->bytes_ready = 0;
    }
    serial_unlock(serial);

    uint16_t new_bytes_ready = 0;
    if(cb) {
        SerialServiceEvent ev = {
            .event = SerialServiceEventTypeDataReceived,
            .data = {
                .buffer = rx_buf,
                .size = rx_len,
            },
        };
        new_bytes_ready = cb(ev, cbctx);
    }

    /* Update internal flow tracking. Don't send a notification here — STM32 only
     * sends flow notifications via the notify_buffer_is_empty callback. */
    if(new_bytes_ready > 0) {
        serial_lock(serial);
        serial->bytes_ready = new_bytes_ready;
        serial_unlock(serial);
    }
    serial_release_active(serial);
    return 0;
}

static int serial_flow_access(
    uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt* ctxt, void* arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    BleSerial* serial = serial_acquire_active();
    if(ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        /* Flow Control value is big-endian uint32 of the free buffer size
         * (matches STM32's REVERSE_BYTES_U32). */
        uint32_t flow_be = __builtin_bswap32((uint32_t)(serial ? serial->buff_size : 0));
        int rc = os_mbuf_append(ctxt->om, &flow_be, sizeof(flow_be));
        serial_release_active(serial);
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    serial_release_active(serial);
    return BLE_ATT_ERR_UNLIKELY;
}

static int serial_rpc_access(
    uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt* ctxt, void* arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    BleSerial* serial = serial_acquire_active();

    if(ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        /* RPC Status is native little-endian uint32 (no byte swap), matching STM32. */
        uint32_t val = serial ? serial->rpc_status : 0;
        int rc = os_mbuf_append(ctxt->om, &val, sizeof(val));
        serial_release_active(serial);
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if(ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t buf[4] = {0};
        uint16_t len = 0;
        if(ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) {
            serial_release_active(serial);
            return BLE_ATT_ERR_UNLIKELY;
        }
        ESP_LOGI(TAG, "gatts: RPC Status write, len=%d", len);
        if(len >= 4 && serial) {
            uint32_t val = 0;
            memcpy(&val, buf, 4);
            serial->rpc_status = val;
            ESP_LOGI(TAG, "  -> RPC Status value=0x%08lx", (unsigned long)val);
            if(val == FuriHalBtSerialRpcStatusNotActive) {
                SerialServiceEventCallback cb = NULL;
                void* cbctx = NULL;
                serial_lock(serial);
                cb = serial->event_callback;
                cbctx = serial->event_context;
                serial_unlock(serial);
                if(cb) {
                    SerialServiceEvent ev = {
                        .event = SerialServiceEventTypesBleResetRequest,
                    };
                    cb(ev, cbctx);
                }
            }
        }
        serial_release_active(serial);
        return 0;
    }
    serial_release_active(serial);
    return BLE_ATT_ERR_UNLIKELY;
}

/* Descriptor for a read-only constant characteristic value, passed as the
 * access callback's `arg`. */
typedef struct {
    const void* data;
    uint16_t len;
} SerialConstDesc;

/* Generic read-only-constant access callback for DIS / BAS characteristics. */
static int serial_read_const_access(
    uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt* ctxt, void* arg) {
    (void)conn_handle;
    (void)attr_handle;
    if(ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    const SerialConstDesc* d = arg;
    int rc = os_mbuf_append(ctxt->om, d->data, d->len);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* Read-const descriptors (must be static: referenced by the const svc table). */
static const SerialConstDesc
    dis_manufacturer_desc = {dis_manufacturer, sizeof(dis_manufacturer) - 1},
    dis_hw_desc = {dis_hw_revision, sizeof(dis_hw_revision) - 1},
    dis_sw_desc = {dis_sw_revision, sizeof(dis_sw_revision) - 1},
    dis_api_desc = {dis_api_version, sizeof(dis_api_version) - 1};

static int bas_level_access(
    uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt* ctxt, void* arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if(ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    int rc = os_mbuf_append(ctxt->om, &bas_battery_level, sizeof(bas_battery_level));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* ---- GATT service definition ----
 * Replaces the three Bluedroid attribute tables (serial / DIS / BAS). NimBLE
 * auto-creates the CCCD descriptors for NOTIFY/INDICATE characteristics, so the
 * explicit CCCD attributes from the Bluedroid table are gone. */
static const struct ble_gatt_svc_def gatt_svcs[] = {
    /* Serial service */
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &serial_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                /* TX (Flipper→Client). Declared NOTIFY|INDICATE: the STM32 TX
                 * char is INDICATE and ble_serial_tx() sends indications so the
                 * DataSent event can fire on client confirmation. */
                .uuid = &serial_tx_uuid.u,
                .access_cb = serial_tx_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY |
                         BLE_GATT_CHR_F_INDICATE | BLE_GATT_CHR_F_READ_ENC,
                .val_handle = &tx_val_handle,
            },
            {
                /* RX (Client→Flipper) */
                .uuid = &serial_rx_uuid.u,
                .access_cb = serial_rx_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &rx_val_handle,
            },
            {
                /* Flow Control (notify) */
                .uuid = &serial_flow_uuid.u,
                .access_cb = serial_flow_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &flow_val_handle,
            },
            {
                /* RPC Status (write + notify) */
                .uuid = &serial_rpc_uuid.u,
                .access_cb = serial_rpc_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &rpc_val_handle,
            },
            {0}, /* end of characteristics */
        },
    },
    /* Device Information Service (0x180A) — required by Flipper Android app. */
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180A),
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = BLE_UUID16_DECLARE(0x2A29), /* Manufacturer Name */
                .access_cb = serial_read_const_access,
                .arg = (void*)&dis_manufacturer_desc,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = BLE_UUID16_DECLARE(0x2A27), /* Hardware Revision */
                .access_cb = serial_read_const_access,
                .arg = (void*)&dis_hw_desc,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = BLE_UUID16_DECLARE(0x2A28), /* Software Revision */
                .access_cb = serial_read_const_access,
                .arg = (void*)&dis_sw_desc,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = &dis_api_version_uuid.u,    /* Custom Flipper API Version */
                .access_cb = serial_read_const_access,
                .arg = (void*)&dis_api_desc,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {0},
        },
    },
    /* Battery Service (0x180F) — required by Flipper Android app for Connected state. */
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180F),
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = BLE_UUID16_DECLARE(0x2A19), /* Battery Level */
                .access_cb = bas_level_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &bas_level_handle,
            },
            {0},
        },
    },
    {0}, /* end of services */
};

/* ---- Advertising ---- */

static int serial_set_adv_fields(const BleSerialConfig* config) {
    struct ble_hs_adv_fields adv_fields = {0};
    struct ble_hs_adv_fields rsp_fields = {0};

    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    /* Advertise 16-bit UUID 0x3080 (same as STM32 — this is what qFlipper scans
     * for). NOTE: the shared NimBLE contract suggested advertising the 128-bit
     * serial service UUID; we intentionally keep the 16-bit 0x3080 because that
     * is the value the Flipper apps actually scan for. */
    static ble_uuid16_t adv_uuid16 = BLE_UUID16_INIT(0x3080);
    adv_fields.uuids16 = &adv_uuid16;
    adv_fields.num_uuids16 = 1;
    adv_fields.uuids16_is_complete = 1;

    size_t name_len = strlen(config->device_name);

    /* Try to fit the device name in the primary adv payload (matches the
     * original). If it overflows the 31-byte adv PDU, fall back to putting the
     * name in the scan response. */
    adv_fields.name = (uint8_t*)config->device_name;
    adv_fields.name_len = name_len;
    adv_fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&adv_fields);
    if(rc == BLE_HS_EMSGSIZE) {
        ESP_LOGW(TAG, "adv: name too big for adv PDU, moving to scan response");
        adv_fields.name = NULL;
        adv_fields.name_len = 0;
        adv_fields.name_is_complete = 0;
        rc = ble_gap_adv_set_fields(&adv_fields);
        if(rc != 0) return rc;

        rsp_fields.name = (uint8_t*)config->device_name;
        rsp_fields.name_len = name_len;
        rsp_fields.name_is_complete = 1;
        rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    }
    return rc;
}

static void serial_try_start_advertising_locked(void) {
    ESP_LOGI(TAG, "try_start_adv: requested=%d synced=%d advertising=%d active=%p connected=%d",
        serial_state.advertising_requested,
        serial_state.synced,
        serial_state.advertising,
        (void*)serial_state.active,
        serial_state.active ? serial_state.active->connected : -1);

    if(!serial_state.advertising_requested) { ESP_LOGW(TAG, "  blocked: not requested"); return; }
    if(!serial_state.synced) { ESP_LOGW(TAG, "  blocked: host not synced yet"); return; }
    if(serial_state.advertising) { ESP_LOGW(TAG, "  blocked: already advertising"); return; }
    if(!serial_state.active) { ESP_LOGW(TAG, "  blocked: no active instance"); return; }
    if(serial_state.active->connected) { ESP_LOGW(TAG, "  blocked: connected"); return; }

    int rc = serial_set_adv_fields(&serial_state.active->config);
    if(rc != 0) {
        ESP_LOGE(TAG, "  -> adv_set_fields failed rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "  -> calling ble_gap_adv_start");
    rc = ble_gap_adv_start(
        serial_state.own_addr_type, NULL, BLE_HS_FOREVER, &serial_adv_params,
        ble_gap_event, NULL);
    if(rc == 0) {
        serial_state.advertising = true;
        ESP_LOGI(TAG, "  -> advertising started OK");
    } else {
        ESP_LOGE(TAG, "  -> ble_gap_adv_start failed rc=%d", rc);
    }
}

/* ---- Post-auth / post-subscribe re-notify helpers ---- */

static void serial_resend_flow_and_rpc(BleSerial* s) {
    if(!s || !s->connected) return;
    /* Re-send Flow Control if a callback (buffer size) was set. */
    if(flow_val_handle && s->event_callback) {
        uint32_t flow_be = __builtin_bswap32((uint32_t)s->buff_size);
        serial_notify(s->conn_handle, flow_val_handle, &flow_be, sizeof(flow_be));
    }
    /* Re-send RPC Status (native LE uint32). */
    if(rpc_val_handle) {
        uint32_t val = s->rpc_status;
        serial_notify(s->conn_handle, rpc_val_handle, &val, sizeof(val));
    }
}

/* ---- Single GAP/GATT event handler ----
 * Collapses all the Bluedroid GAP + GATTS events into one NimBLE callback. */

static int ble_gap_event_body(struct ble_gap_event* event, BleSerial* serial) {
    switch(event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "gap: connect status=%d conn_handle=%d",
            event->connect.status, event->connect.conn_handle);
        if(event->connect.status == 0) {
            serial_lock_global();
            serial_state.advertising = false;
            serial_unlock_global();
            if(serial) {
                serial->conn_handle = event->connect.conn_handle;
                serial_update_connection(serial, true);
            }
            /* Request encryption — triggers Passkey Entry with legacy pairing
             * (replaces esp_ble_set_encryption on the Bluedroid connect event). */
            int rc = ble_gap_security_initiate(event->connect.conn_handle);
            if(rc != 0 && rc != BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "gap: security_initiate rc=%d", rc);
            }
        } else {
            /* Connection failed — resume advertising. */
            serial_lock_global();
            serial_state.advertising = false;
            serial_try_start_advertising_locked();
            serial_unlock_global();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "gap: disconnect reason=0x%x", event->disconnect.reason);
        if(serial) {
            serial_update_connection(serial, false);
        }
        serial_lock_global();
        serial_state.advertising = false;
        serial_try_start_advertising_locked();
        serial_unlock_global();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "gap: adv complete reason=%d", event->adv_complete.reason);
        serial_lock_global();
        serial_state.advertising = false;
        serial_try_start_advertising_locked();
        serial_unlock_global();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "gap: subscribe attr=%d notify=%d indicate=%d",
            event->subscribe.attr_handle,
            event->subscribe.cur_notify,
            event->subscribe.cur_indicate);
        if(!serial) return 0;
        /* Re-send current value when the client subscribes (replaces the
         * Bluedroid CCCD-write re-send logic). */
        if(event->subscribe.attr_handle == rpc_val_handle && event->subscribe.cur_notify) {
            if(serial->connected) {
                uint32_t val = serial->rpc_status;
                serial_notify(serial->conn_handle, rpc_val_handle, &val, sizeof(val));
            }
        } else if(event->subscribe.attr_handle == flow_val_handle && event->subscribe.cur_notify) {
            if(serial->connected && serial->event_callback) {
                uint32_t flow_be = __builtin_bswap32((uint32_t)serial->buff_size);
                serial_notify(serial->conn_handle, flow_val_handle, &flow_be, sizeof(flow_be));
            }
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_TX:
        /* Fired for each notification/indication transmitted. For indications a
         * second event with status == BLE_HS_EDONE marks client confirmation —
         * the direct equivalent of the Bluedroid ESP_GATTS_CONF_EVT. Only fire
         * DataSent for the TX characteristic. */
        if(!serial) return 0;
        if(event->notify_tx.attr_handle == tx_val_handle &&
           event->notify_tx.indication &&
           event->notify_tx.status == BLE_HS_EDONE) {
            SerialServiceEventCallback cb = NULL;
            void* cbctx = NULL;
            serial_lock(serial);
            cb = serial->event_callback;
            cbctx = serial->event_context;
            serial_unlock(serial);
            if(cb) {
                SerialServiceEvent ev = {.event = SerialServiceEventTypeDataSent};
                cb(ev, cbctx);
            }
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "gap: MTU=%d", event->mtu.value);
        /* Bridge MTU update to the GAP event system (max notify payload = MTU-3). */
        if(serial && serial->connected) {
            GapEvent gap_ev = {
                .type = GapEventTypeUpdateMTU,
                .data.max_packet_size = event->mtu.value - 3,
            };
            furi_hal_bt_emit_gap_event(gap_ev);
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "gap: enc change status=%d", event->enc_change.status);
        /* After encryption is established, re-send RPC Status and Flow Control:
         * the initial notifications at connect time are typically lost because
         * the client hasn't subscribed to CCCDs yet (matches Bluedroid AUTH_CMPL). */
        if(event->enc_change.status == 0 && serial) {
            serial_resend_flow_and_rpc(serial);
        }
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        ESP_LOGI(TAG, "gap: passkey action=%d", event->passkey.params.action);
        struct ble_sm_io io = {0};
        io.action = event->passkey.params.action;

        if(event->passkey.params.action == BLE_SM_IOACT_DISP) {
            /* DisplayOnly IO → Passkey Entry: WE choose the passkey and show it
             * on the Flipper display for the user to type on the phone. The
             * Bluedroid version let the stack generate it (PASSKEY_NOTIF); with
             * NimBLE DisplayOnly the host provides it. */
            uint32_t passkey = esp_random() % 1000000u;
            io.passkey = passkey;
            ESP_LOGI(TAG, "gap: passkey display: %06lu", (unsigned long)passkey);
            GapEvent gap_ev = {
                .type = GapEventTypePinCodeShow,
                .data.pin_code = passkey,
            };
            furi_hal_bt_emit_gap_event(gap_ev);
            ble_sm_inject_io(event->passkey.conn_handle, &io);
        } else if(event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            /* Numeric comparison — show the code and auto-confirm (no physical
             * button to reject), matching the Bluedroid NC_REQ path. */
            ESP_LOGI(TAG, "gap: numeric comparison: %06lu",
                (unsigned long)event->passkey.params.numcmp);
            GapEvent gap_ev = {
                .type = GapEventTypePinCodeShow,
                .data.pin_code = event->passkey.params.numcmp,
            };
            furi_hal_bt_emit_gap_event(gap_ev);
            io.numcmp_accept = 1;
            ble_sm_inject_io(event->passkey.conn_handle, &io);
        }
        return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* An already-bonded peer is re-pairing. Delete the old bond and retry so
         * the pairing proceeds instead of failing. */
        struct ble_gap_conn_desc desc;
        if(ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "gap: conn params updated status=%d", event->conn_update.status);
        return 0;

    default:
        return 0;
    }
}

/* NimBLE host-task entry: pin the active instance across the whole handler so
 * ble_serial_free() can't delete it mid-event; refs is drained in ble_serial_free. */
static int ble_gap_event(struct ble_gap_event* event, void* arg) {
    (void)arg;
    BleSerial* serial = serial_acquire_active();
    int rc = ble_gap_event_body(event, serial);
    serial_release_active(serial);
    return rc;
}

/* ---- NimBLE host callbacks ---- */

static void serial_on_reset(int reason) {
    ESP_LOGW(TAG, "nimble host reset, reason=%d", reason);
    serial_lock_global();
    serial_state.synced = false;
    serial_state.advertising = false;
    serial_unlock_global();
}

static void serial_on_sync(void) {
    /* Make sure we have an identity address to advertise with. If a custom
     * random static address was configured, own_addr_type is already
     * BLE_OWN_ADDR_RANDOM and the address was set via ble_hs_id_set_rnd(). */
    int rc = ble_hs_util_ensure_addr(0);
    if(rc != 0) {
        ESP_LOGW(TAG, "sync: ensure_addr rc=%d", rc);
    }

    serial_lock_global();
    serial_state.synced = true;
    serial_try_start_advertising_locked();
    serial_unlock_global();

    if(serial_state.ready_sem) {
        xSemaphoreGive(serial_state.ready_sem);
    }
    ESP_LOGI(TAG, "nimble host synced, stack ready");
}

static void serial_gatt_register_cb(struct ble_gatt_register_ctxt* ctxt, void* arg) {
    (void)arg;
    /* Handles are also captured via the .val_handle pointers in the svc table;
     * this callback is kept for logging / diagnostics. */
    switch(ctxt->op) {
    case BLE_GATT_REGISTER_OP_SVC:
        ESP_LOGD(TAG, "registered service handle=%d", ctxt->svc.handle);
        break;
    case BLE_GATT_REGISTER_OP_CHR:
        ESP_LOGD(TAG, "registered chr def_handle=%d val_handle=%d",
            ctxt->chr.def_handle, ctxt->chr.val_handle);
        break;
    default:
        break;
    }
}

static void serial_host_task(void* param) {
    (void)param;
    ESP_LOGI(TAG, "nimble host task started");
    /* Returns only when nimble_port_stop() is called. */
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ---- BLE stack init (controller unchanged; host swapped to NimBLE) ---- */

static esp_err_t serial_apply_security_config(const BleSerialConfig* config) {
    /* Legacy pairing (no Secure Connections) with MITM + optional bond.
     * SC forces Numeric Comparison when the phone IO is DisplayYesNo, but the
     * Flipper app expects Passkey Entry (PIN display → user enters on phone).
     * Legacy pairing with DisplayOnly IO → Passkey Entry. */
    ble_hs_cfg.sm_io_cap = serial_io_cap(config->pairing);
    ble_hs_cfg.sm_bonding = config->bonding ? 1 : 0;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 0; /* legacy pairing */
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    return ESP_OK;
}

static esp_err_t serial_stack_init_once(void) {
    esp_err_t err = ESP_OK;

    if(!serial_state.mutex) {
        serial_state.mutex = xSemaphoreCreateMutex();
        if(!serial_state.mutex) return ESP_ERR_NO_MEM;
    }

    serial_lock_global();
    if(serial_state.initialized) {
        serial_unlock_global();
        return ESP_OK;
    }
    serial_unlock_global();

    err = nvs_flash_init();
    if(err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if(err != ESP_OK) return err;

    /* ---- Controller: UNCHANGED from the Bluedroid version ---- */
    err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    /* ---- Host: NimBLE (replaces esp_bluedroid_init_with_cfg/enable) ----
     * esp_nimble_init() brings up the NimBLE host on top of the already-inited
     * controller. NOTE for the integrator: on some ESP-IDF versions the combined
     * entry point nimble_port_init() must be used instead (it also inits the
     * controller); if you switch to it, drop the esp_bt_controller_* calls
     * above. Requires CONFIG_BT_NIMBLE_ENABLED=y (Bluedroid disabled) in
     * sdkconfig. */
    err = esp_nimble_init();
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "esp_nimble_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Host configuration. */
    ble_hs_cfg.reset_cb = serial_on_reset;
    ble_hs_cfg.sync_cb = serial_on_sync;
    ble_hs_cfg.gatts_register_cb = serial_gatt_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Standard GAP/GATT services + bonding key store. */
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_store_config_init();

    /* Register our GATT services once. val_handles are filled during host
     * registration (at/after sync). */
    if(!serial_state.svcs_added) {
        int rc = ble_gatts_count_cfg(gatt_svcs);
        if(rc != 0) {
            ESP_LOGE(TAG, "ble_gatts_count_cfg rc=%d", rc);
            return ESP_FAIL;
        }
        rc = ble_gatts_add_svcs(gatt_svcs);
        if(rc != 0) {
            ESP_LOGE(TAG, "ble_gatts_add_svcs rc=%d", rc);
            return ESP_FAIL;
        }
        serial_state.svcs_added = true;
    }

    /* Set preferred MTU (max serial payload + 3-byte ATT header). */
    ble_att_set_preferred_mtu(BLE_SVC_SERIAL_DATA_LEN_MAX + 3);

    /* Start the host task. */
    nimble_port_freertos_init(serial_host_task);

    serial_lock_global();
    serial_state.initialized = true;
    serial_unlock_global();

    return ESP_OK;
}

/* ---- Public API ---- */

void ble_serial_get_default_mac(uint8_t mac[6]) {
    esp_read_mac(mac, ESP_MAC_BT);
}

BleSerial* ble_serial_alloc(const BleSerialConfig* config) {
    if(!config) return NULL;

    esp_err_t err = serial_stack_init_once();
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "BLE stack init failed: %s", esp_err_to_name(err));
        return NULL;
    }

    BleSerial* serial = calloc(1, sizeof(BleSerial));
    if(!serial) return NULL;

    serial->mutex = xSemaphoreCreateMutex();
    if(!serial->mutex) {
        free(serial);
        return NULL;
    }

    memcpy(&serial->config, config, sizeof(serial->config));
    ESP_LOGI(TAG, "alloc name=%s bonding=%d pairing=%d",
        serial->config.device_name, serial->config.bonding, serial->config.pairing);

    serial_lock_global();
    if(serial_state.active) {
        serial_unlock_global();
        vSemaphoreDelete(serial->mutex);
        free(serial);
        return NULL;
    }
    serial_state.active = serial;
    serial_state.advertising = false;
    serial_state.advertising_requested = false;
    serial_unlock_global();

    /* Device name (for GAP + adv). */
    err = (ble_svc_gap_device_name_set(serial->config.device_name) == 0) ? ESP_OK : ESP_FAIL;
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "device_name_set failed");
        goto error;
    }

    ESP_LOGI(TAG, "alloc: applying security config");
    err = serial_apply_security_config(&serial->config);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "Security config failed");
        goto error;
    }

    /* Address selection. */
    serial_lock_global();
    if(serial_has_custom_mac(&serial->config)) {
        uint8_t mac[6];
        memcpy(mac, serial->config.mac, 6);
        serial_make_random_static_addr(mac);
        /* NimBLE ble_addr_t stores the address little-endian (LSB first), same
         * layout as the config mac buffer. */
        int rc = ble_hs_id_set_rnd(mac);
        if(rc != 0) {
            ESP_LOGW(TAG, "ble_hs_id_set_rnd rc=%d (may need to wait for sync)", rc);
        }
        serial_state.own_addr_type = BLE_OWN_ADDR_RANDOM;
    } else {
        serial_state.own_addr_type = BLE_OWN_ADDR_PUBLIC;
    }
    serial_unlock_global();

    /* Prepare the ready semaphore. Created once, NEVER deleted (a late sync give
     * on a timeout path would otherwise hit a deleted handle). Drain any stale
     * give from a previous attempt. */
    if(!serial_state.ready_sem) {
        serial_state.ready_sem = xSemaphoreCreateBinary();
    }
    if(serial_state.ready_sem) {
        xSemaphoreTake(serial_state.ready_sem, 0);
    }

    /* If the host is already synced (stack was previously up), we are ready
     * immediately; otherwise wait for the sync callback. Services were
     * registered synchronously in serial_stack_init_once(). */
    ESP_LOGI(TAG, "alloc: waiting for host sync...");
    bool synced_now;
    serial_lock_global();
    synced_now = serial_state.synced;
    serial_unlock_global();

    if(!synced_now) {
        if(!serial_state.ready_sem ||
           xSemaphoreTake(serial_state.ready_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
            /* Non-fatal: services are registered; advertising will begin once
             * the host finishes syncing and start_advertising was requested. */
            ESP_LOGW(TAG, "alloc: host sync not confirmed within timeout");
        }
    }

    ESP_LOGI(TAG, "Serial service ready (synced=%d)", serial_state.synced);
    return serial;

error:
    serial_lock_global();
    if(serial_state.active == serial) serial_state.active = NULL;
    serial_unlock_global();
    if(serial->mutex) vSemaphoreDelete(serial->mutex);
    free(serial);
    return NULL;
}

void ble_serial_free(BleSerial* serial) {
    if(!serial) return;

    serial_lock_global();
    serial_state.advertising_requested = false;
    if(serial_state.advertising) {
        ble_gap_adv_stop();
        serial_state.advertising = false;
    }
    bool was_connected = serial->connected;
    uint16_t conn_handle = serial->conn_handle;
    if(serial_state.active == serial) serial_state.active = NULL;
    serial_unlock_global();

    /* Terminate any active connection so the phone sees a clean disconnect. */
    if(was_connected) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }

    /* Full host teardown per the shared NimBLE contract. NimBLE runs a single
     * host for the whole firmware and the serial/HID profiles are mutually
     * exclusive (see furi_hal_bt.c), so it is safe to stop and deinit the host
     * here. The controller is left initialized (matches the Bluedroid version,
     * which never tore the controller down). serial_stack_init_once() will
     * re-init the host on the next alloc. */
    nimble_port_stop();
    nimble_port_deinit();

    serial_lock_global();
    serial_state.initialized = false;
    serial_state.svcs_added = false;
    serial_state.synced = false;
    serial_unlock_global();

    /* Drain any in-flight host-task callback that pinned this instance (active was
     * nulled above, so refs only falls to 0) before deleting its mutex + freeing.
     * Closes the capture-then-use UAF. */
    for(;;) {
        serial_lock_global();
        uint32_t refs = serial->refs;
        serial_unlock_global();
        if(refs == 0) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if(serial->mutex) vSemaphoreDelete(serial->mutex);
    free(serial);
}

void ble_serial_reset_initialized(void) {
    /* The global mutex is created lazily in serial_stack_init_once(). If the
     * stack was never brought up (mutex still NULL), there is nothing to reset
     * and taking the NULL mutex would assert — just clear the flag. */
    if(!serial_state.mutex) {
        serial_state.initialized = false;
        return;
    }
    serial_lock_global();
    serial_state.initialized = false;
    serial_unlock_global();
}

void ble_serial_set_state_callback(BleSerial* serial, BleSerialStateCallback callback, void* context) {
    if(!serial) return;
    serial_lock(serial);
    serial->state_callback = callback;
    serial->state_context = context;
    bool connected = serial->connected;
    serial_unlock(serial);
    if(callback) callback(connected, context);
}

bool ble_serial_is_connected(BleSerial* serial) {
    if(!serial) return false;
    serial_lock(serial);
    bool c = serial->connected;
    serial_unlock(serial);
    return c;
}

void ble_serial_set_event_callback(
    BleSerial* serial,
    uint16_t buff_size,
    SerialServiceEventCallback callback,
    void* context) {
    if(!serial) return;

    serial_lock(serial);
    serial->event_callback = callback;
    serial->event_context = context;
    serial->buff_size = buff_size;
    serial->bytes_ready = buff_size;
    bool connected = serial->connected;
    uint16_t conn_handle = serial->conn_handle;
    serial_unlock(serial);

    /* Update flow control characteristic (big-endian, like STM32). */
    if(flow_val_handle && connected) {
        uint32_t flow_be = __builtin_bswap32((uint32_t)buff_size);
        serial_notify(conn_handle, flow_val_handle, &flow_be, sizeof(flow_be));
    }
}

bool ble_serial_tx(BleSerial* serial, uint8_t* data, uint16_t size) {
    if(!serial || !data || size == 0) return false;

    serial_lock(serial);
    bool connected = serial->connected;
    uint16_t conn_handle = serial->conn_handle;
    serial_unlock(serial);

    if(!connected || !tx_val_handle) return false;

    /* Send as an indication (need_confirm == true in the Bluedroid version), so
     * the DataSent event fires on the client confirmation via
     * BLE_GAP_EVENT_NOTIFY_TX (status == BLE_HS_EDONE). */
    struct os_mbuf* om = ble_hs_mbuf_from_flat(data, size);
    if(!om) return false;
    int rc = ble_gatts_indicate_custom(conn_handle, tx_val_handle, om);
    return rc == 0;
}

void ble_serial_set_rpc_active(BleSerial* serial, FuriHalBtSerialRpcStatus status) {
    if(!serial) return;

    /* RPC Status is native LE uint32 (no byte-swap), matching STM32. Flow
     * Control uses big-endian, but RPC Status does not. */
    uint32_t val = (uint32_t)status;
    serial_lock(serial);
    serial->rpc_status = val;
    bool connected = serial->connected;
    uint16_t conn_handle = serial->conn_handle;
    serial_unlock(serial);

    if(rpc_val_handle && connected) {
        serial_notify(conn_handle, rpc_val_handle, &val, sizeof(val));
    }
}

void ble_serial_notify_buffer_is_empty(BleSerial* serial) {
    if(!serial) return;

    /* Match STM32: only send a flow control update when the buffer was fully
     * consumed. */
    serial_lock(serial);
    bool was_empty = (serial->bytes_ready == 0);
    if(was_empty) {
        serial->bytes_ready = serial->buff_size;
    }
    bool connected = serial->connected;
    uint16_t buff_size = serial->buff_size;
    uint16_t conn_handle = serial->conn_handle;
    serial_unlock(serial);

    if(was_empty && connected && flow_val_handle) {
        uint32_t flow_be = __builtin_bswap32((uint32_t)buff_size);
        serial_notify(conn_handle, flow_val_handle, &flow_be, sizeof(flow_be));
    }
}

bool ble_serial_start_advertising(void) {
    ESP_LOGI(TAG, "ble_serial_start_advertising called, active=%p", (void*)serial_state.active);
    bool accepted = false;
    serial_lock_global();
    if(serial_state.active) {
        serial_state.advertising_requested = true;
        serial_try_start_advertising_locked();
        accepted = true;
    } else {
        ESP_LOGW(TAG, "ble_serial_start_advertising: no active instance!");
    }
    serial_unlock_global();
    ESP_LOGI(TAG, "ble_serial_start_advertising returning %d", accepted);
    return accepted;
}

void ble_serial_stop_advertising(void) {
    serial_lock_global();
    serial_state.advertising_requested = false;
    if(serial_state.advertising) {
        ble_gap_adv_stop();
        serial_state.advertising = false;
    }
    serial_unlock_global();
}

bool ble_serial_is_advertising(void) {
    if(!serial_state.initialized || !serial_state.mutex) return false;
    bool adv = false;
    serial_lock_global();
    adv = serial_state.advertising_requested && serial_state.active &&
          !serial_state.active->connected;
    serial_unlock_global();
    return adv;
}

bool ble_serial_is_active(void) {
    if(!serial_state.initialized || !serial_state.mutex) return false;
    bool active = false;
    serial_lock_global();
    active = serial_state.active != NULL;
    serial_unlock_global();
    return active;
}

bool ble_serial_remove_pairing(void) {
    if(!serial_state.initialized) return false;
    /* Delete all bonds from the NimBLE key store (replaces the Bluedroid
     * esp_ble_get_bond_device_list / remove_bond_device loop). */
    int rc = ble_store_clear();
    return rc == 0;
}
