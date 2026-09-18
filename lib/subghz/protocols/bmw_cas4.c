#include "bmw_cas4.h"
#include "protocols_common.h"

// Ported from KAT src/protocols/bmw_cas4.rs
#define BMW_CAS4_BIT_COUNT      64
#define BMW_CAS4_TE_SHORT       500
#define BMW_CAS4_TE_LONG        1000
#define BMW_CAS4_TE_DELTA       150
#define BMW_CAS4_PREAMBLE_MIN_US 300
#define BMW_CAS4_PREAMBLE_MAX_US 700
#define BMW_CAS4_PREAMBLE_MIN    10
#define BMW_CAS4_GAP_MIN         1800
#define BMW_CAS4_BYTE0_MARKER    0x30
#define BMW_CAS4_BYTE6_MARKER    0xC5

static const SubGhzBlockConst subghz_protocol_bmw_cas4_const = {
    .te_short = BMW_CAS4_TE_SHORT,
    .te_long = BMW_CAS4_TE_LONG,
    .te_delta = BMW_CAS4_TE_DELTA,
    .min_count_bit_for_found = BMW_CAS4_BIT_COUNT,
};

typedef enum {
    BmwCas4DecoderStepReset = 0,
    BmwCas4DecoderStepPreamble,
    BmwCas4DecoderStepData,
} BmwCas4DecoderStep;

// Common Manchester state machine, ported verbatim from KAT common.rs
// (mirror of Flipper lib/toolbox/manchester_decoder.h). Events:
//   0 = short high, 1 = short low, 2 = long high, 3 = long low
typedef enum {
    BmwManMid0 = 0,
    BmwManMid1,
    BmwManStart0,
    BmwManStart1,
} BmwManState;

struct SubGhzProtocolDecoderBmwCas4 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    BmwManState man_state;
    uint16_t preamble_count;
    uint8_t raw_data[8];
    uint8_t bit_count;
    uint64_t decode_data;
};

// Returns true if a data bit was emitted; writes it to *out_bit.
static bool bmw_manchester_advance(BmwManState* state, uint8_t event, bool* out_bit) {
    BmwManState s = *state;
    BmwManState ns = BmwManMid1;
    bool emit = false;

    switch(s) {
    case BmwManMid0:
        if(event == 0) {
            ns = BmwManMid0;
        } else if(event == 1) {
            ns = BmwManStart1;
            emit = true;
        } else if(event == 2) {
            ns = BmwManMid0;
        } else {
            ns = BmwManMid1;
            emit = true;
        }
        break;
    case BmwManMid1:
        if(event == 0) {
            ns = BmwManStart0;
            emit = true;
        } else if(event == 1) {
            ns = BmwManMid1;
        } else if(event == 2) {
            ns = BmwManMid0;
            emit = true;
        } else {
            ns = BmwManMid1;
        }
        break;
    case BmwManStart0:
        if(event == 3) {
            ns = BmwManMid1;
        } else {
            ns = BmwManMid0;
        }
        break;
    case BmwManStart1:
        if(event == 1 || event == 3) {
            ns = BmwManMid1;
        } else {
            ns = BmwManMid0;
        }
        break;
    default:
        ns = BmwManMid1;
        break;
    }

    *state = ns;
    if(emit) {
        *out_bit = ((event & 1U) == 1U);
        return true;
    }
    return false;
}

static void bmw_cas4_publish(SubGhzProtocolDecoderBmwCas4* instance) {
    instance->generic.data = instance->decode_data;
    instance->generic.data_count_bit = BMW_CAS4_BIT_COUNT;
    instance->generic.serial = 0;
    instance->generic.btn = 0;
    instance->generic.cnt = 0;

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

void* subghz_protocol_decoder_bmw_cas4_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderBmwCas4* instance = calloc(1, sizeof(SubGhzProtocolDecoderBmwCas4));
    furi_check(instance);
    instance->base.protocol = &bmw_cas4_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_bmw_cas4_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderBmwCas4* instance = context;
    instance->decoder.parser_step = BmwCas4DecoderStepReset;
    instance->man_state = BmwManMid1;
    instance->preamble_count = 0;
    instance->bit_count = 0;
    instance->decode_data = 0;
    memset(instance->raw_data, 0, sizeof(instance->raw_data));
    instance->generic.data = 0;
    instance->generic.data_count_bit = 0;
}

void subghz_protocol_decoder_bmw_cas4_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderBmwCas4* instance = context;

    const uint32_t te_short = subghz_protocol_bmw_cas4_const.te_short;
    const uint32_t te_long = subghz_protocol_bmw_cas4_const.te_long;
    const uint32_t te_delta = subghz_protocol_bmw_cas4_const.te_delta;

    switch(instance->decoder.parser_step) {
    case BmwCas4DecoderStepReset:
        if(level && duration >= BMW_CAS4_PREAMBLE_MIN_US && duration <= BMW_CAS4_PREAMBLE_MAX_US) {
            instance->decoder.parser_step = BmwCas4DecoderStepPreamble;
            instance->preamble_count = 1;
        }
        break;

    case BmwCas4DecoderStepPreamble:
        if(duration >= BMW_CAS4_PREAMBLE_MIN_US && duration <= BMW_CAS4_PREAMBLE_MAX_US) {
            instance->preamble_count++;
        } else if(!level && duration >= BMW_CAS4_GAP_MIN) {
            if(instance->preamble_count >= BMW_CAS4_PREAMBLE_MIN) {
                instance->bit_count = 0;
                instance->decode_data = 0;
                memset(instance->raw_data, 0, sizeof(instance->raw_data));
                instance->man_state = BmwManMid1;
                instance->decoder.parser_step = BmwCas4DecoderStepData;
            } else {
                instance->decoder.parser_step = BmwCas4DecoderStepReset;
            }
        } else {
            instance->decoder.parser_step = BmwCas4DecoderStepReset;
        }
        break;

    case BmwCas4DecoderStepData: {
        if(instance->bit_count >= BMW_CAS4_BIT_COUNT) {
            instance->decoder.parser_step = BmwCas4DecoderStepReset;
            break;
        }

        uint8_t event = 0xFF;
        if(DURATION_DIFF(duration, te_short) < te_delta) {
            event = level ? 0U : 1U;
        } else if(DURATION_DIFF(duration, te_long) < te_delta) {
            event = level ? 2U : 3U;
        }

        if(event == 0xFF) {
            instance->decoder.parser_step = BmwCas4DecoderStepReset;
            break;
        }

        bool bit = false;
        if(bmw_manchester_advance(&instance->man_state, event, &bit)) {
            if(instance->bit_count < BMW_CAS4_BIT_COUNT) {
                uint8_t byte_idx = instance->bit_count / 8U;
                uint8_t bit_pos = 7U - (instance->bit_count % 8U);
                if(bit) {
                    instance->raw_data[byte_idx] |= (uint8_t)(1U << bit_pos);
                }
                instance->decode_data = (instance->decode_data << 1) | (bit ? 1U : 0U);
            }
            instance->bit_count++;

            if(instance->bit_count == BMW_CAS4_BIT_COUNT) {
                if(instance->raw_data[0] == BMW_CAS4_BYTE0_MARKER &&
                   instance->raw_data[6] == BMW_CAS4_BYTE6_MARKER) {
                    bmw_cas4_publish(instance);
                }
                instance->decoder.parser_step = BmwCas4DecoderStepReset;
            }
        }
        break;
    }
    }
}

uint8_t subghz_protocol_decoder_bmw_cas4_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderBmwCas4* instance = context;
    SubGhzBlockDecoder decoder = {
        .decode_data = instance->generic.data,
        .decode_count_bit = instance->generic.data_count_bit,
    };
    return subghz_protocol_blocks_get_hash_data(&decoder, (decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_bmw_cas4_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderBmwCas4* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_bmw_cas4_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderBmwCas4* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, subghz_protocol_bmw_cas4_const.min_count_bit_for_found);
}

void subghz_protocol_decoder_bmw_cas4_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderBmwCas4* instance = context;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%08lX%08lX\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long)(instance->generic.data >> 32),
        (unsigned long)(instance->generic.data & 0xFFFFFFFF));
}

const SubGhzProtocolDecoder subghz_protocol_bmw_cas4_decoder = {
    .alloc = subghz_protocol_decoder_bmw_cas4_alloc,
    .free = pp_decoder_free_default,
    .feed = subghz_protocol_decoder_bmw_cas4_feed,
    .reset = subghz_protocol_decoder_bmw_cas4_reset,
    .get_hash_data = subghz_protocol_decoder_bmw_cas4_get_hash_data,
    .serialize = subghz_protocol_decoder_bmw_cas4_serialize,
    .deserialize = subghz_protocol_decoder_bmw_cas4_deserialize,
    .get_string = subghz_protocol_decoder_bmw_cas4_get_string,
};

// Decode-only: BMW CAS4 is a fixed code with no key-recovery / TX path in KAT.
const SubGhzProtocolEncoder subghz_protocol_bmw_cas4_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol bmw_cas4_protocol = {
    .name = BMW_CAS4_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_bmw_cas4_decoder,
    .encoder = &subghz_protocol_bmw_cas4_encoder,
};
