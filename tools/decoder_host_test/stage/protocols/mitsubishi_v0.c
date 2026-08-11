#include "mitsubishi_v0.h"
#include "protocols_common.h"
#include <string.h>

// Original implementation by @lupettohf

#define MITSUBISHI_BIT_COUNT  96
#define MITSUBISHI_DATA_BYTES 12

static const SubGhzBlockConst subghz_protocol_mitsubishi_const = {
    .te_short = 250,
    .te_long = 500,
    .te_delta = 100,
    .min_count_bit_for_found = 80,
};

typedef enum {
    MitsubishiDecoderStepReset = 0,
    MitsubishiDecoderStepDataSave,
    MitsubishiDecoderStepDataCheck,
} MitsubishiDecoderStep;

struct SubGhzProtocolDecoderMitsubishi {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint8_t decoder_state;
    uint16_t bit_count;
    uint8_t decode_data[MITSUBISHI_DATA_BYTES];
    uint8_t raw_data[MITSUBISHI_DATA_BYTES];
};

static void mitsubishi_unscramble_payload(uint8_t* payload) {
    for(uint8_t i = 0; i < 8; i++) {
        payload[i] = (uint8_t)~payload[i];
    }

    uint16_t counter = ((uint16_t)payload[4] << 8) | payload[5];
    uint8_t hi = (counter >> 8) & 0xFF;
    uint8_t lo = counter & 0xFF;
    uint8_t mask1 = (hi & 0xAAU) | (lo & 0x55U);
    uint8_t mask2 = (lo & 0xAAU) | (hi & 0x55U);
    uint8_t mask3 = mask1 ^ mask2;

    // XOR the counter-derived mask over the SERIAL bytes [0..3] only. Byte [4] is
    // the counter HIGH byte and must NOT be de-masked (mask3 == hi^lo, so
    // payload[4]^mask3 would collapse it to lo, losing the real 16-bit counter).
    for(uint8_t i = 0; i < 4; i++) {
        payload[i] ^= mask3;
    }
}

static void mitsubishi_reset_payload(SubGhzProtocolDecoderMitsubishi* instance) {
    instance->bit_count = 0;
    memset(instance->decode_data, 0, sizeof(instance->decode_data));
}

static bool mitsubishi_collect_pair(
    SubGhzProtocolDecoderMitsubishi* instance,
    uint32_t high,
    uint32_t low) {
    bool bit_value;

    if(pp_is_short(high, &subghz_protocol_mitsubishi_const) &&
       pp_is_long(low, &subghz_protocol_mitsubishi_const)) {
        bit_value = true;
    } else if(
        pp_is_long(high, &subghz_protocol_mitsubishi_const) &&
        pp_is_short(low, &subghz_protocol_mitsubishi_const)) {
        bit_value = false;
    } else {
        return false;
    }

    uint16_t bit_index = instance->bit_count;
    if(bit_index < MITSUBISHI_BIT_COUNT) {
        if(bit_value) {
            uint8_t byte_index = bit_index >> 3;
            uint8_t bit_position = 7 - (bit_index & 0x07);
            instance->decode_data[byte_index] |= (1U << bit_position);
        }
        instance->bit_count++;
    }

    return true;
}

static void mitsubishi_publish_frame(SubGhzProtocolDecoderMitsubishi* instance) {
    uint8_t payload[MITSUBISHI_DATA_BYTES];
    memcpy(payload, instance->decode_data, sizeof(payload));
    // Keep the raw on-air bytes so the encoder can preserve the trailer bytes
    // (7..11) the decoder never interprets when it re-scrambles for a new count.
    memcpy(instance->raw_data, instance->decode_data, sizeof(instance->raw_data));
    mitsubishi_unscramble_payload(payload);

    instance->generic.data_count_bit = instance->bit_count;
    instance->generic.serial = ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) |
                               ((uint32_t)payload[2] << 8) | payload[3];
    instance->generic.cnt = ((uint16_t)payload[4] << 8) | payload[5];
    instance->generic.btn = payload[6];

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

const SubGhzProtocolDecoder subghz_protocol_mitsubishi_decoder = {
    .alloc = subghz_protocol_decoder_mitsubishi_alloc,
    .free = pp_decoder_free_default,
    .feed = subghz_protocol_decoder_mitsubishi_feed,
    .reset = subghz_protocol_decoder_mitsubishi_reset,
    .get_hash_data = subghz_protocol_decoder_mitsubishi_get_hash_data,
    .serialize = subghz_protocol_decoder_mitsubishi_serialize,
    .deserialize = subghz_protocol_decoder_mitsubishi_deserialize,
    .get_string = subghz_protocol_decoder_mitsubishi_get_string,
};

#if PROTOPIRATE_WITH_ENCODER

// Real rolling encoder: the scramble (bitwise-NOT + counter-derived XOR mask) is
// fully reversible and key-less, so bytes 0..6 are recomputed for a new counter.
// Bytes 7..11 are opaque to the decoder, so they are preserved from the captured
// "Raw" field (falls back to zero for legacy captures without it).
#define MITSUBISHI_TE_SHORT_TX 250U
#define MITSUBISHI_TE_LONG_TX  500U
#define MITSUBISHI_TX_GAP_US   10000U
#define MITSUBISHI_UPLOAD_CAPACITY ((MITSUBISHI_BIT_COUNT * 2U) + 2U)
_Static_assert(
    MITSUBISHI_UPLOAD_CAPACITY <= PP_SHARED_UPLOAD_CAPACITY,
    "MITSUBISHI_UPLOAD_CAPACITY exceeds shared upload slab");

typedef struct SubGhzProtocolEncoderMitsubishi {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
    uint8_t raw_data[MITSUBISHI_DATA_BYTES];
} SubGhzProtocolEncoderMitsubishi;

static void
    mitsubishi_encode_raw(uint32_t serial, uint8_t btn, uint16_t cnt, uint8_t raw[MITSUBISHI_DATA_BYTES]) {
    const uint8_t hi = (uint8_t)((cnt >> 8) & 0xFFU);
    const uint8_t lo = (uint8_t)(cnt & 0xFFU);
    const uint8_t mask1 = (uint8_t)((hi & 0xAAU) | (lo & 0x55U));
    const uint8_t mask2 = (uint8_t)((lo & 0xAAU) | (hi & 0x55U));
    const uint8_t mask3 = (uint8_t)(mask1 ^ mask2);

    raw[0] = (uint8_t)~((uint8_t)((serial >> 24) & 0xFFU) ^ mask3);
    raw[1] = (uint8_t)~((uint8_t)((serial >> 16) & 0xFFU) ^ mask3);
    raw[2] = (uint8_t)~((uint8_t)((serial >> 8) & 0xFFU) ^ mask3);
    raw[3] = (uint8_t)~((uint8_t)(serial & 0xFFU) ^ mask3);
    raw[4] = (uint8_t)~hi;
    raw[5] = (uint8_t)~lo;
    raw[6] = (uint8_t)~btn;
    // raw[7..11] preserved by caller.
}

static bool mitsubishi_build_upload(SubGhzProtocolEncoderMitsubishi* instance) {
    furi_check(instance);
    LevelDuration* up = instance->encoder.upload;
    const size_t cap = MITSUBISHI_UPLOAD_CAPACITY;
    size_t i = 0;

    for(uint8_t bit = 0; bit < MITSUBISHI_BIT_COUNT; bit++) {
        const uint8_t byte = instance->raw_data[bit >> 3];
        const bool one = (byte >> (7U - (bit & 7U))) & 1U;
        // decoder: bit 1 = (short high, long low); bit 0 = (long high, short low)
        if(one) {
            i = pp_emit(up, i, cap, true, MITSUBISHI_TE_SHORT_TX);
            i = pp_emit(up, i, cap, false, MITSUBISHI_TE_LONG_TX);
        } else {
            i = pp_emit(up, i, cap, true, MITSUBISHI_TE_LONG_TX);
            i = pp_emit(up, i, cap, false, MITSUBISHI_TE_SHORT_TX);
        }
    }
    i = pp_emit(up, i, cap, false, MITSUBISHI_TX_GAP_US);

    instance->encoder.front = 0;
    instance->encoder.size_upload = i;
    return i > 0;
}

void* subghz_protocol_encoder_mitsubishi_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderMitsubishi* instance = calloc(1, sizeof(SubGhzProtocolEncoderMitsubishi));
    furi_check(instance);
    instance->base.protocol = &mitsubishi_v0_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 8;
    instance->encoder.front = 0;
    instance->encoder.is_running = false;
    pp_encoder_buffer_ensure(instance, MITSUBISHI_UPLOAD_CAPACITY);
    return instance;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_mitsubishi_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderMitsubishi* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    do {
        if(pp_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
           SubGhzProtocolStatusOk) {
            break;
        }

        flipper_format_rewind(flipper_format);
        if(subghz_block_generic_deserialize_check_count_bit(
               &instance->generic,
               flipper_format,
               subghz_protocol_mitsubishi_const.min_count_bit_for_found) !=
           SubGhzProtocolStatusOk) {
            break;
        }

        uint32_t serial = instance->generic.serial;
        uint32_t btn = instance->generic.btn;
        uint32_t cnt = instance->generic.cnt;
        pp_encoder_read_fields(flipper_format, &serial, &btn, &cnt, NULL);
        instance->generic.serial = serial;
        instance->generic.btn = (uint8_t)btn;
        instance->generic.cnt = cnt;

        memset(instance->raw_data, 0, sizeof(instance->raw_data));
        flipper_format_rewind(flipper_format);
        flipper_format_read_hex(flipper_format, "Raw", instance->raw_data, MITSUBISHI_DATA_BYTES);
        mitsubishi_encode_raw(serial, (uint8_t)btn, (uint16_t)cnt, instance->raw_data);

        instance->encoder.repeat = pp_encoder_read_repeat(flipper_format, 8);

        if(!mitsubishi_build_upload(instance) || instance->encoder.size_upload == 0) {
            break;
        }

        instance->encoder.is_running = true;
        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

const SubGhzProtocolEncoder subghz_protocol_mitsubishi_encoder = {
    .alloc = subghz_protocol_encoder_mitsubishi_alloc,
    .free = pp_encoder_free,
    .deserialize = subghz_protocol_encoder_mitsubishi_deserialize,
    .stop = pp_encoder_stop,
    .yield = pp_encoder_yield,
};
#else
const SubGhzProtocolEncoder subghz_protocol_mitsubishi_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};
#endif

const SubGhzProtocol mitsubishi_v0_protocol = {
    .name = MITSUBISHI_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save
#if PROTOPIRATE_WITH_ENCODER
            | SubGhzProtocolFlag_Send
#endif
    ,
#if PROTOPIRATE_WITH_DECODER
    .decoder = &subghz_protocol_mitsubishi_decoder,
#else
    .decoder = NULL,
#endif
#if PROTOPIRATE_WITH_ENCODER
    .encoder = &subghz_protocol_mitsubishi_encoder,
#else
    .encoder = NULL,
#endif
};

void* subghz_protocol_decoder_mitsubishi_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderMitsubishi* instance = calloc(1, sizeof(SubGhzProtocolDecoderMitsubishi));
    furi_check(instance);
    instance->base.protocol = &mitsubishi_v0_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_mitsubishi_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderMitsubishi* instance = context;
    instance->decoder_state = MitsubishiDecoderStepReset;
    instance->decoder.te_last = 0;
    instance->generic.data_count_bit = 0;
    mitsubishi_reset_payload(instance);
}

void subghz_protocol_decoder_mitsubishi_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderMitsubishi* instance = context;

    switch(instance->decoder_state) {
    case MitsubishiDecoderStepReset:
        if(level) {
            instance->decoder.te_last = duration;
            instance->decoder_state = MitsubishiDecoderStepDataCheck;
        }
        break;

    case MitsubishiDecoderStepDataSave:
        if(level) {
            instance->decoder.te_last = duration;
            instance->decoder_state = MitsubishiDecoderStepDataCheck;
        } else {
            instance->decoder_state = MitsubishiDecoderStepReset;
            mitsubishi_reset_payload(instance);
        }
        break;

    case MitsubishiDecoderStepDataCheck:
        if(!level) {
            if(mitsubishi_collect_pair(instance, instance->decoder.te_last, duration)) {
                if(instance->bit_count >= MITSUBISHI_BIT_COUNT) {
                    mitsubishi_publish_frame(instance);
                    mitsubishi_reset_payload(instance);
                    instance->decoder_state = MitsubishiDecoderStepReset;
                } else {
                    instance->decoder_state = MitsubishiDecoderStepDataSave;
                }
            } else {
                mitsubishi_reset_payload(instance);
                instance->decoder_state = MitsubishiDecoderStepReset;
            }
        } else {
            instance->decoder.te_last = duration;
        }
        break;
    }
}

uint8_t subghz_protocol_decoder_mitsubishi_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderMitsubishi* instance = context;
    uint8_t hash = 0;
    for(size_t i = 0; i < sizeof(instance->decode_data); i++) {
        hash ^= instance->decode_data[i];
    }
    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_mitsubishi_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderMitsubishi* instance = context;
    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret == SubGhzProtocolStatusOk) {
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_hex(
            flipper_format, "Raw", instance->raw_data, MITSUBISHI_DATA_BYTES);
        pp_serialize_fields(
            flipper_format,
            PP_FIELD_SERIAL | PP_FIELD_BTN | PP_FIELD_CNT,
            instance->generic.serial,
            instance->generic.btn,
            instance->generic.cnt,
            0);
    }
    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_mitsubishi_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderMitsubishi* instance = context;
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_mitsubishi_const.min_count_bit_for_found);

    if(ret == SubGhzProtocolStatusOk) {
        uint32_t btn = instance->generic.btn;
        pp_encoder_read_fields(
            flipper_format, &instance->generic.serial, &btn, &instance->generic.cnt, NULL);
        instance->generic.btn = (uint8_t)btn;
    }

    return ret;
}

void subghz_protocol_decoder_mitsubishi_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderMitsubishi* instance = context;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Sn:%08lX Cnt:%04lX\r\n"
        "Btn:%02X\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        instance->generic.serial,
        instance->generic.cnt,
        instance->generic.btn);
}
