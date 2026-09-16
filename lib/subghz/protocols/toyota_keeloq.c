#include "toyota_keeloq.h"
#include "protocols_common.h"

// KeeLoq manufacturer-key recovery (try-all-keys) support. Mirrors keeloq.c:
// iterate the resident keystore, apply each key's learning/derivation, decrypt
// the 32-bit hop, and accept only a self-consistent decrypt (button + serial-low
// invariant). See subghz_protocol_decoder_toyota_keeloq_try_recover().
#include "keeloq_common.h"
#include "../subghz_keystore.h"
#include "../subghz_keystore_i.h"
#include "../environment.h"
#include <m-array.h>

#include <string.h>

#define TAG "ToyotaKeeloq"

// ---- Timing constants (from KAT src/protocols/toyota.rs) ----
// Variant select: the width of the very first HIGH pulse of the burst.
// A's short leg (400us) and B's short leg (200us) straddle this threshold.
#define TOYOTA_KEELOQ_VARIANT_SELECT_US 310U

// -- Variant A: Corolla, 433.92MHz, fixed-width PWM pairs --
// LS pair (long HIGH + short LOW) = bit 0; SL pair (short HIGH + long LOW) =
// bit 1. Preamble is >=6 SS (short HIGH + short LOW) pairs; the first pair
// that isn't SS-shaped both ends the preamble AND is itself the first data
// bit (LS or SL). A run of 68 such pairs (bits) makes one frame.
static const SubGhzBlockConst subghz_protocol_toyota_keeloq_const_a = {
    .te_short = 400,
    .te_long = 800,
    .te_delta = 175,
    .min_count_bit_for_found = 68,
};
#define TOYOTA_KEELOQ_A_MIN_PREAMBLE_PAIRS 6U
#define TOYOTA_KEELOQ_A_BITS               68U

// -- Variant B: Tundra/Tacoma, 315MHz, NRZ --
// Preamble is short-HIGH (200us) + long-LOW (390us) pairs, used purely for
// clock lock. Once a pair breaks that shape, NRZ data begins: from that
// point on every single pulse (either level) directly encodes one bit,
// classified against the 287us midpoint (long >=287 -> one bit value, short
// -> the other) rather than a near-nominal match. A run of 67 such pulses
// (bits) makes one frame; anything that fails the classify envelope (e.g.
// the ~1500-2600us inter-repeat sync gap) resets the parser, which doubles
// as the "sync-gap terminates" behavior from the source spec.
static const SubGhzBlockConst subghz_protocol_toyota_keeloq_const_b = {
    .te_short = 200,
    .te_long = 390,
    .te_delta = 80,
    .min_count_bit_for_found = 67,
};
#define TOYOTA_KEELOQ_B_MIDPOINT_US 287U
#define TOYOTA_KEELOQ_B_MIN_PULSE_US 100U
#define TOYOTA_KEELOQ_B_MAX_PULSE_US 600U
#define TOYOTA_KEELOQ_B_BITS          67U
// Variant B (Tundra/NRZ): require a real preamble run AND end it on the
// documented sync-gap LOW (~1500-2600us), NOT by inferring the boundary from
// pulse shape -- a genuine data pair (short,long) is indistinguishable from a
// preamble pair, so a shape-only break can start the 67-bit window at the wrong
// offset and yield a plausible-but-WRONG decode. Gating on the sync gap makes
// the parser fail-safe: it declines ambiguous bursts instead of mis-decoding.
#define TOYOTA_KEELOQ_B_MIN_PREAMBLE_PAIRS 6U
#define TOYOTA_KEELOQ_B_SYNC_MIN_US        1500U
#define TOYOTA_KEELOQ_B_SYNC_MAX_US        2600U

// Frame payload (both variants, once framed): hop(32) | serial(28) | button(4)
// = 64 bits total, packed as (hop<<32)|(serial<<4)|button. The on-air data
// window is wider than 64 bits (68 for A / 67 for B); the extra leading bits
// are treated as header/framing padding ahead of the payload and are
// discarded naturally: shifting bits into a 64-bit accumulator for the full
// on-air bit count keeps exactly the *last* 64 bits shifted in, which is
// defined here to be the payload. This is a documented assumption (no
// per-manufacturer key/spec is resident to verify bit-exact framing) -- see
// plan doc + host-test real-capture results.
#define TOYOTA_KEELOQ_SERIAL_MASK 0x0FFFFFFFUL // 28 bits
#define TOYOTA_KEELOQ_BUTTON_MASK 0xFUL // 4 bits

typedef enum {
    ToyotaKeeloqVariantNone = 0,
    ToyotaKeeloqVariantA = 1,
    ToyotaKeeloqVariantB = 2,
} ToyotaKeeloqVariant;

typedef struct SubGhzProtocolDecoderToyotaKeeloq {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    ToyotaKeeloqVariant variant;
    uint16_t preamble_count;
    uint32_t first_high;
    bool in_data;

    uint64_t data;
    uint8_t bit_count;

    ToyotaKeeloqVariant last_variant; // variant that produced the last decoded frame
    uint32_t hop; // 32-bit KeeLoq hop -- ENCRYPTED (decrypted only if a keystore key matches)
    uint32_t serial; // 28-bit
    uint8_t button; // 4-bit

    // ---- Manufacturer-key recovery (keystore try-all-keys attack) ----
    // Populated by toyota_keeloq_try_recover() on each decoded frame. The decode
    // path NEVER depends on these: a frame always decodes+displays even when no
    // resident key matches (recovery just annotates it).
    SubGhzKeystore* keystore; // resident keystore (from environment), may be NULL

    uint8_t recovery_state; // ToyotaKeeloqRecovery* (none/candidate/confirmed)
    const char* recovered_name; // keystore entry name that matched (static string)
    uint64_t recovered_man; // effective (derived) 64-bit manufacturer key
    uint32_t recovered_plain; // decrypted plaintext 0xBSSSCCCC
    uint16_t recovered_cnt; // decoded 16-bit rolling counter
    uint8_t recovered_cipher; // ToyotaKeeloqCipher* (standard vs AERF nl_extend)

    // Two-frame CONFIRM tracking: a single ~12-bit invariant can false-match, so
    // a match is only upgraded to CONFIRMED when a later frame decrypts under the
    // SAME key+serial to a forward counter. Persists across parser resets.
    bool has_last_recovery;
    uint64_t last_man;
    uint32_t last_serial;
    uint16_t last_cnt;
} SubGhzProtocolDecoderToyotaKeeloq;

typedef enum {
    ToyotaKeeloqRecoveryNone = 0, // no resident key decrypts this hop (replay-only)
    ToyotaKeeloqRecoveryCandidate = 1, // one frame decrypts consistently (may false-match)
    ToyotaKeeloqRecoveryConfirmed = 2, // 2nd frame confirmed counter+ under same key
} ToyotaKeeloqRecoveryState;

typedef enum {
    ToyotaKeeloqCipherStandard = 0, // subghz_protocol_keeloq_common_encrypt/decrypt
    ToyotaKeeloqCipherDerived240 = 1, // AERF nl_extend, outer_limit 0x240
    ToyotaKeeloqCipherDerived210 = 2, // AERF nl_extend, outer_limit 0x210
} ToyotaKeeloqCipher;

typedef enum {
    ToyotaKeeloqDecoderStepReset = 0,
    ToyotaKeeloqDecoderStepAAwaitHigh,
    ToyotaKeeloqDecoderStepAAwaitLow,
    ToyotaKeeloqDecoderStepBAwaitHigh,
    ToyotaKeeloqDecoderStepBAwaitLow,
    ToyotaKeeloqDecoderStepBData,
} ToyotaKeeloqDecoderStep;

static const char* toyota_keeloq_button_name(uint8_t button) {
    // Button-code mapping is a best-effort convention (matches the other
    // KeeLoq-hop RKE decoders in this tree) -- NOT independently confirmed
    // against a real Toyota fob keymap. The raw 4-bit code is always shown
    // alongside this name in get_string().
    switch(button) {
    case 0x1:
        return "Lock";
    case 0x2:
        return "Unlock";
    case 0x4:
        return "Trunk";
    case 0x8:
        return "Panic";
    default:
        return "Unknown";
    }
}

static const char* toyota_keeloq_variant_name(ToyotaKeeloqVariant variant) {
    switch(variant) {
    case ToyotaKeeloqVariantA:
        return "A(Corolla,PWM)";
    case ToyotaKeeloqVariantB:
        return "B(Tundra,NRZ)";
    default:
        return "?";
    }
}

static bool toyota_keeloq_a_is_short(uint32_t duration) {
    return pp_is_short(duration, &subghz_protocol_toyota_keeloq_const_a);
}

static bool toyota_keeloq_a_is_long(uint32_t duration) {
    return pp_is_long(duration, &subghz_protocol_toyota_keeloq_const_a);
}

static bool toyota_keeloq_b_is_short(uint32_t duration) {
    return pp_is_short(duration, &subghz_protocol_toyota_keeloq_const_b);
}

static bool toyota_keeloq_b_is_long(uint32_t duration) {
    return pp_is_long(duration, &subghz_protocol_toyota_keeloq_const_b);
}

// NRZ per-pulse bit classification via midpoint, with a sanity envelope so
// the ~1500-2600us inter-repeat sync gap (and other noise) cannot be
// misread as a data bit.
// The documented Variant-B preamble terminator: a LOW gap of ~1500-2600us.
static bool toyota_keeloq_b_is_sync_gap(uint32_t duration) {
    return duration >= TOYOTA_KEELOQ_B_SYNC_MIN_US && duration <= TOYOTA_KEELOQ_B_SYNC_MAX_US;
}

static bool toyota_keeloq_b_classify_pulse(uint32_t duration, bool* bit_out) {
    if(duration < TOYOTA_KEELOQ_B_MIN_PULSE_US || duration > TOYOTA_KEELOQ_B_MAX_PULSE_US) {
        return false;
    }
    *bit_out = duration >= TOYOTA_KEELOQ_B_MIDPOINT_US;
    return true;
}

static void toyota_keeloq_shift_in_bit(SubGhzProtocolDecoderToyotaKeeloq* instance, bool bit) {
    instance->data = (instance->data << 1) | (bit ? 1ULL : 0ULL);
    instance->bit_count++;
}

// Extracts hop/serial/button from the low 64 bits of the accumulated frame
// (hop<<32 | serial<<4 | button, per the KAT layout). Returns false if the
// serial is zero -- used both to gate "found" and to recompute display
// fields after deserialize.
static bool toyota_keeloq_extract_fields(
    uint64_t data,
    uint32_t* hop_out,
    uint32_t* serial_out,
    uint8_t* button_out) {
    const uint32_t hop = (uint32_t)(data >> 32);
    const uint32_t serial = (uint32_t)((data >> 4) & TOYOTA_KEELOQ_SERIAL_MASK);
    const uint8_t button = (uint8_t)(data & TOYOTA_KEELOQ_BUTTON_MASK);

    if(hop_out) *hop_out = hop;
    if(serial_out) *serial_out = serial;
    if(button_out) *button_out = button;

    return serial != 0U;
}

// ---- KeeLoq manufacturer-key recovery (keystore try-all-keys attack) --------
//
// The 32-bit hop is a standard KeeLoq cipher of plaintext 0xBSSSCCCC:
//   B (bits 31..28) = 4-bit button/function
//   S (bits 27..16) = discriminant = low bits of the fob serial
//   C (bits 15..0)  = 16-bit rolling counter
// A recovered key is one whose decrypt is SELF-CONSISTENT with the frame's own
// fixed part: the decrypted button must equal the on-air button AND the
// decrypted discriminant must equal the frame's serial low byte (or 0, as some
// remotes zero it). That pins the decrypt to THIS serial and rejects ~99.6% of
// wrong keys. It is still only a ~12-bit check, so a single-frame hit is a
// CANDIDATE; a 2nd frame under the same key+serial with a forward counter
// upgrades it to CONFIRMED (see caller). NEVER fabricate a match.

// Self-consistency invariant (mirrors keeloq.c subghz_protocol_keeloq_check_decrypt).
// end_serial = frame serial & 0xFF. Rejects the all-zero/degenerate decrypt.
static bool toyota_keeloq_check_decrypt(uint32_t decrypt, uint8_t btn, uint16_t end_serial) {
    if(decrypt == 0) return false; // degenerate: a zero decrypt is meaningless
    const uint8_t disc = (uint8_t)((decrypt >> 16) & 0xFF);
    if((uint8_t)(decrypt >> 28) != btn) return false; // button must match on-air button
    if(disc != (uint8_t)(end_serial & 0xFF) && disc != 0) return false; // serial-low must pin
    return true;
}

// Try every resident manufacturer key + its learning/derivation against the hop.
// Returns true and fills instance->recovered_* on the FIRST self-consistent
// decrypt. Returns false (recovered_state left None) when nothing matches --
// the honest "key not in keystore, replay-only" outcome. Runs once per decoded
// frame, off the ISR (worker thread), like the keeloq.c decoder.
static bool toyota_keeloq_try_recover(SubGhzProtocolDecoderToyotaKeeloq* instance) {
    if(!instance->keystore) return false;

    const uint32_t hop = instance->hop; // 32-bit encrypted
    const uint8_t btn = instance->button; // 4-bit
    const uint32_t serial = instance->serial & TOYOTA_KEELOQ_SERIAL_MASK; // 28-bit
    // Fixed part exactly as the KeeLoq learning functions expect: btn<<28 | serial.
    const uint32_t fix = ((uint32_t)btn << 28) | serial;
    const uint16_t end_serial = (uint16_t)(serial & 0xFF);

    for
        M_EACH(mc, *subghz_keystore_get_data(instance->keystore), SubGhzKeyArray_t) {
            uint64_t man = 0;
            uint32_t decrypt = 0;
            uint8_t cipher = ToyotaKeeloqCipherStandard;
            bool matched = false;

            switch(mc->type) {
            case KEELOQ_LEARNING_SIMPLE:
            case KEELOQ_LEARNING_SIMPLE_KINGGATES:
            case KEELOQ_LEARNING_SIMPLE_JCM:
            case KEELOQ_LEARNING_UNKNOWN:
                // Simple: the stored key IS the manufacturer key.
                man = mc->key;
                decrypt = subghz_protocol_keeloq_common_decrypt(hop, man);
                matched = toyota_keeloq_check_decrypt(decrypt, btn, end_serial);
                break;
            case KEELOQ_LEARNING_NORMAL:
            case KEELOQ_LEARNING_NORMAL_JAROLIFT:
                man = subghz_protocol_keeloq_common_normal_learning(fix, mc->key);
                decrypt = subghz_protocol_keeloq_common_decrypt(hop, man);
                matched = toyota_keeloq_check_decrypt(decrypt, btn, end_serial);
                break;
            case KEELOQ_LEARNING_SECURE:
                // Unknown seed: try zero-seed then serial-as-seed (as keeloq.c does).
                man = subghz_protocol_keeloq_common_secure_learning(fix, 0, mc->key);
                decrypt = subghz_protocol_keeloq_common_decrypt(hop, man);
                matched = toyota_keeloq_check_decrypt(decrypt, btn, end_serial);
                if(!matched) {
                    man = subghz_protocol_keeloq_common_secure_learning(fix, fix, mc->key);
                    decrypt = subghz_protocol_keeloq_common_decrypt(hop, man);
                    matched = toyota_keeloq_check_decrypt(decrypt, btn, end_serial);
                }
                break;
            case KEELOQ_LEARNING_MAGIC_XOR_TYPE_1:
                man = subghz_protocol_keeloq_common_magic_xor_type1_learning(fix, mc->key);
                decrypt = subghz_protocol_keeloq_common_decrypt(hop, man);
                matched = toyota_keeloq_check_decrypt(decrypt, btn, end_serial);
                break;
            case KEELOQ_LEARNING_MAGIC_SERIAL_TYPE_1:
                man = subghz_protocol_keeloq_common_magic_serial_type1_learning(fix, mc->key);
                decrypt = subghz_protocol_keeloq_common_decrypt(hop, man);
                matched = toyota_keeloq_check_decrypt(decrypt, btn, end_serial);
                break;
            case KEELOQ_LEARNING_MAGIC_SERIAL_TYPE_2:
                man = subghz_protocol_keeloq_common_magic_serial_type2_learning(fix, mc->key);
                decrypt = subghz_protocol_keeloq_common_decrypt(hop, man);
                matched = toyota_keeloq_check_decrypt(decrypt, btn, end_serial);
                break;
            case KEELOQ_LEARNING_MAGIC_SERIAL_TYPE_3:
                man = subghz_protocol_keeloq_common_magic_serial_type3_learning(fix, mc->key);
                decrypt = subghz_protocol_keeloq_common_decrypt(hop, man);
                matched = toyota_keeloq_check_decrypt(decrypt, btn, end_serial);
                break;
            case KEELOQ_LEARNING_AERF:
                // AERF uses the nl_extend cipher (decrypt_derived), not standard KeeLoq.
                man = subghz_protocol_keeloq_common_learning_aerf(fix, mc->key);
                decrypt = subghz_protocol_keeloq_common_decrypt_derived(hop, man, 0x240u);
                if(toyota_keeloq_check_decrypt(decrypt, btn, end_serial)) {
                    cipher = ToyotaKeeloqCipherDerived240;
                    matched = true;
                } else {
                    decrypt = subghz_protocol_keeloq_common_decrypt_derived(hop, man, 0x210u);
                    if(toyota_keeloq_check_decrypt(decrypt, btn, end_serial)) {
                        cipher = ToyotaKeeloqCipherDerived210;
                        matched = true;
                    } else {
                        decrypt = subghz_protocol_keeloq_common_decrypt(hop, man);
                        matched = toyota_keeloq_check_decrypt(decrypt, btn, end_serial);
                    }
                }
                break;
            case KEELOQ_LEARNING_ERREKA:
                man = subghz_protocol_keeloq_common_learning_erreka(fix, 0, mc->key);
                decrypt = subghz_protocol_keeloq_common_decrypt(hop, man);
                matched = toyota_keeloq_check_decrypt(decrypt, btn, end_serial);
                break;
            case KEELOQ_LEARNING_PUJOL:
                man = subghz_protocol_keeloq_common_learning_pujol(fix, mc->key);
                decrypt = subghz_protocol_keeloq_common_decrypt(hop, man);
                matched = toyota_keeloq_check_decrypt(decrypt, btn, end_serial);
                break;
            default:
                // Unknown/reserved type: fall back to simple decrypt only.
                man = mc->key;
                decrypt = subghz_protocol_keeloq_common_decrypt(hop, man);
                matched = toyota_keeloq_check_decrypt(decrypt, btn, end_serial);
                break;
            }

            if(matched) {
                instance->recovered_name = mc->name;
                instance->recovered_man = man;
                instance->recovered_plain = decrypt;
                instance->recovered_cnt = (uint16_t)(decrypt & 0xFFFFu);
                instance->recovered_cipher = cipher;
                return true;
            }
        }

    return false; // no resident key matched -> replay-only
}

static bool
    toyota_keeloq_finish_frame(SubGhzProtocolDecoderToyotaKeeloq* instance, uint16_t bit_count) {
    if(!toyota_keeloq_extract_fields(
           instance->data, &instance->hop, &instance->serial, &instance->button)) {
        return false; // serial == 0 -> reject
    }

    instance->last_variant = instance->variant;

    instance->generic.data = instance->data;
    instance->generic.data_count_bit = bit_count;
    instance->generic.serial = instance->serial;
    instance->generic.btn = instance->button;
    instance->generic.crc_check = false; // no key => no crypto validation (may change below)

    instance->decoder.decode_data = instance->data;
    instance->decoder.decode_count_bit = bit_count;

    // ---- Attempt manufacturer-key recovery (never gates the decode) ----
    instance->recovery_state = ToyotaKeeloqRecoveryNone;
    instance->recovered_name = NULL;
    if(toyota_keeloq_try_recover(instance)) {
        // A resident key decrypted the hop self-consistently. Default: CANDIDATE.
        instance->recovery_state = ToyotaKeeloqRecoveryCandidate;
        // Upgrade to CONFIRMED when a prior frame decrypted under the SAME derived
        // key + serial to a strictly-earlier counter within a small forward window
        // (a real fob monotonically increments; +1 on a normal press). This is the
        // 2-capture check that turns a possible false-match into a real recovery.
        if(instance->has_last_recovery && instance->last_man == instance->recovered_man &&
           instance->last_serial == instance->serial) {
            uint16_t delta = (uint16_t)(instance->recovered_cnt - instance->last_cnt);
            if(delta >= 1 && delta <= 16) {
                instance->recovery_state = ToyotaKeeloqRecoveryConfirmed;
            }
        }
        // Record this recovery for the next frame's confirm check.
        instance->has_last_recovery = true;
        instance->last_man = instance->recovered_man;
        instance->last_serial = instance->serial;
        instance->last_cnt = instance->recovered_cnt;

        // Surface the DECODED counter (not the raw hop) and mark crypto-verified.
        instance->generic.cnt = instance->recovered_cnt;
        instance->generic.crc_check = true;
    } else {
        // Honest: no key in the keystore matches -> replay-only. Keep the raw hop
        // in cnt exactly as the original decode-only path did.
        instance->generic.cnt = instance->hop;
    }

    return true;
}

const SubGhzProtocolDecoder subghz_protocol_toyota_keeloq_decoder = {
    .alloc = subghz_protocol_decoder_toyota_keeloq_alloc,
    .free = subghz_protocol_decoder_toyota_keeloq_free,
    .feed = subghz_protocol_decoder_toyota_keeloq_feed,
    .reset = subghz_protocol_decoder_toyota_keeloq_reset,
    .get_hash_data = subghz_protocol_decoder_toyota_keeloq_get_hash_data,
    .serialize = subghz_protocol_decoder_toyota_keeloq_serialize,
    .deserialize = subghz_protocol_decoder_toyota_keeloq_deserialize,
    .get_string = subghz_protocol_decoder_toyota_keeloq_get_string,
};

#ifdef ENABLE_EMULATE_FEATURE
// One-shot VERBATIM replay encoder. The 32-bit KeeLoq hop is ENCRYPTED under a
// per-manufacturer key that is NOT resident in this firmware, and the rolling
// counter lives inside that encrypted hop -- so the next code is unpredictable
// and there is deliberately NO predictor (a fabricated next code would be
// rejected by a real RX). What IS reconstructable is the captured frame itself:
// the decoder stored the whole (hop|serial|button) word plus the on-air Variant,
// so we rebuild the exact waveform at the decoder's own TE constants for a single
// faithful replay of that one press. No CRC/parity is carried (the hop is opaque
// ciphertext, not a computed checksum), so nothing is recomputed.
//
// NOTE: the decoder shifts the full on-air bit count (68/67) through a 64-bit
// accumulator, so the leading 68-64=4 (A) / 67-64=3 (B) framing bits were
// discarded at capture and are unrecoverable; they are emitted as 0. This still
// round-trips through this firmware's own decoder (which likewise keeps only the
// last 64 bits) and is the honest best-effort for a real RX.
#define TOYOTA_KEELOQ_TX_PREAMBLE_PAIRS 8U
#define TOYOTA_KEELOQ_B_SYNC_TX_US      2000U
#define TOYOTA_KEELOQ_UPLOAD_CAPACITY \
    ((TOYOTA_KEELOQ_TX_PREAMBLE_PAIRS * 2U) + (TOYOTA_KEELOQ_A_BITS * 2U) + 4U)
_Static_assert(
    TOYOTA_KEELOQ_UPLOAD_CAPACITY <= PP_SHARED_UPLOAD_CAPACITY,
    "TOYOTA_KEELOQ_UPLOAD_CAPACITY exceeds shared upload slab");

typedef struct SubGhzProtocolEncoderToyotaKeeloq {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    ToyotaKeeloqVariant variant;
} SubGhzProtocolEncoderToyotaKeeloq;

// Reads on-air bit `b` (MSB-first index) out of the stored 64-bit word, treating
// any position at or above bit 63 as 0 -- avoids UB from a >=64 shift and mirrors
// the framing bits the decoder discarded.
static inline bool toyota_keeloq_upload_bit(uint64_t data, int b) {
    if(b < 0 || b >= 64) return false;
    return ((data >> b) & 1ULL) != 0ULL;
}

static bool toyota_keeloq_build_upload(SubGhzProtocolEncoderToyotaKeeloq* instance) {
    furi_check(instance);

    LevelDuration* up = instance->encoder.upload;
    const size_t cap = TOYOTA_KEELOQ_UPLOAD_CAPACITY;
    const uint64_t data = instance->generic.data;
    size_t index = 0;

    if(instance->variant == ToyotaKeeloqVariantB) {
        // -- Variant B (Tundra/Tacoma, NRZ) --
        const uint32_t te_short = subghz_protocol_toyota_keeloq_const_b.te_short; // 200
        const uint32_t te_long = subghz_protocol_toyota_keeloq_const_b.te_long; // 390
        // Preamble: short-HIGH + long-LOW pairs (clock lock).
        for(size_t p = 0; p < TOYOTA_KEELOQ_TX_PREAMBLE_PAIRS; p++) {
            index = pp_emit(up, index, cap, true, te_short);
            index = pp_emit(up, index, cap, false, te_long);
        }
        // Trailing preamble HIGH then the ~2000us sync-gap LOW that terminates the
        // preamble (mirrors feed()'s BAwaitHigh -> BAwaitLow -> sync-gap path).
        index = pp_emit(up, index, cap, true, te_short);
        index = pp_emit(up, index, cap, false, TOYOTA_KEELOQ_B_SYNC_TX_US);
        // 67 NRZ pulses MSB-first, alternating level (starting HIGH), each pulse
        // long(390)=bit 1 or short(200)=bit 0 -- matches classify_pulse(>=287).
        for(int b = (int)TOYOTA_KEELOQ_B_BITS - 1; b >= 0; b--) {
            const bool bit_value = toyota_keeloq_upload_bit(data, b);
            const bool level = ((((int)TOYOTA_KEELOQ_B_BITS - 1) - b) & 1) == 0;
            index = pp_emit(up, index, cap, level, bit_value ? te_long : te_short);
        }
    } else {
        // -- Variant A (Corolla, fixed-width PWM pairs) --
        const uint32_t te_short = subghz_protocol_toyota_keeloq_const_a.te_short; // 400
        const uint32_t te_long = subghz_protocol_toyota_keeloq_const_a.te_long; // 800
        // Preamble: SS pairs (short-HIGH + short-LOW).
        for(size_t p = 0; p < TOYOTA_KEELOQ_TX_PREAMBLE_PAIRS; p++) {
            index = pp_emit(up, index, cap, true, te_short);
            index = pp_emit(up, index, cap, false, te_short);
        }
        // 68 data bits MSB-first: bit 1 -> SL (short-HIGH + long-LOW),
        // bit 0 -> LS (long-HIGH + short-LOW) -- matches feed()'s is_sl/is_ls.
        for(int b = (int)TOYOTA_KEELOQ_A_BITS - 1; b >= 0; b--) {
            const bool bit_value = toyota_keeloq_upload_bit(data, b);
            if(bit_value) {
                index = pp_emit(up, index, cap, true, te_short);
                index = pp_emit(up, index, cap, false, te_long);
            } else {
                index = pp_emit(up, index, cap, true, te_long);
                index = pp_emit(up, index, cap, false, te_short);
            }
        }
    }

    if(index > cap) {
        return false;
    }

    instance->encoder.front = 0;
    instance->encoder.size_upload = index;
    return true;
}

void* subghz_protocol_encoder_toyota_keeloq_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderToyotaKeeloq* instance =
        calloc(1, sizeof(SubGhzProtocolEncoderToyotaKeeloq));
    furi_check(instance);

    instance->base.protocol = &toyota_keeloq_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.front = 0;
    instance->encoder.is_running = false;
    pp_encoder_buffer_ensure(instance, TOYOTA_KEELOQ_UPLOAD_CAPACITY);

    return instance;
}

SubGhzProtocolStatus subghz_protocol_encoder_toyota_keeloq_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderToyotaKeeloq* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    do {
        if(pp_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
           SubGhzProtocolStatusOk) {
            break;
        }

        flipper_format_rewind(flipper_format);
        if(subghz_block_generic_deserialize(&instance->generic, flipper_format) !=
           SubGhzProtocolStatusOk) {
            break;
        }

        // Accept only the two known on-air widths (68 = Variant A, 67 = Variant B).
        if(instance->generic.data_count_bit != TOYOTA_KEELOQ_A_BITS &&
           instance->generic.data_count_bit != TOYOTA_KEELOQ_B_BITS) {
            break;
        }

        // Pick the variant: default from the bit count, override with the
        // persisted "Variant" field when it is present and sane (mirrors the
        // decoder's deserialize).
        instance->variant = (instance->generic.data_count_bit == TOYOTA_KEELOQ_A_BITS) ?
                                ToyotaKeeloqVariantA :
                                ToyotaKeeloqVariantB;
        uint32_t variant_read = 0;
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "Variant", &variant_read, 1)) {
            if(variant_read == (uint32_t)ToyotaKeeloqVariantA ||
               variant_read == (uint32_t)ToyotaKeeloqVariantB) {
                instance->variant = (ToyotaKeeloqVariant)variant_read;
            }
        }

        instance->encoder.repeat = pp_encoder_read_repeat(flipper_format, 10);

        if(!toyota_keeloq_build_upload(instance)) {
            break;
        }
        if(instance->encoder.size_upload == 0) {
            break;
        }

        instance->encoder.is_running = true;
        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}
#endif

#ifdef ENABLE_EMULATE_FEATURE
const SubGhzProtocolEncoder subghz_protocol_toyota_keeloq_encoder = {
    .alloc = subghz_protocol_encoder_toyota_keeloq_alloc,
    .free = pp_encoder_free,
    .deserialize = subghz_protocol_encoder_toyota_keeloq_deserialize,
    .stop = pp_encoder_stop,
    .yield = pp_encoder_yield,
};
#else
// Decode-only fallback: no key material is resident for the KeeLoq hop, so with
// emulate disabled there is nothing to encode (unconditional NULL-upload stub).
const SubGhzProtocolEncoder subghz_protocol_toyota_keeloq_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};
#endif

const SubGhzProtocol toyota_keeloq_protocol = {
    .name = TOYOTA_KEELOQ_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_toyota_keeloq_decoder,
    .encoder = &subghz_protocol_toyota_keeloq_encoder,
};

void* subghz_protocol_decoder_toyota_keeloq_alloc(SubGhzEnvironment* environment) {
    SubGhzProtocolDecoderToyotaKeeloq* instance =
        calloc(1, sizeof(SubGhzProtocolDecoderToyotaKeeloq));
    furi_check(instance);

    instance->base.protocol = &toyota_keeloq_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    // Resident keystore for manufacturer-key recovery (may be NULL on minimal
    // environments; try_recover() guards for that). Same source as keeloq.c.
    instance->keystore = subghz_environment_get_keystore(environment);

    return instance;
}

void subghz_protocol_decoder_toyota_keeloq_free(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderToyotaKeeloq* instance = context;
    free(instance);
}

void subghz_protocol_decoder_toyota_keeloq_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderToyotaKeeloq* instance = context;

    instance->decoder.parser_step = ToyotaKeeloqDecoderStepReset;
    instance->decoder.te_last = 0;
    instance->variant = ToyotaKeeloqVariantNone;
    instance->preamble_count = 0;
    instance->first_high = 0;
    instance->in_data = false;
    instance->data = 0;
    instance->bit_count = 0;
}

void subghz_protocol_decoder_toyota_keeloq_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderToyotaKeeloq* instance = context;

    switch(instance->decoder.parser_step) {
    case ToyotaKeeloqDecoderStepReset:
        if(level) {
            instance->first_high = duration;
            instance->preamble_count = 0;
            instance->in_data = false;
            if(duration > TOYOTA_KEELOQ_VARIANT_SELECT_US) {
                instance->variant = ToyotaKeeloqVariantA;
                instance->decoder.parser_step = ToyotaKeeloqDecoderStepAAwaitLow;
            } else {
                instance->variant = ToyotaKeeloqVariantB;
                instance->decoder.parser_step = ToyotaKeeloqDecoderStepBAwaitLow;
            }
        }
        break;

    // ---- Variant A: Corolla PWM pairs ----
    case ToyotaKeeloqDecoderStepAAwaitHigh:
        if(level) {
            instance->first_high = duration;
            instance->decoder.parser_step = ToyotaKeeloqDecoderStepAAwaitLow;
        } else {
            instance->decoder.parser_step = ToyotaKeeloqDecoderStepReset;
        }
        break;

    case ToyotaKeeloqDecoderStepAAwaitLow: {
        if(level) {
            instance->decoder.parser_step = ToyotaKeeloqDecoderStepReset;
            break;
        }

        if(!instance->in_data && toyota_keeloq_a_is_short(instance->first_high) &&
           toyota_keeloq_a_is_short(duration)) {
            // SS preamble pair.
            instance->preamble_count++;
            instance->decoder.parser_step = ToyotaKeeloqDecoderStepAAwaitHigh;
            break;
        }

        const bool is_ls = toyota_keeloq_a_is_long(instance->first_high) &&
                            toyota_keeloq_a_is_short(duration); // bit 0
        const bool is_sl = toyota_keeloq_a_is_short(instance->first_high) &&
                            toyota_keeloq_a_is_long(duration); // bit 1

        if(!is_ls && !is_sl) {
            instance->decoder.parser_step = ToyotaKeeloqDecoderStepReset;
            break;
        }

        if(!instance->in_data) {
            if(instance->preamble_count < TOYOTA_KEELOQ_A_MIN_PREAMBLE_PAIRS) {
                instance->decoder.parser_step = ToyotaKeeloqDecoderStepReset;
                break;
            }
            instance->in_data = true;
            instance->data = 0;
            instance->bit_count = 0;
        }

        toyota_keeloq_shift_in_bit(instance, is_sl);

        if(instance->bit_count == TOYOTA_KEELOQ_A_BITS) {
            if(toyota_keeloq_finish_frame(instance, TOYOTA_KEELOQ_A_BITS) &&
               instance->base.callback) {
                instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = ToyotaKeeloqDecoderStepReset;
        } else {
            instance->decoder.parser_step = ToyotaKeeloqDecoderStepAAwaitHigh;
        }
        break;
    }

    // ---- Variant B: Tundra/Tacoma NRZ ----
    case ToyotaKeeloqDecoderStepBAwaitHigh:
        if(level) {
            instance->first_high = duration;
            instance->decoder.parser_step = ToyotaKeeloqDecoderStepBAwaitLow;
        } else {
            instance->decoder.parser_step = ToyotaKeeloqDecoderStepReset;
        }
        break;

    case ToyotaKeeloqDecoderStepBAwaitLow: {
        if(level) {
            instance->decoder.parser_step = ToyotaKeeloqDecoderStepReset;
            break;
        }

        if(toyota_keeloq_b_is_short(instance->first_high) && toyota_keeloq_b_is_long(duration)) {
            // Preamble pair (short HIGH + long LOW), clock-lock only.
            instance->preamble_count++;
            instance->decoder.parser_step = ToyotaKeeloqDecoderStepBAwaitHigh;
            break;
        }

        // Preamble ends ONLY on the documented sync-gap LOW, and only after a
        // real preamble run. This is the fail-safe boundary: a data pair whose
        // LOW leg is a normal ~200/390us bit is NOT a sync gap, so it can never
        // masquerade as the preamble terminator and mis-offset the data window.
        if(instance->preamble_count >= TOYOTA_KEELOQ_B_MIN_PREAMBLE_PAIRS &&
           toyota_keeloq_b_is_sync_gap(duration)) {
            // Data begins with the NEXT pulse (this LOW is the separator, and
            // the just-latched first_high was the last preamble HIGH -- neither
            // is a data bit).
            instance->in_data = true;
            instance->data = 0;
            instance->bit_count = 0;
            instance->decoder.parser_step = ToyotaKeeloqDecoderStepBData;
            break;
        }

        // Anything else (short preamble, or an ambiguous non-sync-gap break):
        // decline rather than risk a wrong decode.
        instance->decoder.parser_step = ToyotaKeeloqDecoderStepReset;
        break;
    }

    case ToyotaKeeloqDecoderStepBData: {
        bool bit = false;
        if(!toyota_keeloq_b_classify_pulse(duration, &bit)) {
            // Out-of-envelope pulse (e.g. the ~1500-2600us inter-repeat sync
            // gap) terminates the attempt -- an incomplete bit count simply
            // fails validity below.
            instance->decoder.parser_step = ToyotaKeeloqDecoderStepReset;
            break;
        }
        toyota_keeloq_shift_in_bit(instance, bit);

        if(instance->bit_count == TOYOTA_KEELOQ_B_BITS) {
            if(toyota_keeloq_finish_frame(instance, TOYOTA_KEELOQ_B_BITS) &&
               instance->base.callback) {
                instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = ToyotaKeeloqDecoderStepReset;
        }
        break;
    }

    default:
        instance->decoder.parser_step = ToyotaKeeloqDecoderStepReset;
        break;
    }

    instance->decoder.te_last = duration;
}

uint8_t subghz_protocol_decoder_toyota_keeloq_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderToyotaKeeloq* instance = context;
    uint8_t hash = subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
    hash ^= (uint8_t)instance->last_variant;
    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_toyota_keeloq_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderToyotaKeeloq* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);

    if(ret == SubGhzProtocolStatusOk) {
        flipper_format_rewind(flipper_format);
        pp_flipper_update_or_insert_u32(flipper_format, FF_SERIAL, instance->serial);
        pp_flipper_update_or_insert_u32(flipper_format, FF_BTN, instance->button);
        pp_flipper_update_or_insert_u32(flipper_format, "Hop", instance->hop);
        pp_flipper_update_or_insert_u32(flipper_format, "Variant", (uint32_t)instance->last_variant);
    }

    return ret;
}

SubGhzProtocolStatus subghz_protocol_decoder_toyota_keeloq_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderToyotaKeeloq* instance = context;

    SubGhzProtocolStatus ret = subghz_block_generic_deserialize(&instance->generic, flipper_format);

    if(ret == SubGhzProtocolStatusOk) {
        if(instance->generic.data_count_bit != TOYOTA_KEELOQ_A_BITS &&
           instance->generic.data_count_bit != TOYOTA_KEELOQ_B_BITS) {
            FURI_LOG_D(TAG, "Wrong number of bits in key");
            return SubGhzProtocolStatusErrorValueBitCount;
        }

        instance->data = instance->generic.data;
        instance->last_variant = (instance->generic.data_count_bit == TOYOTA_KEELOQ_A_BITS) ?
                                      ToyotaKeeloqVariantA :
                                      ToyotaKeeloqVariantB;

        uint32_t variant_read = 0;
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "Variant", &variant_read, 1)) {
            instance->last_variant = (ToyotaKeeloqVariant)variant_read;
        }

        toyota_keeloq_extract_fields(
            instance->data, &instance->hop, &instance->serial, &instance->button);

        instance->generic.serial = instance->serial;
        instance->generic.btn = instance->button;

        // Re-run key recovery on the loaded frame. A single saved frame can only
        // reach CANDIDATE (no second capture to confirm the counter step).
        instance->recovery_state = ToyotaKeeloqRecoveryNone;
        instance->recovered_name = NULL;
        if(toyota_keeloq_try_recover(instance)) {
            instance->recovery_state = ToyotaKeeloqRecoveryCandidate;
            instance->generic.cnt = instance->recovered_cnt;
            instance->generic.crc_check = true;
        } else {
            instance->generic.cnt = instance->hop;
            instance->generic.crc_check = false;
        }
    }

    return ret;
}

void subghz_protocol_decoder_toyota_keeloq_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderToyotaKeeloq* instance = context;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Variant:%s\r\n"
        "Sn:%07lX  Btn:%01X - %s\r\n"
        "Hop(raw,ENC):%08lX\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        toyota_keeloq_variant_name(instance->last_variant),
        (unsigned long)instance->serial,
        instance->button,
        toyota_keeloq_button_name(instance->button),
        (unsigned long)instance->hop);

    if(instance->recovery_state == ToyotaKeeloqRecoveryNone) {
        // Honest outcome: no resident manufacturer key decrypts this hop. The
        // rolling counter is unknowable, so this fob can only be replayed once.
        furi_string_cat_printf(output, "Key: not in keystore (replay-only)\r\n");
        return;
    }

    // A resident key decrypted the hop self-consistently: the effective
    // manufacturer key and rolling counter are RECOVERED.
    const char* state = (instance->recovery_state == ToyotaKeeloqRecoveryConfirmed) ?
                            "CONFIRMED" :
                            "CANDIDATE"; // one frame; a 2nd capture confirms
    furi_string_cat_printf(
        output,
        "Recovered(%s) via %s\r\n"
        "MfKey:%016llX Cnt:%u\r\n",
        state,
        instance->recovered_name ? instance->recovered_name : "?",
        (unsigned long long)instance->recovered_man,
        (unsigned)instance->recovered_cnt);

#ifdef ENABLE_EMULATE_FEATURE
    // Durable-clone predictor: with the key + counter known, the NEXT hops are
    // deterministic. Re-encrypt the recovered plaintext with the counter bumped,
    // reusing the same cipher the decrypt used (standard KeeLoq, or AERF
    // nl_extend for a derived-key match). A real RX accepts these forward codes.
    const uint32_t plain_fixed = instance->recovered_plain & 0xFFFF0000u; // btn|disc
    for(uint32_t i = 1; i <= 3; i++) {
        const uint16_t next_cnt = (uint16_t)(instance->recovered_cnt + i);
        const uint32_t next_plain = plain_fixed | (uint32_t)next_cnt;
        uint32_t next_hop;
        switch(instance->recovered_cipher) {
        case ToyotaKeeloqCipherDerived240:
            next_hop =
                subghz_protocol_keeloq_common_encrypt_derived(next_plain, instance->recovered_man, 0x240u);
            break;
        case ToyotaKeeloqCipherDerived210:
            next_hop =
                subghz_protocol_keeloq_common_encrypt_derived(next_plain, instance->recovered_man, 0x210u);
            break;
        default:
            next_hop = subghz_protocol_keeloq_common_encrypt(next_plain, instance->recovered_man);
            break;
        }
        furi_string_cat_printf(
            output, "Next+%lu Cnt:%u Hop:%08lX\r\n", (unsigned long)i, (unsigned)next_cnt, (unsigned long)next_hop);
    }
#endif
}
