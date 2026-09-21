#include "ble_tracker_hal.h"

#include <host/ble_hs.h>
#include <host/ble_gap.h>
#include <host/util/util.h>
#include <esp_log.h>
#include <furi.h>
#include <string.h>
#include <stdio.h>

#include "tracker_follow.h"   /* ble_adv_extract_local_name */

/* NOTE: this HAL does NOT own the controller/host lifecycle. The controller +
 * NimBLE host are brought up by ble_walk_hal_start() before the tracker scenes
 * call ble_tracker_hal_start_scan() (see scene_tracker_scan.c / _geiger.c).
 * Under Bluedroid the tracker "took over" the single global GAP callback via
 * esp_ble_gap_register_callback(); NimBLE has no global GAP callback — the
 * event handler is passed per-operation to ble_gap_disc(), so there is nothing
 * to register/take-over here. Once ble_walk_hal is also migrated to NimBLE this
 * file needs no further change. */

#define TAG "BleTracker"

// On-demand (was ~1.9 KB always-resident BSS). Allocated on first start_scan,
// freed by ble_tracker_hal_release() at scene exit (NOT stop_scan — scenes read
// the table after stopping). Written by ble_gap_event on the NimBLE host task.
static TrackerDevice* s_devices;
static uint16_t s_device_count = 0;
static volatile bool s_scanning = false;
static volatile bool s_sweep = false;

// ---------------------------------------------------------------------------
// Apple Proximity Pairing model table — ported from Poseidon ble_db.cpp:154
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t key;
    const char* name;
} apple_pp_model_t;

static const apple_pp_model_t APPLE_PP[] = {
    {0x01, "AirPods 1"},
    {0x02, "AirPods Pro"},
    {0x03, "AirPods Max"},
    {0x04, "AppleTV Setup"},
    {0x05, "Beats X"},
    {0x06, "Beats Solo 3"},
    {0x07, "Beats Studio 3"},
    {0x09, "Beats Studio Pro"},
    {0x0A, "Beats Fit Pro"},
    {0x0B, "Beats Flex"},
    {0x0C, "Beats Solo Pro"},
    {0x0D, "Beats Studio Buds"},
    {0x0E, "Beats Studio Buds+"},
    {0x0F, "AirPods 2"},
    {0x10, "AirPods 3"},
    {0x11, "AirPods 4"},
    {0x13, "AirPods Pro 2"},
    {0x14, "AirPods Pro 2 USB-C"},
    {0x19, "PowerBeats Pro"},
    {0x1A, "Beats Fit Pro 2"},
    {0x00, NULL},
};

static const char* apple_pp_lookup(uint8_t model) {
    for(const apple_pp_model_t* p = APPLE_PP; p->name; ++p) {
        if(p->key == model) return p->name;
    }
    return "Apple pairing";
}

// ---------------------------------------------------------------------------
// AD parsing helpers
// ---------------------------------------------------------------------------

// Walks one BLE advertisement TLV buffer and looks for any of:
//   - manufacturer data (type 0xFF) for tracker matchers
//   - 16-bit Service UUID lists (types 0x02/0x03) for Tile (0xFEED, 0xFD84)
// Outputs classification + optional model name into kind_out, name_out.
// Returns true if a tracker kind was identified.
static bool classify_buffer(
    const uint8_t* buf,
    uint8_t len,
    TrackerKind* kind_out,
    char* name_out,
    uint8_t name_out_sz) {
    uint8_t pos = 0;
    while(pos + 1 < len) {
        uint8_t fld_len = buf[pos];
        if(fld_len == 0) break;
        if(pos + fld_len >= len) break;
        uint8_t type = buf[pos + 1];
        const uint8_t* val = &buf[pos + 2];
        uint8_t val_len = fld_len - 1;

        if(type == 0xFF && val_len >= 2) {
            uint16_t cid = val[0] | (val[1] << 8);
            // Apple
            if(cid == 0x004C && val_len >= 3) {
                uint8_t sub = val[2];
                if(sub == 0x12) {
                    *kind_out = TrackerKindAirTag;
                    name_out[0] = '\0';
                    return true;
                }
                if(sub == 0x10) {
                    *kind_out = TrackerKindAppleNearby;
                    name_out[0] = '\0';
                    return true;
                }
                if(sub == 0x07 && val_len >= 4) {
                    *kind_out = TrackerKindApplePP;
                    strncpy(name_out, apple_pp_lookup(val[3]), name_out_sz - 1);
                    name_out[name_out_sz - 1] = '\0';
                    return true;
                }
            }
            // Samsung
            if(cid == 0x0075) {
                *kind_out = TrackerKindSmartTag;
                name_out[0] = '\0';
                return true;
            }
            // Meta Platforms (Ray-Ban Meta smart glasses) — company id 0x058E.
            // The MAC + service UUID are randomized but the company id is fixed,
            // so this catches the glasses for the fox-hunt (geiger) proximity mode.
            if(cid == 0x058E) {
                *kind_out = TrackerKindMetaGlasses;
                name_out[0] = '\0';
                return true;
            }
        } else if((type == 0x02 || type == 0x03) && val_len >= 2) {
            // Incomplete / Complete list of 16-bit Service Class UUIDs
            for(uint8_t i = 0; i + 1 < val_len; i += 2) {
                uint16_t u = val[i] | (val[i + 1] << 8);
                if(u == 0xFEED || u == 0xFD84) {
                    *kind_out = TrackerKindTile;
                    name_out[0] = '\0';
                    return true;
                }
                if(u == 0xFFFA) {
                    *kind_out = TrackerKindDroneRID;
                    name_out[0] = '\0';
                    return true;
                }
            }
        } else if(type == 0x16 && val_len >= 2) {
            // Service Data - 16-bit UUID. Drone Remote ID (ASTM F3411) broadcasts
            // as service data under UUID 0xFFFA followed by the ODID message.
            uint16_t u = val[0] | (val[1] << 8);
            if(u == 0xFFFA) {
                *kind_out = TrackerKindDroneRID;
                name_out[0] = '\0';
                return true;
            }
        }

        pos += fld_len + 1;
    }
    return false;
}

static bool classify_advert(
    const uint8_t* buf,
    uint8_t adv_len,
    uint8_t scan_rsp_len,
    TrackerKind* kind_out,
    char* name_out,
    uint8_t name_out_sz) {
    if(classify_buffer(buf, adv_len, kind_out, name_out, name_out_sz)) return true;
    if(scan_rsp_len > 0 &&
       classify_buffer(buf + adv_len, scan_rsp_len, kind_out, name_out, name_out_sz))
        return true;
    return false;
}

// ---------------------------------------------------------------------------
// Bug-Sweep label helper
// ---------------------------------------------------------------------------

static void sweep_label(
    const uint8_t* adv, uint8_t adv_len, uint8_t scan_rsp_len,
    char* out, uint8_t out_sz) {
    if(ble_adv_extract_local_name(adv, adv_len, out, out_sz)) return;
    if(scan_rsp_len > 0 && ble_adv_extract_local_name(adv + adv_len, scan_rsp_len, out, out_sz))
        return;
    /* No name — try manufacturer company id from AD type 0xFF in the adv buffer. */
    uint8_t pos = 0;
    while(pos + 1 < adv_len) {
        uint8_t fld_len = adv[pos];
        if(fld_len == 0) break;
        if(pos + fld_len >= adv_len) break;
        if(adv[pos + 1] == 0xFF && fld_len >= 3) {
            uint16_t cid = adv[pos + 2] | (adv[pos + 3] << 8);
            snprintf(out, out_sz, "mfg 0x%04X", cid);
            return;
        }
        pos += fld_len + 1;
    }
    out[0] = '\0';
}

// ---------------------------------------------------------------------------
// GAP callback
// ---------------------------------------------------------------------------

static int ble_gap_event(struct ble_gap_event* event, void* arg) {
    UNUSED(arg);
    switch(event->type) {
    case BLE_GAP_EVENT_DISC: {
        /* One advertising report. The tracker scans PASSIVE, so scan responses
         * are never solicited — every report carries just the adv payload in
         * event->disc.data (length_data). We therefore feed the existing
         * classifier with scan_rsp_len = 0 (its scan-response branch was already
         * dead under passive scanning). */
        const uint8_t* adv = event->disc.data;
        uint8_t adv_len = event->disc.length_data;

        /* NimBLE addresses are little-endian (val[0]=LSB). The rest of the app
         * (and the Bluedroid build) treats stored MACs as big-endian / display
         * order, so reverse into a big-endian buffer to keep dedup + logging
         * byte-order identical to before. */
        uint8_t addr_be[6];
        for(int b = 0; b < 6; b++) addr_be[b] = event->disc.addr.val[5 - b];

        TrackerKind kind = TrackerKindUnknown;
        char name[24] = "";
        bool match = classify_advert(adv, adv_len, 0, &kind, name, sizeof(name));
        if(!match) {
            if(!s_sweep) break;
            kind = TrackerKindUnknown;
            sweep_label(adv, adv_len, 0, name, sizeof(name));
        }

        uint32_t now = furi_get_tick();

        if(!s_devices) break; // table freed / not allocated
        // Dedup by MAC
        int found = -1;
        for(int i = 0; i < s_device_count; i++) {
            if(memcmp(s_devices[i].addr, addr_be, 6) == 0) {
                found = i;
                break;
            }
        }
        if(found >= 0) {
            TrackerDevice* d = &s_devices[found];
            if((uint32_t)(now - d->last_seen_ms) >= FOLLOW_STALE_MS) {
                d->first_seen_ms = now;   /* returned from stale → fresh presence */
            }
            d->rssi = event->disc.rssi;
            if(event->disc.rssi > d->best_rssi) d->best_rssi = event->disc.rssi;
            d->last_seen_ms = now;
            // Upgrade name if PP gave us a model after a generic match
            if(d->name[0] == '\0' && name[0] != '\0') {
                strncpy(d->name, name, sizeof(d->name) - 1);
                d->name[sizeof(d->name) - 1] = '\0';
            }
            d->kind = kind;
        } else if(s_device_count < TRACKER_MAX_DEVICES) {
            TrackerDevice* d = &s_devices[s_device_count];
            memcpy(d->addr, addr_be, 6);
            d->addr_type = event->disc.addr.type;
            d->kind = kind;
            d->rssi = event->disc.rssi;
            d->best_rssi = event->disc.rssi;
            d->last_seen_ms = now;
            d->first_seen_ms = now;
            strncpy(d->name, name, sizeof(d->name) - 1);
            d->name[sizeof(d->name) - 1] = '\0';
            __atomic_store_n(&s_device_count, (uint16_t)(s_device_count + 1), __ATOMIC_RELEASE);
        }
        break;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        break;

    default:
        break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool ble_tracker_hal_start_scan(void) {
    if(!s_devices) {
        s_devices = calloc(TRACKER_MAX_DEVICES, sizeof(*s_devices));
        if(!s_devices) {
            ESP_LOGE(TAG, "s_devices alloc failed");
            return false;
        }
    }
    __atomic_store_n(&s_device_count, 0, __ATOMIC_RELEASE);
    memset(s_devices, 0, sizeof(*s_devices) * TRACKER_MAX_DEVICES);

    /* NimBLE ble_gap_disc() both sets params and starts the scan synchronously,
     * replacing the Bluedroid set_scan_params -> (callback) -> start_scanning
     * two-step. The event callback is passed here, not globally registered. */
    struct ble_gap_disc_params disc_params = {
        .itvl = 0x50,           // 50ms (0.625ms units)
        .window = 0x30,         // 30ms
        .filter_policy = 0,     // BLE_HCI_SCAN_FILT_NO_WL — allow all
        .limited = 0,
        .passive = 1,           // was BLE_SCAN_TYPE_PASSIVE
        .filter_duplicates = 0, // report every advert (need repeats for RSSI/dwell)
    };

    int rc = ble_gap_disc(
        BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &disc_params, ble_gap_event, NULL);
    if(rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
        s_scanning = false;
        return false;
    }
    s_scanning = true;
    return true;
}

void ble_tracker_hal_stop_scan(void) {
    if(s_scanning) {
        /* Synchronous; NimBLE does NOT emit a DISC_COMPLETE for a manual cancel,
         * so clear the flag ourselves. */
        ble_gap_disc_cancel();
        s_scanning = false;
    }
}

TrackerDevice* ble_tracker_hal_get_devices(uint16_t* count) {
    if(!s_devices) {
        *count = 0;
        return NULL;
    }
    *count = __atomic_load_n(&s_device_count, __ATOMIC_ACQUIRE);
    return s_devices;
}

// Free the device table. Call from a tracker scene's on_exit AFTER stop_scan
// and after the last get_devices read — NOT from stop_scan (scenes read the
// table after stopping the scan).
void ble_tracker_hal_release(void) {
    free(s_devices);
    s_devices = NULL;
    __atomic_store_n(&s_device_count, 0, __ATOMIC_RELEASE);
}

void ble_tracker_hal_set_sweep(bool enable) {
    s_sweep = enable;
    /* Clear the table so the list reflects the active mode. */
    __atomic_store_n(&s_device_count, 0, __ATOMIC_RELEASE);
    if(s_devices) memset(s_devices, 0, sizeof(*s_devices) * TRACKER_MAX_DEVICES);
}

const char* ble_tracker_kind_label(TrackerKind k) {
    switch(k) {
    case TrackerKindAirTag: return "AirTag";
    case TrackerKindSmartTag: return "SmartTag";
    case TrackerKindTile: return "Tile";
    case TrackerKindApplePP: return "Apple";
    case TrackerKindAppleNearby: return "iPhone/Watch";
    case TrackerKindMetaGlasses: return "Meta Glasses";
    case TrackerKindDroneRID: return "Drone RID";
    default: return "?";
    }
}
