#pragma once
#include <gui/view.h>
#include <stdint.h>

#define STATIONS_VIEW_ROWS 5

typedef struct {
    char lines[STATIONS_VIEW_ROWS][32];
    uint8_t line_count;
    uint16_t ap_count;
    uint16_t client_total;
} WlanStationsViewModel;

View* wlan_stations_view_alloc(void);
void wlan_stations_view_free(View* v);
void wlan_stations_view_update(
    View* v,
    const char lines[][32],
    uint8_t n,
    uint16_t aps,
    uint16_t clients);
