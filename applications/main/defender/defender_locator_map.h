#ifndef DEFENDER_LOCATOR_MAP_H
#define DEFENDER_LOCATOR_MAP_H

#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#define LOCATOR_BAR_MAX 10

/**
 * Map RSSI to a bar level (0..LOCATOR_BAR_MAX).
 * Below floor→0, above ceil→MAX, linear between.
 * Clamps out-of-bounds values.
 */
static inline uint8_t defender_locator_bar_level(int8_t rssi, int8_t floor, int8_t ceil) {
    if(ceil <= floor) return 0; // Guard

    // Clamp rssi to [floor, ceil]
    if(rssi <= floor) return 0;
    if(rssi >= ceil) return LOCATOR_BAR_MAX;

    // Linear interpolation
    int32_t numerator = (int32_t)(rssi - floor) * LOCATOR_BAR_MAX;
    int32_t denominator = (int32_t)(ceil - floor);
    uint8_t bar = (uint8_t)(numerator / denominator);

    // Clamp to valid range
    if(bar > LOCATOR_BAR_MAX) bar = LOCATOR_BAR_MAX;

    return bar;
}

/**
 * Map RSSI to a beep period in milliseconds.
 * stale=true → 0 (muted)
 * ceil → 80 ms (fast)
 * floor → 1200 ms (slow)
 * Linear between. Clamped out-of-bounds.
 */
static inline uint32_t defender_locator_beep_period_ms(int8_t rssi, bool stale, int8_t floor, int8_t ceil) {
    if(stale) return 0; // Stale mutes

    if(ceil <= floor) return 1200; // Guard

    // Clamp rssi to [floor, ceil]
    if(rssi <= floor) return 1200;
    if(rssi >= ceil) return 80;

    // Linear interpolation: period = 1200 + (80 - 1200) * (rssi - floor) / (ceil - floor)
    int32_t numerator = (int32_t)(rssi - floor) * (80 - 1200);
    int32_t denominator = (int32_t)(ceil - floor);
    uint32_t period = 1200 + (uint32_t)(numerator / denominator);

    return period;
}

#endif // DEFENDER_LOCATOR_MAP_H
