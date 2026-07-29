#include "hyundai_kia_rio.h"
#include "protocols_common.h"

// Ported from KAT src/protocols/hyundai_kia_rio.rs
#define HKR_BIT_COUNT 64
#define HKR_TE_SHORT  312
#define HKR_TE_LONG   728
#define HKR_TE_DELTA  150
#define HKR_SYNC_US   10400
#define HKR_SYNC_DELTA 1560
#define HKR_GAP_US    10000

static const SubGhzBlockConst subghz_protocol_hyundai_kia_rio_const = {
    .te_short = HKR_TE_SHORT,
    .te_long = HKR_TE_LONG,
    .te_delta = HKR_TE_DELTA,
    .min_count_bit_for_found = HKR_BIT_COUNT,
};

#ifdef ENABLE_EMULATE_FEATURE
#define HKR_UPLOAD_CAPACITY ((2U) + (2U * HKR_BIT_COUNT) + 2U)
_Static_assert(
    HKR_UPLOAD_CAPACITY <= PP_SHARED_UPLOAD_CAPACITY,
    "HKR_UPLOAD_CAPACITY exceeds shared upload slab");
#endif

typedef enum {
    HkrDecoderStepReset = 0,
    HkrDecoderStepCheckSync,
    HkrDecoderStepSaveDuration,
    HkrDecoderStepCheckDuration,
} HkrDecoderStep;

struct SubGhzProtocolDecoderHyundaiKiaRio {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint64_t data;
    uint8_t bit_count;
};

#ifdef ENABLE_EMULATE_FEATURE
struct SubGhzProtocolEncoderHyundaiKiaRio {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};
#endif

static uint16_t hkr_button_to_mask(uint8_t button) {
    switch(button) {
    case 1:
        return 0x0100;
    case 2:
        return 0x0200;
    case 4:
        return 0x0400;
    case 8:
        return 0x0800;
    default:
        return (uint16_t)((uint16_t)button << 8);
    }
}

static uint8_t hkr_mask_to_button(uint16_t mask) {
    switch(mask) {
    case 0x0100:
        return 1;
    case 0x0200:
        return 2;
    case 0x0400:
        return 4;
    case 0x0800:
        return 8;
    default:
        return (uint8_t)(mask >> 8);
    }
}

static uint16_t hkr_checksum(uint32_t serial, uint16_t mask) {
    uint16_t c = (uint16_t)(serial ^ (serial >> 16));
    c ^= mask;
    return (uint16_t)(~c);
}

static const char* hkr_button_name(uint16_t mask) {
    switch(mask) {
    case 0x0100:
        return "Lock";
    case 0x0200:
        return "Unlock";
    case 0x0400:
        return "Trunk";
    case 0x0800:
        return "Panic";
    default:
        return "Unknown";
    }
}

static void hkr_publish(SubGhzProtocolDecoderHyundaiKiaRio* instance) {
    uint32_t serial = (uint32_t)(instance->data >> 32);
    uint16_t mask = (uint16_t)((instance->data >> 16) & 0xFFFF);
    uint16_t rx_ck = (uint16_t)(instance->data & 0xFFFF);

    if(rx_ck != hkr_checksum(serial, mask)) {
        return;
    }

    instance->generic.data = instance->data;
    instance->generic.data_count_bit = HKR_BIT_COUNT;
    instance->generic.serial = serial;
    instance->generic.btn = hkr_mask_to_button(mask);
    instance->generic.cnt = 0;

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

void* subghz_protocol_decoder_hyundai_kia_rio_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderHyundaiKiaRio* instance =
        calloc(1, sizeof(SubGhzProtocolDecoderHyundaiKiaRio));
    furi_check(instance);
    instance->base.protocol = &hyundai_kia_rio_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_hyundai_kia_rio_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderHyundaiKiaRio* instance = context;
    instance->decoder.parser_step = HkrDecoderStepReset;
    instance->data = 0;
    instance->bit_count = 0;
    instance->generic.data = 0;
    instance->generic.data_count_bit = 0;
}

static void hkr_add_bit(SubGhzProtocolDecoderHyundaiKiaRio* instance, bool bit) {
    instance->data = (instance->data << 1) | (bit ? 1U : 0U);
    instance->bit_count++;
}

void subghz_protocol_decoder_hyundai_kia_rio_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderHyundaiKiaRio* instance = context;

    const uint32_t te_short = subghz_protocol_hyundai_kia_rio_const.te_short;
    const uint32_t te_long = subghz_protocol_hyundai_kia_rio_const.te_long;
    const uint32_t te_delta = subghz_protocol_hyundai_kia_rio_const.te_delta;

    switch(instance->decoder.parser_step) {
    case HkrDecoderStepReset:
        if(level && DURATION_DIFF(duration, te_short) < te_delta) {
            instance->decoder.parser_step = HkrDecoderStepCheckSync;
        }
        break;

    case HkrDecoderStepCheckSync:
        if(!level && DURATION_DIFF(duration, HKR_SYNC_US) < HKR_SYNC_DELTA) {
            instance->decoder.parser_step = HkrDecoderStepSaveDuration;
            instance->data = 0;
            instance->bit_count = 0;
        } else {
            instance->decoder.parser_step = HkrDecoderStepReset;
        }
        break;

    case HkrDecoderStepSaveDuration:
        if(level) {
            if(DURATION_DIFF(duration, te_long) < te_delta) {
                hkr_add_bit(instance, true);
                instance->decoder.parser_step = HkrDecoderStepCheckDuration;
            } else if(DURATION_DIFF(duration, te_short) < te_delta) {
                hkr_add_bit(instance, false);
                instance->decoder.parser_step = HkrDecoderStepCheckDuration;
            } else if(duration > 3000) {
                if(instance->bit_count >= HKR_BIT_COUNT) {
                    hkr_publish(instance);
                }
                instance->decoder.parser_step = HkrDecoderStepReset;
            } else {
                instance->decoder.parser_step = HkrDecoderStepReset;
            }
        } else {
            instance->decoder.parser_step = HkrDecoderStepReset;
        }
        break;

    case HkrDecoderStepCheckDuration:
        if(!level) {
            if(DURATION_DIFF(duration, te_short) < te_delta ||
               DURATION_DIFF(duration, te_long) < te_delta) {
                if(instance->bit_count >= HKR_BIT_COUNT) {
                    hkr_publish(instance);
                    instance->decoder.parser_step = HkrDecoderStepReset;
                } else {
                    instance->decoder.parser_step = HkrDecoderStepSaveDuration;
                }
            } else if(duration > 3000) {
                if(instance->bit_count >= HKR_BIT_COUNT) {
                    hkr_publish(instance);
                }
                instance->decoder.parser_step = HkrDecoderStepReset;
            } else {
                instance->decoder.parser_step = HkrDecoderStepReset;
            }
        } else {
            instance->decoder.parser_step = HkrDecoderStepReset;
        }
        break;
    }
}

uint8_t subghz_protocol_decoder_hyundai_kia_rio_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderHyundaiKiaRio* instance = context;
    SubGhzBlockDecoder decoder = {
        .decode_data = instance->generic.data,
        .decode_count_bit = instance->generic.data_count_bit,
    };
    return subghz_protocol_blocks_get_hash_data(&decoder, (decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_hyundai_kia_rio_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderHyundaiKiaRio* instance = context;
    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret == SubGhzProtocolStatusOk) {
        pp_serialize_fields(
            flipper_format,
            PP_FIELD_SERIAL | PP_FIELD_BTN,
            instance->generic.serial,
            instance->generic.btn,
            0,
            0);
    }
    return ret;
}

SubGhzProtocolStatus subghz_protocol_decoder_hyundai_kia_rio_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderHyundaiKiaRio* instance = context;
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_hyundai_kia_rio_const.min_count_bit_for_found);
    if(ret == SubGhzProtocolStatusOk) {
        instance->generic.serial = (uint32_t)(instance->generic.data >> 32);
        instance->generic.btn =
            hkr_mask_to_button((uint16_t)((instance->generic.data >> 16) & 0xFFFF));
    }
    return ret;
}

void subghz_protocol_decoder_hyundai_kia_rio_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderHyundaiKiaRio* instance = context;
    uint16_t mask = (uint16_t)((instance->generic.data >> 16) & 0xFFFF);

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%08lX%08lX\r\n"
        "Sn:%08lX Btn:%s\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long)(instance->generic.data >> 32),
        (unsigned long)(instance->generic.data & 0xFFFFFFFF),
        (unsigned long)instance->generic.serial,
        hkr_button_name(mask));
}

#ifdef ENABLE_EMULATE_FEATURE
static bool hkr_build_upload(SubGhzProtocolEncoderHyundaiKiaRio* instance) {
    LevelDuration* up = instance->encoder.upload;
    const size_t cap = HKR_UPLOAD_CAPACITY;
    size_t index = 0;

    const uint64_t word = instance->generic.data;

    index = pp_emit(up, index, cap, true, HKR_TE_SHORT);
    index = pp_emit(up, index, cap, false, HKR_SYNC_US);

    for(int8_t b = HKR_BIT_COUNT - 1; b >= 0; b--) {
        if((word >> b) & 1U) {
            index = pp_emit(up, index, cap, true, HKR_TE_LONG);
            index = pp_emit(up, index, cap, false, HKR_TE_SHORT);
        } else {
            index = pp_emit(up, index, cap, true, HKR_TE_SHORT);
            index = pp_emit(up, index, cap, false, HKR_TE_LONG);
        }
    }

    index = pp_emit(up, index, cap, false, HKR_GAP_US);

    if(index > cap) {
        return false;
    }
    instance->encoder.front = 0;
    instance->encoder.size_upload = index;
    return true;
}

void* subghz_protocol_encoder_hyundai_kia_rio_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderHyundaiKiaRio* instance =
        calloc(1, sizeof(SubGhzProtocolEncoderHyundaiKiaRio));
    furi_check(instance);
    instance->base.protocol = &hyundai_kia_rio_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 3;
    pp_encoder_buffer_ensure(instance, HKR_UPLOAD_CAPACITY);
    return instance;
}

SubGhzProtocolStatus subghz_protocol_encoder_hyundai_kia_rio_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderHyundaiKiaRio* instance = context;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    if(pp_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
       SubGhzProtocolStatusOk) {
        return SubGhzProtocolStatusError;
    }

    SubGhzProtocolStatus load_st = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, HKR_BIT_COUNT);
    if(load_st != SubGhzProtocolStatusOk) {
        return load_st;
    }

    uint32_t serial = (uint32_t)(instance->generic.data >> 32);
    uint32_t btn = hkr_mask_to_button((uint16_t)((instance->generic.data >> 16) & 0xFFFF));
    uint32_t cnt = 0;
    pp_encoder_read_fields(flipper_format, &serial, &btn, &cnt, NULL);

    uint16_t mask = hkr_button_to_mask((uint8_t)btn);
    uint16_t ck = hkr_checksum(serial, mask);
    instance->generic.data =
        ((uint64_t)serial << 32) | ((uint64_t)mask << 16) | (uint64_t)ck;
    instance->generic.data_count_bit = HKR_BIT_COUNT;
    instance->generic.serial = serial;
    instance->generic.btn = (uint8_t)btn;

    flipper_format_rewind(flipper_format);
    uint8_t key_data[8];
    pp_u64_to_bytes_be(instance->generic.data, key_data);
    if(!flipper_format_update_hex(flipper_format, FF_KEY, key_data, sizeof(key_data))) {
        return SubGhzProtocolStatusErrorParserKey;
    }

    instance->encoder.repeat = pp_encoder_read_repeat(flipper_format, 3);
    if(!hkr_build_upload(instance)) {
        return SubGhzProtocolStatusError;
    }
    instance->encoder.is_running = true;
    return SubGhzProtocolStatusOk;
}
#endif

const SubGhzProtocolDecoder subghz_protocol_hyundai_kia_rio_decoder = {
    .alloc = subghz_protocol_decoder_hyundai_kia_rio_alloc,
    .free = pp_decoder_free_default,
    .feed = subghz_protocol_decoder_hyundai_kia_rio_feed,
    .reset = subghz_protocol_decoder_hyundai_kia_rio_reset,
    .get_hash_data = subghz_protocol_decoder_hyundai_kia_rio_get_hash_data,
    .serialize = subghz_protocol_decoder_hyundai_kia_rio_serialize,
    .deserialize = subghz_protocol_decoder_hyundai_kia_rio_deserialize,
    .get_string = subghz_protocol_decoder_hyundai_kia_rio_get_string,
};

#ifdef ENABLE_EMULATE_FEATURE
const SubGhzProtocolEncoder subghz_protocol_hyundai_kia_rio_encoder = {
    .alloc = subghz_protocol_encoder_hyundai_kia_rio_alloc,
    .free = pp_encoder_free,
    .deserialize = subghz_protocol_encoder_hyundai_kia_rio_deserialize,
    .stop = pp_encoder_stop,
    .yield = pp_encoder_yield,
};
#else
const SubGhzProtocolEncoder subghz_protocol_hyundai_kia_rio_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};
#endif

const SubGhzProtocol hyundai_kia_rio_protocol = {
    .name = HYUNDAI_KIA_RIO_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_hyundai_kia_rio_decoder,
    .encoder = &subghz_protocol_hyundai_kia_rio_encoder,
};
