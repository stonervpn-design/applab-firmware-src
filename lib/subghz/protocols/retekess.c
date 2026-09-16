#include "retekess.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

/*
 * Retekess restaurant / coaster pager (T111 / T119 family).
 * OOK PWM, 433.92 MHz, 25-bit FIXED code, repeated ~44x.
 *   bit 1 = HIGH te_long  + LOW te_short   (long-high)
 *   bit 0 = HIGH te_short + LOW te_long     (short-high)
 * Frames are separated by a long carrier-off gap (>~2 ms, typically ~8.5 ms).
 * The 25-bit code = 16-bit station id (high) + 9-bit pager number (low).
 * Reverse-engineered from real captures (Pager1=0x1442100, Pager2=0x1442080).
 */

#define TAG "SubGhzProtocolRetekess"

// Frame-boundary / preamble gap: any LOW longer than this ends/starts a frame.
// The largest in-frame LOW is te_long (~950us for a bit-0), so te_long*2 cleanly
// separates data bits from the inter-frame gap.
#define RETEKESS_TE_GAP_MIN (RETEKESS_TE_LONG * 2)
// Gap the encoder emits between frames (matches the real remote's carrier-off).
#define RETEKESS_TE_GAP_TX  8500
#define RETEKESS_TE_SHORT   300
#define RETEKESS_TE_LONG    950
#define RETEKESS_UPLOAD_MAX 128

static const SubGhzBlockConst subghz_protocol_retekess_const = {
    .te_short = RETEKESS_TE_SHORT,
    .te_long = RETEKESS_TE_LONG,
    // Tight delta: Retekess is 300/950; a looser window let generic PWM remotes
    // (PT2260/Princeton at 390/1170, fans, doorbells) cross-decode. 150 still
    // covers Retekess's real spread (~283-321 short, ~941-1032 long) but rejects
    // the 1170us long pulse of the PT2260 family.
    .te_delta = 150,
    .min_count_bit_for_found = 25,
};

struct SubGhzProtocolDecoderRetekess {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint32_t last_data;
    uint32_t te;
};

struct SubGhzProtocolEncoderRetekess {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};

typedef enum {
    RetekessDecoderStepReset = 0,
    RetekessDecoderStepSaveDuration,
    RetekessDecoderStepCheckDuration,
} RetekessDecoderStep;

const SubGhzProtocolDecoder subghz_protocol_retekess_decoder = {
    .alloc = subghz_protocol_decoder_retekess_alloc,
    .free = subghz_protocol_decoder_retekess_free,

    .feed = subghz_protocol_decoder_retekess_feed,
    .reset = subghz_protocol_decoder_retekess_reset,

    .get_hash_data = subghz_protocol_decoder_retekess_get_hash_data,
    .serialize = subghz_protocol_decoder_retekess_serialize,
    .deserialize = subghz_protocol_decoder_retekess_deserialize,
    .get_string = subghz_protocol_decoder_retekess_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_retekess_encoder = {
    .alloc = subghz_protocol_encoder_retekess_alloc,
    .free = subghz_protocol_encoder_retekess_free,

    .deserialize = subghz_protocol_encoder_retekess_deserialize,
    .stop = subghz_protocol_encoder_retekess_stop,
    .yield = subghz_protocol_encoder_retekess_yield,
};

const SubGhzProtocol subghz_protocol_retekess = {
    .name = SUBGHZ_PROTOCOL_RETEKESS_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,

    .decoder = &subghz_protocol_retekess_decoder,
    .encoder = &subghz_protocol_retekess_encoder,
};

// ============================================================================
// ENCODER
// ============================================================================

void* subghz_protocol_encoder_retekess_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderRetekess* instance = malloc(sizeof(SubGhzProtocolEncoderRetekess));
    instance->base.protocol = &subghz_protocol_retekess;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 20;
    instance->encoder.size_upload = RETEKESS_UPLOAD_MAX;
    instance->encoder.upload = malloc(RETEKESS_UPLOAD_MAX * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_retekess_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderRetekess* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

static bool subghz_protocol_encoder_retekess_get_upload(SubGhzProtocolEncoderRetekess* instance) {
    furi_assert(instance);
    const uint32_t te_s = RETEKESS_TE_SHORT;
    const uint32_t te_l = RETEKESS_TE_LONG;
    size_t index = 0;

    // Two identical frames in one upload — the decoder only fires the callback
    // after two consecutive matching frames (anti-noise), so a single upload
    // cycle is self-sufficient (both on-air and for host round-trip).
    // Leading gap so the decoder's reset arms on the first frame too.
    instance->encoder.upload[index++] = level_duration_make(false, RETEKESS_TE_GAP_TX);

    for(uint8_t f = 0; f < 2; f++) {
        for(uint8_t i = instance->generic.data_count_bit; i > 0; i--) {
            bool b = bit_read(instance->generic.data, i - 1);
            if(i > 1) {
                if(b) {
                    // bit 1: long-high, short-low
                    instance->encoder.upload[index++] = level_duration_make(true, te_l);
                    instance->encoder.upload[index++] = level_duration_make(false, te_s);
                } else {
                    // bit 0: short-high, long-low
                    instance->encoder.upload[index++] = level_duration_make(true, te_s);
                    instance->encoder.upload[index++] = level_duration_make(false, te_l);
                }
            } else {
                // Last (LSB) bit: HIGH only — on the real remote its LOW is
                // swallowed by the following carrier-off gap, so the bit is
                // conveyed by the HIGH width alone (long=1 / short=0).
                instance->encoder.upload[index++] = level_duration_make(true, b ? te_l : te_s);
            }
        }
        // Inter-frame carrier-off gap (also absorbs the last bit's LOW). Triggers
        // the decoder's frame-boundary commit.
        instance->encoder.upload[index++] = level_duration_make(false, RETEKESS_TE_GAP_TX);
    }

    instance->encoder.size_upload = index;
    instance->encoder.front = 0;
    return true;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_retekess_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderRetekess* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    do {
        ret = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_retekess_const.min_count_bit_for_found);
        if(ret != SubGhzProtocolStatusOk) {
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);
        if(!subghz_protocol_encoder_retekess_get_upload(instance)) {
            ret = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.is_running = true;
    } while(false);
    return ret;
}

void subghz_protocol_encoder_retekess_stop(void* context) {
    SubGhzProtocolEncoderRetekess* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_retekess_yield(void* context) {
    SubGhzProtocolEncoderRetekess* instance = context;

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
// DECODER
// ============================================================================

void* subghz_protocol_decoder_retekess_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderRetekess* instance = malloc(sizeof(SubGhzProtocolDecoderRetekess));
    instance->base.protocol = &subghz_protocol_retekess;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->last_data = 0;
    instance->te = RETEKESS_TE_SHORT;
    return instance;
}

void subghz_protocol_decoder_retekess_free(void* context) {
    furi_assert(context);
    free(context);
}

void subghz_protocol_decoder_retekess_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderRetekess* instance = context;
    instance->decoder.parser_step = RetekessDecoderStepReset;
    instance->last_data = 0;
}

void subghz_protocol_decoder_retekess_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderRetekess* instance = context;
    const uint32_t te_s = subghz_protocol_retekess_const.te_short;
    const uint32_t te_l = subghz_protocol_retekess_const.te_long;
    const uint32_t te_d = subghz_protocol_retekess_const.te_delta;

    switch(instance->decoder.parser_step) {
    case RetekessDecoderStepReset:
        // Arm on a long carrier-off gap (frame boundary).
        if((!level) && (duration > RETEKESS_TE_GAP_MIN)) {
            instance->decoder.parser_step = RetekessDecoderStepSaveDuration;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
        }
        break;

    case RetekessDecoderStepSaveDuration:
        if(level) {
            instance->decoder.te_last = duration;
            instance->decoder.parser_step = RetekessDecoderStepCheckDuration;
        }
        break;

    case RetekessDecoderStepCheckDuration:
        if(!level) {
            if(duration > RETEKESS_TE_GAP_MIN) {
                // The final (LSB) data bit's LOW merged into this carrier-off gap,
                // so the trailing HIGH (te_last) is still unpaired — classify it by
                // width and add it as the 25th bit before committing.
                if(DURATION_DIFF(instance->decoder.te_last, te_l) < te_d) {
                    subghz_protocol_blocks_add_bit(&instance->decoder, 1);
                } else if(DURATION_DIFF(instance->decoder.te_last, te_s) < te_d) {
                    subghz_protocol_blocks_add_bit(&instance->decoder, 0);
                }
                // Frame boundary: commit if a full 25-bit frame matched the
                // previous one (two-consecutive-frame anti-noise gate).
                if(instance->decoder.decode_count_bit ==
                   subghz_protocol_retekess_const.min_count_bit_for_found) {
                    if((instance->last_data == instance->decoder.decode_data) &&
                       instance->decoder.decode_data) {
                        instance->generic.data = instance->decoder.decode_data;
                        instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                        instance->te = te_s;
                        if(instance->base.callback)
                            instance->base.callback(&instance->base, instance->base.context);
                    }
                    instance->last_data = instance->decoder.decode_data;
                }
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->decoder.parser_step = RetekessDecoderStepSaveDuration;
                break;
            }
            if((DURATION_DIFF(instance->decoder.te_last, te_s) < te_d) &&
               (DURATION_DIFF(duration, te_l) < te_d)) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 0);
                instance->decoder.parser_step = RetekessDecoderStepSaveDuration;
            } else if(
                (DURATION_DIFF(instance->decoder.te_last, te_l) < te_d) &&
                (DURATION_DIFF(duration, te_s) < te_d)) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 1);
                instance->decoder.parser_step = RetekessDecoderStepSaveDuration;
            } else {
                instance->decoder.parser_step = RetekessDecoderStepReset;
            }
        } else {
            instance->decoder.parser_step = RetekessDecoderStepReset;
        }
        break;
    }
}

uint8_t subghz_protocol_decoder_retekess_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderRetekess* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_retekess_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderRetekess* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_retekess_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderRetekess* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_retekess_const.min_count_bit_for_found);
}

void subghz_protocol_decoder_retekess_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderRetekess* instance = context;
    uint32_t code = (uint32_t)(instance->generic.data & 0x1FFFFFF);
    uint32_t station = (code >> 9) & 0xFFFF;
    uint32_t pager = code & 0x1FF;
    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:0x%07lX\r\n"
        "Station:0x%04lX Pager:%lu\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        code,
        station,
        pager);
}
