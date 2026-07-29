// Optional GPS-tagged loot. Formats a Wigle-style CSV row (pure) and, when the GNSS
// component has a valid fix, appends captures to a wardrive CSV. All GNSS use is
// guarded so the app is unaffected when no GPS hardware/fix is present.
#pragma once

#include "fg_types.h"
#include <storage/storage.h>

int fg_geo_format_row(
    char* out,
    size_t cap,
    const uint8_t bssid[6],
    const char* ssid,
    uint8_t channel,
    int8_t rssi,
    double lat,
    double lon);

void fg_geo_init(void); // lazily start GNSS if the component is present
bool fg_geo_has_fix(void);
// Append a geo-tagged row for a fresh capture; returns true if a row was written.
bool fg_geo_tag(Storage* s, const uint8_t bssid[6], const char* ssid, uint8_t channel, int8_t rssi);
