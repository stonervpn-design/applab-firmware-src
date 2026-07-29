#include "../applications/main/power_monitor/power_estimate.c"
#include <assert.h>
#include <stdio.h>

int main(void) {
    PowerCurve c;
    power_curve_reset(&c);
    assert(c.filled == 0);
    power_curve_push(&c, 50);
    power_curve_push(&c, 60);
    power_curve_push(&c, 200); // clamps to 100
    assert(c.filled == 3);
    assert(power_curve_get(&c, 0) == 50);  // oldest
    assert(power_curve_get(&c, 2) == 100); // newest (clamped)

    // ring wraps, oldest dropped
    for (int i = 0; i < PWR_CURVE_LEN + 20; i++) power_curve_push(&c, (uint8_t)(i % 101));
    assert(c.filled == PWR_CURVE_LEN);

    // discharge: 1000 mAh at 50% = 500 mAh usable, 500 mA draw → 60 min
    assert(power_estimate_minutes(false, 500, 1000, 50) == 60);
    // charge: 1000 mAh, 20% → 80% headroom = 800 mAh, 800 mA → 60 min
    assert(power_estimate_minutes(true, 800, 1000, 20) == 60);
    // zero current → -1
    assert(power_estimate_minutes(false, 0, 1000, 50) == -1);
    // full while charging → -1 (no headroom)
    assert(power_estimate_minutes(true, 500, 1000, 100) == -1);
    // empty while discharging → -1
    assert(power_estimate_minutes(false, 500, 1000, 0) == -1);

    printf("test_power_estimate OK\n");
    return 0;
}
