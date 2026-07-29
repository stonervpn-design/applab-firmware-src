#include "scher_khan.h"
#include "protocols_common.h"

#include "protocols_common.h"

//https://phreakerclub.com/72
//https://phreakerclub.com/forum/showthread.php?t=7&page=2
//https://phreakerclub.com/forum/showthread.php?t=274&highlight=magicar
//!!!  https://phreakerclub.com/forum/showthread.php?t=489&highlight=magicar&page=5

#define TAG "SubGhzProtocolScherKhan"

static const SubGhzBlockConst subghz_protocol_scher_khan_const = {
    .te_short = 750,
    .te_long = 1100,
    .te_delta = 160,
    .min_count_bit_for_found = 35,
};

#define SCHER_KHAN_MIN_COUNT_BIT  35U
#define SCHER_KHAN_MAX_COUNT_BIT  128U
#define SCHER_KHAN_PREAMBLE_PAIRS 6U
#define SCHER_KHAN_UPLOAD_CAPACITY \
    ((SCHER_KHAN_PREAMBLE_PAIRS * 2U) + 2U + (SCHER_KHAN_MAX_COUNT_BIT * 2U) + 2U)
_Static_assert(
    SCHER_KHAN_UPLOAD_CAPACITY <= PP_SHARED_UPLOAD_CAPACITY,
    "SCHER_KHAN_UPLOAD_CAPACITY exceeds shared upload slab");

struct SubGhzProtocolDecoderScherKhan {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint16_t header_count;
    const char* protocol_name;
};

typedef struct {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
} SubGhzProtocolEncoderScherKhan;

typedef enum {
    ScherKhanDecoderStepReset = 0,
    ScherKhanDecoderStepCheckPreambula,
    ScherKhanDecoderStepSaveDuration,
    ScherKhanDecoderStepCheckDuration,
} ScherKhanDecoderStep;

const SubGhzProtocolDecoder subghz_protocol_scher_khan_decoder = {
    .alloc = subghz_protocol_decoder_scher_khan_alloc,
    .free = pp_decoder_free_default,

    .feed = subghz_protocol_decoder_scher_khan_feed,
    .reset = subghz_protocol_decoder_scher_khan_reset,

    .get_hash_data = pp_decoder_hash_blocks,
    .serialize = subghz_protocol_decoder_scher_khan_serialize,
    .deserialize = subghz_protocol_decoder_scher_khan_deserialize,
    .get_string = subghz_protocol_decoder_scher_khan_get_string,
};

#ifdef ENABLE_EMULATE_FEATURE
static void* subghz_protocol_encoder_scher_khan_alloc(SubGhzEnvironment* environment);
static SubGhzProtocolStatus
    subghz_protocol_encoder_scher_khan_deserialize(void* context, FlipperFormat* flipper_format);

const SubGhzProtocolEncoder subghz_protocol_scher_khan_encoder = {
    .alloc = subghz_protocol_encoder_scher_khan_alloc,
    .free = pp_encoder_free,

    .deserialize = subghz_protocol_encoder_scher_khan_deserialize,
    .stop = pp_encoder_stop,
    .yield = pp_encoder_yield,
};
#else
const SubGhzProtocolEncoder subghz_protocol_scher_khan_encoder = {
    .alloc = NULL,
    .free = NULL,

    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};
#endif

const SubGhzProtocol subghz_protocol_scher_khan = {
    .name = SUBGHZ_PROTOCOL_SCHER_KHAN_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Send,

    .decoder = &subghz_protocol_scher_khan_decoder,
    .encoder = &subghz_protocol_scher_khan_encoder,
};

void* subghz_protocol_decoder_scher_khan_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderScherKhan* instance = calloc(1, sizeof(SubGhzProtocolDecoderScherKhan));
    instance->base.protocol = &subghz_protocol_scher_khan;
    instance->generic.protocol_name = instance->base.protocol->name;

    return instance;
}

void subghz_protocol_decoder_scher_khan_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderScherKhan* instance = context;
    instance->decoder.parser_step = ScherKhanDecoderStepReset;
}

void subghz_protocol_decoder_scher_khan_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderScherKhan* instance = context;

    switch(instance->decoder.parser_step) {
    case ScherKhanDecoderStepReset:
        if((level) && (DURATION_DIFF(duration, subghz_protocol_scher_khan_const.te_short * 2) <
                       subghz_protocol_scher_khan_const.te_delta)) {
            instance->decoder.parser_step = ScherKhanDecoderStepCheckPreambula;
            instance->decoder.te_last = duration;
            instance->header_count = 0;
        }
        break;
    case ScherKhanDecoderStepCheckPreambula:
        if(level) {
            if((DURATION_DIFF(duration, subghz_protocol_scher_khan_const.te_short * 2) <
                subghz_protocol_scher_khan_const.te_delta) ||
               (DURATION_DIFF(duration, subghz_protocol_scher_khan_const.te_short) <
                subghz_protocol_scher_khan_const.te_delta)) {
                instance->decoder.te_last = duration;
            } else {
                instance->decoder.parser_step = ScherKhanDecoderStepReset;
            }
        } else if(
            (DURATION_DIFF(duration, subghz_protocol_scher_khan_const.te_short * 2) <
             subghz_protocol_scher_khan_const.te_delta) ||
            (DURATION_DIFF(duration, subghz_protocol_scher_khan_const.te_short) <
             subghz_protocol_scher_khan_const.te_delta)) {
            if(DURATION_DIFF(
                   instance->decoder.te_last, subghz_protocol_scher_khan_const.te_short * 2) <
               subghz_protocol_scher_khan_const.te_delta) {
                // Found header
                instance->header_count++;
                break;
            } else if(
                DURATION_DIFF(
                    instance->decoder.te_last, subghz_protocol_scher_khan_const.te_short) <
                subghz_protocol_scher_khan_const.te_delta) {
                // Found start bit
                if(instance->header_count >= 2) {
                    instance->decoder.parser_step = ScherKhanDecoderStepSaveDuration;
                    instance->decoder.decode_data = 0;
                    instance->decoder.decode_count_bit = 1;
                } else {
                    instance->decoder.parser_step = ScherKhanDecoderStepReset;
                }
            } else {
                instance->decoder.parser_step = ScherKhanDecoderStepReset;
            }
        } else {
            instance->decoder.parser_step = ScherKhanDecoderStepReset;
        }
        break;
    case ScherKhanDecoderStepSaveDuration:
        if(level) {
            if(duration >= (subghz_protocol_scher_khan_const.te_delta * 2UL +
                            subghz_protocol_scher_khan_const.te_long)) {
                //Found stop bit
                instance->decoder.parser_step = ScherKhanDecoderStepReset;
                if(instance->decoder.decode_count_bit >=
                   subghz_protocol_scher_khan_const.min_count_bit_for_found) {
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
                instance->decoder.parser_step = ScherKhanDecoderStepCheckDuration;
            }

        } else {
            instance->decoder.parser_step = ScherKhanDecoderStepReset;
        }
        break;
    case ScherKhanDecoderStepCheckDuration:
        if(!level) {
            if((DURATION_DIFF(
                    instance->decoder.te_last, subghz_protocol_scher_khan_const.te_short) <
                subghz_protocol_scher_khan_const.te_delta) &&
               (DURATION_DIFF(duration, subghz_protocol_scher_khan_const.te_short) <
                subghz_protocol_scher_khan_const.te_delta)) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 0);
                instance->decoder.parser_step = ScherKhanDecoderStepSaveDuration;
            } else if(
                (DURATION_DIFF(
                     instance->decoder.te_last, subghz_protocol_scher_khan_const.te_long) <
                 subghz_protocol_scher_khan_const.te_delta) &&
                (DURATION_DIFF(duration, subghz_protocol_scher_khan_const.te_long) <
                 subghz_protocol_scher_khan_const.te_delta)) {
                subghz_protocol_blocks_add_bit(&instance->decoder, 1);
                instance->decoder.parser_step = ScherKhanDecoderStepSaveDuration;
            } else {
                instance->decoder.parser_step = ScherKhanDecoderStepReset;
            }
        } else {
            instance->decoder.parser_step = ScherKhanDecoderStepReset;
        }
        break;
    }
}

/** 
 * Analysis of received data
 * @param instance Pointer to a SubGhzBlockGeneric* instance
 * @param protocol_name 
 */
static void subghz_protocol_scher_khan_check_remote_controller(
    SubGhzBlockGeneric* instance,
    const char** protocol_name) {
    /* 
    * MAGICAR 51 bit 00000001A99121DE83C3 MAGIC CODE, Dynamic
    * 0E8C1619E830C -> 000011101000110000010110 0001 1001 1110 1000001100001100
    * 0E8C1629D830D -> 000011101000110000010110 0010 1001 1101 1000001100001101
    * 0E8C1649B830E -> 000011101000110000010110 0100 1001 1011 1000001100001110
    * 0E8C16897830F -> 000011101000110000010110 1000 1001 0111 1000001100001111
    *                             Serial         Key  Ser ~Key   CNT
    */

    switch(instance->data_count_bit) {
    case 35: //MAGIC CODE, Static
        *protocol_name = "MAGIC CODE, Static";
        instance->serial = 0;
        instance->btn = 0;
        instance->cnt = 0;
        break;
    case 51: //MAGIC CODE, Dynamic
        *protocol_name = "MAGIC CODE, Dynamic";
        instance->serial = ((instance->data >> 24) & 0xFFFFFF0) | ((instance->data >> 20) & 0x0F);
        instance->btn = (instance->data >> 24) & 0x0F;
        instance->cnt = instance->data & 0xFFFF;
        break;
    case 57: //MAGIC CODE PRO / PRO2
        *protocol_name = "MAGIC CODE PRO/PRO2";
        instance->serial = 0;
        instance->btn = 0;
        instance->cnt = 0;
        break;
    case 63: //MAGIC CODE, Dynamic Response
        *protocol_name = "MAGIC CODE, Response";
        instance->serial = 0;
        instance->btn = 0;
        instance->cnt = 0;
        break;
    case 64: //MAGICAR, Response ???
        *protocol_name = "MAGICAR, Response";
        instance->serial = 0;
        instance->btn = 0;
        instance->cnt = 0;
        break;
    case 81: // MAGIC CODE PRO / PRO2 Response ???
    case 82: // MAGIC CODE PRO / PRO2 Response ???
        *protocol_name = "MAGIC CODE PRO,\n Response";
        instance->serial = 0;
        instance->btn = 0;
        instance->cnt = 0;
        break;

    default:
        *protocol_name = "Unknown";
        instance->serial = 0;
        instance->btn = 0;
        instance->cnt = 0;
        break;
    }
}

SubGhzProtocolStatus subghz_protocol_decoder_scher_khan_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderScherKhan* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    do {
        if(preset != NULL) {
            if(!flipper_format_insert_or_update_uint32(
                   flipper_format, FF_FREQUENCY, &preset->frequency, 1)) {
                break;
            }

            const char* preset_name = furi_string_get_cstr(preset->name);
            const char* short_preset = pp_get_short_preset_name(preset_name);
            if(!flipper_format_insert_or_update_string_cstr(
                   flipper_format, FF_PRESET, short_preset)) {
                break;
            }
        }

        if(!flipper_format_insert_or_update_string_cstr(
               flipper_format, FF_PROTOCOL, instance->generic.protocol_name)) {
            break;
        }

        uint32_t bits = instance->generic.data_count_bit;
        if(!flipper_format_insert_or_update_uint32(flipper_format, FF_BIT, &bits, 1)) {
            break;
        }

        char key_str[20];
        snprintf(key_str, sizeof(key_str), "%016llX", instance->generic.data);
        if(!flipper_format_insert_or_update_string_cstr(flipper_format, FF_KEY, key_str)) {
            break;
        }

        if(!flipper_format_insert_or_update_uint32(
               flipper_format, FF_SERIAL, &instance->generic.serial, 1)) {
            break;
        }

        uint32_t temp = instance->generic.btn;
        if(!flipper_format_insert_or_update_uint32(flipper_format, FF_BTN, &temp, 1)) {
            break;
        }

        if(!flipper_format_insert_or_update_uint32(
               flipper_format, FF_CNT, &instance->generic.cnt, 1)) {
            break;
        }

        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_scher_khan_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderScherKhan* instance = context;
    return subghz_block_generic_deserialize(&instance->generic, flipper_format);
}

void subghz_protocol_decoder_scher_khan_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderScherKhan* instance = context;

    subghz_protocol_scher_khan_check_remote_controller(
        &instance->generic, &instance->protocol_name);

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:0x%lX%08lX\r\n"
        "Sn:%07lX Btn:%X\r\n"
        "Cntr:%04lX\r\n"
        "Pt: %s\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (uint32_t)(instance->generic.data >> 32),
        (uint32_t)instance->generic.data,
        instance->generic.serial,
        instance->generic.btn,
        instance->generic.cnt,
        instance->protocol_name);
}

#ifdef ENABLE_EMULATE_FEATURE

static void* subghz_protocol_encoder_scher_khan_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderScherKhan* instance = calloc(1, sizeof(SubGhzProtocolEncoderScherKhan));
    furi_check(instance);

    instance->base.protocol = &subghz_protocol_scher_khan;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = 10;
    pp_encoder_buffer_ensure(instance, SCHER_KHAN_UPLOAD_CAPACITY);
    instance->encoder.is_running = false;

    return instance;
}

/**
 * Generating an upload from data.
 * Scher-Khan / MAGIC CODE frames carry no on-air crypto or checksum - the decoder
 * captures the pulse-width bitstream verbatim - so emulation replays the stored
 * bits unchanged and there is no rolling counter to advance on this side.
 * @param instance Pointer to a SubGhzProtocolEncoderScherKhan instance
 * @return true On success
 */
static bool
    subghz_protocol_encoder_scher_khan_get_upload(SubGhzProtocolEncoderScherKhan* instance) {
    furi_check(instance);

    const uint32_t bit_count = instance->generic.data_count_bit;
    if(bit_count < SCHER_KHAN_MIN_COUNT_BIT || bit_count > SCHER_KHAN_MAX_COUNT_BIT) {
        FURI_LOG_E(TAG, "Unsupported bit count %lu", bit_count);
        return false;
    }

    // The decoder counts a leading start bit that is not stored in data, so the
    // number of stored data bits is (data_count_bit - 1).
    const uint32_t data_bits = bit_count - 1U;

    const size_t cap = SCHER_KHAN_UPLOAD_CAPACITY;
    const uint32_t te_short = (uint32_t)subghz_protocol_scher_khan_const.te_short;
    const uint32_t te_long = (uint32_t)subghz_protocol_scher_khan_const.te_long;

    LevelDuration* up = instance->encoder.upload;
    size_t index = 0;

    // Preamble: header pairs of te_short * 2 (decoder requires at least two)
    index = pp_emit_short_pairs(up, index, cap, te_short * 2U, SCHER_KHAN_PREAMBLE_PAIRS);

    // Start bit: short high + short low (consumed by the decoder preamble step)
    index = pp_emit(up, index, cap, true, te_short);
    index = pp_emit(up, index, cap, false, te_short);

    // Data bits, MSB first: 1 -> long/long, 0 -> short/short
    for(uint32_t i = data_bits; i > 0; i--) {
        const uint32_t pos = i - 1U;
        const bool bit = (pos < 64U) ? (bool)bit_read(instance->generic.data, pos) : false;
        const uint32_t te = bit ? te_long : te_short;
        index = pp_emit(up, index, cap, true, te);
        index = pp_emit(up, index, cap, false, te);
    }

    // Stop bit: a long high (>= te_long + 2*te_delta) plus an inter-frame gap
    index = pp_emit(up, index, cap, true, te_short * 2U);
    index = pp_emit(up, index, cap, false, te_short * 2U);

    instance->encoder.size_upload = index;
    return true;
}

static SubGhzProtocolStatus
    subghz_protocol_encoder_scher_khan_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderScherKhan* instance = context;

    if(pp_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
       SubGhzProtocolStatusOk) {
        return SubGhzProtocolStatusError;
    }

    uint32_t bits = 0;
    if(pp_encoder_read_bit(flipper_format, NULL, 0, &bits) != SubGhzProtocolStatusOk) {
        return SubGhzProtocolStatusErrorValueBitCount;
    }
    instance->generic.data_count_bit = bits;

    uint64_t key = 0;
    if(!pp_flipper_read_hex_u64(flipper_format, FF_KEY, &key)) {
        return SubGhzProtocolStatusError;
    }
    instance->generic.data = key;

    // Informational only - Scher-Khan has no crypto, these do not rebuild the frame
    uint32_t serial = 0;
    uint32_t btn = 0;
    uint32_t cnt = 0;
    pp_encoder_read_fields(flipper_format, &serial, &btn, &cnt, NULL);
    instance->generic.serial = serial;
    instance->generic.btn = (uint8_t)btn;
    instance->generic.cnt = (uint16_t)cnt;

    instance->encoder.repeat = pp_encoder_read_repeat(flipper_format, 10);

    if(!subghz_protocol_encoder_scher_khan_get_upload(instance)) {
        return SubGhzProtocolStatusError;
    }

    instance->encoder.is_running = true;
    return SubGhzProtocolStatusOk;
}

#endif
