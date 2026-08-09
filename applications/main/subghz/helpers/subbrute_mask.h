#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifndef PUBLIC_BUILD

#include "subghz_chip_id.h" // SubGhzChipFamily

// Function-mask brute (PRIVATE-only): enumerate only the UNKNOWN bits of a
// fixed-code frame, holding the known bits fixed from a capture + chip-ID. This
// collapses a 2^N full sweep to 2^k over the k masked bits. Pure + furi-free so
// it is host-testable (tools/subbrute_mask/mask_replica.py).
// See docs/superpowers/specs/2026-08-09-function-mask-brute-design.md.

/** Number of variable (masked) bits = popcount(var_mask). */
uint8_t subbrute_mask_var_bits(uint64_t var_mask);

/** Total candidates to enumerate = 2^var_bits (binary v1). Returns 0 for a
 * degenerate mask (0 bits) or one so wide it would overflow uint64_t (>63 bits) —
 * the caller must reject a 0 result rather than sweep. */
uint64_t subbrute_mask_max_value(uint64_t var_mask);

/** Candidate #step: scatter the low var_bits of `step` into the set positions of
 * `var_mask` (LSB of step -> lowest set mask bit), keeping every non-masked bit
 * from `fixed`:  data = (fixed & ~var_mask) | pdep(step, var_mask).
 * step 0 -> fixed bits only; step (2^k - 1) -> fixed | var_mask. */
uint64_t subbrute_mask_candidate(uint64_t fixed, uint64_t var_mask, uint64_t step);

/** Default variable-bit mask for a chip-ID (P1) family: the button/function
 * field, held at the low bits of the frame (address/serial bits stay fixed from
 * the capture). Princeton/Came/Nice/Holtek/Ansonic -> low 4 (data nibble);
 * Chamberlain/Linear/LinearDelta -> low 2 (DIP/button); Unknown -> low 4. Never
 * exceeds `bits`. This is the one-button-press auto-seed; the mask editor (Phase
 * 4) lets the operator refine it. */
uint64_t subbrute_mask_default_for_chip(SubGhzChipFamily family, int32_t bits);

#endif // PUBLIC_BUILD
