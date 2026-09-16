#include "compustar.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include "../defines.h"

/*
 * Compustar 1WG3R / 1WAMR-1900 — car remote decoder
 *
 * Ported from the rtl_433 device driver "compustar_1wg3r"
 * (src/devices/compustar_1wg3r.c, Copyright (C) 2024 Ethan Halsall, GPLv2+).
 *
 * RF:
 *   - Modulation: OOK, Pulse Width Modulation (OOK_PULSE_PWM)
 *   - short pulse (mark) ~708us  => bit 1
 *   - long  pulse (mark) ~1076us => bit 0
 *   - sync_width ~1448us / reset_limit ~1532us: a gap this long ends the frame
 *   - Fixed code (no rolling counter)
 *
 * Frame layout (36 bits, MSB first):
 *
 *     IIIIIIIIIIIIIIII xxx bbbbbbbb iiiiiiii z
 *
 *   - I: 16-bit remote ID              (frame bits 0..15)
 *   - x: 3-bit unknown, always 0b111   (frame bits 16..18)
 *   - i: 8-bit inverted button code    (frame bits 19..26)
 *   - b: 8-bit button code             (frame bits 27..34)
 *   - z: 1-bit unknown, always 0       (frame bit 35)
 *
 *   Integrity (MIC): (~button_inverse & 0xFF) == button
 *   Sanity: ID != 0x0000 and ID != 0xFFFF; unknown field == 0b111
 *
 * feed() re-implements the rtl_433 PWM slicer as a pulse-duration state
 * machine: each mark is sliced to a bit by proximity to the short/long
 * width, a long gap flushes the accumulated frame, and the recovered 36-bit
 * word is validated with the MIC + sanity checks before it is emitted. Those
 * checks reject the burst-width false positives this codebase has fought.
 */

#define TAG "Compustar"

#define COMPUSTAR_TE_SHORT 708   /* short mark => bit 1 */
#define COMPUSTAR_TE_LONG  1076  /* long mark  => bit 0 */
#define COMPUSTAR_TE_DELTA 250   /* tolerance (~30%) */
#define COMPUSTAR_PULSE_MIN 400  /* reject marks shorter than this */
#define COMPUSTAR_PULSE_MAX 1330 /* reject marks longer than this (sync lives in the gap) */
#define COMPUSTAR_GAP_END  1400  /* gap >= this ends the frame (sync 1448 / reset 1532) */
#define COMPUSTAR_MID      ((COMPUSTAR_TE_SHORT + COMPUSTAR_TE_LONG) / 2) /* 892 */
#define COMPUSTAR_BIT_COUNT 36
#define COMPUSTAR_MAX_BITS 40
#define COMPUSTAR_GAP_TX   1500 /* TX terminating gap (>= GAP_END 1400) to flush the frame on RX */

static const SubGhzBlockConst subghz_protocol_compustar_const = {
    .te_short = COMPUSTAR_TE_SHORT,
    .te_long = COMPUSTAR_TE_LONG,
    .te_delta = COMPUSTAR_TE_DELTA,
    .min_count_bit_for_found = COMPUSTAR_BIT_COUNT,
};

typedef enum {
    CompustarDecoderStepReset = 0,
    CompustarDecoderStepData,
} CompustarDecoderStep;

struct SubGhzProtocolDecoderCompustar {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
};

// ============================================================================
// Encoder (verbatim replay => PERMANENT clone; fixed code, no rolling counter)
//
// The captured 36-bit frame is self-contained: ID + always-0b111 field + the
// button and its bitwise-inverse MIC + a trailing 0. Nothing changes per press,
// so re-emitting generic.data byte-for-byte at the decoder's TE timings is a
// permanent clone the RX accepts on every button press.
// ============================================================================

#ifdef ENABLE_EMULATE_FEATURE

struct SubGhzProtocolEncoderCompustar {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};

/**
 * Build the OOK PWM upload from the decoded 36-bit frame.
 * Each bit is one mark (level high) sliced by width — short(708us)=>1,
 * long(1076us)=>0 — followed by the normal short inter-pulse gap. The final
 * gap is stretched to COMPUSTAR_GAP_TX so the RX flushes the frame.
 */
static bool subghz_protocol_encoder_compustar_get_upload(SubGhzProtocolEncoderCompustar* instance) {
    furi_assert(instance);
    size_t index = 0;
    size_t size_upload = (size_t)instance->generic.data_count_bit * 2;
    if(size_upload > instance->encoder.size_upload) {
        FURI_LOG_E(TAG, "Size upload exceeds allocated encoder buffer.");
        return false;
    }
    instance->encoder.size_upload = size_upload;

    uint64_t data = instance->generic.data & 0xFFFFFFFFFULL; /* 36 bits */
    for(uint8_t i = instance->generic.data_count_bit; i > 0; i--) {
        /* MSB-first mark: bit 1 => short mark, bit 0 => long mark */
        instance->encoder.upload[index++] = level_duration_make(
            true, bit_read(data, i - 1) ? COMPUSTAR_TE_SHORT : COMPUSTAR_TE_LONG);
        /* inter-pulse space; the last one is stretched to flush the frame */
        instance->encoder.upload[index++] =
            level_duration_make(false, (i == 1) ? COMPUSTAR_GAP_TX : COMPUSTAR_TE_SHORT);
    }
    return true;
}

void* subghz_protocol_encoder_compustar_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderCompustar* instance = malloc(sizeof(SubGhzProtocolEncoderCompustar));

    instance->base.protocol = &subghz_protocol_compustar;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = 5;
    instance->encoder.size_upload = (COMPUSTAR_MAX_BITS * 2) + 2;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_compustar_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderCompustar* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

SubGhzProtocolStatus
    subghz_protocol_encoder_compustar_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderCompustar* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    do {
        ret = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic, flipper_format, subghz_protocol_compustar_const.min_count_bit_for_found);
        if(ret != SubGhzProtocolStatusOk) {
            break;
        }
        if(instance->generic.data_count_bit != COMPUSTAR_BIT_COUNT) {
            FURI_LOG_E(TAG, "Wrong number of bits in key");
            ret = SubGhzProtocolStatusErrorValueBitCount;
            break;
        }
        // Optional value
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        if(!subghz_protocol_encoder_compustar_get_upload(instance)) {
            ret = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);

    return ret;
}

void subghz_protocol_encoder_compustar_stop(void* context) {
    SubGhzProtocolEncoderCompustar* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_compustar_yield(void* context) {
    SubGhzProtocolEncoderCompustar* instance = context;

    if(instance->encoder.repeat == 0 || !instance->encoder.is_running) {
        instance->encoder.is_running = false;
        return level_duration_reset();
    }

    LevelDuration ret = instance->encoder.upload[instance->encoder.front];

    if(++instance->encoder.front == instance->encoder.size_upload) {
        if(!subghz_block_generic_global.endless_tx) instance->encoder.repeat--;
        instance->encoder.front = 0;
    }

    return ret;
}

#endif /* ENABLE_EMULATE_FEATURE */


// ============================================================================
// Protocol registration
// ============================================================================

const SubGhzProtocolDecoder subghz_protocol_compustar_decoder = {
    .alloc = subghz_protocol_decoder_compustar_alloc,
    .free = subghz_protocol_decoder_compustar_free,
    .feed = subghz_protocol_decoder_compustar_feed,
    .reset = subghz_protocol_decoder_compustar_reset,
    .get_hash_data = subghz_protocol_decoder_compustar_get_hash_data,
    .serialize = subghz_protocol_decoder_compustar_serialize,
    .deserialize = subghz_protocol_decoder_compustar_deserialize,
    .get_string = subghz_protocol_decoder_compustar_get_string,
};

#ifdef ENABLE_EMULATE_FEATURE
const SubGhzProtocolEncoder subghz_protocol_compustar_encoder = {
    // Fixed code (no rolling counter): verbatim replay of the captured 36-bit
    // frame => PERMANENT clone.
    .alloc = subghz_protocol_encoder_compustar_alloc,
    .free = subghz_protocol_encoder_compustar_free,
    .deserialize = subghz_protocol_encoder_compustar_deserialize,
    .stop = subghz_protocol_encoder_compustar_stop,
    .yield = subghz_protocol_encoder_compustar_yield,
};
#else
const SubGhzProtocolEncoder subghz_protocol_compustar_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};
#endif

const SubGhzProtocol subghz_protocol_compustar = {
    .name = COMPUSTAR_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_compustar_decoder,
    .encoder = &subghz_protocol_compustar_encoder,
};

// ============================================================================
// Button decoding (mirrors the rtl_433 button_map)
// ============================================================================

typedef struct {
    const char* name;
    uint8_t len;
    const uint8_t* vals;
} CompustarButton;

static const uint8_t compustar_btn_lock[] =
    {0x03, 0x05, 0x09, 0x0b, 0x0d, 0x0f, 0x1f, 0x17, 0x13, 0x15, 0x19, 0x1b, 0x1d};
static const uint8_t compustar_btn_panic[] = {0x18};
static const uint8_t compustar_btn_start[] = {0x09, 0x0a, 0x0c, 0x0b, 0x0e, 0x0d, 0x04, 0x1f,
                                              0x08, 0x19, 0x1a, 0x1c, 0x1b, 0x1e, 0x1d, 0x12};
static const uint8_t compustar_btn_trunk[] =
    {0x05, 0x06, 0x0c, 0x0e, 0x0d, 0x1f, 0x17, 0x02, 0x15, 0x16, 0x1c, 0x1e, 0x1d, 0x08, 0x14};
static const uint8_t compustar_btn_unlock[] =
    {0x03, 0x06, 0x0a, 0x0b, 0x0e, 0x1f, 0x07, 0x17, 0x13, 0x16, 0x1a, 0x1b, 0x1e};
static const uint8_t compustar_btn_longpress[] = {
    0x23, 0x31, 0x13, 0x16, 0x17, 0x1a, 0x1b, 0x1e, 0x15, 0x16, 0x1c, 0x1e, 0x1d, 0x08,
    0x14, 0x08, 0x19, 0x1a, 0x1c, 0x1b, 0x1e, 0x1d, 0x12, 0x13, 0x15, 0x19, 0x1b, 0x1d};

static const CompustarButton compustar_button_map[6] = {
    {"Lock", sizeof(compustar_btn_lock), compustar_btn_lock},
    {"Panic", sizeof(compustar_btn_panic), compustar_btn_panic},
    {"Start", sizeof(compustar_btn_start), compustar_btn_start},
    {"Trunk", sizeof(compustar_btn_trunk), compustar_btn_trunk},
    {"Unlock", sizeof(compustar_btn_unlock), compustar_btn_unlock},
    {"Long Press", sizeof(compustar_btn_longpress), compustar_btn_longpress},
};

static void subghz_protocol_compustar_append_button(FuriString* output, uint8_t button) {
    uint8_t code = button & 0x7f;
    int matches = 0;
    for(int i = 0; i < 6; i++) {
        for(int j = 0; j < compustar_button_map[i].len; j++) {
            if(code == compustar_button_map[i].vals[j]) {
                if(matches) furi_string_cat_printf(output, "; ");
                furi_string_cat_printf(output, "%s", compustar_button_map[i].name);
                matches++;
                break;
            }
        }
    }
    if(!matches) furi_string_cat_printf(output, "?");
    if(button & 0x80) furi_string_cat_printf(output, "; Secondary Mode");
}

// ============================================================================
// Field extraction / validation
// ============================================================================

/* Parse generic.data (36-bit frame) into serial / btn / cnt. Returns true if
 * the MIC + sanity checks pass. */
static bool subghz_protocol_compustar_check(SubGhzBlockGeneric* instance) {
    if(instance->data_count_bit != COMPUSTAR_BIT_COUNT) return false;
    uint64_t data = instance->data & 0xFFFFFFFFFULL; /* 36 bits */

    uint16_t id = (uint16_t)((data >> 20) & 0xFFFF);
    if(id == 0x0000 || id == 0xFFFF) return false;

    /* 3-bit unknown field must be 0b111 */
    if(((data >> 17) & 0x7) != 0x7) return false;

    uint8_t button_inverse = (uint8_t)((data >> 9) & 0xFF);
    uint8_t button = (uint8_t)((data >> 1) & 0xFF);

    /* MIC: inverted button field must be the bitwise NOT of the button field */
    if((uint8_t)(~button_inverse) != button) return false;

    instance->serial = id;
    instance->btn = button;
    instance->cnt = 0; /* fixed code, no rolling counter */
    return true;
}

static void subghz_protocol_compustar_process_frame(SubGhzProtocolDecoderCompustar* instance) {
    uint16_t count = instance->decoder.decode_count_bit;

    /* rtl_433 accepts >= 35 bits; the final z bit is always 0, so normalise a
     * 35-bit capture up to the canonical 36-bit width by appending it. */
    if(count == 35) {
        instance->decoder.decode_data <<= 1;
        count = 36;
    }
    if(count != COMPUSTAR_BIT_COUNT) return;

    instance->generic.data = instance->decoder.decode_data & 0xFFFFFFFFFULL;
    instance->generic.data_count_bit = COMPUSTAR_BIT_COUNT;

    if(!subghz_protocol_compustar_check(&instance->generic)) return;

    FURI_LOG_D(
        TAG,
        "Id:%04lX Btn:%02X",
        (unsigned long)instance->generic.serial,
        instance->generic.btn);

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}







// ============================================================================
// Decoder: alloc / free / reset
// ============================================================================

void* subghz_protocol_decoder_compustar_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderCompustar* instance = malloc(sizeof(SubGhzProtocolDecoderCompustar));
    instance->base.protocol = &subghz_protocol_compustar;
    instance->generic.protocol_name = subghz_protocol_compustar.name;
    return instance;
}

void subghz_protocol_decoder_compustar_free(void* context) {
    furi_assert(context);
    free(context);
}

void subghz_protocol_decoder_compustar_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderCompustar* instance = context;
    instance->decoder.parser_step = CompustarDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
}

// ============================================================================
// Decoder: feed — PWM slicer state machine
//
// Each mark (level == true) is sliced to a bit: closer to the short width => 1,
// closer to the long width => 0; a mark outside [PULSE_MIN, PULSE_MAX] is noise
// and resets the frame. A gap (level == false) >= GAP_END flushes the frame.
// ============================================================================

void subghz_protocol_decoder_compustar_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderCompustar* instance = context;

    switch(instance->decoder.parser_step) {
    case CompustarDecoderStepReset:
        if(level && duration >= COMPUSTAR_PULSE_MIN && duration <= COMPUSTAR_PULSE_MAX) {
            /* First data pulse: start a fresh frame */
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            subghz_protocol_blocks_add_bit(
                &instance->decoder, (duration < COMPUSTAR_MID) ? 1 : 0);
            instance->decoder.parser_step = CompustarDecoderStepData;
        }
        break;

    case CompustarDecoderStepData:
        if(level) {
            /* Mark: slice to a bit */
            if(duration < COMPUSTAR_PULSE_MIN || duration > COMPUSTAR_PULSE_MAX) {
                /* Out-of-range mark: noise, drop the frame */
                instance->decoder.parser_step = CompustarDecoderStepReset;
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                break;
            }
            if(instance->decoder.decode_count_bit >= COMPUSTAR_MAX_BITS) {
                /* Overlong burst: drop */
                instance->decoder.parser_step = CompustarDecoderStepReset;
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                break;
            }
            subghz_protocol_blocks_add_bit(
                &instance->decoder, (duration < COMPUSTAR_MID) ? 1 : 0);
        } else {
            /* Space: a long gap ends the frame */
            if(duration >= COMPUSTAR_GAP_END) {
                subghz_protocol_compustar_process_frame(instance);
                instance->decoder.parser_step = CompustarDecoderStepReset;
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
            }
            /* short gaps are the normal inter-pulse spacing: ignore */
        }
        break;
    }
}

// ============================================================================
// Decoder: hash / serialize / deserialize / string
// ============================================================================

uint8_t subghz_protocol_decoder_compustar_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderCompustar* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_compustar_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderCompustar* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_compustar_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderCompustar* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_compustar_const.min_count_bit_for_found);
}

void subghz_protocol_decoder_compustar_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderCompustar* instance = context;

    subghz_protocol_compustar_check(&instance->generic);

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:0x%09llX\r\n"
        "Id:0x%04lX  Btn:0x%02X\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long long)(instance->generic.data & 0xFFFFFFFFFULL),
        (unsigned long)(instance->generic.serial & 0xFFFF),
        instance->generic.btn);

    subghz_protocol_compustar_append_button(output, instance->generic.btn);
}
