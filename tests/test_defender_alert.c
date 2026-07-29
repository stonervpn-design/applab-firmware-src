#include <stdio.h>
#include <assert.h>
#include <string.h>

// Include the pure header (no firmware dependencies).
#include "../applications/main/defender/defender_alert.h"

/**
 * Test: category labels.
 */
static void test_labels(void) {
    assert(strcmp(defender_cat_label(DefenderCatWifiDeauth), "WIFI/DEAUTH") == 0);
    assert(strcmp(defender_cat_label(DefenderCatWifiRogueAp), "WIFI/ROGUE_AP") == 0);
    assert(strcmp(defender_cat_label(DefenderCatWifiBeaconFlood), "WIFI/BEACON_FLOOD") == 0);
    assert(strcmp(defender_cat_label(DefenderCatWifiKarma), "WIFI/KARMA") == 0);
    assert(strcmp(defender_cat_label(DefenderCatWifiHwFp), "WIFI/HW_FP") == 0);
    assert(strcmp(defender_cat_label(DefenderCatBleTracker), "BLE/TRACKER") == 0);
    assert(strcmp(defender_cat_label(DefenderCatBleSpam), "BLE/SPAM") == 0);
    assert(strcmp(defender_cat_label(DefenderCatSubghzJam), "SUBGHZ/JAM") == 0);
    assert(strcmp(defender_cat_label(DefenderCatRf24Occ), "RF24/OCC") == 0);
    assert(strcmp(defender_cat_label(DefenderCatDrone), "DRONE") == 0);
    assert(strcmp(defender_cat_label(DefenderCatCount), "UNKNOWN") == 0);
    printf("✓ test_labels passed\n");
}

/**
 * Test: debounce fires on first check.
 */
static void test_debounce_first_fires(void) {
    DefenderDebounceSet s = {0};
    uint8_t A[6] = {1, 2, 3, 4, 5, 6};

    bool result = defender_debounce_check(&s, DefenderCatWifiDeauth, A, 1000, 8000);
    assert(result == true);  // First check fires.
    printf("✓ test_debounce_first_fires passed\n");
}

/**
 * Test: debounce suppresses immediate repeat.
 */
static void test_debounce_suppresses(void) {
    DefenderDebounceSet s = {0};
    uint8_t A[6] = {1, 2, 3, 4, 5, 6};

    defender_debounce_check(&s, DefenderCatWifiDeauth, A, 1000, 8000);
    bool result = defender_debounce_check(&s, DefenderCatWifiDeauth, A, 1500, 8000);
    assert(result == false);  // Repeat within stale period suppressed.
    printf("✓ test_debounce_suppresses passed\n");
}

/**
 * Test: debounce re-arms after stale period.
 */
static void test_debounce_rearm_on_stale(void) {
    DefenderDebounceSet s = {0};
    uint8_t A[6] = {1, 2, 3, 4, 5, 6};

    defender_debounce_check(&s, DefenderCatWifiDeauth, A, 1000, 8000);
    bool result = defender_debounce_check(&s, DefenderCatWifiDeauth, A, 9000, 8000);
    assert(result == true);  // After stale period (>= 8000ms), re-arms.
    printf("✓ test_debounce_rearm_on_stale passed\n");
}

/**
 * Test: distinct offenders alert separately.
 */
static void test_debounce_distinct_offender(void) {
    DefenderDebounceSet s = {0};
    uint8_t A[6] = {1, 2, 3, 4, 5, 6};
    uint8_t B[6] = {1, 2, 3, 4, 5, 7};

    defender_debounce_check(&s, DefenderCatWifiDeauth, A, 1000, 8000);
    bool result = defender_debounce_check(&s, DefenderCatWifiDeauth, B, 1500, 8000);
    assert(result == true);  // Different offender fires.
    printf("✓ test_debounce_distinct_offender passed\n");
}

/**
 * Test: distinct categories alert separately.
 */
static void test_debounce_distinct_cat(void) {
    DefenderDebounceSet s = {0};
    uint8_t A[6] = {1, 2, 3, 4, 5, 6};

    defender_debounce_check(&s, DefenderCatWifiDeauth, A, 1000, 8000);
    bool result = defender_debounce_check(&s, DefenderCatBleSpam, A, 1500, 8000);
    assert(result == true);  // Different category fires.
    printf("✓ test_debounce_distinct_cat passed\n");
}

/**
 * Test: table saturation and LRU eviction.
 */
static void test_debounce_saturation(void) {
    DefenderDebounceSet s = {0};

    // Fill all 16 slots with distinct offenders at t=1000.
    uint8_t offenders[16][6];
    for(int i = 0; i < 16; i++) {
        offenders[i][0] = 1;
        offenders[i][1] = 2;
        offenders[i][2] = 3;
        offenders[i][3] = 4;
        offenders[i][4] = 5;
        offenders[i][5] = (uint8_t)i;  // Unique byte in position 5.
        defender_debounce_check(&s, DefenderCatWifiDeauth, offenders[i], 1000, 8000);
    }

    // Verify all 16 are used.
    int used_count = 0;
    for(int i = 0; i < DEFENDER_DEBOUNCE_MAX; i++) {
        if(s.e[i].used) used_count++;
    }
    assert(used_count == 16);

    // Add a 17th distinct offender at t=1100 → should evict oldest (LRU).
    uint8_t new_offender[6] = {1, 2, 3, 4, 5, 99};
    bool result = defender_debounce_check(&s, DefenderCatWifiDeauth, new_offender, 1100, 8000);
    assert(result == true);  // New entry fires.

    // Verify still 16 used (evicted one).
    used_count = 0;
    for(int i = 0; i < DEFENDER_DEBOUNCE_MAX; i++) {
        if(s.e[i].used) used_count++;
    }
    assert(used_count == 16);

    // Re-check first offender at t=1100 (within stale) → should fire (was evicted).
    result = defender_debounce_check(&s, DefenderCatWifiDeauth, offenders[0], 1100, 8000);
    assert(result == true);  // Evicted entry can fire again.

    printf("✓ test_debounce_saturation passed\n");
}

/**
 * Main test runner.
 */
int main(void) {
    printf("Running defender_alert tests...\n");

    test_labels();
    test_debounce_first_fires();
    test_debounce_suppresses();
    test_debounce_rearm_on_stale();
    test_debounce_distinct_offender();
    test_debounce_distinct_cat();
    test_debounce_saturation();

    printf("\nALL DEFENDER ALERT TESTS PASSED\n");
    return 0;
}
