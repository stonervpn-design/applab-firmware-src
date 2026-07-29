#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <assert.h>

// Include the mapping header
#include "../applications/main/defender/defender_locator_map.h"

int main(void) {
    printf("Testing Defender Locator mappings...\n");

    int8_t floor = -90;
    int8_t ceil = -40;

    // Test bar level
    printf("\nTesting bar level...\n");

    // At floor → 0
    uint8_t bar = defender_locator_bar_level(-90, floor, ceil);
    printf("  bar(-90, -90, -40) = %u (expect 0)\n", bar);
    assert(bar == 0);

    // At ceil → LOCATOR_BAR_MAX (10)
    bar = defender_locator_bar_level(-40, floor, ceil);
    printf("  bar(-40, -90, -40) = %u (expect %u)\n", bar, LOCATOR_BAR_MAX);
    assert(bar == LOCATOR_BAR_MAX);

    // Below floor (clamp) → 0
    bar = defender_locator_bar_level(-100, floor, ceil);
    printf("  bar(-100, -90, -40) = %u (expect 0, clamped)\n", bar);
    assert(bar == 0);

    // Above ceil (clamp) → LOCATOR_BAR_MAX
    bar = defender_locator_bar_level(-30, floor, ceil);
    printf("  bar(-30, -90, -40) = %u (expect %u, clamped)\n", bar, LOCATOR_BAR_MAX);
    assert(bar == LOCATOR_BAR_MAX);

    // Midpoint → 5
    bar = defender_locator_bar_level(-65, floor, ceil);
    printf("  bar(-65, -90, -40) = %u (expect 5, midpoint)\n", bar);
    assert(bar == 5);

    // Test beep period
    printf("\nTesting beep period...\n");

    // At ceil (not stale) → 80 ms
    uint32_t period = defender_locator_beep_period_ms(-40, false, floor, ceil);
    printf("  period(-40, false, -90, -40) = %u (expect 80)\n", period);
    assert(period == 80);

    // At floor (not stale) → 1200 ms
    period = defender_locator_beep_period_ms(-90, false, floor, ceil);
    printf("  period(-90, false, -90, -40) = %u (expect 1200)\n", period);
    assert(period == 1200);

    // At midpoint (not stale) → ~640 ms (1200 - 1120*25/50 = 640)
    period = defender_locator_beep_period_ms(-65, false, floor, ceil);
    printf("  period(-65, false, -90, -40) = %u (expect ~640)\n", period);
    // Linear interpolation: 1200 + (80-1200) * ((-65-(-90))/((-40)-(-90)))
    // = 1200 + (-1120) * (25/50) = 1200 - 560 = 640
    assert(period >= 630 && period <= 650);

    // Stale flag mutes → 0
    period = defender_locator_beep_period_ms(-50, true, floor, ceil);
    printf("  period(-50, true, -90, -40) = %u (expect 0, stale mutes)\n", period);
    assert(period == 0);

    period = defender_locator_beep_period_ms(-90, true, floor, ceil);
    printf("  period(-90, true, -90, -40) = %u (expect 0, stale mutes)\n", period);
    assert(period == 0);

    printf("\nALL DEFENDER LOCATOR TESTS PASSED\n");
    return 0;
}
