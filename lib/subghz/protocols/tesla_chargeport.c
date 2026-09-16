#include "tesla_chargeport.h"
#include "protocols_common.h"
#include "../defines.h" // ENABLE_EMULATE_FEATURE (gates the TX encoder)

/*
 * Tesla charge-port door opener — UNIVERSAL FIXED CODE
 *
 * This is not a per-car rolling/RKE remote: every Tesla accepts the same
 * static 344-bit OOK payload to pop the charge-port flap. It is benign,
 * universal, and public-visible.
 *
 * RF profile:
 *   - 315.00 MHz (US/NA) and 433.92 MHz (EU/AUS), same payload on both bands
 *   - OOK / ASK, plain On-Off-Keying (direct level-per-bit, MSB-first)
 *   - Bit clock TE = 400 us per bit; consecutive equal bits merge into one
 *     longer pulse (run-length). Frame = fixed 344 bits (43 bytes).
 *   - Inter-message gap = 23 ms of carrier OFF; message repeated 5x per press.
 *
 * Canonical 43-byte payload (transmit byte[0] bit7 first):
 *   {0x02,0xAA,0xAA,0xAA, 0x2B,0x2C,0xCB,0x33,0x33,0x2D,0x34,0xB5,0x2B,0x4D,
 *    0x32,0xAD,0x2C,0x56,0x59,0x96,0x66,0x66,0x5A,0x69,0x6A,0x56,0x9A,0x65,
 *    0x5A,0x58,0xAC,0xB3,0x2C,0xCC,0xCC,0xB4,0xD2,0xD4,0xAD,0x34,0xCA,0xB4,0xA0}
 *   bytes[0..3]=preamble, bytes[4..42]=fixed data.
 *
 * Recognition uses the distinctive 64-bit window right after the preamble
 * (bytes[4..11] = 0x2B2CCB33332D34B5). A clocked-OOK reconstruction shifts
 * every 400us slot into a 64-bit rolling register; when it equals the
 * fingerprint the frame is valid (false-positive rate ~2^-64). There is no
 * CRC in the protocol, so the exact fingerprint match IS the validation.
 *
 * The stored generic.data holds only the 64-bit fingerprint (the full 344-bit
 * payload exceeds the 64-bit generic.data). Emulate ignores the stored data
 * and always regenerates the hard-coded 43-byte constant.
 *
 * Signal reference: github.com/fredilarsen/TeslaChargeDoorOpener
 */

#define TAG "TeslaChargePort"

#define TESLA_TE          400U /* us per bit slot */
#define TESLA_RESET_GAP   2000U /* > this OFF gap ends a message (23ms inter-frame) */
#define TESLA_RESIDUAL_MAX 180U /* max |duration - n*TE| before it is noise */
#define TESLA_MAX_RUN     8U /* clamp: reject implausibly long single runs */

#define TESLA_FINGERPRINT_BITS 64
#define TESLA_FINGERPRINT      0x2B2CCB33332D34B5ULL

/* Full canonical 43-byte payload replayed verbatim on emulate. */
#define TESLA_PAYLOAD_BYTES 43U
static const uint8_t tesla_chargeport_payload[TESLA_PAYLOAD_BYTES] = {
    0x02, 0xAA, 0xAA, 0xAA, 0x2B, 0x2C, 0xCB, 0x33, 0x33, 0x2D, 0x34, 0xB5, 0x2B, 0x4D, 0x32,
    0xAD, 0x2C, 0x56, 0x59, 0x96, 0x66, 0x66, 0x5A, 0x69, 0x6A, 0x56, 0x9A, 0x65, 0x5A, 0x58,
    0xAC, 0xB3, 0x2C, 0xCC, 0xCC, 0xB4, 0xD2, 0xD4, 0xAD, 0x34, 0xCA, 0xB4, 0xA0};

static const SubGhzBlockConst subghz_protocol_tesla_chargeport_const = {
    .te_short = TESLA_TE,
    .te_long = TESLA_TE,
    .te_delta = TESLA_RESIDUAL_MAX,
    .min_count_bit_for_found = TESLA_FINGERPRINT_BITS,
};

struct SubGhzProtocolDecoderTeslaChargePort {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint64_t sr; /* rolling 64-bit reconstruction register */
    uint32_t bitcount; /* total bits clocked since last reset */
};

typedef struct SubGhzProtocolDecoderTeslaChargePort SubGhzProtocolDecoderTeslaChargePort;

// ============================================================================
// Decoder: alloc / free / reset
// ============================================================================

static void* subghz_protocol_decoder_tesla_chargeport_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderTeslaChargePort* instance =
        calloc(1, sizeof(SubGhzProtocolDecoderTeslaChargePort));
    furi_check(instance);
    instance->base.protocol = &tesla_chargeport_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

static void subghz_protocol_decoder_tesla_chargeport_free(void* context) {
    furi_check(context);
    free(context);
}

static void subghz_protocol_decoder_tesla_chargeport_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderTeslaChargePort* instance = context;
    instance->sr = 0;
    instance->bitcount = 0;
    instance->generic.data = 0;
    instance->generic.data_count_bit = 0;
}

// ============================================================================
// Decoder: feed — clocked OOK reconstruction (TE = 400us per bit slot)
// ============================================================================

static void
    subghz_protocol_decoder_tesla_chargeport_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderTeslaChargePort* instance = context;

    /* Inter-message 23ms OFF gap ends the current message. */
    if(!level && duration > TESLA_RESET_GAP) {
        instance->sr = 0;
        instance->bitcount = 0;
        return;
    }

    /* Number of 400us slots covered by this pulse (rounded). */
    uint32_t n = (duration + (TESLA_TE / 2U)) / TESLA_TE;

    /* Cheap noise reject: too short, or not a clean multiple of the bit clock. */
    if(duration < (TESLA_TE / 2U) || DURATION_DIFF(duration, n * TESLA_TE) > TESLA_RESIDUAL_MAX) {
        instance->sr = 0;
        instance->bitcount = 0;
        return;
    }

    if(n < 1U) n = 1U;
    if(n > TESLA_MAX_RUN) {
        instance->sr = 0;
        instance->bitcount = 0;
        return;
    }

    for(uint32_t k = 0; k < n; k++) {
        instance->sr = (instance->sr << 1) | (level ? 1ULL : 0ULL);
        instance->bitcount++;
        if(instance->bitcount >= (uint32_t)TESLA_FINGERPRINT_BITS &&
           instance->sr == TESLA_FINGERPRINT) {
            instance->generic.data = TESLA_FINGERPRINT;
            instance->generic.data_count_bit = TESLA_FINGERPRINT_BITS;
            instance->generic.serial = 0;
            instance->generic.btn = 0;
            instance->generic.cnt = 0;

            if(instance->base.callback) {
                instance->base.callback(&instance->base, instance->base.context);
            }

            /* Emit once per message; the remaining bits and the 5 repeats
             * must not re-fire (SubGhz also dedups by hash). */
            instance->sr = 0;
            instance->bitcount = 0;
        }
    }
}

// ============================================================================
// Decoder: hash / serialize / deserialize / get_string
// ============================================================================

static uint8_t subghz_protocol_decoder_tesla_chargeport_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderTeslaChargePort* instance = context;
    SubGhzBlockDecoder decoder = {
        .decode_data = instance->generic.data,
        .decode_count_bit = instance->generic.data_count_bit,
    };
    return subghz_protocol_blocks_get_hash_data(&decoder, (decoder.decode_count_bit / 8) + 1);
}

static SubGhzProtocolStatus subghz_protocol_decoder_tesla_chargeport_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderTeslaChargePort* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

static SubGhzProtocolStatus subghz_protocol_decoder_tesla_chargeport_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderTeslaChargePort* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_tesla_chargeport_const.min_count_bit_for_found);
}

static void
    subghz_protocol_decoder_tesla_chargeport_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderTeslaChargePort* instance = context;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Universal\r\n"
        "Key:%08lX%08lX\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long)(instance->generic.data >> 32),
        (unsigned long)(instance->generic.data & 0xFFFFFFFF));
}

// ============================================================================
// Encoder — regenerates the hard-coded 43-byte constant (fixed universal code)
// ============================================================================

#ifdef ENABLE_EMULATE_FEATURE

#define TESLA_CHARGEPORT_UPLOAD_CAPACITY 512U
_Static_assert(
    TESLA_CHARGEPORT_UPLOAD_CAPACITY <= PP_SHARED_UPLOAD_CAPACITY,
    "TESLA_CHARGEPORT_UPLOAD_CAPACITY exceeds shared upload slab");

#define TESLA_ENC_GAP_US 23000U /* inter-message 23ms OFF gap */

typedef struct SubGhzProtocolEncoderTeslaChargePort {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
} SubGhzProtocolEncoderTeslaChargePort;

static bool
    tesla_chargeport_build_upload(SubGhzProtocolEncoderTeslaChargePort* instance) {
    furi_check(instance);
    LevelDuration* up = instance->encoder.upload;
    if(!up) {
        return false;
    }

    const size_t cap = TESLA_CHARGEPORT_UPLOAD_CAPACITY;
    size_t index = 0;

    /* 344 bits, MSB-first over the canonical 43-byte payload. Level = carrier
     * on/off per 400us slot; consecutive equal bits merge into one run. */
    for(uint16_t bit_index = 0; bit_index < TESLA_PAYLOAD_BYTES * 8U; bit_index++) {
        const bool bit =
            ((tesla_chargeport_payload[bit_index >> 3U] >> (7U - (bit_index & 7U))) & 1U) != 0U;
        index = pp_emit_merge(up, index, cap, bit, TESLA_TE);
    }

    /* Trailing inter-message gap so the repeats are correctly spaced. */
    index = pp_emit_merge(up, index, cap, false, TESLA_ENC_GAP_US);

    if(index > cap) {
        return false;
    }

    instance->encoder.front = 0;
    instance->encoder.size_upload = index;
    return true;
}

static void* subghz_protocol_encoder_tesla_chargeport_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderTeslaChargePort* instance =
        calloc(1, sizeof(SubGhzProtocolEncoderTeslaChargePort));
    furi_check(instance);
    instance->base.protocol = &tesla_chargeport_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 5;
    instance->encoder.front = 0;
    instance->encoder.is_running = false;
    pp_encoder_buffer_ensure(instance, TESLA_CHARGEPORT_UPLOAD_CAPACITY);
    return instance;
}

static SubGhzProtocolStatus subghz_protocol_encoder_tesla_chargeport_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderTeslaChargePort* instance = context;
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
            subghz_protocol_tesla_chargeport_const.min_count_bit_for_found);
        if(load_st != SubGhzProtocolStatusOk) {
            break;
        }

        instance->encoder.repeat = pp_encoder_read_repeat(flipper_format, 5);

        /* Universal fixed code: the on-air frame is always the constant payload
         * regardless of the stored Key. */
        if(!tesla_chargeport_build_upload(instance)) {
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

#endif // ENABLE_EMULATE_FEATURE

// ============================================================================
// Protocol registration
// ============================================================================

const SubGhzProtocolDecoder subghz_protocol_tesla_chargeport_decoder = {
    .alloc = subghz_protocol_decoder_tesla_chargeport_alloc,
    .free = subghz_protocol_decoder_tesla_chargeport_free,
    .feed = subghz_protocol_decoder_tesla_chargeport_feed,
    .reset = subghz_protocol_decoder_tesla_chargeport_reset,
    .get_hash_data = subghz_protocol_decoder_tesla_chargeport_get_hash_data,
    .serialize = subghz_protocol_decoder_tesla_chargeport_serialize,
    .deserialize = subghz_protocol_decoder_tesla_chargeport_deserialize,
    .get_string = subghz_protocol_decoder_tesla_chargeport_get_string,
};

#ifdef ENABLE_EMULATE_FEATURE
const SubGhzProtocolEncoder subghz_protocol_tesla_chargeport_encoder = {
    .alloc = subghz_protocol_encoder_tesla_chargeport_alloc,
    .free = pp_encoder_free,
    .deserialize = subghz_protocol_encoder_tesla_chargeport_deserialize,
    .stop = pp_encoder_stop,
    .yield = pp_encoder_yield,
};
#else
const SubGhzProtocolEncoder subghz_protocol_tesla_chargeport_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};
#endif

const SubGhzProtocol tesla_chargeport_protocol = {
    .name = TESLA_CHARGEPORT_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_tesla_chargeport_decoder,
    .encoder = &subghz_protocol_tesla_chargeport_encoder,
};
