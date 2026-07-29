#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "../ble_spam/tracker_follow.h"

/**
 * Alert decision for unwanted tracker detection.
 *
 * Alert only when a brand-known tracker has escalated to FollowFollowing AND its
 * RSSI is rising/steady (within RISE_MARGIN dB of best_rssi → still close / approaching).
 * This is the "rising RSSI while dwelling" stalking signal.
 */
#define BLE_TRACKER_RISE_MARGIN 6

static inline bool defender_ble_tracker_should_alert(
    FollowState st, int8_t rssi, int8_t best_rssi, bool known_brand) {
    // Alert only if:
    // 1. Known brand (not Unknown)
    // 2. FOLLOWING state (dwelt >= 5 min)
    // 3. RSSI is within RISE_MARGIN dB of best (not moving away / approaching or steady)
    return known_brand && st == FollowFollowing && (int)(best_rssi - rssi) <= BLE_TRACKER_RISE_MARGIN;
}
