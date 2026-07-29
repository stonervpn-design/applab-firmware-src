#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TRACKER_MAX_DEVICES 40

typedef enum {
    TrackerKindUnknown,
    TrackerKindAirTag,        // Apple FindMy: mfg 0x004C + sub 0x12
    TrackerKindSmartTag,      // Samsung: mfg 0x0075
    TrackerKindTile,          // Service UUID 0xFEED / 0xFD84
    TrackerKindApplePP,       // Apple Proximity Pairing: mfg 0x004C + sub 0x07 (AirPods etc.)
    TrackerKindAppleNearby,   // Apple Nearby: mfg 0x004C + sub 0x10 (iPhone/Watch)
    TrackerKindMetaGlasses,   // Ray-Ban Meta smart glasses: mfg 0x058E (fox-hunt target)
    TrackerKindDroneRID,      // Drone Remote ID (ASTM F3411): service data UUID 0xFFFA
} TrackerKind;

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;    /* NimBLE ble_addr_t.type (0=public,1=random,...) */
    TrackerKind kind;
    char name[24];        // model name (Apple PP) or empty
    int8_t rssi;
    int8_t best_rssi;
    uint32_t last_seen_ms;
    uint32_t first_seen_ms;
} TrackerDevice;

bool ble_tracker_hal_start_scan(void);
void ble_tracker_hal_stop_scan(void);
/** Free the on-demand device table. Call from a tracker scene's on_exit after
 *  stop_scan and the final get_devices read. */
void ble_tracker_hal_release(void);
TrackerDevice* ble_tracker_hal_get_devices(uint16_t* count);

const char* ble_tracker_kind_label(TrackerKind k);

/* Bug-Sweep mode: when enabled, devices that match no known tracker brand are
 * ALSO recorded (kind = TrackerKindUnknown, labeled by BLE local name or
 * "mfg 0xXXXX"). Toggling clears the device table. Default disabled. */
void ble_tracker_hal_set_sweep(bool enable);
