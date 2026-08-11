#pragma once

// Private-only: 3-mode BLE GATT fuzz/DoS run engine. Drives the stimulus
// primitives added to ble_walk_hal (Task 3) against a locked target, on its
// own FuriThread, mirroring the Sub-GHz Protocol Fuzzer's worker shape
// (applications/main/subghz/helpers/fuzzer_worker.c: start/stop/pause/step/
// callback, per-iteration stimulus, index/counter, mark_last). Unreferenced
// until the Task 6 scene wires it up, so an unguarded-but-unreferenced object
// simply gc's out of a public link -- guarded anyway (whole file) because it
// consumes the guarded ble_walk_hal fuzz/DoS API surface, which does not
// exist in a PUBLIC_BUILD.
#ifndef PUBLIC_BUILD

#include "ble_walk_hal.h"
#include "fuzzer_mutate.h" // fuzz_core component: FuzzConfig/FuzzFrame/FuzzMode/FuzzStrategy

#include <stdint.h>
#include <stdbool.h>

typedef struct BleWalkFuzz BleWalkFuzz;

typedef enum {
    // Mode A: fuzzer_mutate_frame() sweep -> write_char[_norsp] against a
    // locked, already-connected target + characteristic handle.
    BleFuzzModeSweep,
    // Mode B: ATT/GATT stack-abuse -- MTU edge-value cycle, then a
    // write_char_long length sweep (FuzzStrategyLength) past a nominal size
    // with varied offsets.
    BleFuzzModeStack,
    // Mode C: connection-flood DoS against a locked target (owns the HAL's
    // single-connection state; no concurrent connected write).
    BleFuzzModeDos,
} BleFuzzMode;

/** Fired after each engine iteration (any mode) with the current index and
 * counter (mode-dependent meaning: frames sent for A/B, completed connect
 * cycles for C) so the view can update. */
typedef void (*BleWalkFuzzCallback)(void* context, uint32_t index, uint32_t counter);

BleWalkFuzz* ble_walk_fuzz_alloc(void);
void ble_walk_fuzz_free(BleWalkFuzz* instance);

/** Copies `device` (by value -- addr, addr_type, adv_data, ...) into the
 * engine as the locked target. Does not hold a pointer into caller storage.
 * Required before _start(); Sweep/Stack additionally require a char handle
 * (see ble_walk_fuzz_config()). */
bool ble_walk_fuzz_set_target(BleWalkFuzz* instance, const BleWalkDevice* device);

/**
 * Configures the run. Resets index/counter to 0.
 *
 * `char_handle`: target characteristic for Sweep/Stack (ignored by DoS).
 * `write_norsp`: true = Write Without Response (fire-and-forget, fastest);
 *                false = Write With Response (flow-controlled).
 * `rate_ms`: inter-iteration delay for Sweep/Stack. For DoS this value is
 *            reinterpreted as the flood's per-connection dwell in HAL units
 *            (hold_count = rate_ms / 20, per ble_walk_hal's "20ms/unit,
 *            0 == max churn" contract) -- the config surface this task's
 *            brief specifies has no separate hold_count parameter, and
 *            rate_ms is the closest existing knob to "how long to sit on
 *            each connection", so DoS reuses it rather than adding a new
 *            parameter to every mode's config call.
 * `count`: Sweep/Stack -- optional cap on frame count (0 = run the full
 *          fuzzer_mutate_count()/derived sweep; a smaller nonzero value
 *          stops early). DoS -- optional cap on completed connect cycles
 *          (0 = unbounded, run until _stop()).
 */
bool ble_walk_fuzz_config(
    BleWalkFuzz* instance,
    BleFuzzMode mode,
    FuzzConfig cfg,
    uint16_t char_handle,
    bool write_norsp,
    uint32_t rate_ms,
    uint32_t count);

/** Starts the engine thread. Refuses (returns false) if: no target locked;
 * Sweep/Stack with char_handle == 0; Sweep with no active HAL connection
 * (caller must ble_walk_hal_connect() before arming -- the engine never
 * connects for you in A/B); already running (any mode -- only one mode may
 * run at a time). */
bool ble_walk_fuzz_start(BleWalkFuzz* instance);

/** Stops (blocking join) and idles the stimulus. For DoS this also calls
 * ble_walk_hal_dos_flood_stop(). */
void ble_walk_fuzz_stop(BleWalkFuzz* instance);

/** Pauses/resumes in place (index/counter frozen while paused). */
void ble_walk_fuzz_pause(BleWalkFuzz* instance, bool paused);

/** Manually advances exactly one iteration. Valid when stopped, or
 * running-and-paused. Not supported for DoS (the flood is HAL-thread-driven
 * and has no single-cycle step primitive) -- returns false. */
bool ble_walk_fuzz_step(BleWalkFuzz* instance);

bool ble_walk_fuzz_running(BleWalkFuzz* instance);
uint32_t ble_walk_fuzz_index(BleWalkFuzz* instance);
uint32_t ble_walk_fuzz_counter(BleWalkFuzz* instance);
/** Total planned iterations for the current config (0 for DoS -- unbounded
 * unless a count cap was set). */
uint32_t ble_walk_fuzz_total(BleWalkFuzz* instance);

void ble_walk_fuzz_set_callback(BleWalkFuzz* instance, BleWalkFuzzCallback callback, void* context);

/** Writes a text report to `path`: the full target block (address formatted
 * from addr+addr_type, name, rssi, ble_adv_decode() flags/UUIDs/txpower/mfr
 * on the locked device's adv_data), the last stimulus payload sent, and
 * mode/strategy/seed/index/char_handle/negotiated MTU. False if no target is
 * locked, no iteration has run yet, the engine is running-and-unpaused (same
 * race guard as fuzzer_worker_mark_last -- pause or stop first), or the file
 * write failed. */
bool ble_walk_fuzz_mark_last(BleWalkFuzz* instance, const char* path);

#endif // PUBLIC_BUILD
