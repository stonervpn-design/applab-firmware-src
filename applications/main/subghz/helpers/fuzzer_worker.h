#pragma once

#include "fuzzer_mutate.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief TX worker for the Sub-GHz Protocol Fuzzer.
 *
 * Cloned from SubBruteWorker's thread skeleton (subbrute_worker.c/.h): a
 * FuriThread drives a sweep, each tick mutating the seed via fuzzer_mutate_frame(),
 * rendering it via fuzzer_frame_build(), and transmitting it through the same
 * deserialize -> subghz_devices_start_async_tx() sequence used by SubBrute and
 * the stock Transmitter. Private-only feature; this file has no PUBLIC_BUILD
 * guard because it is only referenced by guarded scenes further up the stack,
 * so an unreferenced object simply gc's out of a public link.
 */
typedef struct FuzzerWorker FuzzerWorker;

/** Fired after each transmit attempt (success or failure) with the index that
 * was just attempted and the total frame count for the current sweep. */
typedef void (*FuzzerWorkerCallback)(void* context, uint32_t index, uint32_t total);

/** Allocates a FuzzerWorker bound to the internal CC1101. */
FuzzerWorker* fuzzer_worker_alloc(void);

/** Stops (if running) and frees a FuzzerWorker instance. */
void fuzzer_worker_free(FuzzerWorker* instance);

/**
 * @brief Configure the sweep. Resets index to 0.
 *
 * Input-safety clamp (trust boundary): fuzzer_mutate_frame() writes fixed-size
 * FUZZ_MAX_BYTES/FUZZ_MAX_DUR arrays without its own bounds checks (Tasks 1/3
 * gap), so this call clamps cfg->seed_nbits and cfg->field_off+cfg->field_len to
 * <= FUZZ_MAX_BYTES*8, and cfg->seed_ndur to <= FUZZ_MAX_DUR before storing.
 *
 * @param preset Full FuriHalSubGhzPreset enum name, e.g.
 *               "FuriHalSubGhzPresetOok650Async" (matches
 *               subbrute_protocol_convert_preset()'s expected input).
 */
void fuzzer_worker_set_config(
    FuzzerWorker* instance,
    const FuzzConfig* cfg,
    const char* protocol,
    uint32_t freq,
    const char* preset,
    uint32_t te,
    uint8_t repeat);

/** Starts the auto-sweep thread. Returns false if already running, unconfigured
 * (0 frames), or the configured frequency fails the SubGhz TX frequency check. */
bool fuzzer_worker_start(FuzzerWorker* instance);

/** Stops the auto-sweep thread (blocking join) and idles the radio. */
void fuzzer_worker_stop(FuzzerWorker* instance);

/** Pauses/resumes the auto-sweep thread in place (radio left idle while
 * paused; index does not advance). Does not stop the thread. */
void fuzzer_worker_pause(FuzzerWorker* instance, bool paused);

/** Manually transmits exactly one frame (the one at the current index) and
 * advances the index. Valid when the worker is stopped, or running-and-paused.
 * Returns false if auto-running-and-unpaused, the sweep is already exhausted,
 * or the transmit itself failed (bad mutate/build/frequency). */
bool fuzzer_worker_step(FuzzerWorker* instance);

bool fuzzer_worker_running(FuzzerWorker* instance);
uint32_t fuzzer_worker_index(FuzzerWorker* instance);

void fuzzer_worker_set_callback(
    FuzzerWorker* instance,
    FuzzerWorkerCallback callback,
    void* context);

/** Writes the last transmitted frame to `path` as a .sub file. False if no
 * frame has been sent yet, or the write failed. Only succeeds when the worker
 * is stopped or running-and-paused — pause() or stop() before calling this,
 * otherwise it refuses (the sweep thread mutates the last-frame buffers every
 * tick while auto-running-and-unpaused, so marking mid-sweep would race). */
bool fuzzer_worker_mark_last(FuzzerWorker* instance, const char* path);
