/*
 * crapto1.h — MIFARE Classic crypto1 key recovery (mfkey32 core)
 *
 * Vendored from the Flipper Zero "Mfkey32" application:
 *     https://github.com/noproto/FlipperMfkey  (fap/mfkey32.c)
 * which is itself derived from the crapto1 / Craptev1 library by bla and the
 * nfc-tools / Proxmark project. Licensed under the GNU General Public License
 * version 3 (see the LICENSE file in that repository).
 *
 * Only the RAM-bounded recovery core has been extracted; the GUI, storage and
 * dictionary code was removed and progress/cancellation is exposed through a
 * callback. The recovery mathematics are unchanged from the upstream source.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Optional cancellation callback. Return true to abort the (potentially long)
 * recovery. Called periodically during state enumeration. Pass NULL to run to
 * completion.
 */
typedef bool (*Crapto1CancelFn)(void* context);

/**
 * Parameters for a single mfkey32 (two-auth, same-key) recovery.
 *
 * uid_xor_nt0 = uid ^ nt0, uid_xor_nt1 = uid ^ nt1,
 * p64b        = crapto1_prng_successor(nt1, 64).
 */
typedef struct {
    uint64_t key; //!< [out] recovered 48-bit key on success
    uint32_t nr0_enc;
    uint32_t uid_xor_nt0;
    uint32_t uid_xor_nt1;
    uint32_t nr1_enc;
    uint32_t p64b;
    uint32_t ar1_enc;
} Crapto1Params;

/** crypto1 weak-PRNG successor: advance the tag-nonce LFSR by n steps. */
uint32_t crapto1_prng_successor(uint32_t x, uint32_t n);

/**
 * Core mfkey32 recovery.
 *
 * Enumerate the crypto1 LFSR states consistent with the first auth's keystream
 *   ks2 = ar0_enc ^ crapto1_prng_successor(nt0, 64)
 * (MSB-chunked, peak heap ~115 KB — suitable for MCUs without PSRAM), roll each
 * candidate back past nr0_enc and (uid ^ nt0) to obtain the key state, and keep
 * only the state that also reproduces the SECOND auth's ar1_enc. On a unique
 * match the 48-bit key is written to p->key.
 *
 * @return true if a key was recovered (p->key valid), false otherwise (no match,
 *         cancelled, or out of memory).
 */
bool crapto1_recover(Crapto1Params* p, uint32_t ks2, Crapto1CancelFn cancel, void* cancel_ctx);

#ifdef __cplusplus
}
#endif
