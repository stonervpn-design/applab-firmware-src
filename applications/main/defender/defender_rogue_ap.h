#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Rogue-AP detector (pure evaluator) — passive blue-team analysis of observed
 * beacon frames. No ESP/Furi deps, so it is host-testable.
 *
 * Detects two attacks from beacon observations alone:
 *   - Evil twin: the same SSID advertised by two or more distinct BSSIDs (a
 *     rogue AP impersonating a legitimate network).
 *   - Beacon flood: an abnormal number of distinct SSIDs in the window
 *     (mdk3/beacon-flood style noise), optionally concentrated on one source.
 */

#define ROGUE_SSID_MAX 33 /* 32 SSID chars + NUL */
#define ROGUE_EVILTWIN_MIN_BSSIDS 2u /* same SSID from >= this many BSSIDs -> evil twin */
#define ROGUE_FLOOD_SSID_THRESHOLD 15u /* distinct SSIDs in window -> beacon flood */

typedef struct {
    char ssid[ROGUE_SSID_MAX]; /* NUL-terminated; empty = hidden, ignored for SSID logic */
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi;
    uint32_t ts_ms;
} DefenderBeaconRec;

typedef struct {
    bool eviltwin_alert;
    char eviltwin_ssid[ROGUE_SSID_MAX]; /* the impersonated SSID (most BSSIDs) */
    uint8_t eviltwin_bssids; /* how many distinct BSSIDs advertised it */

    bool flood_alert;
    uint16_t distinct_ssids; /* distinct non-hidden SSIDs seen in the window */

    uint16_t ap_count; /* distinct BSSIDs seen in the window */
} DefenderRogueResult;

/**
 * Evaluate beacon records in a sliding time window.
 *
 * @param recs       Array of beacon records
 * @param n          Number of records
 * @param now_ms     Current time (window reference)
 * @param window_ms  Window size in ms (records older than now-window are skipped)
 * @param out        Output (zeroed on entry)
 */
void defender_rogue_eval(
    const DefenderBeaconRec* recs,
    size_t n,
    uint32_t now_ms,
    uint32_t window_ms,
    DefenderRogueResult* out);

#ifdef __cplusplus
}
#endif
