#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/**
 * Defender alert categories.
 */
typedef enum {
    DefenderCatWifiDeauth,
    DefenderCatWifiRogueAp,
    DefenderCatWifiBeaconFlood,
    DefenderCatWifiKarma,
    DefenderCatWifiHwFp,
    DefenderCatBleTracker,
    DefenderCatBleSpam,
    DefenderCatSubghzJam,
    DefenderCatRf24Occ,
    DefenderCatDrone,
    DefenderCatCount,
} DefenderCategory;

/**
 * Single alert record.
 */
typedef struct {
    DefenderCategory cat;
    uint8_t severity;      // 0=info, 1=warn, 2=critical
    uint8_t offender[6];   // BSSID / BLE MAC; all-zero if n/a
    int8_t rssi;
    uint32_t ts_ms;
    char msg[48];
} DefenderAlert;

/**
 * Debounce configuration.
 */
#define DEFENDER_DEBOUNCE_MAX 16
#define DEFENDER_ALERT_RING 16
#define DEFENDER_ALERT_STALE_MS 8000u

/**
 * Per-(category, offender) debounce entry.
 */
typedef struct {
    uint8_t cat;
    uint8_t offender[6];
    uint32_t last_fire_ms;
    bool used;
} DefenderDebounceEntry;

/**
 * Debounce set: tracks which (cat, offender) pairs have fired recently.
 */
typedef struct {
    DefenderDebounceEntry e[DEFENDER_DEBOUNCE_MAX];
} DefenderDebounceSet;

/**
 * Pure debounce check: returns true iff (cat, offender) should fire now.
 *
 * On first sight: fires and records the entry.
 * On repeat within stale_ms: suppressed (returns false).
 * On repeat after stale_ms: re-arms (fires again).
 * On table full: LRU evicts the oldest entry.
 *
 * PURE (no side effects except the debounce table).
 */
static inline bool defender_debounce_check(
    DefenderDebounceSet* s,
    uint8_t cat,
    const uint8_t off[6],
    uint32_t now_ms,
    uint32_t stale_ms) {

    // Linear scan for existing (cat, offender) entry.
    int found_idx = -1;
    int oldest_idx = -1;
    int oldest_unused_idx = -1;
    uint32_t oldest_time = UINT32_MAX;

    for(int i = 0; i < DEFENDER_DEBOUNCE_MAX; i++) {
        if(s->e[i].used && s->e[i].cat == cat && memcmp(s->e[i].offender, off, 6) == 0) {
            found_idx = i;
            break;
        }
        if(!s->e[i].used && oldest_unused_idx == -1) {
            oldest_unused_idx = i;  // Remember first unused slot.
        } else if(s->e[i].used && s->e[i].last_fire_ms < oldest_time) {
            oldest_time = s->e[i].last_fire_ms;
            oldest_idx = i;  // Track LRU slot.
        }
    }

    if(found_idx >= 0) {
        // Entry exists: check if stale.
        uint32_t elapsed = now_ms - s->e[found_idx].last_fire_ms;
        if(elapsed >= stale_ms) {
            // Re-arm: entry is stale.
            s->e[found_idx].last_fire_ms = now_ms;
            return true;
        } else {
            // Suppress: entry is still live.
            return false;
        }
    } else {
        // New entry: use first unused slot if available, else evict LRU.
        if(oldest_unused_idx != -1) {
            oldest_idx = oldest_unused_idx;
        } else if(oldest_idx == -1) {
            oldest_idx = 0;  // Fallback (shouldn't happen if DEBOUNCE_MAX > 0).
        }
        s->e[oldest_idx].cat = cat;
        memcpy(s->e[oldest_idx].offender, off, 6);
        s->e[oldest_idx].last_fire_ms = now_ms;
        s->e[oldest_idx].used = true;
        return true;
    }
}

/**
 * Human-readable category label.
 */
static inline const char* defender_cat_label(DefenderCategory c) {
    static const char* labels[DefenderCatCount] = {
        "WIFI/DEAUTH",
        "WIFI/ROGUE_AP",
        "WIFI/BEACON_FLOOD",
        "WIFI/KARMA",
        "WIFI/HW_FP",
        "BLE/TRACKER",
        "BLE/SPAM",
        "SUBGHZ/JAM",
        "RF24/OCC",
        "DRONE",
    };
    if(c >= DefenderCatCount) return "UNKNOWN";
    return labels[c];
}

/**
 * Alert bus (firmware): manages debounce, LED/beep, and ring buffer.
 * Wired into the main app by the integrator.
 */
typedef struct {
    DefenderAlert ring[DEFENDER_ALERT_RING];
    uint8_t head;
    uint8_t count;
    DefenderDebounceSet debounce;
    uint8_t beep_ticks;
    uint8_t blink_ticks;  // Countdown to stop LED blink (100ms per tick)
    void* notifications;  // NotificationApp*
    void* logger;         // DefenderLogger* (NULL until T3)
} DefenderAlertBus;

/**
 * Initialize the alert bus.
 */
void defender_alert_init(DefenderAlertBus* bus, void* notifications);

/**
 * Raise an alert: debounce, LED/beep, ring buffer, log.
 */
void defender_alert_raise(DefenderAlertBus* bus, const DefenderAlert* a);

/**
 * Tick: countdown beep_ticks, stop speaker when 0.
 */
void defender_alert_tick(DefenderAlertBus* bus);

/**
 * Feed: expose alert ring in newest-first order (for UI).
 * Returns count; out[] points to alerts (or NULL if none).
 */
uint8_t defender_alert_feed(DefenderAlertBus* bus, const DefenderAlert** out);
