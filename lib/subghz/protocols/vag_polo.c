#include "vag_polo.h"
#include "protocols_common.h"

#include <string.h>

#define TAG "VAGPolo"

// =============================================================================
// PROTOCOL CONSTANTS
// =============================================================================

static const SubGhzBlockConst subghz_protocol_vag_polo_const = {
    .te_short = 300,
    .te_long = 590,
    .te_delta = 130,
    .min_count_bit_for_found = 64,
};

#define VAG_POLO_SYNC          0x3F91u // 16-bit header after the all-1s preamble
#define VAG_POLO_PREAMBLE_ONES 32u // require >= this many 1-bits before accepting sync
#define VAG_POLO_PAYLOAD_BITS  80u // button byte (8) + 9-byte rolling code (72)
#define VAG_POLO_PAYLOAD_BYTES 10u

// =============================================================================
// STRUCT DEFINITIONS
// =============================================================================

typedef struct SubGhzProtocolDecoderVagPolo {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    ManchesterState manchester_state;
    uint16_t sync_reg;
    uint16_t preamble_ones;
    uint8_t payload[VAG_POLO_PAYLOAD_BYTES];
    uint16_t payload_bits;

    uint8_t button; // raw button byte (0x7x = unlock, 0xBx = lock)
    uint8_t code[9]; // 9-byte rolling code
} SubGhzProtocolDecoderVagPolo;

typedef enum {
    VagPoloDecoderStepReset = 0,
    VagPoloDecoderStepPreamble = 5,
    VagPoloDecoderStepData = 6,
} VagPoloDecoderStep;

// =============================================================================
// HELPERS
// =============================================================================

static const char* vag_polo_button_name(uint8_t button) {
    switch((button >> 4) & 0x0F) {
    case 0x7:
        return "Unlock";
    case 0xB:
        return "Lock";
    default:
        return "?";
    }
}

static bool vag_polo_button_valid(uint8_t button) {
    uint8_t hi = (button >> 4) & 0x0F;
    return (hi == 0x7) || (hi == 0xB);
}

static bool vag_polo_get_event(uint32_t duration, bool level, ManchesterEvent* event) {
    const uint32_t tol = (uint32_t)subghz_protocol_vag_polo_const.te_delta + 20U;

    if((uint32_t)DURATION_DIFF(duration, subghz_protocol_vag_polo_const.te_short) < tol) {
        *event = level ? ManchesterEventShortLow : ManchesterEventShortHigh;
        return true;
    }
    if((uint32_t)DURATION_DIFF(duration, subghz_protocol_vag_polo_const.te_long) < tol) {
        *event = level ? ManchesterEventLongLow : ManchesterEventLongHigh;
        return true;
    }
    return false;
}

// pack the decoded frame into generic + instance fields (for display/serialize)
static void vag_polo_commit(SubGhzProtocolDecoderVagPolo* instance) {
    instance->button = instance->payload[0];
    memcpy(instance->code, &instance->payload[1], 9);

    // pseudo-serial from the first 4 rolling bytes (display/index only)
    instance->generic.serial = ((uint32_t)instance->code[0] << 24) |
                               ((uint32_t)instance->code[1] << 16) |
                               ((uint32_t)instance->code[2] << 8) | (uint32_t)instance->code[3];
    instance->generic.btn =
        (((instance->button >> 4) & 0x0F) == 0x7) ? 0x01 : 0x02; // 1=unlock, 2=lock
    instance->generic.cnt = 0;
    // low 64 bits of the rolling code -> generic.data (hashing / Key field)
    instance->generic.data = pp_bytes_to_u64_be(&instance->code[1]);
    instance->generic.data_count_bit = subghz_protocol_vag_polo_const.min_count_bit_for_found;
}

// =============================================================================
// PROTOCOL INTERFACE
// =============================================================================

const SubGhzProtocolDecoder subghz_protocol_vag_polo_decoder = {
    .alloc = subghz_protocol_decoder_vag_polo_alloc,
    .free = pp_decoder_free_default,
    .feed = subghz_protocol_decoder_vag_polo_feed,
    .reset = subghz_protocol_decoder_vag_polo_reset,
    .get_hash_data = subghz_protocol_decoder_vag_polo_get_hash_data,
    .serialize = subghz_protocol_decoder_vag_polo_serialize,
    .deserialize = subghz_protocol_decoder_vag_polo_deserialize,
    .get_string = subghz_protocol_decoder_vag_polo_get_string,
};

// Rolling code is encrypted (no public VW key): detect + display only, no TX.
const SubGhzProtocolEncoder subghz_protocol_vag_polo_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol vag_polo_protocol = {
    .name = VAG_POLO_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_vag_polo_decoder,
    .encoder = &subghz_protocol_vag_polo_encoder,
};

// =============================================================================
// DECODER
// =============================================================================

void* subghz_protocol_decoder_vag_polo_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderVagPolo* instance = calloc(1, sizeof(SubGhzProtocolDecoderVagPolo));
    furi_check(instance);
    instance->base.protocol = &vag_polo_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_vag_polo_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderVagPolo* instance = context;
    instance->decoder.parser_step = VagPoloDecoderStepReset;
    instance->manchester_state = ManchesterStateStart1;
    instance->sync_reg = 0;
    instance->preamble_ones = 0;
    instance->payload_bits = 0;
    memset(instance->payload, 0, sizeof(instance->payload));
}

void subghz_protocol_decoder_vag_polo_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderVagPolo* instance = context;
    ManchesterEvent event = ManchesterEventReset;
    bool bit = false;

    switch(instance->decoder.parser_step) {
    case VagPoloDecoderStepReset:
        if(level && ((uint32_t)DURATION_DIFF(duration, subghz_protocol_vag_polo_const.te_short) <
                     (uint32_t)subghz_protocol_vag_polo_const.te_delta + 20U)) {
            instance->decoder.parser_step = VagPoloDecoderStepPreamble;
            instance->manchester_state = ManchesterStateMid1;
            instance->sync_reg = 0;
            instance->preamble_ones = 0;
        }
        break;

    case VagPoloDecoderStepPreamble:
        if(!vag_polo_get_event(duration, level, &event)) {
            instance->decoder.parser_step = VagPoloDecoderStepReset;
            break;
        }
        if(manchester_advance(
               instance->manchester_state, event, &instance->manchester_state, &bit)) {
            instance->sync_reg = (uint16_t)((instance->sync_reg << 1) | (bit ? 1U : 0U));
            if(bit) {
                instance->preamble_ones++;
            } else if(instance->preamble_ones < VAG_POLO_PREAMBLE_ONES) {
                // a 0 before enough preamble 1s => noise, resync
                instance->preamble_ones = 0;
                instance->sync_reg = 0;
                break;
            }
            if((instance->sync_reg == VAG_POLO_SYNC) &&
               (instance->preamble_ones >= VAG_POLO_PREAMBLE_ONES)) {
                instance->payload_bits = 0;
                memset(instance->payload, 0, sizeof(instance->payload));
                instance->decoder.parser_step = VagPoloDecoderStepData;
            }
        }
        break;

    case VagPoloDecoderStepData:
        if(!vag_polo_get_event(duration, level, &event)) {
            instance->decoder.parser_step = VagPoloDecoderStepReset;
            break;
        }
        if(manchester_advance(
               instance->manchester_state, event, &instance->manchester_state, &bit)) {
            uint16_t byte_index = instance->payload_bits >> 3;
            if(byte_index < VAG_POLO_PAYLOAD_BYTES) {
                instance->payload[byte_index] =
                    (uint8_t)((instance->payload[byte_index] << 1) | (bit ? 1U : 0U));
            }
            instance->payload_bits++;

            if(instance->payload_bits >= VAG_POLO_PAYLOAD_BITS) {
                if(vag_polo_button_valid(instance->payload[0])) {
                    vag_polo_commit(instance);
                    if(instance->base.callback) {
                        instance->base.callback(&instance->base, instance->base.context);
                    }
                }
                instance->decoder.parser_step = VagPoloDecoderStepReset;
                instance->manchester_state = ManchesterStateStart1;
                instance->sync_reg = 0;
                instance->preamble_ones = 0;
                instance->payload_bits = 0;
            }
        }
        break;
    }
}

uint8_t subghz_protocol_decoder_vag_polo_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderVagPolo* instance = context;
    uint8_t hash = instance->button;
    for(uint8_t i = 0; i < 9; i++) hash ^= instance->code[i];
    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_vag_polo_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderVagPolo* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret == SubGhzProtocolStatusOk) {
        if(!flipper_format_write_hex(flipper_format, "Code", instance->code, 9)) {
            return SubGhzProtocolStatusErrorParserOthers;
        }
        uint32_t btn_raw = instance->button;
        if(!flipper_format_write_uint32(flipper_format, "Btn_raw", &btn_raw, 1)) {
            return SubGhzProtocolStatusErrorParserOthers;
        }
        pp_serialize_fields(
            flipper_format,
            PP_FIELD_SERIAL | PP_FIELD_BTN,
            instance->generic.serial,
            instance->generic.btn,
            0,
            0);
        ret = pp_write_display(
            flipper_format, instance->generic.protocol_name, vag_polo_button_name(instance->button));
    }
    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_vag_polo_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderVagPolo* instance = context;

    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_vag_polo_const.min_count_bit_for_found);
    if(ret == SubGhzProtocolStatusOk) {
        flipper_format_rewind(flipper_format);
        if(!flipper_format_read_hex(flipper_format, "Code", instance->code, 9)) {
            memset(instance->code, 0, sizeof(instance->code));
        }
        uint32_t btn_raw = 0;
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "Btn_raw", &btn_raw, 1)) {
            instance->button = (uint8_t)btn_raw;
        }
    }
    return ret;
}

void subghz_protocol_decoder_vag_polo_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderVagPolo* instance = context;

    furi_string_cat_printf(
        output,
        "%s (rolling)\r\n"
        "Btn: %02X - %s\r\n"
        "Code: %02X%02X%02X%02X%02X%02X%02X%02X%02X\r\n",
        instance->generic.protocol_name,
        instance->button,
        vag_polo_button_name(instance->button),
        instance->code[0],
        instance->code[1],
        instance->code[2],
        instance->code[3],
        instance->code[4],
        instance->code[5],
        instance->code[6],
        instance->code[7],
        instance->code[8]);
}
