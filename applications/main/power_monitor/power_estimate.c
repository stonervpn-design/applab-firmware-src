#include "power_estimate.h"

void power_curve_reset(PowerCurve* c) {
    memset(c, 0, sizeof(*c));
}

void power_curve_push(PowerCurve* c, uint8_t charge_pct) {
    if(charge_pct > 100) charge_pct = 100;
    c->samples[c->head] = charge_pct;
    c->head = (uint8_t)((c->head + 1) % PWR_CURVE_LEN);
    if(c->filled < PWR_CURVE_LEN) c->filled++;
}

uint8_t power_curve_get(const PowerCurve* c, uint8_t idx_from_oldest) {
    if(idx_from_oldest >= c->filled) return 0;
    uint8_t start = (c->filled == PWR_CURVE_LEN) ? c->head : 0;
    return c->samples[(start + idx_from_oldest) % PWR_CURVE_LEN];
}

int32_t power_estimate_minutes(bool charging, uint32_t current_ma,
                               uint32_t capacity_mah, uint8_t charge_pct) {
    if(current_ma == 0) return -1;
    if(charge_pct > 100) charge_pct = 100;
    uint32_t pct_span = charging ? (uint32_t)(100 - charge_pct) : (uint32_t)charge_pct;
    if(pct_span == 0) return -1;
    // mAh to move, then hours = mAh/mA, minutes = *60.
    uint32_t mah = (capacity_mah * pct_span) / 100;
    if(mah == 0) return -1;
    return (int32_t)((mah * 60) / current_ma);
}
