#include "ble_hid.h"

#include <inttypes.h>
#include <string.h>
#include <stdlib.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <esp_bt.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <nvs_flash.h>

/* NimBLE host stack */
#include <esp_nimble_hci.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <host/ble_hs.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_uuid.h>
#include <host/ble_store.h>
#include <host/util/util.h>

/* Defined in the NimBLE store-config module but not declared in a public header
 * (ESP-IDF quirk); ESP-IDF's own examples forward-declare it like this. */
void ble_store_config_init(void);
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>
#include <store/config/ble_store_config.h>
#include <os/os_mbuf.h>

#define TAG "ble_hid"

#define BLE_HID_REPORT_ID_KEYBOARD  1
#define BLE_HID_REPORT_ID_MOUSE     2
#define BLE_HID_REPORT_ID_CONSUMER  3
#define BLE_HID_SERVICE_UUID        0x1812
#define BLE_HID_APPEARANCE_KEYBOARD 0x03C1

/* HID report reference types (HOGP) */
#define BLE_HID_REPORT_TYPE_INPUT  0x01
#define BLE_HID_REPORT_TYPE_OUTPUT 0x02

/* GATT / HOGP characteristic + descriptor UUIDs */
#define UUID16_CHR_HID_INFORMATION 0x2A4A
#define UUID16_CHR_REPORT_MAP      0x2A4B
#define UUID16_CHR_HID_CONTROL_PT  0x2A4C
#define UUID16_CHR_REPORT          0x2A4D
#define UUID16_CHR_PROTOCOL_MODE   0x2A4E
#define UUID16_DSC_REPORT_REF      0x2908
#define UUID16_SVC_BATTERY         0x180F
#define UUID16_CHR_BATTERY_LEVEL   0x2A19
#define UUID16_SVC_DEVICE_INFO     0x180A
#define UUID16_CHR_MANUFACTURER    0x2A29
#define UUID16_CHR_PNP_ID          0x2A50

#define BLE_HID_KEYBOARD_KEYS_MAX 6

typedef struct {
    uint8_t modifiers;
    uint8_t reserved;
    uint8_t keys[BLE_HID_KEYBOARD_KEYS_MAX];
} __attribute__((packed)) BleHidKeyboardReport;

typedef struct {
    uint8_t buttons;
    int8_t x;
    int8_t y;
    int8_t wheel;
} __attribute__((packed)) BleHidMouseReport;

typedef struct {
    uint16_t key;
} __attribute__((packed)) BleHidConsumerReport;

struct BleHid {
    BleHidConfig config;
    SemaphoreHandle_t mutex;
    BleHidStateCallback state_callback;
    void* state_context;
    BleHidKeyboardReport keyboard_report;
    BleHidMouseReport mouse_report;
    BleHidConsumerReport consumer_report;
    uint8_t led_state;
    bool connected;
    /* In-flight host-task callbacks that pinned this instance (guarded by the
     * global mutex). ble_hid_free() drains this to 0 before freeing — see
     * ble_hid_acquire_active(). */
    uint32_t refs;
};

typedef struct {
    bool initialized; /* NimBLE host started */
    bool synced; /* ble_hs sync complete, addr available */
    bool advertising_requested;
    bool advertising;
    bool rand_addr_enabled;
    uint8_t own_addr_type;
    uint8_t rand_addr[6];
    uint16_t conn_handle;
    uint8_t protocol_mode; /* HOGP protocol mode (1 = report protocol) */

    /* Characteristic value handles filled in by NimBLE at registration */
    uint16_t hid_kb_input_handle;
    uint16_t hid_kb_output_handle;
    uint16_t hid_mouse_input_handle;
    uint16_t hid_consumer_input_handle;
    uint16_t battery_level_handle;

    /* CCCD subscription state (per input report) */
    bool kb_notify;
    bool mouse_notify;
    bool consumer_notify;

    SemaphoreHandle_t mutex;
    BleHid* active;
} BleHidGlobalState;

static BleHidGlobalState ble_hid_state = {
    .conn_handle = BLE_HS_CONN_HANDLE_NONE,
    .own_addr_type = BLE_OWN_ADDR_PUBLIC,
    .protocol_mode = 1,
};

/* ---- Static HID data (report map identical to the Bluedroid version) ---- */

static const uint8_t ble_hid_report_map[] = {
    0x05, 0x01,
    0x09, 0x06,
    0xA1, 0x01,
    0x85, BLE_HID_REPORT_ID_KEYBOARD,
    0x05, 0x07,
    0x19, 0xE0,
    0x29, 0xE7,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x08,
    0x81, 0x02,
    0x95, 0x01,
    0x75, 0x08,
    0x81, 0x01,
    0x05, 0x08,
    0x95, 0x08,
    0x75, 0x01,
    0x19, 0x01,
    0x29, 0x08,
    0x91, 0x02,
    0x95, BLE_HID_KEYBOARD_KEYS_MAX,
    0x75, 0x08,
    0x15, 0x00,
    0x25, 0x65,
    0x05, 0x07,
    0x19, 0x00,
    0x29, 0x65,
    0x81, 0x00,
    0xC0,
    0x05, 0x01,
    0x09, 0x02,
    0xA1, 0x01,
    0x09, 0x01,
    0xA1, 0x00,
    0x85, BLE_HID_REPORT_ID_MOUSE,
    0x05, 0x09,
    0x19, 0x01,
    0x29, 0x03,
    0x15, 0x00,
    0x25, 0x01,
    0x95, 0x03,
    0x75, 0x01,
    0x81, 0x02,
    0x95, 0x01,
    0x75, 0x05,
    0x81, 0x03,
    0x05, 0x01,
    0x09, 0x30,
    0x09, 0x31,
    0x09, 0x38,
    0x15, 0x81,
    0x25, 0x7F,
    0x75, 0x08,
    0x95, 0x03,
    0x81, 0x06,
    0xC0,
    0xC0,
    0x05, 0x0C,
    0x09, 0x01,
    0xA1, 0x01,
    0x85, BLE_HID_REPORT_ID_CONSUMER,
    0x15, 0x00,
    0x26, 0xFF, 0x03,
    0x19, 0x00,
    0x2A, 0xFF, 0x03,
    0x95, 0x01,
    0x75, 0x10,
    0x81, 0x00,
    0xC0,
};

/* HID Information: bcdHID = 0x0111, country = 0x00, flags = RemoteWake|NormallyConnectable */
static const uint8_t ble_hid_info[] = {0x11, 0x01, 0x00, 0x03};

/* Report Reference descriptors: {report id, report type} */
static const uint8_t rr_kb_input[] = {BLE_HID_REPORT_ID_KEYBOARD, BLE_HID_REPORT_TYPE_INPUT};
static const uint8_t rr_kb_output[] = {BLE_HID_REPORT_ID_KEYBOARD, BLE_HID_REPORT_TYPE_OUTPUT};
static const uint8_t rr_mouse_input[] = {BLE_HID_REPORT_ID_MOUSE, BLE_HID_REPORT_TYPE_INPUT};
static const uint8_t rr_consumer_input[] = {BLE_HID_REPORT_ID_CONSUMER, BLE_HID_REPORT_TYPE_INPUT};

/* Device Information */
static const char ble_hid_manufacturer[] = "Flipper Devices";
/* PnP ID: vendor id source (0x02 = USB), vendor id, product id, product version (all LE) */
static const uint8_t ble_hid_pnp_id[] = {0x02, 0xC0, 0x16, 0xDF, 0x05, 0x00, 0x01};

/* ---- Locking helpers ---- */

static void ble_hid_lock_global(void) {
    if(ble_hid_state.mutex) {
        xSemaphoreTake(ble_hid_state.mutex, portMAX_DELAY);
    }
}

static void ble_hid_unlock_global(void) {
    if(ble_hid_state.mutex) {
        xSemaphoreGive(ble_hid_state.mutex);
    }
}

static void ble_hid_lock(BleHid* ble_hid) {
    if(ble_hid && ble_hid->mutex) {
        xSemaphoreTake(ble_hid->mutex, portMAX_DELAY);
    }
}

static void ble_hid_unlock(BleHid* ble_hid) {
    if(ble_hid && ble_hid->mutex) {
        xSemaphoreGive(ble_hid->mutex);
    }
}

/*
 * Host-task callbacks capture ble_hid_state.active, release the global lock, then
 * dereference/lock the instance. ble_hid_free() could delete that instance in the
 * gap -> use-after-free. These pin the active instance with a refcount taken under
 * the global lock (where `active` is valid), which ble_hid_free() drains to 0
 * before freeing. Taking the per-instance mutex here instead would invert the
 * documented instance->global lock order (ble_hid_send_report) and deadlock, so a
 * refcount is used. Every acquire that returns non-NULL MUST be released.
 */
static BleHid* ble_hid_acquire_active(void) {
    ble_hid_lock_global();
    BleHid* a = ble_hid_state.active;
    if(a) a->refs++;
    ble_hid_unlock_global();
    return a;
}

static void ble_hid_release_active(BleHid* a) {
    if(!a) return;
    ble_hid_lock_global();
    if(a->refs > 0) a->refs--;
    ble_hid_unlock_global();
}

/*
 * Lock ordering rule (to avoid deadlock):
 *   - The input path (app thread) may hold the per-instance mutex and then take
 *     the global mutex (ble_hid_send_report).
 *   - The host-callback path (NimBLE task) NEVER holds the global mutex while
 *     acquiring the per-instance mutex: it copies out the active pointer under
 *     the global lock, releases it, and only then calls ble_hid_update_connection.
 */

/* ---- Security mapping ---- */

static uint8_t ble_hid_io_cap(BleHidPairingMode pairing) {
    switch(pairing) {
    case BleHidPairingModeDisplayOnly:
        return BLE_HS_IO_DISPLAY_ONLY;
    case BleHidPairingModeInputOnly:
        return BLE_HS_IO_KEYBOARD_ONLY;
    case BleHidPairingModeVerifyYesNo:
    default:
        return BLE_HS_IO_DISPLAY_YESNO;
    }
}

static void ble_hid_make_random_static_addr(uint8_t mac[6]) {
    mac[0] &= 0xFE;
    mac[0] |= 0xC0;
}

/* Public API: unchanged behavior. Returns a derived static-random-flavored MAC. */
void ble_hid_get_default_mac(uint8_t mac[6]) {
    if(esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) {
        memset(mac, 0, 6);
        return;
    }

    esp_derive_local_mac(mac, mac);
    ble_hid_make_random_static_addr(mac);
}

/* ---- Connection state bridge (preserved public callback semantics) ---- */

static void ble_hid_update_connection(BleHid* ble_hid, bool connected) {
    if(!ble_hid) {
        return;
    }

    ble_hid_lock(ble_hid);
    ble_hid->connected = connected;
    BleHidStateCallback callback = ble_hid->state_callback;
    void* context = ble_hid->state_context;
    ble_hid_unlock(ble_hid);

    if(callback) {
        callback(connected, context);
    }
}

/* ---- GATT access callbacks ---- */

static int ble_hid_chr_hid_info(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt* ctxt,
    void* arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if(ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    int rc = os_mbuf_append(ctxt->om, ble_hid_info, sizeof(ble_hid_info));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int ble_hid_chr_report_map(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt* ctxt,
    void* arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if(ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    int rc = os_mbuf_append(ctxt->om, ble_hid_report_map, sizeof(ble_hid_report_map));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int ble_hid_chr_control_point(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt* ctxt,
    void* arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    /* HID Control Point is write-without-response (suspend/exit-suspend). Accept and ignore. */
    if(ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int ble_hid_chr_protocol_mode(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt* ctxt,
    void* arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if(ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t mode;
        ble_hid_lock_global();
        mode = ble_hid_state.protocol_mode;
        ble_hid_unlock_global();
        int rc = os_mbuf_append(ctxt->om, &mode, 1);
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if(ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t mode = 1;
        uint16_t out_len = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, &mode, sizeof(mode), &out_len);
        if(rc != 0 || out_len != 1) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        ble_hid_lock_global();
        ble_hid_state.protocol_mode = mode;
        ble_hid_unlock_global();
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/* Read handler for the three input report characteristics. arg encodes the report id. */
static int ble_hid_chr_input_report(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt* ctxt,
    void* arg) {
    (void)conn_handle;
    (void)attr_handle;
    if(ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t report_id = (uint8_t)(uintptr_t)arg;

    BleHid* ble_hid = ble_hid_acquire_active();

    uint8_t buf[8] = {0};
    size_t len = 0;

    if(ble_hid) {
        ble_hid_lock(ble_hid);
        switch(report_id) {
        case BLE_HID_REPORT_ID_KEYBOARD:
            len = sizeof(ble_hid->keyboard_report);
            memcpy(buf, &ble_hid->keyboard_report, len);
            break;
        case BLE_HID_REPORT_ID_MOUSE:
            len = sizeof(ble_hid->mouse_report);
            memcpy(buf, &ble_hid->mouse_report, len);
            break;
        case BLE_HID_REPORT_ID_CONSUMER:
            len = sizeof(ble_hid->consumer_report);
            memcpy(buf, &ble_hid->consumer_report, len);
            break;
        default:
            break;
        }
        ble_hid_unlock(ble_hid);
        ble_hid_release_active(ble_hid);
    }

    int rc = os_mbuf_append(ctxt->om, buf, len);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* Keyboard LED output report (read + write-without-response). */
static int ble_hid_chr_kb_output_report(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt* ctxt,
    void* arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    BleHid* ble_hid = ble_hid_acquire_active();
    int ret = BLE_ATT_ERR_UNLIKELY; /* single exit so the ref is always released */

    if(ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t led = 0;
        if(ble_hid) {
            ble_hid_lock(ble_hid);
            led = ble_hid->led_state;
            ble_hid_unlock(ble_hid);
        }
        int rc = os_mbuf_append(ctxt->om, &led, 1);
        ret = rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    } else if(ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t led = 0;
        uint16_t out_len = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, &led, sizeof(led), &out_len);
        if(rc != 0 || out_len < 1) {
            ret = BLE_ATT_ERR_UNLIKELY;
        } else {
            if(ble_hid) {
                ble_hid_lock(ble_hid);
                ble_hid->led_state = led;
                ble_hid_unlock(ble_hid);
            }
            ret = 0;
        }
    }

    ble_hid_release_active(ble_hid);
    return ret;
}

/* Report Reference descriptor read (arg -> 2-byte {report id, type}). */
static int ble_hid_dsc_report_ref(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt* ctxt,
    void* arg) {
    (void)conn_handle;
    (void)attr_handle;
    if(ctxt->op != BLE_GATT_ACCESS_OP_READ_DSC) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    int rc = os_mbuf_append(ctxt->om, arg, 2);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int ble_hid_chr_battery_level(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt* ctxt,
    void* arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if(ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint8_t level = 100;
    int rc = os_mbuf_append(ctxt->om, &level, 1);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int ble_hid_chr_manufacturer(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt* ctxt,
    void* arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if(ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    int rc = os_mbuf_append(ctxt->om, ble_hid_manufacturer, strlen(ble_hid_manufacturer));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int ble_hid_chr_pnp_id(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt* ctxt,
    void* arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if(ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    int rc = os_mbuf_append(ctxt->om, ble_hid_pnp_id, sizeof(ble_hid_pnp_id));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* ---- GATT service definition table ---- */

static const struct ble_gatt_svc_def ble_hid_gatt_svcs[] = {
    /* HID Service */
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(BLE_HID_SERVICE_UUID),
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = BLE_UUID16_DECLARE(UUID16_CHR_HID_INFORMATION),
                    .access_cb = ble_hid_chr_hid_info,
                    .flags = BLE_GATT_CHR_F_READ,
                },
                {
                    .uuid = BLE_UUID16_DECLARE(UUID16_CHR_REPORT_MAP),
                    .access_cb = ble_hid_chr_report_map,
                    .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
                },
                {
                    /* Keyboard input report */
                    .uuid = BLE_UUID16_DECLARE(UUID16_CHR_REPORT),
                    .access_cb = ble_hid_chr_input_report,
                    .arg = (void*)(uintptr_t)BLE_HID_REPORT_ID_KEYBOARD,
                    .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                             BLE_GATT_CHR_F_NOTIFY,
                    .val_handle = &ble_hid_state.hid_kb_input_handle,
                    .descriptors =
                        (struct ble_gatt_dsc_def[]){
                            {
                                .uuid = BLE_UUID16_DECLARE(UUID16_DSC_REPORT_REF),
                                .att_flags = BLE_ATT_F_READ,
                                .access_cb = ble_hid_dsc_report_ref,
                                .arg = (void*)rr_kb_input,
                            },
                            {0},
                        },
                },
                {
                    /* Keyboard LED output report */
                    .uuid = BLE_UUID16_DECLARE(UUID16_CHR_REPORT),
                    .access_cb = ble_hid_chr_kb_output_report,
                    .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                             BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_READ_ENC |
                             BLE_GATT_CHR_F_WRITE_ENC,
                    .val_handle = &ble_hid_state.hid_kb_output_handle,
                    .descriptors =
                        (struct ble_gatt_dsc_def[]){
                            {
                                .uuid = BLE_UUID16_DECLARE(UUID16_DSC_REPORT_REF),
                                .att_flags = BLE_ATT_F_READ,
                                .access_cb = ble_hid_dsc_report_ref,
                                .arg = (void*)rr_kb_output,
                            },
                            {0},
                        },
                },
                {
                    /* Mouse input report */
                    .uuid = BLE_UUID16_DECLARE(UUID16_CHR_REPORT),
                    .access_cb = ble_hid_chr_input_report,
                    .arg = (void*)(uintptr_t)BLE_HID_REPORT_ID_MOUSE,
                    .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                             BLE_GATT_CHR_F_NOTIFY,
                    .val_handle = &ble_hid_state.hid_mouse_input_handle,
                    .descriptors =
                        (struct ble_gatt_dsc_def[]){
                            {
                                .uuid = BLE_UUID16_DECLARE(UUID16_DSC_REPORT_REF),
                                .att_flags = BLE_ATT_F_READ,
                                .access_cb = ble_hid_dsc_report_ref,
                                .arg = (void*)rr_mouse_input,
                            },
                            {0},
                        },
                },
                {
                    /* Consumer control input report */
                    .uuid = BLE_UUID16_DECLARE(UUID16_CHR_REPORT),
                    .access_cb = ble_hid_chr_input_report,
                    .arg = (void*)(uintptr_t)BLE_HID_REPORT_ID_CONSUMER,
                    .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                             BLE_GATT_CHR_F_NOTIFY,
                    .val_handle = &ble_hid_state.hid_consumer_input_handle,
                    .descriptors =
                        (struct ble_gatt_dsc_def[]){
                            {
                                .uuid = BLE_UUID16_DECLARE(UUID16_DSC_REPORT_REF),
                                .att_flags = BLE_ATT_F_READ,
                                .access_cb = ble_hid_dsc_report_ref,
                                .arg = (void*)rr_consumer_input,
                            },
                            {0},
                        },
                },
                {
                    .uuid = BLE_UUID16_DECLARE(UUID16_CHR_HID_CONTROL_PT),
                    .access_cb = ble_hid_chr_control_point,
                    .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
                },
                {
                    .uuid = BLE_UUID16_DECLARE(UUID16_CHR_PROTOCOL_MODE),
                    .access_cb = ble_hid_chr_protocol_mode,
                    .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP,
                },
                {0},
            },
    },
    /* Battery Service */
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(UUID16_SVC_BATTERY),
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = BLE_UUID16_DECLARE(UUID16_CHR_BATTERY_LEVEL),
                    .access_cb = ble_hid_chr_battery_level,
                    .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                    .val_handle = &ble_hid_state.battery_level_handle,
                },
                {0},
            },
    },
    /* Device Information Service */
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(UUID16_SVC_DEVICE_INFO),
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = BLE_UUID16_DECLARE(UUID16_CHR_MANUFACTURER),
                    .access_cb = ble_hid_chr_manufacturer,
                    .flags = BLE_GATT_CHR_F_READ,
                },
                {
                    .uuid = BLE_UUID16_DECLARE(UUID16_CHR_PNP_ID),
                    .access_cb = ble_hid_chr_pnp_id,
                    .flags = BLE_GATT_CHR_F_READ,
                },
                {0},
            },
    },
    {0},
};

/* ---- Advertising ---- */

static int ble_hid_set_adv_fields(const BleHidConfig* config) {
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.appearance = BLE_HID_APPEARANCE_KEYBOARD;
    fields.appearance_is_present = 1;

    static ble_uuid16_t hid_uuid;
    hid_uuid = (ble_uuid16_t)BLE_UUID16_INIT(BLE_HID_SERVICE_UUID);
    fields.uuids16 = &hid_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if(rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return rc;
    }

    /* Name goes into scan response to avoid the 31-byte adv payload limit. */
    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    size_t name_len = strlen(config->device_name);
    rsp.name = (uint8_t*)config->device_name;
    rsp.name_len = name_len;
    rsp.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if(rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_rsp_set_fields failed: %d", rc);
    }
    return rc;
}

static int ble_gap_event(struct ble_gap_event* event, void* arg);

static bool ble_hid_try_start_advertising_locked(void) {
    if(!ble_hid_state.active) {
        ESP_LOGI(TAG, "adv_try skipped: no active profile");
        return false;
    }

    if(!ble_hid_state.synced || !ble_hid_state.advertising_requested ||
       ble_hid_state.advertising || ble_hid_state.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGI(
            TAG,
            "adv_try blocked: synced=%d requested=%d advertising=%d connected=%d",
            ble_hid_state.synced,
            ble_hid_state.advertising_requested,
            ble_hid_state.advertising,
            ble_hid_state.conn_handle != BLE_HS_CONN_HANDLE_NONE);
        return false;
    }

    int rc = ble_hid_set_adv_fields(&ble_hid_state.active->config);
    if(rc != 0) {
        return false;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = 0x20;
    adv_params.itvl_max = 0x30;

    rc = ble_gap_adv_start(
        ble_hid_state.own_addr_type,
        NULL,
        BLE_HS_FOREVER,
        &adv_params,
        ble_gap_event,
        NULL);
    if(rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
        return false;
    }

    ESP_LOGI(
        TAG,
        "adv_start own_addr_type=%d name=%s",
        ble_hid_state.own_addr_type,
        ble_hid_state.active->config.device_name);
    ble_hid_state.advertising = true;
    return true;
}

/* ---- GAP event handler (single callback for all connection events) ---- */

static int ble_gap_event(struct ble_gap_event* event, void* arg) {
    (void)arg;

    switch(event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        ESP_LOGI(TAG, "gap event: CONNECT status=%d", event->connect.status);
        BleHid* active = NULL;
        if(event->connect.status == 0) {
            ble_hid_lock_global();
            ble_hid_state.conn_handle = event->connect.conn_handle;
            ble_hid_state.advertising = false;
            active = ble_hid_state.active;
            if(active) active->refs++; /* pin across the release below (see acquire_active) */
            ble_hid_unlock_global();

            /* HOGP reports require an encrypted link; nudge the peer to pair/encrypt. */
            int rc = ble_gap_security_initiate(event->connect.conn_handle);
            if(rc != 0 && rc != BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "ble_gap_security_initiate: %d", rc);
            }

            ble_hid_update_connection(active, true);
            ble_hid_release_active(active);
        } else {
            /* Connection failed; resume advertising. */
            ble_hid_lock_global();
            ble_hid_try_start_advertising_locked();
            ble_hid_unlock_global();
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        ESP_LOGI(TAG, "gap event: DISCONNECT reason=%d", event->disconnect.reason);
        ble_hid_lock_global();
        ble_hid_state.conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ble_hid_state.kb_notify = false;
        ble_hid_state.mouse_notify = false;
        ble_hid_state.consumer_notify = false;
        BleHid* active = ble_hid_state.active;
        if(active) active->refs++; /* pin across the release below (see acquire_active) */
        ble_hid_unlock_global();

        ble_hid_update_connection(active, false);
        ble_hid_release_active(active);

        ble_hid_lock_global();
        ble_hid_try_start_advertising_locked();
        ble_hid_unlock_global();
        return 0;
    }

    case BLE_GAP_EVENT_SUBSCRIBE: {
        ESP_LOGI(
            TAG,
            "gap event: SUBSCRIBE attr=%d notify=%d",
            event->subscribe.attr_handle,
            event->subscribe.cur_notify);
        ble_hid_lock_global();
        uint16_t h = event->subscribe.attr_handle;
        bool notify = event->subscribe.cur_notify;
        if(h == ble_hid_state.hid_kb_input_handle) {
            ble_hid_state.kb_notify = notify;
        } else if(h == ble_hid_state.hid_mouse_input_handle) {
            ble_hid_state.mouse_notify = notify;
        } else if(h == ble_hid_state.hid_consumer_input_handle) {
            ble_hid_state.consumer_notify = notify;
        }
        ble_hid_unlock_global();
        return 0;
    }

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "gap event: MTU=%d", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "gap event: ENC_CHANGE status=%d", event->enc_change.status);
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io pkey;
        memset(&pkey, 0, sizeof(pkey));
        if(event->passkey.params.action == BLE_SM_IOACT_DISP) {
            pkey.action = BLE_SM_IOACT_DISP;
            pkey.passkey = BLE_HID_PASSKEY_DEFAULT;
            ESP_LOGI(TAG, "BLE passkey (display): %06" PRIu32, (uint32_t)pkey.passkey);
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        } else if(event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            pkey.action = BLE_SM_IOACT_NUMCMP;
            pkey.numcmp_accept = 1;
            ESP_LOGI(TAG, "BLE numeric comparison: %06" PRIu32, event->passkey.params.numcmp);
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        } else if(event->passkey.params.action == BLE_SM_IOACT_INPUT) {
            pkey.action = BLE_SM_IOACT_INPUT;
            pkey.passkey = BLE_HID_PASSKEY_DEFAULT;
            ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        }
        return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* Peer re-pairing while a bond already exists: delete the old bond and retry. */
        struct ble_gap_conn_desc desc;
        if(ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "gap event: ADV_COMPLETE reason=%d", event->adv_complete.reason);
        ble_hid_lock_global();
        ble_hid_state.advertising = false;
        ble_hid_try_start_advertising_locked();
        ble_hid_unlock_global();
        return 0;

    default:
        return 0;
    }
}

/* ---- NimBLE host lifecycle ---- */

static void ble_hid_on_reset(int reason) {
    ESP_LOGE(TAG, "NimBLE host reset; reason=%d", reason);
    ble_hid_lock_global();
    ble_hid_state.synced = false;
    ble_hid_state.advertising = false;
    ble_hid_state.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    ble_hid_unlock_global();
}

static void ble_hid_on_sync(void) {
    /* Ensure we have an identity address. */
    int rc = ble_hs_util_ensure_addr(0);
    if(rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed: %d", rc);
    }

    ble_hid_lock_global();
    if(ble_hid_state.rand_addr_enabled) {
        rc = ble_hs_id_set_rnd(ble_hid_state.rand_addr);
        if(rc != 0) {
            ESP_LOGE(TAG, "ble_hs_id_set_rnd failed: %d", rc);
            ble_hid_state.rand_addr_enabled = false;
        } else {
            ble_hid_state.own_addr_type = BLE_OWN_ADDR_RANDOM;
        }
    }

    if(!ble_hid_state.rand_addr_enabled) {
        uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;
        rc = ble_hs_id_infer_auto(0, &own_addr_type);
        if(rc != 0) {
            ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        }
        ble_hid_state.own_addr_type = own_addr_type;
    }

    ble_hid_state.synced = true;
    ble_hid_try_start_advertising_locked();
    ble_hid_unlock_global();

    ESP_LOGI(TAG, "NimBLE host synced; own_addr_type=%d", ble_hid_state.own_addr_type);
}

static void ble_hid_gatts_register_cb(struct ble_gatt_register_ctxt* ctxt, void* arg) {
    (void)arg;
    if(ctxt->op == BLE_GATT_REGISTER_OP_CHR) {
        ESP_LOGD(
            TAG,
            "registered chr val_handle=%d def_handle=%d",
            ctxt->chr.val_handle,
            ctxt->chr.def_handle);
    }
}

static void ble_hid_host_task(void* param) {
    (void)param;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run(); /* returns only when nimble_port_stop() is called */
    nimble_port_freertos_deinit();
}

static void ble_hid_apply_security_config(const BleHidConfig* config) {
    ble_hs_cfg.sm_io_cap = ble_hid_io_cap(config->pairing);
    ble_hs_cfg.sm_bonding = config->bonding ? 1 : 0;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
}

static esp_err_t ble_hid_stack_init_once(void) {
    esp_err_t err = ESP_OK;

    if(!ble_hid_state.mutex) {
        ble_hid_state.mutex = xSemaphoreCreateMutex();
        if(!ble_hid_state.mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    ble_hid_lock_global();
    if(ble_hid_state.initialized) {
        ble_hid_unlock_global();
        return ESP_OK;
    }
    ble_hid_unlock_global();

    err = nvs_flash_init();
    if((err == ESP_ERR_NVS_NO_FREE_PAGES) || (err == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if(err != ESP_OK) {
        return err;
    }

    /* Controller bring-up is UNCHANGED from the Bluedroid version. */
    err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    /* NimBLE host init (replaces esp_bluedroid_init/enable). */
    err = nimble_port_init();
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Host configuration callbacks. */
    ble_hs_cfg.reset_cb = ble_hid_on_reset;
    ble_hs_cfg.sync_cb = ble_hid_on_sync;
    ble_hs_cfg.gatts_register_cb = ble_hid_gatts_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Standard GAP/GATT services. */
    ble_svc_gap_init();
    ble_svc_gatt_init();

    /* Register our HID / Battery / Device-Info services. */
    int rc = ble_gatts_count_cfg(ble_hid_gatt_svcs);
    if(rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(ble_hid_gatt_svcs);
    if(rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return ESP_FAIL;
    }

    /* Persistent bond storage. */
    ble_store_config_init();

    ble_hid_lock_global();
    ble_hid_state.initialized = true;
    ble_hid_unlock_global();

    /* Start the NimBLE host task (drives nimble_port_run). */
    nimble_port_freertos_init(ble_hid_host_task);

    return ESP_OK;
}

/* ---- Random static address ---- */

static bool ble_hid_has_custom_mac(const BleHidConfig* config) {
    static const uint8_t zero_mac[6] = {0};
    return memcmp(config->mac, zero_mac, sizeof(zero_mac)) != 0;
}

static void ble_hid_prepare_random_address(const BleHidConfig* config) {
    /*
     * NimBLE addresses are little-endian (index 5 is the MSB), whereas the
     * Bluedroid API used index 0 as the MSB. Byte-reverse the configured MAC so
     * the over-the-air address matches the original firmware, then force the two
     * most-significant bits to 0b11 to make it a valid static random address.
     * NOTE (integrator): verify the observed advertised address matches the
     * previous Bluedroid build for a given config->mac.
     */
    uint8_t addr[6];
    for(int i = 0; i < 6; i++) {
        addr[i] = config->mac[5 - i];
    }
    addr[5] &= 0xFE;
    addr[5] |= 0xC0;

    ble_hid_lock_global();
    memcpy(ble_hid_state.rand_addr, addr, sizeof(addr));
    ble_hid_state.rand_addr_enabled = true;
    ble_hid_unlock_global();
}

/* ---- Report sending (NimBLE notify) ---- */

static bool ble_hid_send_report(BleHid* ble_hid, uint8_t report_id, void* data, size_t length) {
    if(!ble_hid) {
        return false;
    }

    ble_hid_lock_global();
    uint16_t conn_handle = ble_hid_state.conn_handle;
    uint16_t val_handle = 0;
    bool notify = false;
    switch(report_id) {
    case BLE_HID_REPORT_ID_KEYBOARD:
        val_handle = ble_hid_state.hid_kb_input_handle;
        notify = ble_hid_state.kb_notify;
        break;
    case BLE_HID_REPORT_ID_MOUSE:
        val_handle = ble_hid_state.hid_mouse_input_handle;
        notify = ble_hid_state.mouse_notify;
        break;
    case BLE_HID_REPORT_ID_CONSUMER:
        val_handle = ble_hid_state.hid_consumer_input_handle;
        notify = ble_hid_state.consumer_notify;
        break;
    default:
        break;
    }
    ble_hid_unlock_global();

    if(conn_handle == BLE_HS_CONN_HANDLE_NONE || !notify || val_handle == 0) {
        return false;
    }

    struct os_mbuf* om = ble_hs_mbuf_from_flat(data, length);
    if(!om) {
        return false;
    }

    int rc = ble_gatts_notify_custom(conn_handle, val_handle, om);
    return rc == 0;
}

/* ---- Public lifecycle ---- */

BleHid* ble_hid_alloc(const BleHidConfig* config) {
    if(!config) {
        return NULL;
    }

    esp_err_t err = ble_hid_stack_init_once();
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "BLE stack init failed: %s", esp_err_to_name(err));
        return NULL;
    }

    BleHid* ble_hid = calloc(1, sizeof(BleHid));
    if(!ble_hid) {
        return NULL;
    }

    ble_hid->mutex = xSemaphoreCreateMutex();
    if(!ble_hid->mutex) {
        free(ble_hid);
        return NULL;
    }

    memcpy(&ble_hid->config, config, sizeof(ble_hid->config));
    ESP_LOGI(
        TAG,
        "alloc name=%s mac=%02x:%02x:%02x:%02x:%02x:%02x bonding=%d pairing=%d",
        ble_hid->config.device_name,
        ble_hid->config.mac[0],
        ble_hid->config.mac[1],
        ble_hid->config.mac[2],
        ble_hid->config.mac[3],
        ble_hid->config.mac[4],
        ble_hid->config.mac[5],
        ble_hid->config.bonding,
        ble_hid->config.pairing);

    ble_hid_lock_global();
    if(ble_hid_state.active) {
        ble_hid_unlock_global();
        vSemaphoreDelete(ble_hid->mutex);
        free(ble_hid);
        return NULL;
    }
    ble_hid_state.active = ble_hid;
    ble_hid_state.advertising = false;
    ble_hid_state.advertising_requested = false;
    ble_hid_state.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    ble_hid_state.kb_notify = false;
    ble_hid_state.mouse_notify = false;
    ble_hid_state.consumer_notify = false;
    ble_hid_state.rand_addr_enabled = false;
    ble_hid_state.protocol_mode = 1;
    ble_hid_unlock_global();

    /* Security parameters live in ble_hs_cfg (global). */
    ble_hid_apply_security_config(&ble_hid->config);

    /* Device name for GAP + advertising. */
    int rc = ble_svc_gap_device_name_set(ble_hid->config.device_name);
    if(rc != 0) {
        ESP_LOGE(TAG, "ble_svc_gap_device_name_set failed: %d", rc);
        goto error;
    }

    if(ble_hid_has_custom_mac(&ble_hid->config)) {
        ble_hid_prepare_random_address(&ble_hid->config);
        /* If the host is already synced, apply the address now. */
        ble_hid_lock_global();
        if(ble_hid_state.synced) {
            if(ble_hs_id_set_rnd(ble_hid_state.rand_addr) == 0) {
                ble_hid_state.own_addr_type = BLE_OWN_ADDR_RANDOM;
            } else {
                ble_hid_state.rand_addr_enabled = false;
            }
        }
        ble_hid_unlock_global();
    }

    return ble_hid;

error:
    ble_hid_lock_global();
    if(ble_hid_state.active == ble_hid) {
        ble_hid_state.active = NULL;
    }
    ble_hid_unlock_global();

    if(ble_hid->mutex) {
        vSemaphoreDelete(ble_hid->mutex);
    }
    free(ble_hid);
    return NULL;
}

void ble_hid_free(BleHid* ble_hid) {
    if(!ble_hid) {
        return;
    }

    /*
     * As in the Bluedroid version, we keep the NimBLE host running across
     * alloc/free cycles (the GATT services are registered once). We only stop
     * advertising, drop any active connection, and clear the active profile.
     * Full host teardown (nimble_port_stop + nimble_port_deinit) is intentionally
     * NOT done here to preserve the original module behavior.
     */
    ble_hid_lock_global();
    bool was_advertising = ble_hid_state.advertising;
    uint16_t conn_handle = ble_hid_state.conn_handle;
    ble_hid_state.advertising_requested = false;
    ble_hid_state.advertising = false;
    if(ble_hid_state.active == ble_hid) {
        ble_hid_state.active = NULL;
    }
    ble_hid_unlock_global();

    if(was_advertising) {
        ble_gap_adv_stop();
    }
    if(conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }

    /* Drain any in-flight host-task callback that pinned this instance before we
     * delete its mutex + free it. `active` is already NULL above, so no new ref
     * can be taken and this count only falls to 0. Closes the capture-then-use UAF. */
    for(;;) {
        ble_hid_lock_global();
        uint32_t refs = ble_hid->refs;
        ble_hid_unlock_global();
        if(refs == 0) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if(ble_hid->mutex) {
        vSemaphoreDelete(ble_hid->mutex);
    }

    free(ble_hid);
}

void ble_hid_set_state_callback(BleHid* ble_hid, BleHidStateCallback callback, void* context) {
    if(!ble_hid) {
        return;
    }

    ble_hid_lock(ble_hid);
    ble_hid->state_callback = callback;
    ble_hid->state_context = context;
    bool connected = ble_hid->connected;
    ble_hid_unlock(ble_hid);

    if(callback) {
        callback(connected, context);
    }
}

bool ble_hid_is_connected(BleHid* ble_hid) {
    if(!ble_hid) {
        return false;
    }

    ble_hid_lock(ble_hid);
    bool connected = ble_hid->connected;
    ble_hid_unlock(ble_hid);
    return connected;
}

bool ble_hid_kb_press(BleHid* ble_hid, uint16_t button) {
    if(!ble_hid) {
        return false;
    }

    ble_hid_lock(ble_hid);
    for(size_t i = 0; i < BLE_HID_KEYBOARD_KEYS_MAX; i++) {
        if(ble_hid->keyboard_report.keys[i] == (button & 0xFF)) {
            break;
        }
        if(ble_hid->keyboard_report.keys[i] == 0) {
            ble_hid->keyboard_report.keys[i] = button & 0xFF;
            break;
        }
    }
    ble_hid->keyboard_report.modifiers |= (button >> 8);
    bool result = ble_hid_send_report(
        ble_hid,
        BLE_HID_REPORT_ID_KEYBOARD,
        &ble_hid->keyboard_report,
        sizeof(ble_hid->keyboard_report));
    ble_hid_unlock(ble_hid);
    return result;
}

bool ble_hid_kb_release(BleHid* ble_hid, uint16_t button) {
    if(!ble_hid) {
        return false;
    }

    ble_hid_lock(ble_hid);
    for(size_t i = 0; i < BLE_HID_KEYBOARD_KEYS_MAX; i++) {
        if(ble_hid->keyboard_report.keys[i] == (button & 0xFF)) {
            ble_hid->keyboard_report.keys[i] = 0;
            break;
        }
    }
    ble_hid->keyboard_report.modifiers &= ~(button >> 8);
    bool result = ble_hid_send_report(
        ble_hid,
        BLE_HID_REPORT_ID_KEYBOARD,
        &ble_hid->keyboard_report,
        sizeof(ble_hid->keyboard_report));
    ble_hid_unlock(ble_hid);
    return result;
}

bool ble_hid_kb_release_all(BleHid* ble_hid) {
    if(!ble_hid) {
        return false;
    }

    ble_hid_lock(ble_hid);
    memset(&ble_hid->keyboard_report, 0, sizeof(ble_hid->keyboard_report));
    bool result = ble_hid_send_report(
        ble_hid,
        BLE_HID_REPORT_ID_KEYBOARD,
        &ble_hid->keyboard_report,
        sizeof(ble_hid->keyboard_report));
    ble_hid_unlock(ble_hid);
    return result;
}

bool ble_hid_mouse_move(BleHid* ble_hid, int8_t dx, int8_t dy) {
    if(!ble_hid) {
        return false;
    }

    ble_hid_lock(ble_hid);
    ble_hid->mouse_report.x = dx;
    ble_hid->mouse_report.y = dy;
    bool result = ble_hid_send_report(
        ble_hid, BLE_HID_REPORT_ID_MOUSE, &ble_hid->mouse_report, sizeof(ble_hid->mouse_report));
    ble_hid->mouse_report.x = 0;
    ble_hid->mouse_report.y = 0;
    ble_hid_unlock(ble_hid);
    return result;
}

bool ble_hid_mouse_press(BleHid* ble_hid, uint8_t button) {
    if(!ble_hid) {
        return false;
    }

    ble_hid_lock(ble_hid);
    ble_hid->mouse_report.buttons |= button;
    bool result = ble_hid_send_report(
        ble_hid, BLE_HID_REPORT_ID_MOUSE, &ble_hid->mouse_report, sizeof(ble_hid->mouse_report));
    ble_hid_unlock(ble_hid);
    return result;
}

bool ble_hid_mouse_release(BleHid* ble_hid, uint8_t button) {
    if(!ble_hid) {
        return false;
    }

    ble_hid_lock(ble_hid);
    ble_hid->mouse_report.buttons &= ~button;
    bool result = ble_hid_send_report(
        ble_hid, BLE_HID_REPORT_ID_MOUSE, &ble_hid->mouse_report, sizeof(ble_hid->mouse_report));
    ble_hid_unlock(ble_hid);
    return result;
}

bool ble_hid_mouse_release_all(BleHid* ble_hid) {
    if(!ble_hid) {
        return false;
    }

    ble_hid_lock(ble_hid);
    ble_hid->mouse_report.buttons = 0;
    ble_hid->mouse_report.x = 0;
    ble_hid->mouse_report.y = 0;
    ble_hid->mouse_report.wheel = 0;
    bool result = ble_hid_send_report(
        ble_hid, BLE_HID_REPORT_ID_MOUSE, &ble_hid->mouse_report, sizeof(ble_hid->mouse_report));
    ble_hid_unlock(ble_hid);
    return result;
}

bool ble_hid_mouse_scroll(BleHid* ble_hid, int8_t delta) {
    if(!ble_hid) {
        return false;
    }

    ble_hid_lock(ble_hid);
    ble_hid->mouse_report.wheel = delta;
    bool result = ble_hid_send_report(
        ble_hid, BLE_HID_REPORT_ID_MOUSE, &ble_hid->mouse_report, sizeof(ble_hid->mouse_report));
    ble_hid->mouse_report.wheel = 0;
    ble_hid_unlock(ble_hid);
    return result;
}

bool ble_hid_consumer_press(BleHid* ble_hid, uint16_t button) {
    if(!ble_hid) {
        return false;
    }

    ble_hid_lock(ble_hid);
    ble_hid->consumer_report.key = button;
    bool result = ble_hid_send_report(
        ble_hid,
        BLE_HID_REPORT_ID_CONSUMER,
        &ble_hid->consumer_report,
        sizeof(ble_hid->consumer_report));
    ble_hid_unlock(ble_hid);
    return result;
}

bool ble_hid_consumer_release(BleHid* ble_hid, uint16_t button) {
    if(!ble_hid) {
        return false;
    }

    ble_hid_lock(ble_hid);
    if(ble_hid->consumer_report.key == button) {
        ble_hid->consumer_report.key = 0;
    }
    bool result = ble_hid_send_report(
        ble_hid,
        BLE_HID_REPORT_ID_CONSUMER,
        &ble_hid->consumer_report,
        sizeof(ble_hid->consumer_report));
    ble_hid_unlock(ble_hid);
    return result;
}

bool ble_hid_consumer_release_all(BleHid* ble_hid) {
    if(!ble_hid) {
        return false;
    }

    ble_hid_lock(ble_hid);
    ble_hid->consumer_report.key = 0;
    bool result = ble_hid_send_report(
        ble_hid,
        BLE_HID_REPORT_ID_CONSUMER,
        &ble_hid->consumer_report,
        sizeof(ble_hid->consumer_report));
    ble_hid_unlock(ble_hid);
    return result;
}

uint8_t ble_hid_get_led_state(BleHid* ble_hid) {
    if(!ble_hid) {
        return 0;
    }

    ble_hid_lock(ble_hid);
    uint8_t led_state = ble_hid->led_state;
    ble_hid_unlock(ble_hid);
    return led_state;
}

bool ble_hid_start_advertising(void) {
    bool accepted = false;

    ble_hid_lock_global();
    if(ble_hid_state.active) {
        ESP_LOGI(TAG, "start_advertising requested");
        ble_hid_state.advertising_requested = true;
        ble_hid_try_start_advertising_locked();
        accepted = true;
    }
    ble_hid_unlock_global();

    return accepted;
}

void ble_hid_stop_advertising(void) {
    ble_hid_lock_global();
    ESP_LOGI(TAG, "stop_advertising requested");
    ble_hid_state.advertising_requested = false;
    bool was_advertising = ble_hid_state.advertising;
    ble_hid_state.advertising = false;
    ble_hid_unlock_global();

    if(was_advertising) {
        ble_gap_adv_stop();
    }
}

bool ble_hid_is_advertising(void) {
    bool advertising = false;

    ble_hid_lock_global();
    advertising = ble_hid_state.advertising_requested && ble_hid_state.active != NULL &&
                  !ble_hid_state.active->connected;
    ble_hid_unlock_global();

    return advertising;
}

bool ble_hid_is_active(void) {
    bool active = false;

    ble_hid_lock_global();
    active = ble_hid_state.active != NULL;
    ble_hid_unlock_global();

    return active;
}

bool ble_hid_remove_pairing(void) {
    if(!ble_hid_state.initialized) {
        return false;
    }

    ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
    int num_peers = 0;
    int rc = ble_store_util_bonded_peers(
        peers, &num_peers, sizeof(peers) / sizeof(peers[0]));
    if(rc != 0) {
        ESP_LOGE(TAG, "ble_store_util_bonded_peers failed: %d", rc);
        return false;
    }

    bool success = true;
    for(int i = 0; i < num_peers; i++) {
        rc = ble_gap_unpair(&peers[i]);
        if(rc != 0) {
            ESP_LOGW(TAG, "ble_gap_unpair failed: %d", rc);
            success = false;
        }
    }

    return success;
}
