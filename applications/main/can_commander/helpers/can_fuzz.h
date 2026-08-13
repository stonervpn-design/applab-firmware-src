#pragma once
#include "../can_frame.h"

// Pure CAN-frame fuzz mutation core. No IDF/furi deps -> host-testable.
// Every frame written to `out` is TWAI-valid: dlc in [0,8], id masked to
// CAN_ID_STD_MAX/CAN_ID_EXT_MAX per `extended`. Deterministic given the same
// (base, mode, seed, step).

typedef enum {
    CFZ_DATA_BITFLIP, // flip exactly one data bit (within dlc*8 bits)
    CFZ_DATA_RANDOM, // replace all dlc data bytes with PRNG output
    CFZ_ID_SWEEP, // sweep id across the valid range via step % (max+1)
    CFZ_DLC_SWEEP, // sweep dlc 0..8 via step % 9, randomize the new bytes
    CFZ_FULL_RANDOM, // randomize id (masked), dlc (0..8) and data bytes
} CanFuzzMode;

#define CFZ_MODE_COUNT 5

// Mutate `base` into `out` per `mode`, seeded by (seed, step) for
// deterministic, reproducible output. `id`/`dlc`/`data` start as a copy of
// `base`'s fields (extended/rtr always carried through unchanged) and are
// then mutated per-mode below. The result is always clamped TWAI-valid:
// dlc <= 8, id masked to CAN_ID_STD_MAX or CAN_ID_EXT_MAX per out->extended.
// If `base` or `out` is NULL, this is a no-op.
void can_fuzz_next(const CanFrame* base, CanFuzzMode mode, uint32_t seed, uint32_t step, CanFrame* out);
