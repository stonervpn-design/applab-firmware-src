#pragma once

#include <gui/view.h>

typedef struct {
    char label[28];
    int8_t rssi;
    bool stale;
    int8_t floor;
    int8_t ceil;
} DefenderLocatorModel;

View* defender_locator_view_alloc(void);
void defender_locator_view_free(View* view);
