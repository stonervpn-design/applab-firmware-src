#pragma once

#include <gui/view.h>
#include "../ble_tracker_hal.h"

#define TRACKER_LIST_ITEMS_ON_SCREEN 4

typedef struct {
    TrackerDevice devices[TRACKER_MAX_DEVICES];
    uint16_t count;
    uint16_t selected;
    uint16_t window_offset;
    uint32_t now_ms;    // furi_get_tick() at publish, for follow-state calc
    bool sweep_mode;    // header label: false=Trackers, true=Bug Sweep
} TrackerListModel;

View* tracker_list_view_alloc(void);
void tracker_list_view_free(View* view);
