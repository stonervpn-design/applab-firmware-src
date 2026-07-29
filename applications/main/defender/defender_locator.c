#include "defender_locator.h"
#include "views/defender_locator_view.h"
#include <stdlib.h>
#include <string.h>

struct DefenderLocator {
    View* view;
    int8_t floor;
    int8_t ceil;
    int8_t ema_rssi;      // EMA-smoothed RSSI
    bool have_sample;     // Have we received at least one sample?
};

DefenderLocator* defender_locator_alloc(void) {
    DefenderLocator* locator = malloc(sizeof(DefenderLocator));
    if(!locator) return NULL;
    locator->view = defender_locator_view_alloc();
    locator->floor = -90;
    locator->ceil = -40;
    locator->ema_rssi = 0;
    locator->have_sample = false;

    // Initialize model
    DefenderLocatorModel* model = view_get_model(locator->view);
    strncpy(model->label, "", sizeof(model->label) - 1);
    model->label[sizeof(model->label) - 1] = '\0';
    model->rssi = 0;
    model->stale = true;
    model->floor = locator->floor;
    model->ceil = locator->ceil;
    view_commit_model(locator->view, true);

    return locator;
}

void defender_locator_free(DefenderLocator* locator) {
    if(locator) {
        defender_locator_view_free(locator->view);
        free(locator);
    }
}

void defender_locator_set_bounds(DefenderLocator* locator, int8_t rssi_floor, int8_t rssi_ceil) {
    if(!locator) return;

    locator->floor = rssi_floor;
    locator->ceil = rssi_ceil;

    // Update model
    DefenderLocatorModel* model = view_get_model(locator->view);
    model->floor = rssi_floor;
    model->ceil = rssi_ceil;
    view_commit_model(locator->view, true);
}

void defender_locator_set_label(DefenderLocator* locator, const char* label) {
    if(!locator) return;

    DefenderLocatorModel* model = view_get_model(locator->view);
    strncpy(model->label, label, sizeof(model->label) - 1);
    model->label[sizeof(model->label) - 1] = '\0';
    view_commit_model(locator->view, true);
}

void defender_locator_push(DefenderLocator* locator, int8_t rssi, bool stale) {
    if(!locator) return;

    // Apply EMA smoothing: ema = have ? (ema*7 + rssi*3)/10 : rssi
    if(locator->have_sample) {
        locator->ema_rssi = (int8_t)((locator->ema_rssi * 7 + rssi * 3) / 10);
    } else {
        locator->ema_rssi = rssi;
        locator->have_sample = true;
    }

    // Update model with smoothed RSSI
    DefenderLocatorModel* model = view_get_model(locator->view);
    model->rssi = locator->ema_rssi;
    model->stale = stale;
    view_commit_model(locator->view, true);
}

View* defender_locator_get_view(DefenderLocator* locator) {
    if(!locator) return NULL;
    return locator->view;
}
