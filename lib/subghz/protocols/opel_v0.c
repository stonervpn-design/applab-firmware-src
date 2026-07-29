#include "opel_v0.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

/*
 * Opel V0 — Opel/GM keyfob decoder (experimental)
 *
 * Signal structure (433.92 MHz, OOK 650):
 *   - Carrier frequency ~1200 Hz (period ~830us per carrier cycle)
 *   - Carrier grouped into bursts of ~4 cycles (~3500us) separated by ~5400us gaps
 *   - Data encoded in burst width: long burst (~3500us) vs short burst (~2500us)
 *   - Preamble: continuous carrier >50000us
 *
 * Decoder approach:
 *   - Merge marks separated by tiny spaces (<120us) into carrier bursts
 *   - Classify bursts by total width: >threshold = 1, <=threshold = 0
 *   - Collect bits until gap >20000us (frame end)
 *
 * NOTE: This is a first-pass decoder. The exact encoding is not fully understood.
 *       The bit threshold and frame structure need refinement with more signal samples.
 */

#define TAG "OpelV0"

/* Timing constants derived from signal analysis */
#define OPEL_V0_CARRIER_PERIOD  830   /* single carrier cycle (mark + space) */
#define OPEL_V0_MERGE_THRESHOLD 120   /* spaces below this are within a carrier burst */
#define OPEL_V0_GAP_MIN         4000  /* minimum gap between bursts */
#define OPEL_V0_GAP_MAX         7000  /* maximum gap between bursts */
#define OPEL_V0_PREAMBLE_MIN    50000 /* minimum preamble carrier duration */
#define OPEL_V0_BIT_THRESHOLD   3200  /* burst width threshold: >3200 = 1, <=3200 = 0 */
#define OPEL_V0_MIN_BITS        40    /* minimum bits for a valid frame */
#define OPEL_V0_MAX_BITS        150   /* maximum bits to collect */
#define OPEL_V0_FRAME_GAP       20000 /* gap that ends a frame */

static const SubGhzBlockConst subghz_protocol_opel_v0_const = {
    .te_short = 830,
    .te_long = 5400,
    .te_delta = 300,
    .min_count_bit_for_found = OPEL_V0_MIN_BITS,
};

// ============================================================================
// Decoder state machine
// ============================================================================

typedef enum {
    OpelV0DecoderStepReset = 0,
    OpelV0DecoderStepPreamble,
    OpelV0DecoderStepData,
} OpelV0DecoderStep;

struct SubGhzProtocolDecoderOpelV0 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint32_t current_burst;  /* accumulated mark width for current burst */
    uint32_t preamble_len;   /* accumulated preamble carrier length */
    uint16_t bit_count;      /* bits collected so far */

    /* Store up to MAX_BITS burst widths for analysis */
    uint16_t burst_widths[OPEL_V0_MAX_BITS];
};

struct SubGhzProtocolEncoderOpelV0 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};

// ============================================================================
// Protocol registration
// ============================================================================

#ifdef SUBGHZ_DECODER_TEST
static SubGhzBlockGeneric* subghz_protocol_decoder_opel_v0_get_generic(void* context) {
    SubGhzProtocolDecoderOpelV0* instance = context;
    return &instance->generic;
}
#endif

const SubGhzProtocolDecoder subghz_protocol_opel_v0_decoder = {
    .alloc = subghz_protocol_decoder_opel_v0_alloc,
    .free = subghz_protocol_decoder_opel_v0_free,
    .feed = subghz_protocol_decoder_opel_v0_feed,
    .reset = subghz_protocol_decoder_opel_v0_reset,
    .get_hash_data = subghz_protocol_decoder_opel_v0_get_hash_data,
    .serialize = subghz_protocol_decoder_opel_v0_serialize,
    .deserialize = subghz_protocol_decoder_opel_v0_deserialize,
    .get_string = subghz_protocol_decoder_opel_v0_get_string,
#ifdef SUBGHZ_DECODER_TEST
    .get_generic = subghz_protocol_decoder_opel_v0_get_generic,
#endif
};

const SubGhzProtocolEncoder subghz_protocol_opel_v0_encoder = {
    .alloc = subghz_protocol_encoder_opel_v0_alloc,
    .free = subghz_protocol_encoder_opel_v0_free,
    .deserialize = subghz_protocol_encoder_opel_v0_deserialize,
    .stop = subghz_protocol_encoder_opel_v0_stop,
    .yield = subghz_protocol_encoder_opel_v0_yield,
};

const SubGhzProtocol subghz_protocol_opel_v0 = {
    .name = SUBGHZ_PROTOCOL_OPEL_V0_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_opel_v0_decoder,
    .encoder = &subghz_protocol_opel_v0_encoder,
};

// ============================================================================
// Process a completed frame
// ============================================================================

static void subghz_protocol_opel_v0_process_frame(SubGhzProtocolDecoderOpelV0* instance) {
    if(instance->bit_count < OPEL_V0_MIN_BITS) return;

    /* Classify burst widths into bits */
    uint64_t data = 0;
    uint32_t data_2 = 0;
    uint16_t count = instance->bit_count;
    if(count > 96) count = 96;

    for(uint16_t i = 0; i < count; i++) {
        uint8_t bit = (instance->burst_widths[i] > OPEL_V0_BIT_THRESHOLD) ? 1 : 0;
        if(i < 64) {
            data = (data << 1) | bit;
        } else {
            data_2 = (data_2 << 1) | bit;
        }
    }

    /* Pad data to 64 bits if shorter */
    if(count < 64) {
        data <<= (64 - count);
    }

    instance->generic.data = data;
    instance->generic.data_count_bit = count;

    /* Store extra bits in seed field */
    if(count > 64) {
        uint8_t extra = count - 64;
        data_2 <<= (32 - extra);
        instance->generic.seed = data_2;
    } else {
        instance->generic.seed = 0;
    }

    FURI_LOG_D(TAG, "Frame: %d bits, Key:%016llX", count, data);

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

// ============================================================================
// Encoder (OOK burst-width replay)
//
// Reproduces the waveform the decoder classifies back into the stored bits:
//   [long preamble carrier][for each bit: a mark burst + inter-burst gap]
// Bit value is encoded in burst width (>OPEL_V0_BIT_THRESHOLD => 1, else 0);
// bits are separated by gaps in the [GAP_MIN,GAP_MAX] window, the last by a
// >FRAME_GAP gap that ends the frame. Bit order mirrors the decoder's packing
// (data holds bits 0..63 left-aligned; seed holds the >64 tail), so this
// round-trips the decoder's Key/Seed/Bit exactly.
// ============================================================================

#define OPEL_V0_ENC_MARK_ONE    4000  /* > BIT_THRESHOLD  -> decodes as 1 */
#define OPEL_V0_ENC_MARK_ZERO   2500  /* <= BIT_THRESHOLD -> decodes as 0 */
#define OPEL_V0_ENC_BIT_GAP     5000  /* within [GAP_MIN, GAP_MAX] */
#define OPEL_V0_ENC_FRAME_GAP   25000 /* > FRAME_GAP */
#define OPEL_V0_ENC_PREAMBLE    55000 /* >= PREAMBLE_MIN */
#define OPEL_V0_ENC_MAX_BITS    96
#define OPEL_V0_ENC_UPLOAD_SIZE (2 + OPEL_V0_ENC_MAX_BITS * 2)

void* subghz_protocol_encoder_opel_v0_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderOpelV0* instance = malloc(sizeof(SubGhzProtocolEncoderOpelV0));
    instance->base.protocol = &subghz_protocol_opel_v0;
    instance->generic.protocol_name = subghz_protocol_opel_v0.name;
    instance->encoder.repeat = 3;
    instance->encoder.size_upload = OPEL_V0_ENC_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_opel_v0_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderOpelV0* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

static void subghz_protocol_encoder_opel_v0_get_upload(SubGhzProtocolEncoderOpelV0* instance) {
    uint16_t count = instance->generic.data_count_bit;
    if(count > OPEL_V0_ENC_MAX_BITS) count = OPEL_V0_ENC_MAX_BITS;
    uint64_t data = instance->generic.data;
    uint32_t seed = (uint32_t)instance->generic.seed;

    size_t idx = 0;
    const size_t enc_cap = OPEL_V0_ENC_UPLOAD_SIZE;
    // Preamble carrier + a gap to close it.
    if(idx < enc_cap)
        instance->encoder.upload[idx++] =
            level_duration_make(true, (uint32_t)OPEL_V0_ENC_PREAMBLE);
    if(idx < enc_cap)
        instance->encoder.upload[idx++] =
            level_duration_make(false, (uint32_t)OPEL_V0_ENC_BIT_GAP);

    for(uint16_t i = 0; i < count; i++) {
        uint8_t bit;
        if(i < 64) {
            bit = (uint8_t)((data >> (63 - i)) & 1);
        } else {
            bit = (uint8_t)((seed >> (31 - (i - 64))) & 1);
        }
        uint32_t mark = bit ? OPEL_V0_ENC_MARK_ONE : OPEL_V0_ENC_MARK_ZERO;
        if(idx < enc_cap)
            instance->encoder.upload[idx++] = level_duration_make(true, mark);
        uint32_t gap = (i < count - 1) ? OPEL_V0_ENC_BIT_GAP : OPEL_V0_ENC_FRAME_GAP;
        if(idx < enc_cap)
            instance->encoder.upload[idx++] = level_duration_make(false, gap);
    }

    instance->encoder.size_upload = idx;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_opel_v0_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderOpelV0* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        instance->generic.seed = 0;
        if(subghz_block_generic_deserialize(&instance->generic, flipper_format) !=
           SubGhzProtocolStatusOk) {
            break;
        }
        if(instance->generic.data_count_bit < OPEL_V0_MIN_BITS ||
           instance->generic.data_count_bit > OPEL_V0_ENC_MAX_BITS) {
            res = SubGhzProtocolStatusErrorValueBitCount;
            break;
        }
        uint32_t seed = 0;
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "Seed", &seed, 1)) {
            instance->generic.seed = seed;
        }
        flipper_format_rewind(flipper_format);
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        subghz_protocol_encoder_opel_v0_get_upload(instance);
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
        res = SubGhzProtocolStatusOk;
    } while(false);
    return res;
}

void subghz_protocol_encoder_opel_v0_stop(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderOpelV0* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_opel_v0_yield(void* context) {
    SubGhzProtocolEncoderOpelV0* instance = context;
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

// ============================================================================
// Decoder: alloc / free / reset
// ============================================================================

void* subghz_protocol_decoder_opel_v0_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderOpelV0* instance = malloc(sizeof(SubGhzProtocolDecoderOpelV0));
    instance->base.protocol = &subghz_protocol_opel_v0;
    instance->generic.protocol_name = subghz_protocol_opel_v0.name;
    return instance;
}

void subghz_protocol_decoder_opel_v0_free(void* context) {
    furi_assert(context);
    free(context);
}

void subghz_protocol_decoder_opel_v0_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderOpelV0* instance = context;
    instance->decoder.parser_step = OpelV0DecoderStepReset;
    instance->current_burst = 0;
    instance->preamble_len = 0;
    instance->bit_count = 0;
}

// ============================================================================
// Decoder: feed
//
// Strategy: accumulate mark durations. Spaces < MERGE_THRESHOLD are added to
// the current burst (they're intra-carrier gaps). Spaces in the GAP range
// (4000-7000us) terminate one burst and start the next. Spaces > FRAME_GAP
// terminate the frame.
// ============================================================================

void subghz_protocol_decoder_opel_v0_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderOpelV0* instance = context;

    switch(instance->decoder.parser_step) {
    case OpelV0DecoderStepReset:
        if(level) {
            /* Start accumulating carrier */
            instance->current_burst = duration;
            instance->preamble_len = 0;
            instance->bit_count = 0;
            instance->decoder.parser_step = OpelV0DecoderStepPreamble;
        }
        break;

    case OpelV0DecoderStepPreamble:
        if(level) {
            /* Mark: add to current burst */
            instance->current_burst += duration;
        } else {
            /* Space */
            if(duration < OPEL_V0_MERGE_THRESHOLD) {
                /* Tiny space: within carrier cycle, merge */
                instance->current_burst += duration;
            } else if(duration < OPEL_V0_GAP_MAX) {
                /* Medium gap: end of a carrier burst */
                instance->preamble_len += instance->current_burst;

                if(instance->preamble_len >= OPEL_V0_PREAMBLE_MIN) {
                    /* Preamble found, start collecting data */
                    FURI_LOG_D(TAG, "Preamble: %luus", instance->preamble_len);
                    instance->decoder.parser_step = OpelV0DecoderStepData;
                    instance->bit_count = 0;
                }
                instance->current_burst = 0;
            } else {
                /* Very large gap: reset */
                instance->decoder.parser_step = OpelV0DecoderStepReset;
            }
        }
        break;

    case OpelV0DecoderStepData:
        if(level) {
            /* Mark: accumulate into current burst */
            instance->current_burst += duration;

            /* If burst is getting very large, it's a new preamble */
            if(instance->current_burst > 10000) {
                /* End current frame */
                subghz_protocol_opel_v0_process_frame(instance);
                /* Start new preamble */
                instance->preamble_len = instance->current_burst;
                instance->current_burst = 0;
                instance->bit_count = 0;
                instance->decoder.parser_step = OpelV0DecoderStepPreamble;
            }
        } else {
            /* Space */
            if(duration < OPEL_V0_MERGE_THRESHOLD) {
                /* Tiny space: within carrier, merge */
                instance->current_burst += duration;
            } else if(duration >= OPEL_V0_GAP_MIN && duration <= OPEL_V0_GAP_MAX) {
                /* Normal data gap: end of one burst = one bit */
                if(instance->current_burst > 500 &&
                   instance->bit_count < OPEL_V0_MAX_BITS) {
                    instance->burst_widths[instance->bit_count] =
                        (instance->current_burst > 0xFFFF) ? 0xFFFF : instance->current_burst;
                    instance->bit_count++;
                }
                instance->current_burst = 0;
            } else if(duration > OPEL_V0_FRAME_GAP) {
                /* Frame end gap */
                if(instance->current_burst > 500 &&
                   instance->bit_count < OPEL_V0_MAX_BITS) {
                    instance->burst_widths[instance->bit_count] =
                        (instance->current_burst > 0xFFFF) ? 0xFFFF : instance->current_burst;
                    instance->bit_count++;
                }
                subghz_protocol_opel_v0_process_frame(instance);
                instance->current_burst = 0;
                instance->bit_count = 0;
                instance->preamble_len = 0;
                instance->decoder.parser_step = OpelV0DecoderStepReset;
            } else {
                /* Gap between preamble range and data range: could be noise */
                /* Treat as within-burst if burst is accumulating */
                if(instance->current_burst > 0) {
                    instance->current_burst += duration;
                }
            }
        }
        break;
    }
}

// ============================================================================
// Decoder: hash / serialize / deserialize
// ============================================================================

uint8_t subghz_protocol_decoder_opel_v0_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderOpelV0* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_opel_v0_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderOpelV0* instance = context;
    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    // generic_serialize only stores the 64-bit data; persist the high bits (seed,
    // shown as Key2 for >64-bit frames) or they reload as uninitialised memory.
    if(ret == SubGhzProtocolStatusOk) {
        uint32_t seed = (uint32_t)instance->generic.seed;
        if(!flipper_format_write_uint32(flipper_format, "Seed", &seed, 1)) {
            return SubGhzProtocolStatusErrorParserOthers;
        }
    }
    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_opel_v0_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderOpelV0* instance = context;
    instance->generic.seed = 0;
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_opel_v0_const.min_count_bit_for_found);
    if(ret == SubGhzProtocolStatusOk) {
        uint32_t seed = 0;
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "Seed", &seed, 1)) {
            instance->generic.seed = seed;
        }
    }
    return ret;
}

// ============================================================================
// Decoder: get_string
// ============================================================================

void subghz_protocol_decoder_opel_v0_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderOpelV0* instance = context;

    uint16_t bits = instance->generic.data_count_bit;

    if(bits <= 64) {
        furi_string_cat_printf(
            output,
            "%s %dbit\r\n"
            "Key:%016llX\r\n"
            "Hop:%016llX",
            instance->generic.protocol_name,
            bits,
            instance->generic.data,
            instance->generic.data);
    } else {
        furi_string_cat_printf(
            output,
            "%s %dbit\r\n"
            "Key:%016llX\r\n"
            "Key2:%08lX\r\n"
            "Hop:%016llX%08lX",
            instance->generic.protocol_name,
            bits,
            instance->generic.data,
            (uint32_t)(instance->generic.seed),
            instance->generic.data,
            (uint32_t)(instance->generic.seed));
    }
}
