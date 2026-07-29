#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define PWR_CURVE_LEN 96

typedef struct {
    uint8_t samples[PWR_CURVE_LEN];
    uint8_t head;
    uint8_t filled;
} PowerCurve;

void power_curve_reset(PowerCurve* c);
void power_curve_push(PowerCurve* c, uint8_t charge_pct);
uint8_t power_curve_get(const PowerCurve* c, uint8_t idx_from_oldest);
int32_t power_estimate_minutes(bool charging, uint32_t current_ma,
                               uint32_t capacity_mah, uint8_t charge_pct);
