#pragma once

#include <gui/view.h>
#include "../ble_tracker_hal.h"

typedef struct {
    uint8_t target_addr[6];
    TrackerKind target_kind;
    char target_name[24];
    int8_t rssi;             // raw latest RSSI
    int8_t rssi_smooth;      // EMA of rssi (drives label/bar/beep)
    int8_t rssi_peak;        // decaying peak-hold marker (~1 dB/s)
    int8_t trend;            // +1 rising, -1 falling, 0 steady
    uint32_t last_seen_ms;
    bool stale;
} TrackerGeigerModel;

View* tracker_geiger_view_alloc(void);
void tracker_geiger_view_free(View* view);
