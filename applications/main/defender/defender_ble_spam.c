#include "defender_ble_spam.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/**
 * Small stack-based set to track distinct MACs (cap 64).
 */
typedef struct {
    uint8_t macs[64][6];
    uint16_t count;
} MacSet;

/**
 * Add a MAC to the set if not already present.
 * Returns true if newly added; false if already existed.
 */
static bool mac_set_add(MacSet* s, const uint8_t addr[6]) {
    // Check if MAC already exists.
    for(uint16_t i = 0; i < s->count; i++) {
        if(memcmp(s->macs[i], addr, 6) == 0) {
            return false;  // Already in set.
        }
    }
    // Add if not at capacity.
    if(s->count < 64) {
        memcpy(s->macs[s->count], addr, 6);
        s->count++;
        return true;
    }
    return false;  // Set at capacity.
}

void defender_blespam_eval(
    const DefenderBleSpamRec* recs,
    size_t n,
    uint32_t now_ms,
    uint32_t window_ms,
    uint32_t spam_alert,
    uint32_t mac_flood_alert,
    DefenderBleSpamResult* out) {

    // Initialize output.
    memset(out, 0, sizeof(*out));

    if(n == 0) {
        // No records: no spam.
        out->alert = false;
        return;
    }

    // Calculate window bounds: records with ts_ms in (now_ms - window_ms, now_ms].
    uint32_t window_start = now_ms - window_ms;
    uint32_t window_end = now_ms;

    MacSet mac_set = {0};

    // Scan all records.
    for(size_t i = 0; i < n; i++) {
        const DefenderBleSpamRec* rec = &recs[i];

        // Check if record is in-window.
        // Handle wrap-around: if now_ms < window_start (wrapped), include all with ts > window_start.
        bool in_window = false;
        if(now_ms >= window_ms) {
            // No wrap: simple comparison.
            in_window = (rec->ts_ms > window_start) && (rec->ts_ms <= window_end);
        } else {
            // Wrap-around: include ts in (window_start, 0xFFFFFFFF] or [0, window_end].
            in_window = (rec->ts_ms > window_start) || (rec->ts_ms <= window_end);
        }

        if(!in_window) {
            continue;
        }

        // Count spam types.
        if(rec->company_id == BLE_CID_APPLE) {
            out->continuity++;
        }
        if(rec->service_uuid == BLE_UUID_FASTPAIR) {
            out->fastpair++;
        }
        if(rec->company_id == BLE_CID_MICROSOFT) {
            out->swiftpair++;
        }

        // Track distinct random MACs.
        if(rec->is_random_mac) {
            mac_set_add(&mac_set, rec->addr);
        }
    }

    out->distinct_random_macs = mac_set.count;

    // Determine if alert should fire.
    uint16_t spam_total = out->continuity + out->fastpair + out->swiftpair;
    out->alert = (spam_total >= spam_alert) || (out->distinct_random_macs >= mac_flood_alert);
}
