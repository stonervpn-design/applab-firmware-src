#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * WiFi deauth/disassoc flood detector (pure evaluator).
 * No ESP or Furi dependencies — suitable for host testing.
 */

#define DEAUTH_RATE_ALERT 20u
#define DEAUTH_WINDOW_MS 1000u
#define DEAUTH_BROADCAST_WEIGHT 2u

/**
 * A captured deauth/disassoc record.
 */
typedef struct {
    uint8_t bssid[6];       /* Target BSSID (address 3 from frame) */
    bool broadcast;         /* True if this was a broadcast deauth/disassoc */
    int8_t rssi;            /* Signal strength at time of capture */
    uint32_t ts_ms;         /* Timestamp in milliseconds */
} DefenderDeauthRec;

/**
 * Result of evaluating deauth/disassoc rate in a sliding window.
 */
typedef struct {
    bool alert;             /* True if weighted count >= rate_alert threshold */
    uint32_t weighted_count; /* Total count in window; broadcast weighted DEAUTH_BROADCAST_WEIGHT */
    uint8_t top_bssid[6];   /* BSSID of the top offender in the window */
    uint16_t top_count;     /* Raw (unweighted) count for the top offender */
    int8_t top_rssi;        /* RSSI of the top offender (from last record seen) */
} DefenderDeauthResult;

/**
 * Evaluate deauth/disassoc rate in a sliding time window.
 *
 * Scans records, skips any with timestamp older than (now_ms - window_ms),
 * accumulates weighted_count (broadcast counts DEAUTH_BROADCAST_WEIGHT),
 * tracks per-BSSID counts to find the top offender.
 *
 * @param recs        Array of deauth records
 * @param n           Number of records
 * @param now_ms      Current time in milliseconds (reference point for window)
 * @param window_ms   Window size in milliseconds
 * @param rate_alert  Threshold for alert (weighted count >= this triggers alert)
 * @param out         Output result (zeroed on entry, filled on return)
 */
void defender_deauth_eval(
    const DefenderDeauthRec* recs,
    size_t n,
    uint32_t now_ms,
    uint32_t window_ms,
    uint32_t rate_alert,
    DefenderDeauthResult* out);

#ifdef __cplusplus
}
#endif
