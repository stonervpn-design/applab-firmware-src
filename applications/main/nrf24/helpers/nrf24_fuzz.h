#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* nRF24 packet fuzzer mutators (Task 7 of the nRF24 Red-Team Suite).
 *
 * Furi-free by design (host-testable in isolation): only libc headers.
 * Pure logic -- does NOT depend on nrf24_hw.h; the Task-8 scene feeds
 * `out`/`out_len` to nrf24_tx_raw() itself. Mirrors the shape of
 * components/fuzz_core/fuzzer_mutate.{c,h} (subghz's fuzzer), scaled
 * down to a single flat byte-buffer payload model (no protocol-bit /
 * raw-duration split, since nRF24 payloads are already flat bytes).
 *
 * Every mode is a deterministic pure function of
 * (base, base_len, mode, seed, step): same inputs -> same
 * (out, out_len) every time, so a scene can re-derive/replay any
 * fuzzed frame from just the 4 scalar inputs plus the base payload. */

typedef enum {
    N24F_BITFLIP, /* flip exactly one bit, walking the frame as step increases */
    N24F_LENSWEEP, /* sweep out_len 1..32, content = base repeated/truncated */
    N24F_FIELD, /* cycle one byte through all 256 values, then move to the next byte */
    N24F_RANDOM, /* out_len = base_len, content = seeded PRNG stream */
} Nrf24FuzzMode;

/* Produce the `step`-th mutated frame of `base` (length `base_len`) under
 * `mode`, deterministic given (base, base_len, mode, seed, step).
 *
 * - base_len is clamped to [1, 32] (0 or >32 both clamp to 1..32 by
 *   truncating/treating out-of-range as invalid input; see .c for the
 *   exact clamp). base may be NULL only when mode == N24F_RANDOM (random
 *   fill never reads base).
 * - out must point to a 32-byte buffer; out_len must be non-NULL, except
 *   that NULL out or NULL out_len makes this call a safe no-op (nothing
 *   is written, nothing is read besides the NULL checks).
 * - See nrf24_fuzz.c's top-of-function-per-mode comments for the exact
 *   per-mode formulas (bit index, byte/value cycling, PRNG seeding). */
void nrf24_fuzz_next(
    const uint8_t* base,
    uint8_t base_len,
    Nrf24FuzzMode mode,
    uint32_t seed,
    uint32_t step,
    uint8_t out[32],
    uint8_t* out_len);
