#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * Sub-GHz jamming detector: pure noise-floor EMA + sustained-energy state machine.
 *
 * No firmware dependencies; only C stdlib (<stdint.h>, <stdbool.h>, <math.h>).
 *
 * Behavior:
 * - Floor EMA tracks noise minima: fast down (50/50 when signal drops),
 *   slow up (99/1 when signal rises).
 * - Jam alert: RSSI >= (floor + JAM_DB_OVER) AND packet_decoded==false
 *   for >= JAM_SUSTAIN_MS.
 * - Reset: any signal drop below threshold, or any packet decoded, resets
 *   the over_since_ms timer.
 */

#define JAM_DB_OVER      18.0f   // dB above noise floor to trigger
#define JAM_SUSTAIN_MS   400u    // must sustain this long (ms)

/**
 * Jammer detector state.
 */
typedef struct {
    float    floor_ema;         // Exponential moving average of noise floor (dBm)
    bool     have_floor;        // Set after first sample
    uint32_t over_since_ms;     // 0 = not over threshold; else timestamp when over began
    bool     jamming;           // True when over-threshold sustained for JAM_SUSTAIN_MS
} DefenderJammerState;

/**
 * Feed one RSSI sample to the jammer detector.
 *
 * @param st             Jammer state (modified)
 * @param rssi           Current RSSI in dBm (e.g., -60.0f)
 * @param packet_decoded True if a valid packet decoded since last feed
 * @param now_ms         Current timestamp (ms); used to measure sustain duration
 */
void defender_jammer_feed(
    DefenderJammerState* st,
    float rssi,
    bool packet_decoded,
    uint32_t now_ms);
