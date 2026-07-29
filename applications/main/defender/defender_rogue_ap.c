#include "defender_rogue_ap.h"

// Bounded working tables (no dynamic allocation; no-PSRAM friendly).
#define RG_MAX_SSID 24
#define RG_MAX_BSSID 48
#define RG_BSSID_PER_SSID 3 // enough to detect evil-twin (>= 2) exactly

static bool rg_mac_eq(const uint8_t* a, const uint8_t* b) {
    return memcmp(a, b, 6) == 0;
}

void defender_rogue_eval(
    const DefenderBeaconRec* recs,
    size_t n,
    uint32_t now_ms,
    uint32_t window_ms,
    DefenderRogueResult* out) {
    memset(out, 0, sizeof(*out));
    if(!recs) return;

    struct SsidEntry {
        char ssid[ROGUE_SSID_MAX];
        uint8_t bssids[RG_BSSID_PER_SSID][6];
        uint8_t stored; // bssids actually stored (<= RG_BSSID_PER_SSID)
        uint8_t count; // distinct bssids seen (may exceed stored)
    } st[RG_MAX_SSID];
    uint8_t st_n = 0;

    uint8_t bset[RG_MAX_BSSID][6];
    uint16_t bset_n = 0;

    const uint32_t cutoff = (now_ms >= window_ms) ? (now_ms - window_ms) : 0u;

    for(size_t i = 0; i < n; i++) {
        const DefenderBeaconRec* r = &recs[i];
        if(r->ts_ms < cutoff) continue;

        // distinct BSSID set (AP count)
        bool bfound = false;
        for(uint16_t b = 0; b < bset_n; b++) {
            if(rg_mac_eq(bset[b], r->bssid)) {
                bfound = true;
                break;
            }
        }
        if(!bfound && bset_n < RG_MAX_BSSID) {
            memcpy(bset[bset_n++], r->bssid, 6);
        }

        // SSID table (ignore hidden/empty SSIDs)
        if(r->ssid[0] == '\0') continue;
        int si = -1;
        for(uint8_t s = 0; s < st_n; s++) {
            if(strncmp(st[s].ssid, r->ssid, ROGUE_SSID_MAX) == 0) {
                si = s;
                break;
            }
        }
        if(si < 0) {
            if(st_n >= RG_MAX_SSID) continue;
            si = st_n++;
            strncpy(st[si].ssid, r->ssid, ROGUE_SSID_MAX - 1);
            st[si].ssid[ROGUE_SSID_MAX - 1] = '\0';
            st[si].stored = 0;
            st[si].count = 0;
        }
        // record this BSSID against the SSID if not already seen
        bool seen = false;
        for(uint8_t k = 0; k < st[si].stored; k++) {
            if(rg_mac_eq(st[si].bssids[k], r->bssid)) {
                seen = true;
                break;
            }
        }
        if(!seen) {
            if(st[si].stored < RG_BSSID_PER_SSID) {
                memcpy(st[si].bssids[st[si].stored++], r->bssid, 6);
            }
            if(st[si].count < 0xFF) st[si].count++;
        }
    }

    out->ap_count = bset_n;
    out->distinct_ssids = st_n;

    // Evil twin: SSID advertised by the most (>= threshold) distinct BSSIDs.
    uint8_t best = 0;
    for(uint8_t s = 0; s < st_n; s++) {
        if(st[s].count >= ROGUE_EVILTWIN_MIN_BSSIDS && st[s].count > best) {
            best = st[s].count;
            out->eviltwin_alert = true;
            strncpy(out->eviltwin_ssid, st[s].ssid, ROGUE_SSID_MAX - 1);
            out->eviltwin_ssid[ROGUE_SSID_MAX - 1] = '\0';
            out->eviltwin_bssids = st[s].count;
        }
    }

    // Beacon flood: too many distinct SSIDs in the window.
    if(st_n >= ROGUE_FLOOD_SSID_THRESHOLD) {
        out->flood_alert = true;
    }
}
