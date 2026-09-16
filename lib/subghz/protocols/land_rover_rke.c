#include "land_rover_rke.h"
#include "protocols_common.h"

// KeeLoq manufacturer-key RECOVERY (keystore try-all-keys attack). The 32-bit
// hop is normally surfaced encrypted; this pass tries every resident
// manufacturer key + its learning/derivation and, if one decrypts a hop that
// is self-consistent with the observed serial+button, recovers the effective
// key -> decodes the counter -> enables next-code prediction (durable clone).
// See the honda_v8.c / keeloq.c find_and_get_manufacture template.
#include "keeloq_common.h"
#include "../subghz_keystore.h"
#include <m-array.h>

#include <string.h>

#define TAG "LandRoverRke"

// ---- Timing constants (from KAT src/protocols/land_rover_rke.rs) ----
// Fixed-width PWM, 1000us bit: bit=1 is 700us HIGH + 300us LOW, bit=0 is
// 300us HIGH + 700us LOW. Tolerance ~20%.
static const SubGhzBlockConst subghz_protocol_land_rover_rke_const = {
    .te_short = 300,
    .te_long = 700,
    .te_delta = 140, // ~20% of te_long / ~47% of te_short (matches land_rover_v0's asymmetric-delta convention)
    .min_count_bit_for_found = 66,
};

// Preamble: >=16 (up to 20) pairs of 400us HIGH + 600us LOW.
#define LAND_ROVER_RKE_PREAMBLE_HIGH_US   400U
#define LAND_ROVER_RKE_PREAMBLE_LOW_US    600U
#define LAND_ROVER_RKE_PREAMBLE_DELTA_US  100U
#define LAND_ROVER_RKE_MIN_PREAMBLE_PAIRS 16U

// Total on-air frame width in bits (V[65:0]): extra_bits holds V[65:64].
#define LAND_ROVER_RKE_BIT_COUNT 66U

#ifdef ENABLE_EMULATE_FEATURE
// TX preamble length (>= the decoder's 16-pair minimum; use 20).
#define LAND_ROVER_RKE_PREAMBLE_TX_PAIRS 20U
#define LAND_ROVER_RKE_UPLOAD_CAPACITY \
    ((LAND_ROVER_RKE_PREAMBLE_TX_PAIRS * 2U) + 2U + (LAND_ROVER_RKE_BIT_COUNT * 2U))
_Static_assert(
    LAND_ROVER_RKE_UPLOAD_CAPACITY <= PP_SHARED_UPLOAD_CAPACITY,
    "LAND_ROVER_RKE_UPLOAD_CAPACITY exceeds shared upload slab");
#endif

// Sync: 400us HIGH (same nominal as a preamble pulse) + 9600us LOW. The long
// 9600us gap is the key discriminator that gates false matches.
#define LAND_ROVER_RKE_SYNC_LOW_US       9600U
#define LAND_ROVER_RKE_SYNC_LOW_DELTA_US 1920U // ~20% of 9600

typedef struct SubGhzProtocolDecoderLandRoverRke {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint16_t preamble_count;

    // 66-bit frame accumulator: extra_bits holds V[65:64], data holds V[63:0].
    uint64_t data;
    uint8_t extra_bits;
    uint8_t bit_count;
    bool pending_bit;

    uint32_t hop; // 32-bit KeeLoq hop — ENCRYPTED, surfaced raw, never decrypted
    uint32_t serial; // 24-bit
    uint8_t button; // 4-bit
    uint8_t flags; // 4-bit
    uint8_t status; // 2-bit

    // ---- KeeLoq manufacturer-key recovery (keystore try-all-keys) ----
    SubGhzKeystore* keystore; // resident manufacturer keys (env-owned, not freed here)

    uint8_t recover_state; // LandRoverRkeRecover* — NONE / CANDIDATE / CONFIRMED
    uint64_t recovered_key; // effective (derived) manufacturer key that decrypts the hop
    uint16_t recovered_cnt; // decoded KeeLoq counter (rolling code) from the winning decrypt
    uint32_t recovered_plain_hi; // winning plaintext's upper 16 bits (btn+disc), for faithful re-encrypt
    uint8_t recovered_type; // KEELOQ_LEARNING_* of the winning key (picks encrypt cipher)
    bool recovered_hop_rev; // true if the winning decrypt used the bit-reversed hop ordering
    const char* recovered_name; // winning key's keystore name (env-owned string, not freed)

    // Prior recovered frame (for the two-frame CONFIRM: counter must advance).
    bool has_prev_recover;
    uint32_t prev_serial;
    uint16_t prev_cnt;
    uint64_t prev_key;
} SubGhzProtocolDecoderLandRoverRke;

typedef enum {
    LandRoverRkeRecoverNone = 0, // no resident key decrypts this hop (replay-only)
    LandRoverRkeRecoverCandidate, // single frame passed the invariant (~could false-match)
    LandRoverRkeRecoverConfirmed, // a 2nd frame decrypted to counter+delta under same key
} LandRoverRkeRecoverState;

typedef enum {
    LandRoverRkeDecoderStepReset = 0,
    LandRoverRkeDecoderStepPreambleLow,
    LandRoverRkeDecoderStepPreambleHigh,
    LandRoverRkeDecoderStepDataHigh,
    LandRoverRkeDecoderStepDataLow,
} LandRoverRkeDecoderStep;

static const char* land_rover_rke_button_name(uint8_t button) {
    switch(button) {
    case 0x1:
        return "Lock";
    case 0x2:
        return "Unlock";
    case 0x4:
        return "Boot";
    case 0x8:
        return "Panic";
    default:
        return "Unknown";
    }
}

static bool land_rover_rke_matches(uint32_t duration, uint32_t nominal, uint32_t delta) {
    return DURATION_DIFF(duration, nominal) < delta;
}

static bool land_rover_rke_is_preamble_high(uint32_t duration) {
    return land_rover_rke_matches(
        duration, LAND_ROVER_RKE_PREAMBLE_HIGH_US, LAND_ROVER_RKE_PREAMBLE_DELTA_US);
}

static bool land_rover_rke_is_preamble_low(uint32_t duration) {
    return land_rover_rke_matches(
        duration, LAND_ROVER_RKE_PREAMBLE_LOW_US, LAND_ROVER_RKE_PREAMBLE_DELTA_US);
}

static bool land_rover_rke_is_sync_low(uint32_t duration) {
    return land_rover_rke_matches(
        duration, LAND_ROVER_RKE_SYNC_LOW_US, LAND_ROVER_RKE_SYNC_LOW_DELTA_US);
}

static bool land_rover_rke_is_bit_long(uint32_t duration) {
    return pp_is_long(duration, &subghz_protocol_land_rover_rke_const);
}

static bool land_rover_rke_is_bit_short(uint32_t duration) {
    return pp_is_short(duration, &subghz_protocol_land_rover_rke_const);
}

static void
    land_rover_rke_shift_in_bit(SubGhzProtocolDecoderLandRoverRke* instance, bool bit) {
    const uint8_t carry = (uint8_t)((instance->data >> 63) & 1U);
    instance->data = (instance->data << 1) | (bit ? 1ULL : 0ULL);
    instance->extra_bits = (uint8_t)(((instance->extra_bits << 1) | carry) & 0x3U);
    instance->bit_count++;
}

// Extracts hop/serial/button/flags/status from the 66-bit frame
// (extra_bits:data == V[65:0], MSB-first per the KAT layout) into the
// instance fields. Returns false (no fields written beyond zero) if the
// serial is zero — used both to gate "found" and to recompute display
// fields after deserialize.
static bool land_rover_rke_extract_fields(uint8_t extra_bits, uint64_t data, uint32_t* hop_out,
    uint32_t* serial_out, uint8_t* button_out, uint8_t* flags_out, uint8_t* status_out) {
    const uint32_t hop = ((uint32_t)(extra_bits & 0x3U) << 30) |
                          (uint32_t)((data >> 34) & 0x3FFFFFFFULL);
    const uint32_t serial = (uint32_t)((data >> 10) & 0xFFFFFFULL);
    const uint8_t button = (uint8_t)((data >> 6) & 0xFULL);
    const uint8_t flags = (uint8_t)((data >> 2) & 0xFULL);
    const uint8_t status = (uint8_t)(data & 0x3ULL);

    if(hop_out) *hop_out = hop;
    if(serial_out) *serial_out = serial;
    if(button_out) *button_out = button;
    if(flags_out) *flags_out = flags;
    if(status_out) *status_out = status;

    return serial != 0U;
}

// ─────────────────────────────────────────────────────────────────────────
//  KeeLoq manufacturer-key recovery
//
//  A standard KeeLoq hop decrypts to plaintext 0xB SSS CCCC:
//    B  = button   (top 4 bits)   -> must equal the frame's cleartext button
//    S  = discrimination          -> low bits mirror the fixed serial's low bits
//    C  = 16-bit counter (rolling code)
//  We try every resident manufacturer key under its own learning/derivation
//  (mirroring keeloq.c's find_and_get_manufacture) and accept a decrypt only if
//  it self-pins to the observed serial+button. With a ~12-bit invariant and a
//  few-hundred-key store a SINGLE frame can still false-match, so one frame is
//  labelled CANDIDATE; CONFIRMED requires a 2nd capture that decrypts (same key)
//  to an advanced counter. On no match we stay replay-only — never faked.
// ─────────────────────────────────────────────────────────────────────────

// The Land Rover hop is surfaced MSB-first off-air; the true KeeLoq bit order
// for this (never-officially-decrypted) protocol is unknown, so recovery tries
// both the as-extracted hop and its 32-bit mirror. Doubling the search doubles
// the false-match odds — which is exactly why a lone frame is only CANDIDATE.
static uint32_t land_rover_rke_reverse32(uint32_t v) {
    v = ((v & 0x55555555u) << 1) | ((v >> 1) & 0x55555555u);
    v = ((v & 0x33333333u) << 2) | ((v >> 2) & 0x33333333u);
    v = ((v & 0x0F0F0F0Fu) << 4) | ((v >> 4) & 0x0F0F0F0Fu);
    v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    return (v << 16) | (v >> 16);
}

// Self-consistency invariant (mirrors keeloq.c subghz_protocol_keeloq_check_decrypt):
// button nibble must match, the decrypt's serial-low byte must match the frame's
// serial-low byte (or be zero, as some HCS variants zero the discriminator), and
// reject the all-zero / degenerate plaintext that any key can accidentally yield.
static bool land_rover_rke_decrypt_ok(
    uint32_t decrypt,
    uint8_t button,
    uint32_t serial,
    uint16_t* cnt_out) {
    if(decrypt == 0U) return false;
    if((uint8_t)(decrypt >> 28) != (button & 0xFU)) return false;

    const uint8_t dec_disc = (uint8_t)((decrypt >> 16) & 0xFFU);
    const uint8_t ser_low = (uint8_t)(serial & 0xFFU);
    if(dec_disc != ser_low && dec_disc != 0U) return false;

    const uint16_t cnt = (uint16_t)(decrypt & 0xFFFFU);
    if(cnt == 0U && dec_disc == 0U) return false; // reject degenerate zero-plaintext
    if(cnt_out) *cnt_out = cnt;
    return true;
}

// Derive the effective key for one keystore entry per its learning type and
// decrypt the hop with it (mirrors keeloq.c's per-type switch). Returns the
// candidate plaintext in *decrypt and the effective (derived) key in *eff_key.
static void land_rover_rke_decrypt_with_key(
    const SubGhzKey* mk,
    uint32_t hop,
    uint32_t fix,
    uint32_t* decrypt,
    uint64_t* eff_key) {
    uint64_t man;
    switch(mk->type) {
    case KEELOQ_LEARNING_NORMAL:
        man = subghz_protocol_keeloq_common_normal_learning(fix, mk->key);
        break;
    case KEELOQ_LEARNING_SECURE:
        // No seed field on-air for this protocol — try zero seed (common case).
        man = subghz_protocol_keeloq_common_secure_learning(fix, 0, mk->key);
        break;
    case KEELOQ_LEARNING_MAGIC_XOR_TYPE_1:
        man = subghz_protocol_keeloq_common_magic_xor_type1_learning(fix, mk->key);
        break;
    case KEELOQ_LEARNING_MAGIC_SERIAL_TYPE_1:
        man = subghz_protocol_keeloq_common_magic_serial_type1_learning(fix, mk->key);
        break;
    case KEELOQ_LEARNING_MAGIC_SERIAL_TYPE_2:
        man = subghz_protocol_keeloq_common_magic_serial_type2_learning(fix, mk->key);
        break;
    case KEELOQ_LEARNING_MAGIC_SERIAL_TYPE_3:
        man = subghz_protocol_keeloq_common_magic_serial_type3_learning(fix, mk->key);
        break;
    case KEELOQ_LEARNING_PUJOL:
        man = subghz_protocol_keeloq_common_learning_pujol(fix, mk->key);
        break;
    case KEELOQ_LEARNING_ERREKA:
        man = subghz_protocol_keeloq_common_learning_erreka(fix, 0, mk->key);
        break;
    case KEELOQ_LEARNING_AERF:
        // AERF uses the nl_extend cipher, not the standard KeeLoq cipher. Decode
        // with 0x210 rounds — the exact inverse of encrypt_derived(...,0x210) the
        // predictor uses, so a recovered AERF hop round-trips faithfully.
        man = subghz_protocol_keeloq_common_learning_aerf(fix, mk->key);
        *eff_key = man;
        *decrypt = subghz_protocol_keeloq_common_decrypt_derived(hop, man, 0x210u);
        return;
    case KEELOQ_LEARNING_SIMPLE:
    case KEELOQ_LEARNING_SIMPLE_JCM:
    case KEELOQ_LEARNING_UNKNOWN:
    default:
        man = mk->key; // simple learning: key used directly
        break;
    }
    *eff_key = man;
    *decrypt = subghz_protocol_keeloq_common_decrypt(hop, man);
}

// Try every resident key (both hop bit-orderings) against the current frame.
// On a hit, populate recovered_* and upgrade the recover_state, applying the
// two-frame CONFIRM rule. On no hit, recover_state = None (replay-only).
static void land_rover_rke_try_recover(SubGhzProtocolDecoderLandRoverRke* instance) {
    instance->recover_state = LandRoverRkeRecoverNone;
    instance->recovered_key = 0;
    instance->recovered_cnt = 0;
    instance->recovered_plain_hi = 0;
    instance->recovered_type = KEELOQ_LEARNING_UNKNOWN;
    instance->recovered_hop_rev = false;
    instance->recovered_name = NULL;

    if(!instance->keystore) return;
    if(instance->serial == 0U) return;

    // fix = 32-bit fixed portion the learning functions consume: button in the
    // top nibble + the (<=28-bit) serial, exactly as keeloq.c assembles it.
    const uint32_t fix = ((uint32_t)(instance->button & 0xFU) << 28) |
                         (instance->serial & 0x0FFFFFFFU);
    const uint32_t hop_variants[2] = {
        instance->hop, land_rover_rke_reverse32(instance->hop)};

    for(uint8_t v = 0; v < 2; v++) {
        const uint32_t hop = hop_variants[v];
        for
            M_EACH(mk, *subghz_keystore_get_data(instance->keystore), SubGhzKeyArray_t) {
                uint32_t decrypt = 0;
                uint64_t eff_key = 0;
                land_rover_rke_decrypt_with_key(mk, hop, fix, &decrypt, &eff_key);

                uint16_t cnt = 0;
                if(!land_rover_rke_decrypt_ok(decrypt, instance->button, instance->serial, &cnt)) {
                    continue;
                }

                instance->recovered_key = eff_key;
                instance->recovered_cnt = cnt;
                instance->recovered_plain_hi = decrypt & 0xFFFF0000U; // exact btn+disc bits
                instance->recovered_type = (uint8_t)mk->type;
                instance->recovered_hop_rev = (v == 1);
                instance->recovered_name = mk->name;

                // Two-frame CONFIRM: same serial + same effective key + an advanced
                // counter (within a sane rolling window) => this is not a fluke.
                if(instance->has_prev_recover && instance->prev_serial == instance->serial &&
                   instance->prev_key == eff_key) {
                    const uint16_t diff = (uint16_t)(cnt - instance->prev_cnt);
                    if(diff >= 1U && diff <= 256U) {
                        instance->recover_state = LandRoverRkeRecoverConfirmed;
                    } else {
                        instance->recover_state = LandRoverRkeRecoverCandidate;
                    }
                } else {
                    instance->recover_state = LandRoverRkeRecoverCandidate;
                }

                instance->has_prev_recover = true;
                instance->prev_serial = instance->serial;
                instance->prev_cnt = cnt;
                instance->prev_key = eff_key;
                return; // first self-consistent key wins
            }
    }
    // No resident key matched: leave replay-only. (Do NOT clear has_prev_recover —
    // a later frame might still recover and CONFIRM against an earlier one.)
}

// Re-encrypt a future counter under the recovered key using the cipher its
// learning type selects (AERF => nl_extend, else standard KeeLoq). Used only to
// DISPLAY predicted next hops (durable-clone preview); does not drive TX here.
static uint32_t land_rover_rke_predict_hop(
    const SubGhzProtocolDecoderLandRoverRke* instance,
    uint16_t counter) {
    // Reuse the winning frame's exact plaintext upper half (button + full
    // discrimination bits) and advance only the 16-bit counter — this matches
    // what the fob's own KeeLoq encoder feeds in, so the predicted hop is faithful.
    const uint32_t plain = instance->recovered_plain_hi | (uint32_t)counter;
    uint32_t hop = (instance->recovered_type == KEELOQ_LEARNING_AERF) ?
                       subghz_protocol_keeloq_common_encrypt_derived(
                           plain, instance->recovered_key, 0x210u) :
                       subghz_protocol_keeloq_common_encrypt(plain, instance->recovered_key);
    if(instance->recovered_hop_rev) hop = land_rover_rke_reverse32(hop);
    return hop;
}

static bool land_rover_rke_finish_frame(SubGhzProtocolDecoderLandRoverRke* instance) {
    if(!land_rover_rke_extract_fields(
           instance->extra_bits,
           instance->data,
           &instance->hop,
           &instance->serial,
           &instance->button,
           &instance->flags,
           &instance->status)) {
        return false; // serial == 0 -> reject (spec: "Serial must be non-zero")
    }

    instance->generic.data = instance->data;
    instance->generic.data_count_bit = subghz_protocol_land_rover_rke_const.min_count_bit_for_found;
    instance->generic.serial = instance->serial;
    instance->generic.btn = instance->button;
    instance->generic.cnt = instance->hop; // raw ENCRYPTED hop, not decoded
    instance->generic.crc_check = false; // decode-only: no crypto/CRC validation possible

    instance->decoder.decode_data = instance->data;
    instance->decoder.decode_count_bit = subghz_protocol_land_rover_rke_const.min_count_bit_for_found;

    // Attempt manufacturer-key recovery against the resident keystore. This never
    // gates the accept path: the frame decodes+displays either way; recovery only
    // ADDS the decoded counter + key + next-code prediction when a key matches.
    land_rover_rke_try_recover(instance);

    return true;
}

const SubGhzProtocolDecoder subghz_protocol_land_rover_rke_decoder = {
    .alloc = subghz_protocol_decoder_land_rover_rke_alloc,
    .free = subghz_protocol_decoder_land_rover_rke_free,
    .feed = subghz_protocol_decoder_land_rover_rke_feed,
    .reset = subghz_protocol_decoder_land_rover_rke_reset,
    .get_hash_data = subghz_protocol_decoder_land_rover_rke_get_hash_data,
    .serialize = subghz_protocol_decoder_land_rover_rke_serialize,
    .deserialize = subghz_protocol_decoder_land_rover_rke_deserialize,
    .get_string = subghz_protocol_decoder_land_rover_rke_get_string,
};

// The 32-bit KeeLoq hop is ENCRYPTED under an unknown per-manufacturer key, so
// the next rolling code CANNOT be predicted (no predictor is provided — that
// would emit codes a real RX rejects). What *is* fully reconstructable is the
// captured frame itself: the decoder retains every on-air bit (V[65:0] as
// extra_bits:data) and all TE timings are known, so we can rebuild a
// byte-identical waveform for a ONE-SHOT verbatim replay of the captured press.
#ifdef ENABLE_EMULATE_FEATURE
typedef struct SubGhzProtocolEncoderLandRoverRke {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint64_t data; // V[63:0]
    uint8_t extra_bits; // V[65:64]
} SubGhzProtocolEncoderLandRoverRke;

// Rebuild the exact captured waveform from the retained frame bits. No field is
// recomputed (verbatim replay): the encrypted hop, cleartext counter/serial,
// button and flags are re-emitted exactly as captured. Preamble/sync/data use
// the same TE constants the decoder's feed() accepts.
static bool
    land_rover_rke_encoder_build_upload(SubGhzProtocolEncoderLandRoverRke* instance) {
    furi_check(instance);

    LevelDuration* up = instance->encoder.upload;
    const size_t cap = LAND_ROVER_RKE_UPLOAD_CAPACITY;
    const uint32_t te_short = subghz_protocol_land_rover_rke_const.te_short; // 300us
    const uint32_t te_long = subghz_protocol_land_rover_rke_const.te_long; // 700us
    size_t index = 0;

    // Preamble: pairs of 400us HIGH + 600us LOW (decoder needs >=16 pairs).
    for(size_t p = 0; p < LAND_ROVER_RKE_PREAMBLE_TX_PAIRS; p++) {
        index = pp_emit(up, index, cap, true, LAND_ROVER_RKE_PREAMBLE_HIGH_US);
        index = pp_emit(up, index, cap, false, LAND_ROVER_RKE_PREAMBLE_LOW_US);
    }

    // Sync: 400us HIGH (preamble-shaped) + the discriminating 9600us LOW.
    index = pp_emit(up, index, cap, true, LAND_ROVER_RKE_PREAMBLE_HIGH_US);
    index = pp_emit(up, index, cap, false, LAND_ROVER_RKE_SYNC_LOW_US);

    // Data: 66 bits MSB-first, V[65] down to V[0].
    //   bit=1 -> 700us HIGH + 300us LOW; bit=0 -> 300us HIGH + 700us LOW.
    for(int8_t i = (int8_t)(LAND_ROVER_RKE_BIT_COUNT - 1U); i >= 0; i--) {
        const bool bit_value = (i < 64) ? (((instance->data >> i) & 1U) != 0U) :
                                          (((instance->extra_bits >> (i - 64)) & 1U) != 0U);
        if(bit_value) {
            index = pp_emit(up, index, cap, true, te_long);
            index = pp_emit(up, index, cap, false, te_short);
        } else {
            index = pp_emit(up, index, cap, true, te_short);
            index = pp_emit(up, index, cap, false, te_long);
        }
    }

    if(index > cap) {
        return false;
    }

    instance->encoder.front = 0;
    instance->encoder.size_upload = index;
    return true;
}

void* subghz_protocol_encoder_land_rover_rke_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);

    SubGhzProtocolEncoderLandRoverRke* instance =
        calloc(1, sizeof(SubGhzProtocolEncoderLandRoverRke));
    furi_check(instance);

    instance->base.protocol = &land_rover_rke_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.front = 0;
    instance->encoder.is_running = false;
    pp_encoder_buffer_ensure(instance, LAND_ROVER_RKE_UPLOAD_CAPACITY);

    return instance;
}

SubGhzProtocolStatus subghz_protocol_encoder_land_rover_rke_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderLandRoverRke* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    do {
        if(pp_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
           SubGhzProtocolStatusOk) {
            break;
        }

        flipper_format_rewind(flipper_format);
        SubGhzProtocolStatus load_st = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_land_rover_rke_const.min_count_bit_for_found);
        if(load_st != SubGhzProtocolStatusOk) {
            break;
        }

        // Recover the retained frame verbatim: V[63:0] from the Key, V[65:64]
        // from the ExtraBits field the decoder persisted. Nothing is recomputed.
        instance->data = instance->generic.data;

        uint32_t extra_bits_read = 0;
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "ExtraBits", &extra_bits_read, 1)) {
            instance->extra_bits = (uint8_t)(extra_bits_read & 0x3U);
        } else {
            instance->extra_bits = 0;
        }

        instance->encoder.repeat = pp_encoder_read_repeat(flipper_format, 10);

        if(!land_rover_rke_encoder_build_upload(instance)) {
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

const SubGhzProtocolEncoder subghz_protocol_land_rover_rke_encoder = {
    .alloc = subghz_protocol_encoder_land_rover_rke_alloc,
    .free = pp_encoder_free,
    .deserialize = subghz_protocol_encoder_land_rover_rke_deserialize,
    .stop = pp_encoder_stop,
    .yield = pp_encoder_yield,
};
#else
// Emulate feature disabled: no encoder (the verbatim-replay upload is
// emulate-only, matching this tree's ENABLE_EMULATE_FEATURE convention).
const SubGhzProtocolEncoder subghz_protocol_land_rover_rke_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};
#endif

const SubGhzProtocol land_rover_rke_protocol = {
    .name = LAND_ROVER_RKE_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_land_rover_rke_decoder,
    .encoder = &subghz_protocol_land_rover_rke_encoder,
};

void* subghz_protocol_decoder_land_rover_rke_alloc(SubGhzEnvironment* environment) {
    SubGhzProtocolDecoderLandRoverRke* instance =
        calloc(1, sizeof(SubGhzProtocolDecoderLandRoverRke));
    furi_check(instance);

    instance->base.protocol = &land_rover_rke_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    // Resident manufacturer keystore for the try-all-keys recovery. Env-owned:
    // borrowed pointer, never freed here. NULL is tolerated (recovery no-ops).
    instance->keystore = subghz_environment_get_keystore(environment);

    return instance;
}

void subghz_protocol_decoder_land_rover_rke_free(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderLandRoverRke* instance = context;
    free(instance);
}

void subghz_protocol_decoder_land_rover_rke_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderLandRoverRke* instance = context;

    instance->decoder.parser_step = LandRoverRkeDecoderStepReset;
    instance->decoder.te_last = 0;
    instance->preamble_count = 0;
    instance->data = 0;
    instance->extra_bits = 0;
    instance->bit_count = 0;
    instance->pending_bit = false;

    // Fresh RX session: drop any prior recovery + the two-frame CONFIRM history.
    instance->recover_state = LandRoverRkeRecoverNone;
    instance->recovered_key = 0;
    instance->recovered_cnt = 0;
    instance->recovered_plain_hi = 0;
    instance->recovered_type = KEELOQ_LEARNING_UNKNOWN;
    instance->recovered_hop_rev = false;
    instance->recovered_name = NULL;
    instance->has_prev_recover = false;
    instance->prev_serial = 0;
    instance->prev_cnt = 0;
    instance->prev_key = 0;
}

void subghz_protocol_decoder_land_rover_rke_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderLandRoverRke* instance = context;

    switch(instance->decoder.parser_step) {
    case LandRoverRkeDecoderStepReset:
        if(level && land_rover_rke_is_preamble_high(duration)) {
            instance->preamble_count = 0;
            instance->decoder.parser_step = LandRoverRkeDecoderStepPreambleLow;
        }
        break;

    case LandRoverRkeDecoderStepPreambleLow:
        if(!level && land_rover_rke_is_preamble_low(duration)) {
            instance->preamble_count++;
            instance->decoder.parser_step = LandRoverRkeDecoderStepPreambleHigh;
        } else if(
            !level && land_rover_rke_is_sync_low(duration) &&
            instance->preamble_count >= LAND_ROVER_RKE_MIN_PREAMBLE_PAIRS) {
            // Sync detected: 400us HIGH (already consumed as a preamble-shaped
            // pulse) followed by the discriminating 9600us LOW. Start data.
            instance->data = 0;
            instance->extra_bits = 0;
            instance->bit_count = 0;
            instance->pending_bit = false;
            instance->decoder.parser_step = LandRoverRkeDecoderStepDataHigh;
        } else {
            instance->decoder.parser_step = LandRoverRkeDecoderStepReset;
        }
        break;

    case LandRoverRkeDecoderStepPreambleHigh:
        if(level && land_rover_rke_is_preamble_high(duration)) {
            instance->decoder.parser_step = LandRoverRkeDecoderStepPreambleLow;
        } else {
            instance->decoder.parser_step = LandRoverRkeDecoderStepReset;
        }
        break;

    case LandRoverRkeDecoderStepDataHigh:
        if(level && land_rover_rke_is_bit_long(duration)) {
            instance->pending_bit = true; // 700us HIGH -> bit 1
            instance->decoder.parser_step = LandRoverRkeDecoderStepDataLow;
        } else if(level && land_rover_rke_is_bit_short(duration)) {
            instance->pending_bit = false; // 300us HIGH -> bit 0
            instance->decoder.parser_step = LandRoverRkeDecoderStepDataLow;
        } else {
            instance->decoder.parser_step = LandRoverRkeDecoderStepReset;
        }
        break;

    case LandRoverRkeDecoderStepDataLow: {
        const bool low_ok = !level &&
                             (instance->pending_bit ? land_rover_rke_is_bit_short(duration) :
                                                       land_rover_rke_is_bit_long(duration));
        if(!low_ok) {
            instance->decoder.parser_step = LandRoverRkeDecoderStepReset;
            break;
        }

        land_rover_rke_shift_in_bit(instance, instance->pending_bit);

        if(instance->bit_count == subghz_protocol_land_rover_rke_const.min_count_bit_for_found) {
            if(land_rover_rke_finish_frame(instance) && instance->base.callback) {
                instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = LandRoverRkeDecoderStepReset;
        } else {
            instance->decoder.parser_step = LandRoverRkeDecoderStepDataHigh;
        }
        break;
    }
    default:
        break;
    }

    instance->decoder.te_last = duration;
}

uint8_t subghz_protocol_decoder_land_rover_rke_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderLandRoverRke* instance = context;
    uint8_t hash = subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
    hash ^= instance->extra_bits;
    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_land_rover_rke_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderLandRoverRke* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);

    if(ret == SubGhzProtocolStatusOk) {
        flipper_format_rewind(flipper_format);
        pp_flipper_update_or_insert_u32(flipper_format, FF_SERIAL, instance->serial);
        pp_flipper_update_or_insert_u32(flipper_format, FF_BTN, instance->button);
        pp_flipper_update_or_insert_u32(flipper_format, "Hop", instance->hop);
        pp_flipper_update_or_insert_u32(flipper_format, "Flags", instance->flags);
        pp_flipper_update_or_insert_u32(flipper_format, "Status", instance->status);
        pp_flipper_update_or_insert_u32(flipper_format, "ExtraBits", instance->extra_bits);

        // Persist a recovered manufacturer key + decoded counter (durable clone).
        // Written only on an actual recovery — a replay-only capture stays clean.
        if(instance->recover_state != LandRoverRkeRecoverNone) {
            pp_flipper_update_or_insert_u32(
                flipper_format, "MfKeyHi", (uint32_t)(instance->recovered_key >> 32));
            pp_flipper_update_or_insert_u32(
                flipper_format, "MfKeyLo", (uint32_t)(instance->recovered_key & 0xFFFFFFFFU));
            pp_flipper_update_or_insert_u32(flipper_format, "RecCnt", instance->recovered_cnt);
        }
    }

    return ret;
}

SubGhzProtocolStatus subghz_protocol_decoder_land_rover_rke_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderLandRoverRke* instance = context;

    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_land_rover_rke_const.min_count_bit_for_found);

    if(ret == SubGhzProtocolStatusOk) {
        instance->data = instance->generic.data;

        uint32_t extra_bits_read = 0;
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "ExtraBits", &extra_bits_read, 1)) {
            instance->extra_bits = (uint8_t)(extra_bits_read & 0x3U);
        } else {
            instance->extra_bits = 0;
        }

        land_rover_rke_extract_fields(
            instance->extra_bits,
            instance->data,
            &instance->hop,
            &instance->serial,
            &instance->button,
            &instance->flags,
            &instance->status);

        instance->generic.serial = instance->serial;
        instance->generic.btn = instance->button;
        instance->generic.cnt = instance->hop;
        instance->generic.crc_check = false;

        // A loaded .sub is a single frame, so recovery can only reach CANDIDATE
        // (CONFIRM needs a live 2nd capture). Reset the two-frame history first so
        // stale in-RAM state can't fabricate a CONFIRMED on a freshly loaded file.
        instance->has_prev_recover = false;
        instance->prev_serial = 0;
        instance->prev_cnt = 0;
        instance->prev_key = 0;
        land_rover_rke_try_recover(instance);
    }

    return ret;
}

void subghz_protocol_decoder_land_rover_rke_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderLandRoverRke* instance = context;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Sn:%06lX  Btn:%01X - %s\r\n"
        "Hop(raw,ENC):%08lX\r\n"
        "Flags:%01X  Status:%01X\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long)instance->serial,
        instance->button,
        land_rover_rke_button_name(instance->button),
        (unsigned long)instance->hop,
        instance->flags,
        instance->status);

    if(instance->recover_state == LandRoverRkeRecoverNone) {
        // No resident manufacturer key decrypts this hop — cannot decode the
        // counter or predict the next code. The one-shot replay still works.
        furi_string_cat_printf(output, "Key: not in keystore (replay-only)\r\n");
    } else {
        const char* tag = (instance->recover_state == LandRoverRkeRecoverConfirmed) ?
                              "CONFIRMED" :
                              "CANDIDATE";
        furi_string_cat_printf(
            output,
            "Key %s [%s]\r\n"
            "MfKey:%016llX Cnt:%u\r\n",
            tag,
            instance->recovered_name ? instance->recovered_name : "?",
            (unsigned long long)instance->recovered_key,
            (unsigned)instance->recovered_cnt);
        // Durable-clone preview: the next rolling hops this fob will emit, so a
        // synchronized transmit stays ahead of the receiver's counter window.
        for(uint16_t i = 1; i <= 3; i++) {
            const uint16_t next_cnt = (uint16_t)(instance->recovered_cnt + i);
            furi_string_cat_printf(
                output,
                "Next+%u:%08lX\r\n",
                (unsigned)i,
                (unsigned long)land_rover_rke_predict_hop(instance, next_cnt));
        }
    }
}
