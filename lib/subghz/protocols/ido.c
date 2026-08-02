#include "ido.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#define TAG "SubGhzProtocolIdo117/111"

static const SubGhzBlockConst subghz_protocol_ido_const = {
    .te_short = 450,
    .te_long = 1450,
    .te_delta = 150,
    .min_count_bit_for_found = 48,
};

struct SubGhzProtocolDecoderIDo {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
};

struct SubGhzProtocolEncoderIDo {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};

typedef enum {
    IDoDecoderStepReset = 0,
    IDoDecoderStepFoundPreambula,
    IDoDecoderStepSaveDuration,
    IDoDecoderStepCheckDuration,
} IDoDecoderStep;

const SubGhzProtocolDecoder subghz_protocol_ido_decoder = {
    .alloc = subghz_protocol_decoder_ido_alloc,
    .free = subghz_protocol_decoder_ido_free,

    .feed = subghz_protocol_decoder_ido_feed,
    .reset = subghz_protocol_decoder_ido_reset,

    .get_hash_data = subghz_protocol_decoder_ido_get_hash_data,
    .deserialize = subghz_protocol_decoder_ido_deserialize,
    .serialize = subghz_protocol_decoder_ido_serialize,
    .get_string = subghz_protocol_decoder_ido_get_string,
};

// Encoder (replay). iDo is OOK/AM PWM: preamble HIGH/LOW ~te_short*10, then per
// bit HIGH te_short followed by LOW te_long (bit 0) or LOW te_short (bit 1);
// a trailing long HIGH triggers the decoder's publish. Reproduces exactly the
// waveform subghz_protocol_decoder_ido_feed accepts, so encode->decode is a
// bit-exact round-trip of generic.data / data_count_bit.
static void* subghz_protocol_encoder_ido_alloc(SubGhzEnvironment* environment);
static void subghz_protocol_encoder_ido_free(void* context);
static SubGhzProtocolStatus
    subghz_protocol_encoder_ido_deserialize(void* context, FlipperFormat* flipper_format);
static void subghz_protocol_encoder_ido_stop(void* context);
static LevelDuration subghz_protocol_encoder_ido_yield(void* context);

const SubGhzProtocolEncoder subghz_protocol_ido_encoder = {
    .alloc = subghz_protocol_encoder_ido_alloc,
    .free = subghz_protocol_encoder_ido_free,

    .deserialize = subghz_protocol_encoder_ido_deserialize,
    .stop = subghz_protocol_encoder_ido_stop,
    .yield = subghz_protocol_encoder_ido_yield,
};

const SubGhzProtocol subghz_protocol_ido = {
    .name = SUBGHZ_PROTOCOL_IDO_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,

    .decoder = &subghz_protocol_ido_decoder,
    .encoder = &subghz_protocol_ido_encoder,
};

void* subghz_protocol_decoder_ido_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderIDo* instance = malloc(sizeof(SubGhzProtocolDecoderIDo));
    instance->base.protocol = &subghz_protocol_ido;
    instance->generic.protocol_name = instance->base.protocol->name;

    return instance;
}

void subghz_protocol_decoder_ido_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderIDo* instance = context;
    free(instance);
}

void subghz_protocol_decoder_ido_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderIDo* instance = context;
    instance->decoder.parser_step = IDoDecoderStepReset;
}

void subghz_protocol_decoder_ido_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderIDo* instance = context;

    switch(instance->decoder.parser_step) {
    case IDoDecoderStepReset:
        if((level) && (DURATION_DIFF(duration, subghz_protocol_ido_const.te_short * 10) <
                       subghz_protocol_ido_const.te_delta * 5)) {
            instance->decoder.parser_step = IDoDecoderStepFoundPreambula;
        }
        break;
    case IDoDecoderStepFoundPreambula:
        if((!level) && (DURATION_DIFF(duration, subghz_protocol_ido_const.te_short * 10) <
                        subghz_protocol_ido_const.te_delta * 5)) {
            //Found Preambula
            instance->decoder.parser_step = IDoDecoderStepSaveDuration;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
        } else {
            instance->decoder.parser_step = IDoDecoderStepReset;
        }
        break;
    case IDoDecoderStepSaveDuration:
        if(level) {
            if(duration >= ((uint32_t)subghz_protocol_ido_const.te_short * 5 +
                            subghz_protocol_ido_const.te_delta)) {
                instance->decoder.parser_step = IDoDecoderStepFoundPreambula;
                /* Require the exact bit count (siblings use ==). With >=, a long
                 * burst overflows the uint64 decode_data past 64 bits and
                 * get_string's shift-by-(nbits-1) becomes undefined behavior. */
                if(instance->decoder.decode_count_bit ==
                   subghz_protocol_ido_const.min_count_bit_for_found) {
                    instance->generic.data = instance->decoder.decode_data;
                    instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                    if(instance->base.callback)
                        instance->base.callback(&instance->base, instance->base.context);
                }
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                break;
            } else {
                instance->decoder.te_last = duration;
                instance->decoder.parser_step = IDoDecoderStepCheckDuration;
            }

        } else {
            instance->decoder.parser_step = IDoDecoderStepReset;
        }
        break;
    case IDoDecoderStepCheckDuration:
        if(!level) {
            if((DURATION_DIFF(instance->decoder.te_last, subghz_protocol_ido_const.te_short) <
                subghz_protocol_ido_const.te_delta) &&
               (DURATION_DIFF(duration, subghz_protocol_ido_const.te_long) <
                subghz_protocol_ido_const.te_delta * 3)) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 0);
                instance->decoder.parser_step = IDoDecoderStepSaveDuration;
            } else if(
                (DURATION_DIFF(instance->decoder.te_last, subghz_protocol_ido_const.te_short) <
                 subghz_protocol_ido_const.te_delta * 3) &&
                (DURATION_DIFF(duration, subghz_protocol_ido_const.te_short) <
                 subghz_protocol_ido_const.te_delta)) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 1);
                instance->decoder.parser_step = IDoDecoderStepSaveDuration;
            } else {
                instance->decoder.parser_step = IDoDecoderStepReset;
            }
        } else {
            instance->decoder.parser_step = IDoDecoderStepReset;
        }
        break;
    }
}

/** 
 * Analysis of received data
 * @param instance Pointer to a SubGhzBlockGeneric* instance
 */
static void subghz_protocol_ido_check_remote_controller(SubGhzBlockGeneric* instance) {
    uint64_t code_found_reverse =
        subghz_protocol_blocks_reverse_key(instance->data, instance->data_count_bit);
    uint32_t code_fix = code_found_reverse & 0xFFFFFF;

    instance->serial = code_fix & 0xFFFFF;
    instance->btn = (code_fix >> 20) & 0x0F;
}

uint8_t subghz_protocol_decoder_ido_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderIDo* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_ido_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderIDo* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_ido_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderIDo* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, subghz_protocol_ido_const.min_count_bit_for_found);
}

void subghz_protocol_decoder_ido_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderIDo* instance = context;

    subghz_protocol_ido_check_remote_controller(&instance->generic);
    uint64_t code_found_reverse = subghz_protocol_blocks_reverse_key(
        instance->generic.data, instance->generic.data_count_bit);
    uint32_t code_fix = code_found_reverse & 0xFFFFFF;
    uint32_t code_hop = (code_found_reverse >> 24) & 0xFFFFFF;

    // push protocol data to global variable
    subghz_block_generic_global.btn_is_available = false;
    subghz_block_generic_global.current_btn = instance->generic.btn;
    subghz_block_generic_global.btn_length_bit = 4;
    //

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:0x%lX%08lX\r\n"
        "Fix:%06lX \r\n"
        "Hop:%06lX \r\n"
        "Sn:%05lX Btn:%X\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (uint32_t)(instance->generic.data >> 32),
        (uint32_t)instance->generic.data,
        code_fix,
        code_hop,
        instance->generic.serial,
        instance->generic.btn);
}

// ============================================================================
// ENCODER (replay)
// ============================================================================

#define IDO_ENCODER_UPLOAD_MAX 160

static void* subghz_protocol_encoder_ido_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderIDo* instance = malloc(sizeof(SubGhzProtocolEncoderIDo));
    instance->base.protocol = &subghz_protocol_ido;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 3;
    instance->encoder.size_upload = IDO_ENCODER_UPLOAD_MAX;
    instance->encoder.upload = malloc(IDO_ENCODER_UPLOAD_MAX * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

static void subghz_protocol_encoder_ido_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderIDo* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

/**
 * Build one frame into the upload buffer.
 * [PREAMBLE H*10 L*10][ per bit: H te_short, L (te_long if 0 / te_short if 1) ]
 * [terminator H*10 L*10] (long HIGH publishes; pair also seeds the next repeat).
 * Bits are MSB-first out of generic.data, exactly as the decoder reassembles them.
 */
static void subghz_protocol_encoder_ido_get_upload(SubGhzProtocolEncoderIDo* instance) {
    furi_assert(instance);
    size_t index = 0;
    const size_t enc_cap = IDO_ENCODER_UPLOAD_MAX;
    uint32_t te_short = subghz_protocol_ido_const.te_short;
    uint32_t te_long = subghz_protocol_ido_const.te_long;
    size_t nbits = instance->generic.data_count_bit;

    if(index < enc_cap)
        instance->encoder.upload[index++] = level_duration_make(true, te_short * 10);
    if(index < enc_cap)
        instance->encoder.upload[index++] = level_duration_make(false, te_short * 10);

    for(size_t i = 0; i < nbits; i++) {
        bool bit = (instance->generic.data >> (nbits - 1 - i)) & 1;
        if(index < enc_cap)
            instance->encoder.upload[index++] = level_duration_make(true, te_short);
        if(index < enc_cap)
            instance->encoder.upload[index++] =
                level_duration_make(false, bit ? te_short : te_long);
    }

    if(index < enc_cap)
        instance->encoder.upload[index++] = level_duration_make(true, te_short * 10);
    if(index < enc_cap)
        instance->encoder.upload[index++] = level_duration_make(false, te_short * 10);

    instance->encoder.size_upload = index;
}

static SubGhzProtocolStatus
    subghz_protocol_encoder_ido_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderIDo* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic, flipper_format, subghz_protocol_ido_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) break;

        // generic.data is a uint64; a bit count above 64 would overflow the buffer.
        if(instance->generic.data_count_bit > 64) {
            FURI_LOG_E(TAG, "Bit count too large: %u", instance->generic.data_count_bit);
            res = SubGhzProtocolStatusErrorValueBitCount;
            break;
        }

        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        subghz_protocol_encoder_ido_get_upload(instance);
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
        res = SubGhzProtocolStatusOk;
    } while(false);
    return res;
}

static void subghz_protocol_encoder_ido_stop(void* context) {
    SubGhzProtocolEncoderIDo* instance = context;
    instance->encoder.is_running = false;
}

static LevelDuration subghz_protocol_encoder_ido_yield(void* context) {
    SubGhzProtocolEncoderIDo* instance = context;

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
