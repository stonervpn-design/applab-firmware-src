#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 802.11 management-frame fuzz mutators (WF1 Task 1).
 *
 * Furi-free by design (host-testable in isolation): only libc headers.
 * Pure logic -- does NOT depend on wlan_hal.h; the scene layer feeds
 * `out`/`out_len` to wlan_hal_send_raw() itself. Mirrors the shape of
 * applications/main/nrf24/helpers/nrf24_fuzz.{c,h} (the nRF24 fuzzer),
 * scaled to variable-length 802.11 frames (24-byte header minimum, up
 * to WLAN_FUZZ_MAX bytes).
 *
 * Every mode is a deterministic pure function of
 * (base, base_len, mode, seed, step): same inputs -> same
 * (out, out_len) every time, so a scene can re-derive/replay any
 * fuzzed frame from just the 4 scalar inputs plus the base frame. */

typedef enum {
    WMF_BITFLIP, /* flip exactly one bit somewhere in the base frame */
    WMF_FIELD, /* corrupt one whole field (frame-control/duration/seqctl/first body byte) */
    WMF_IE_MANGLE, /* corrupt a single byte in the IE region (offset >= 24) */
    WMF_LENSWEEP, /* sweep out_len across [24, WLAN_FUZZ_MAX], padding with PRNG bytes */
    WMF_RANDOM, /* keep the 24-byte header, fill a PRNG-length random body */
} WlanFuzzMode;

#define WMF_MODE_COUNT 5

/* Effective TX ceiling. wlan_mgmt_fuzz_next() never writes past out[WLAN_FUZZ_MAX - 1]. */
#define WLAN_FUZZ_MAX 256

/* Produce the `step`-th mutated frame of `base` (length `base_len`) under
 * `mode`, deterministic given (base, base_len, mode, seed, step).
 *
 * Returns false, leaving out/out_len untouched, when: base is NULL, out is
 * NULL, out_len is NULL, or base_len < 24 (802.11 frames have a 24-byte
 * minimum MAC header). Otherwise writes the mutated frame to `out` (which
 * must point to a WLAN_FUZZ_MAX-byte buffer), sets `*out_len` to a value in
 * [24, WLAN_FUZZ_MAX], and returns true.
 *
 * See wlan_mgmt_fuzz.c's per-mode comments for the exact per-mode formulas. */
bool wlan_mgmt_fuzz_next(
    const uint8_t* base,
    uint16_t base_len,
    WlanFuzzMode mode,
    uint32_t seed,
    uint32_t step,
    uint8_t* out,
    uint16_t* out_len);
