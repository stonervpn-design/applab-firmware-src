#include "toyota_keeloq.h"
#include "protocols_common.h"

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
    uint32_t hop; // 32-bit KeeLoq hop -- ENCRYPTED, surfaced raw, never decrypted
    uint32_t serial; // 28-bit
    uint8_t button; // 4-bit
} SubGhzProtocolDecoderToyotaKeeloq;

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
    instance->generic.cnt = instance->hop; // raw ENCRYPTED hop, not decoded
    instance->generic.crc_check = false; // decode-only: no crypto/CRC validation possible

    instance->decoder.decode_data = instance->data;
    instance->decoder.decode_count_bit = bit_count;

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

// Decode-only: no key material is resident in this firmware for the KeeLoq
// hop, so there is nothing to encode (mirrors land_rover_rke.c / fiat_v0.c's
// unconditional NULL-upload stub).
const SubGhzProtocolEncoder subghz_protocol_toyota_keeloq_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol toyota_keeloq_protocol = {
    .name = TOYOTA_KEELOQ_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable,
    .decoder = &subghz_protocol_toyota_keeloq_decoder,
    .encoder = &subghz_protocol_toyota_keeloq_encoder,
};

void* subghz_protocol_decoder_toyota_keeloq_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderToyotaKeeloq* instance =
        calloc(1, sizeof(SubGhzProtocolDecoderToyotaKeeloq));
    furi_check(instance);

    instance->base.protocol = &toyota_keeloq_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;

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
        instance->generic.cnt = instance->hop;
        instance->generic.crc_check = false;
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
        "Hop(raw,ENC):%08lX\r\n"
        "Crc:NOT CHECKED (decode-only, no key)\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        toyota_keeloq_variant_name(instance->last_variant),
        (unsigned long)instance->serial,
        instance->button,
        toyota_keeloq_button_name(instance->button),
        (unsigned long)instance->hop);
}
