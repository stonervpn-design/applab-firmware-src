/**
 * @file target_input.h
 * Abstract input driver interface — each target provides its own implementation.
 *
 * Waveshare boards:  CST816S touch gestures + BOOT button
 * LilyGo T-Embed:   Rotary encoder + key buttons
 */

#pragma once

#include <furi.h>
#include <input.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize target-specific input hardware.
 * Called once at input service startup.
 */
void target_input_init(void);

/**
 * Poll target-specific input hardware and publish events.
 * Called every INPUT_POLL_MS from the input service main loop.
 *
 * @param pubsub           Input event pubsub channel
 * @param sequence_counter Monotonic sequence counter (caller owns, callee increments)
 */
void target_input_poll(FuriPubSub* pubsub, uint32_t* sequence_counter);

/**
 * Enable/disable BMI270 tilt -> Left/Right (System settings toggle, default on).
 * Only meaningful on boards with a tilt driver (m5stick_s3); the declaration is
 * harmless where unused, and callers guard the call with BOARD_HAS_TILT_NAV.
 */
void target_input_set_tilt_enabled(bool enabled);

/** Current tilt-nav enabled state (for the settings toggle's initial value). */
bool target_input_get_tilt_enabled(void);

/**
 * Runtime suppression of tilt -> Left/Right nav WITHOUT touching the persisted
 * enable (tilt-to-wake stays active). The Sub-GHz Read / Read RAW capture views
 * set this true while a capture is live so an accidental roll can't open the
 * frequency menu mid-capture. No-op on boards without a tilt driver.
 */
void target_input_set_tilt_nav_suppressed(bool suppressed);

#ifdef __cplusplus
}
#endif
