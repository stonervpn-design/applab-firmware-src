#include "mitsubishi_v0.h"
#include "protocols_common.h"
#include <string.h>

// Original implementation by @lupettohf

#define MITSUBISHI_BIT_COUNT  96
#define MITSUBISHI_DATA_BYTES 12

#define MITSUBISHI_UPLOAD_CAPACITY (MITSUBISHI_BIT_COUNT * 2U)
_Static_assert(
    MITSUBISHI_UPLOAD_CAPACITY <= PP_SHARED_UPLOAD_CAPACITY,
    "MITSUBISHI_UPLOAD_CAPACITY exceeds shared upload slab");

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

    for(uint8_t i = 0; i < 5; i++) {
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

#ifdef ENABLE_EMULATE_FEATURE
void* subghz_protocol_encoder_mitsubishi_alloc(SubGhzEnvironment* environment);
SubGhzProtocolStatus
    subghz_protocol_encoder_mitsubishi_deserialize(void* context, FlipperFormat* flipper_format);

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
    .flag = SubGhzProtocolFlag_868 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_mitsubishi_decoder,
    .encoder = &subghz_protocol_mitsubishi_encoder,
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

// ============================================================================
// ENCODER IMPLEMENTATION
// ============================================================================
#ifdef ENABLE_EMULATE_FEATURE

typedef struct SubGhzProtocolEncoderMitsubishi {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
} SubGhzProtocolEncoderMitsubishi;

// Inverse of mitsubishi_unscramble_payload(): turn the clear fields
// (serial in [0..3], counter in [4..5], button in [6]) into the on-air bytes.
// The decoder derives its de-scramble mask solely from the post-inversion
// bytes [4] and [5], and that mask reduces algebraically to (A[4] ^ A[5]).
// Because of this the decoded counter always has equal high and low bytes, so
// we rebuild the frame with the mask forced to zero (byte[4] == byte[5]); the
// XOR stage then vanishes and only the bitwise inversion of bytes [0..7]
// remains to undo.
static void mitsubishi_scramble_payload(uint8_t* payload) {
    payload[4] = payload[5];
    for(uint8_t i = 0; i < 8; i++) {
        payload[i] = (uint8_t)~payload[i];
    }
}

void* subghz_protocol_encoder_mitsubishi_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderMitsubishi* instance =
        malloc(sizeof(SubGhzProtocolEncoderMitsubishi));

    instance->base.protocol = &mitsubishi_v0_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = 10;
    pp_encoder_buffer_ensure(instance, MITSUBISHI_UPLOAD_CAPACITY);
    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    instance->generic.serial = 0;
    instance->generic.btn = 0;
    instance->generic.cnt = 0;

    return instance;
}

static void
    subghz_protocol_encoder_mitsubishi_get_upload(SubGhzProtocolEncoderMitsubishi* instance) {
    furi_check(instance);

    const uint32_t te_short = subghz_protocol_mitsubishi_const.te_short;
    const uint32_t te_long = subghz_protocol_mitsubishi_const.te_long;

    uint8_t payload[MITSUBISHI_DATA_BYTES] = {0};
    payload[0] = (uint8_t)(instance->generic.serial >> 24);
    payload[1] = (uint8_t)(instance->generic.serial >> 16);
    payload[2] = (uint8_t)(instance->generic.serial >> 8);
    payload[3] = (uint8_t)(instance->generic.serial);
    payload[4] = (uint8_t)(instance->generic.cnt >> 8);
    payload[5] = (uint8_t)(instance->generic.cnt);
    payload[6] = instance->generic.btn;

    mitsubishi_scramble_payload(payload);

    LevelDuration* up = instance->encoder.upload;
    const size_t cap = MITSUBISHI_UPLOAD_CAPACITY;
    size_t index = 0;

    for(uint16_t bit_index = 0; bit_index < MITSUBISHI_BIT_COUNT; bit_index++) {
        uint8_t byte_index = bit_index >> 3;
        uint8_t bit_position = 7 - (bit_index & 0x07);
        bool bit = (payload[byte_index] >> bit_position) & 1;

        if(bit) {
            index = pp_emit(up, index, cap, true, te_short);
            index = pp_emit(up, index, cap, false, te_long);
        } else {
            index = pp_emit(up, index, cap, true, te_long);
            index = pp_emit(up, index, cap, false, te_short);
        }
    }

    instance->encoder.size_upload = index;
    instance->encoder.front = 0;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_mitsubishi_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderMitsubishi* instance = context;

    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    // The decoder serializes the real 96-bit frame length, so the encoder must
    // check against MITSUBISHI_BIT_COUNT (not min_count_bit_for_found, which is
    // set lower and would reject a genuine saved key on the exact-match test).
    if(subghz_block_generic_deserialize_check_count_bit(
           &instance->generic, flipper_format, MITSUBISHI_BIT_COUNT) != SubGhzProtocolStatusOk) {
        return SubGhzProtocolStatusError;
    }

    uint32_t btn = instance->generic.btn;
    pp_encoder_read_fields(
        flipper_format, &instance->generic.serial, &btn, &instance->generic.cnt, NULL);
    instance->generic.btn = (uint8_t)btn;

    instance->encoder.repeat = pp_encoder_read_repeat(flipper_format, 10);

    pp_encoder_buffer_ensure(instance, MITSUBISHI_UPLOAD_CAPACITY);
    subghz_protocol_encoder_mitsubishi_get_upload(instance);
    instance->encoder.is_running = true;

    return SubGhzProtocolStatusOk;
}

#endif
