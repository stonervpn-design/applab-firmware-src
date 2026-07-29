#include "defender_ble_tracker.h"
#include "defender_ble_tracker_eval.h"
#include "../ble_spam/tracker_follow.h"

int defender_ble_tracker_poll(const TrackerDevice* devs, uint16_t count, uint32_t now_ms) {
    if(!devs || count == 0) return -1;

    int best_idx = -1;
    int8_t best_rssi = -128;

    for(uint16_t i = 0; i < count; i++) {
        const TrackerDevice* d = &devs[i];

        // Compute follow state from dwell time and staleness
        FollowState st = tracker_follow_state(d->first_seen_ms, d->last_seen_ms, now_ms);

        // Check if this is a known brand
        bool known = d->kind != TrackerKindUnknown;

        // Decide: should we alert on this device?
        if(defender_ble_tracker_should_alert(st, d->rssi, d->best_rssi, known)) {
            // Track the strongest offender (highest RSSI = closest device)
            if(best_idx == -1 || d->rssi > best_rssi) {
                best_idx = i;
                best_rssi = d->rssi;
            }
        }
    }

    return best_idx;
}
