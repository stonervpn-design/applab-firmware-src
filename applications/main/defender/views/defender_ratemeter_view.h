#pragma once

#include <gui/view.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char title[24];
    uint32_t rate;
    uint8_t top_bssid[6];
    uint16_t top_count;
    int8_t top_rssi;
    bool alert;
} DefenderRatemeterModel;

View* defender_ratemeter_view_alloc(void);
void defender_ratemeter_view_free(View* view);
