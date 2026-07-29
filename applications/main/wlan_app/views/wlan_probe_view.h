#pragma once
#include <gui/view.h>
#include <stdint.h>

#define PROBE_VIEW_ROWS 5

typedef struct {
    char lines[PROBE_VIEW_ROWS][32];
    uint8_t line_count;
    uint16_t unique_clients;
    uint16_t unique_ssids;
} WlanProbeViewModel;

View* wlan_probe_view_alloc(void);
void wlan_probe_view_free(View* v);
void wlan_probe_view_update(View* v, const char lines[][32], uint8_t n,
                            uint16_t clients, uint16_t ssids);
