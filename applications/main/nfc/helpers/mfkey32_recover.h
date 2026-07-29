#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Optional cancellation callback for a long recovery. Return true to abort.
 * Pass NULL to run to completion. (Mirrors Crapto1CancelFn.)
 */
typedef bool (*Mfkey32CancelFn)(void* context);

/**
 * Recover a MIFARE Classic sector key from two captured "mfkey32" auth
 * attempts that used the SAME key (the standard reader attack).
 *
 * The tuple mirrors exactly what `mfkey32_logger` stores per param set:
 *   uid  = cuid, and for each of the two auths the cleartext tag nonce `nt`,
 *   the encrypted reader nonce `nr_enc` and the encrypted reader answer
 *   `ar_enc`.
 *
 * @param[in]  uid      card UID / cuid
 * @param[in]  nt0      tag nonce, auth 0 (cleartext)
 * @param[in]  nr0_enc  encrypted reader nonce, auth 0
 * @param[in]  ar0_enc  encrypted reader answer, auth 0
 * @param[in]  nt1      tag nonce, auth 1 (cleartext)
 * @param[in]  nr1_enc  encrypted reader nonce, auth 1
 * @param[in]  ar1_enc  encrypted reader answer, auth 1
 * @param[out] out_key  recovered 48-bit key (MSB-first in the low 6 bytes)
 * @return true if a key was recovered, false otherwise.
 */
bool mfkey32_recover(
    uint32_t uid,
    uint32_t nt0,
    uint32_t nr0_enc,
    uint32_t ar0_enc,
    uint32_t nt1,
    uint32_t nr1_enc,
    uint32_t ar1_enc,
    uint64_t* out_key);

/** As mfkey32_recover(), but abortable via a cancellation callback. */
bool mfkey32_recover_cancellable(
    uint32_t uid,
    uint32_t nt0,
    uint32_t nr0_enc,
    uint32_t ar0_enc,
    uint32_t nt1,
    uint32_t nr1_enc,
    uint32_t ar1_enc,
    uint64_t* out_key,
    Mfkey32CancelFn cancel,
    void* cancel_ctx);

/**
 * Correctness self-test: runs mfkey32_recover() on a published mfkey32 test
 * vector (uid 0x2a234f80 + two auths -> key 0xA0A1A2A3A4A5, from the
 * FlipperMfkey `standard_tests/a0a1a2a3a4a5_only.log` data set) and returns
 * true only if the expected key is recovered. Used to prove the recovery core
 * before it is trusted. Heavy (~115 KB heap, up to ~1 min) - not for hot paths.
 */
bool mfkey32_recover_selftest(void);

#ifdef __cplusplus
}
#endif
