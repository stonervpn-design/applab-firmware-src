#include "defender_deauth.h"

/**
 * Per-BSSID tracking entry for finding the top offender.
 */
typedef struct {
    uint8_t bssid[6];
    uint16_t count;
    int8_t last_rssi;
} DefenderBssidEntry;

void defender_deauth_eval(
    const DefenderDeauthRec* recs,
    size_t n,
    uint32_t now_ms,
    uint32_t window_ms,
    uint32_t rate_alert,
    DefenderDeauthResult* out) {

    /* Zero out result */
    memset(out, 0, sizeof(*out));

    /* Per-BSSID tracking (cap 16 distinct) */
    DefenderBssidEntry bssids[16];
    memset(bssids, 0, sizeof(bssids));
    uint32_t bssid_count = 0;

    /* First pass: accumulate weighted count and track per-BSSID */
    for(size_t i = 0; i < n; i++) {
        const DefenderDeauthRec* r = &recs[i];

        /* Skip if outside window using wrap-safe modular compare.
         * A record is in-window if: (now_ms - r->ts_ms) < window_ms
         * This handles wrap-around correctly due to unsigned math.
         */
        uint32_t age = now_ms - r->ts_ms;
        if(age >= window_ms) {
            continue;  /* Too old, skip */
        }

        /* Add to weighted count */
        uint32_t weight = r->broadcast ? DEAUTH_BROADCAST_WEIGHT : 1;
        out->weighted_count += weight;

        /* Track per-BSSID */
        int bssid_idx = -1;
        for(uint32_t j = 0; j < bssid_count; j++) {
            if(memcmp(bssids[j].bssid, r->bssid, 6) == 0) {
                bssid_idx = (int)j;
                break;
            }
        }

        if(bssid_idx >= 0) {
            /* Existing BSSID */
            bssids[bssid_idx].count++;
            bssids[bssid_idx].last_rssi = r->rssi;
        } else if(bssid_count < 16) {
            /* New BSSID (still room in table) */
            memcpy(bssids[bssid_count].bssid, r->bssid, 6);
            bssids[bssid_count].count = 1;
            bssids[bssid_count].last_rssi = r->rssi;
            bssid_count++;
        }
        /* If table is full, overflow BSSIDs are ignored for top-offender tracking */
    }

    /* Find top offender (raw count, not weighted) */
    uint16_t top_raw_count = 0;
    int top_idx = -1;
    for(uint32_t i = 0; i < bssid_count; i++) {
        if(bssids[i].count > top_raw_count) {
            top_raw_count = bssids[i].count;
            top_idx = (int)i;
        }
    }

    if(top_idx >= 0) {
        memcpy(out->top_bssid, bssids[top_idx].bssid, 6);
        out->top_count = bssids[top_idx].count;
        out->top_rssi = bssids[top_idx].last_rssi;
    }

    /* Alert threshold */
    out->alert = out->weighted_count >= rate_alert;
}
