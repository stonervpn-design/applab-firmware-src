#include "defender_alert.h"
#include "defender_logger.h"

#include <furi.h>
#include <furi_hal_light.h>
#include <furi_hal_speaker.h>
#include <notification/notification_messages.h>

/**
 * Initialize the alert bus.
 */
void defender_alert_init(DefenderAlertBus* bus, void* notifications) {
    furi_assert(bus);
    memset(bus, 0, sizeof(DefenderAlertBus));
    bus->notifications = notifications;
    bus->logger = NULL;
}

/**
 * Raise an alert: debounce check → if fire → LED/beep + ring + log.
 */
void defender_alert_raise(DefenderAlertBus* bus, const DefenderAlert* a) {
    furi_assert(bus);
    furi_assert(a);

    // Debounce check.
    if(!defender_debounce_check(&bus->debounce, a->cat, a->offender, a->ts_ms, DEFENDER_ALERT_STALE_MS)) {
        return;  // Suppressed by debounce.
    }

    // Push into ring buffer (newest at head, wrap around).
    bus->ring[bus->head] = *a;
    bus->head = (bus->head + 1) % DEFENDER_ALERT_RING;
    if(bus->count < DEFENDER_ALERT_RING) {
        bus->count++;
    }

    // Fire LED: red blink, 200ms on, 400ms off.
    furi_hal_light_blink_start(LightRed, 0xFF, 200, 400);
    bus->blink_ticks = 80;  // 8000ms stale window (100ms per tick) → stop blink when stale.

    // Fire beep: one tone, debounced.
    if(!furi_hal_speaker_is_mine() && furi_hal_speaker_acquire(50)) {
        furi_hal_speaker_start(880.0f, 0.5f);
        bus->beep_ticks = 2;  // Stopped in defender_alert_tick.
    }

    // Log alert (guarded for T2 before logger exists).
    if(bus->logger) {
        defender_logger_log_alert(bus->logger, a);
    }
}

/**
 * Tick handler: countdown beep/blink, stop speaker/LED when done.
 */
void defender_alert_tick(DefenderAlertBus* bus) {
    furi_assert(bus);

    if(bus->beep_ticks > 0 && --bus->beep_ticks == 0) {
        if(furi_hal_speaker_is_mine()) {
            furi_hal_speaker_stop();
            furi_hal_speaker_release();
        }
    }

    if(bus->blink_ticks > 0 && --bus->blink_ticks == 0) {
        furi_hal_light_blink_stop();
    }
}

/**
 * Feed: return newest-first view of alerts.
 *
 * out[] = pointer to start of ring (oldest), count = number of alerts.
 * Caller walks newest-first by:
 *   - Start at index (head - 1 + count) % count
 *   - Decrement, wrap
 *
 * If no alerts: *out = NULL, return 0.
 */
uint8_t defender_alert_feed(DefenderAlertBus* bus, const DefenderAlert** out) {
    furi_assert(bus);
    furi_assert(out);

    if(bus->count == 0) {
        *out = NULL;
        return 0;
    }

    *out = bus->ring;
    return bus->count;
}
