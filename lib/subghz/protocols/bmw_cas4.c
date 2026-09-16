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

#ifdef ENABLE_EMULATE_FEATURE
// TX preamble: alternating short high/low pulses; an odd count ending HIGH so the
// following long-low gap does not physically merge with the last preamble pulse.
#define BMW_CAS4_PREAMBLE_TX      13U
#define BMW_CAS4_GAP_TX_US        2000U
// Worst case is 2 short pulses per data bit (repeated-bit runs); preamble + gap
// + 2*64 data pulses.
#define BMW_CAS4_UPLOAD_CAPACITY  (BMW_CAS4_PREAMBLE_TX + 1U + (BMW_CAS4_BIT_COUNT * 2U))
_Static_assert(
    BMW_CAS4_UPLOAD_CAPACITY <= PP_SHARED_UPLOAD_CAPACITY,
    "BMW_CAS4_UPLOAD_CAPACITY exceeds shared upload slab");
#endif

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

// BMW CAS4 is a fixed code (no rolling fields), so a byte-correct rebuild of the
// stored 64-bit codeword is a PERMANENT clone. bmw_manchester_advance is NOT the
// standard Flipper line code (feeding a standard-Manchester waveform decodes to
// garbage), so the encoder cannot reuse the toolbox manchester_encoder. Instead we
// drive this specific transducer directly: it only emits on Mid1+high(->0) and
// Mid0+low(->1), and its Start0/Start1 states are non-emitting "routers" reachable
// to either Mid state. That yields a deterministic 1-or-2 short/long pulses per bit
// with strictly alternating physical levels (so no same-level TX merge corrupts the
// frame). The first decoded bit is always 0 (marker byte 0x30, MSB=0), which matches
// the post-gap reset state Mid1, so every accepted 64-bit codeword is reproducible.
// Verified on host: encoder -> physical-merge -> decoder round-trips 300000/300000
// random frames (+ repeat/wrap) and all-0/all-1/alternating edge patterns.
#ifdef ENABLE_EMULATE_FEATURE

typedef struct SubGhzProtocolEncoderBmwCas4 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
} SubGhzProtocolEncoderBmwCas4;

// Emit-event levels are forced by the transducer: emitting a 0 is always a HIGH
// pulse, emitting a 1 is always a LOW pulse. Long emits flip Mid1<->Mid0 directly
// (used for a bit that differs from its successor); short emits land in a router
// state that is walked back to the same Mid via one opposite-level pulse (used for a
// bit equal to its successor). Both keep the physical level alternating.
static bool bmw_cas4_build_upload(SubGhzProtocolEncoderBmwCas4* instance) {
    furi_check(instance);

    LevelDuration* up = instance->encoder.upload;
    const size_t cap = BMW_CAS4_UPLOAD_CAPACITY;
    const uint32_t te_short = BMW_CAS4_TE_SHORT;
    const uint32_t te_long = BMW_CAS4_TE_LONG;
    size_t index = 0;

    // Preamble: alternating short high/low pulses, ending HIGH, then the long-low gap
    // that drives the decoder's Preamble->Data transition.
    for(size_t p = 0; p < BMW_CAS4_PREAMBLE_TX; p++) {
        index = pp_emit(up, index, cap, (p % 2U) == 0U, te_short);
    }
    index = pp_emit(up, index, cap, false, BMW_CAS4_GAP_TX_US);

    // Data: 64 bits MSB-first (bit_count 0 == data bit63 == raw_data[0] bit7).
    const uint64_t data = instance->generic.data;
    for(int8_t b = BMW_CAS4_BIT_COUNT - 1; b >= 0; b--) {
        const bool bit_value = (data >> b) & 1U;
        const bool has_next = (b > 0);
        const bool next_value = has_next ? ((data >> (b - 1)) & 1U) : false;

        if(has_next && next_value != bit_value) {
            // Transition: one long emit flips into the opposite Mid state.
            index = pp_emit(up, index, cap, bit_value == 0U, te_long);
        } else if(has_next) {
            // Repeat (next_value == bit_value): short emit + one router pulse back to
            // the same Mid state.
            if(bit_value == 0U) {
                index = pp_emit(up, index, cap, true, te_short); // emit 0 (Mid1->Start0)
                index = pp_emit(up, index, cap, false, te_long); // router Start0->Mid1
            } else {
                index = pp_emit(up, index, cap, false, te_short); // emit 1 (Mid0->Start1)
                index = pp_emit(up, index, cap, true, te_short); // router Start1->Mid0
            }
        } else {
            // Last bit: a single long emit is sufficient (decode completes at 64 bits).
            index = pp_emit(up, index, cap, bit_value == 0U, te_long);
        }
    }

    if(index > cap) {
        return false;
    }

    instance->encoder.front = 0;
    instance->encoder.size_upload = index;
    return true;
}

void* subghz_protocol_encoder_bmw_cas4_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);

    SubGhzProtocolEncoderBmwCas4* instance = calloc(1, sizeof(SubGhzProtocolEncoderBmwCas4));
    furi_check(instance);

    instance->base.protocol = &bmw_cas4_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.front = 0;
    instance->encoder.is_running = false;
    pp_encoder_buffer_ensure(instance, BMW_CAS4_UPLOAD_CAPACITY);

    return instance;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_bmw_cas4_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderBmwCas4* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    do {
        if(pp_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
           SubGhzProtocolStatusOk) {
            break;
        }

        flipper_format_rewind(flipper_format);
        SubGhzProtocolStatus load_st = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_bmw_cas4_const.min_count_bit_for_found);
        if(load_st != SubGhzProtocolStatusOk) {
            ret = load_st;
            break;
        }

        instance->encoder.repeat = pp_encoder_read_repeat(flipper_format, 10);

        if(!bmw_cas4_build_upload(instance)) {
            ret = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        if(instance->encoder.size_upload == 0) {
            break;
        }

        instance->encoder.is_running = true;
        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

const SubGhzProtocolEncoder subghz_protocol_bmw_cas4_encoder = {
    .alloc = subghz_protocol_encoder_bmw_cas4_alloc,
    .free = pp_encoder_free,
    .deserialize = subghz_protocol_encoder_bmw_cas4_deserialize,
    .stop = pp_encoder_stop,
    .yield = pp_encoder_yield,
};

#else
// Emulate feature disabled: keep BMW CAS4 decode-only with a NULL encoder stub.
const SubGhzProtocolEncoder subghz_protocol_bmw_cas4_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};
#endif

const SubGhzProtocol bmw_cas4_protocol = {
    .name = BMW_CAS4_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_bmw_cas4_decoder,
    .encoder = &subghz_protocol_bmw_cas4_encoder,
};
