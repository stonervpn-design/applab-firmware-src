#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include "../applications/main/defender/defender_ble_tracker_eval.h"

/**
 * Test: Unknown kind never alerts
 */
void test_unknown_never_alerts(void) {
    bool result = defender_ble_tracker_should_alert(FollowFollowing, -50, -50, false);
    assert(!result);
}

/**
 * Test: Known kind + FOLLOWING + rising RSSI → alert
 * FollowFollowing, rssi=-52, best=-50, known=true → true (within 6 dB)
 */
void test_following_rising(void) {
    bool result = defender_ble_tracker_should_alert(FollowFollowing, -52, -50, true);
    assert(result);
}

/**
 * Test: Known kind + FOLLOWING + falling RSSI → no alert
 * FollowFollowing, rssi=-70, best=-50, known=true → false (dropped 20 dB)
 */
void test_following_falling(void) {
    bool result = defender_ble_tracker_should_alert(FollowFollowing, -70, -50, true);
    assert(!result);
}

/**
 * Test: Known kind + SUSPECT (not FOLLOWING) → no alert
 * FollowSuspect, rssi=-50, best=-50, known=true → false
 */
void test_suspect_no_alert(void) {
    bool result = defender_ble_tracker_should_alert(FollowSuspect, -50, -50, true);
    assert(!result);
}

/**
 * Test: Boundary at RISE_MARGIN (6 dB)
 * rssi = best - 6 = -56 → true (exactly at boundary, inclusive)
 */
void test_boundary_at_margin(void) {
    bool result = defender_ble_tracker_should_alert(FollowFollowing, -56, -50, true);
    assert(result);
}

/**
 * Test: Boundary beyond RISE_MARGIN (7 dB)
 * rssi = best - 7 = -57 → false (just outside boundary)
 */
void test_boundary_beyond_margin(void) {
    bool result = defender_ble_tracker_should_alert(FollowFollowing, -57, -50, true);
    assert(!result);
}

int main(void) {
    test_unknown_never_alerts();
    test_following_rising();
    test_following_falling();
    test_suspect_no_alert();
    test_boundary_at_margin();
    test_boundary_beyond_margin();

    printf("ALL DEFENDER BLE TRACKER TESTS PASSED\n");
    return 0;
}
