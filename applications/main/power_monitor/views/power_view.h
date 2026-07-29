#pragma once
#include <gui/view.h>
#include "../power_estimate.h"

typedef struct {
    uint8_t charge_pct;
    float voltage;      // gauge voltage (V)
    int32_t current_ma; // signed gauge current (mA); >0 charging
    bool charging;
    int32_t est_minutes; // -1 if unknown
    PowerCurve curve;    // charge-% history (owned here, copied in on update)
} PowerViewModel;

View* power_view_alloc(void);
void power_view_free(View* v);
// Push a fresh reading; the view pushes charge_pct into its own curve.
void power_view_update(View* v, uint8_t charge_pct, float voltage,
                       int32_t current_ma, bool charging, int32_t est_minutes);
