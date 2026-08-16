#pragma once

/**
 * KeeLoq "Instant Unlock" next-code SYNTHESIS.
 *
 * Given a fob identity already recovered by the (in-tree) KeeLoq decoder
 * from a single received hop frame -- manufacturer key, learning type,
 * serial, button, and the counter that was in that frame -- this produces
 * the fix/hop pair for an ARBITRARY counter (typically last_seen+1 or +2),
 * i.e. a code the car's rolling-code receiver will accept as "slightly
 * ahead" without desyncing the real fob.
 *
 * This is a PURE function: no furi runtime, no radio I/O, no keystore
 * lookup. It reuses the exact crypto primitives from
 * lib/subghz/protocols/keeloq_common.c (subghz_protocol_keeloq_common_encrypt
 * / _normal_learning) via local ABI-compatible prototypes so this file has
 * zero furi/m-array include dependency and can be compiled standalone for
 * both the ESP32 firmware and a plain host binary (see
 * tools/keeloq_instant_host_test/).
 *
 * The fix/hop bit layout and per-learning-type device-key derivation are
 * copied verbatim from the encoder's generic (no manufacturer-name
 * override) path in lib/subghz/protocols/keeloq.c so a frame built here is
 * indistinguishable, to both a real receiver and this codebase's own
 * decoder, from one the encoder would have produced for the same
 * serial/btn/counter. See keeloq_instant.c for the exact line references.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Learning-type codes accepted by keeloq_instant_synthesize(). Numerically
 * identical to KEELOQ_LEARNING_SIMPLE / KEELOQ_LEARNING_NORMAL in
 * lib/subghz/protocols/keeloq_common.h -- callers already holding a
 * SubGhzKeyArray_t manufacture_code->type value from the keystore can pass
 * it straight through with no translation. Duplicated here (rather than
 * including keeloq_common.h) so this header stays furi-free.
 */
#define KEELOQ_INSTANT_LEARNING_SIMPLE 1u
#define KEELOQ_INSTANT_LEARNING_NORMAL 2u

/**
 * Synthesize the fix/hop pair for (serial, btn, counter) under the given
 * manufacturer key and learning type.
 *
 * @param mf_key        manufacturer key from the keystore (64-bit)
 * @param learning_type KEELOQ_INSTANT_LEARNING_* (== keystore's KEELOQ_LEARNING_*)
 * @param serial        28-bit fob serial (only the low 28 bits are used)
 * @param btn            4-bit button code (only the low 4 bits are used)
 * @param counter       target rolling counter to synthesize for
 * @param out_fix       receives fix = (btn<<28) | (serial & 0x0FFFFFFF)
 * @param out_hop       receives the encrypted hop code for `counter`
 * @return true if `learning_type` is supported and out_fix/out_hop were
 *         written; false (outputs untouched) for an unsupported/unknown
 *         learning type, so the caller can report "unsupported" instead of
 *         transmitting a bogus frame.
 */
bool keeloq_instant_synthesize(
    uint64_t mf_key,
    uint8_t learning_type,
    uint32_t serial,
    uint8_t btn,
    uint16_t counter,
    uint32_t* out_fix,
    uint32_t* out_hop);

#ifdef __cplusplus
}
#endif
