#include "ford_v3.h"
#include "protocols_common.h"
#include "protocols_common.h"
#include <string.h>

#define FORD_V3_TE_SHORT     240U
#define FORD_V3_TE_LONG      480U
#define FORD_V3_TE_DELTA     60U
#define FORD_V3_DATA_BITS    104U
#define FORD_V3_DATA_BYTES   13U
#define FORD_V3_PREAMBLE_MIN 30U

#define FORD_V3_BTN_LOCK   0x01U
#define FORD_V3_BTN_UNLOCK 0x02U

static const SubGhzBlockConst subghz_protocol_ford_v3_const = {
    .te_short = FORD_V3_TE_SHORT,
    .te_long = FORD_V3_TE_LONG,
    .te_delta = FORD_V3_TE_DELTA,
    .min_count_bit_for_found = FORD_V3_DATA_BITS,
};

typedef enum {
    FordV3DecoderStepReset = 0,
    FordV3DecoderStepPreamble = 1,
    FordV3DecoderStepData = 2,
} FordV3DecoderStep;

typedef struct SubGhzProtocolDecoderFordV3 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    ManchesterState manchester_state;
    uint8_t raw_bytes[FORD_V3_DATA_BYTES];
    uint8_t bit_count;
    uint16_t preamble_count;

    uint32_t serial;
    uint16_t counter;
} SubGhzProtocolDecoderFordV3;

static void ford_v3_reset_data(SubGhzProtocolDecoderFordV3* instance);
static void ford_v3_add_bit(SubGhzProtocolDecoderFordV3* instance, bool bit);
static void ford_v3_parse_fields(SubGhzProtocolDecoderFordV3* instance);
static void ford_v3_emit_if_ready(SubGhzProtocolDecoderFordV3* instance);
static const char* ford_v3_button_name(uint8_t btn);

static const char* ford_v3_button_name(uint8_t btn) {
    switch(btn) {
    case FORD_V3_BTN_LOCK:
        return "Lock";
    case FORD_V3_BTN_UNLOCK:
        return "Unlock";
    default:
        return "?";
    }
}

static void ford_v3_reset_data(SubGhzProtocolDecoderFordV3* instance) {
    memset(instance->raw_bytes, 0, sizeof(instance->raw_bytes));
    instance->bit_count = 0;
    instance->preamble_count = 0;
    manchester_advance(
        instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
}

static void ford_v3_add_bit(SubGhzProtocolDecoderFordV3* instance, bool bit) {
    if(instance->bit_count >= FORD_V3_DATA_BITS) {
        return;
    }

    const uint8_t byte_index = instance->bit_count / 8U;
    const uint8_t bit_in_byte = 7U - (instance->bit_count % 8U);
    if(bit) {
        instance->raw_bytes[byte_index] |= (uint8_t)(1U << bit_in_byte);
    }
    instance->bit_count++;
}

static void ford_v3_parse_fields(SubGhzProtocolDecoderFordV3* instance) {
    const uint8_t* b = instance->raw_bytes;

    instance->serial = ((uint32_t)b[1] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 8) |
                       (uint32_t)b[4];
    instance->counter =
        (uint16_t)((((uint16_t)(uint8_t)~b[7]) << 8) | (uint8_t)~b[8]);

    instance->generic.serial = instance->serial;
    instance->generic.btn = (b[6] & 0x01U) ? FORD_V3_BTN_UNLOCK : FORD_V3_BTN_LOCK;
    instance->generic.cnt = instance->counter;
}

static void ford_v3_emit_if_ready(SubGhzProtocolDecoderFordV3* instance) {
    if(instance->bit_count < FORD_V3_DATA_BITS) {
        return;
    }

    /* False-positive gate (confirmed VW-OOK -> FordV3). (a) Reject degenerate
     * all-0x00 / all-0xFF frames that a non-Ford OOK burst decodes into. */
    const uint8_t* rb = instance->raw_bytes;
    bool all_zero = true;
    bool all_ff = true;
    for(uint8_t i = 0; i < FORD_V3_DATA_BYTES; i++) {
        if(rb[i] != 0x00U) all_zero = false;
        if(rb[i] != 0xFFU) all_ff = false;
    }
    if(all_zero || all_ff) {
        return;
    }

    instance->generic.data_count_bit = FORD_V3_DATA_BITS;
    ford_v3_parse_fields(instance);

    /* (b) Require a known Lock/Unlock button and (c) a plausible non-zero
     * serial before reporting a decode. */
    if(instance->generic.btn != FORD_V3_BTN_LOCK &&
       instance->generic.btn != FORD_V3_BTN_UNLOCK) {
        return;
    }
    if(instance->generic.serial == 0) {
        return;
    }

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

void* subghz_protocol_decoder_ford_v3_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);

    SubGhzProtocolDecoderFordV3* instance = calloc(1, sizeof(SubGhzProtocolDecoderFordV3));
    furi_check(instance);

    instance->base.protocol = &ford_protocol_v3;
    instance->generic.protocol_name = instance->base.protocol->name;

    return instance;
}

void subghz_protocol_decoder_ford_v3_reset(void* context) {
    furi_check(context);

    SubGhzProtocolDecoderFordV3* instance = context;
    instance->decoder.parser_step = FordV3DecoderStepReset;
    ford_v3_reset_data(instance);
}

void subghz_protocol_decoder_ford_v3_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);

    SubGhzProtocolDecoderFordV3* instance = context;

    switch(instance->decoder.parser_step) {
    case FordV3DecoderStepReset:
        if(pp_is_short(duration, &subghz_protocol_ford_v3_const)) {
            ford_v3_reset_data(instance);
            instance->preamble_count = 1U;
            instance->decoder.parser_step = FordV3DecoderStepPreamble;
        }
        break;

    case FordV3DecoderStepPreamble:
        if(pp_is_short(duration, &subghz_protocol_ford_v3_const)) {
            instance->preamble_count++;
        } else if(
            instance->preamble_count >= FORD_V3_PREAMBLE_MIN &&
            pp_is_long(duration, &subghz_protocol_ford_v3_const)) {
            instance->manchester_state = ManchesterStateMid1;

            const ManchesterEvent event =
                level ? ManchesterEventLongHigh : ManchesterEventLongLow;

            bool data_bit = false;
            const bool valid = manchester_advance(
                instance->manchester_state, event, &instance->manchester_state, &data_bit);
            if(valid) {
                ford_v3_add_bit(instance, data_bit);
            }
            instance->decoder.parser_step = FordV3DecoderStepData;
        } else {
            instance->decoder.parser_step = FordV3DecoderStepReset;
        }
        break;

    case FordV3DecoderStepData: {
        if(!pp_is_short(duration, &subghz_protocol_ford_v3_const) &&
           !pp_is_long(duration, &subghz_protocol_ford_v3_const)) {
            ford_v3_emit_if_ready(instance);
            instance->decoder.parser_step = FordV3DecoderStepReset;

            if(pp_is_short(duration, &subghz_protocol_ford_v3_const)) {
                ford_v3_reset_data(instance);
                instance->preamble_count = 1U;
                instance->decoder.parser_step = FordV3DecoderStepPreamble;
            }
            break;
        }

        ManchesterEvent event;
        if(level) {
            event = pp_is_short(duration, &subghz_protocol_ford_v3_const) ? ManchesterEventShortHigh :
                                                                           ManchesterEventLongHigh;
        } else {
            event = pp_is_short(duration, &subghz_protocol_ford_v3_const) ? ManchesterEventShortLow :
                                                                           ManchesterEventLongLow;
        }

        bool data_bit = false;
        const bool valid = manchester_advance(
            instance->manchester_state, event, &instance->manchester_state, &data_bit);

        if(valid) {
            ford_v3_add_bit(instance, data_bit);
            if(instance->bit_count >= FORD_V3_DATA_BITS) {
                ford_v3_emit_if_ready(instance);
                instance->decoder.parser_step = FordV3DecoderStepReset;
            }
        }
        break;
    }
    }
}

uint8_t subghz_protocol_decoder_ford_v3_get_hash_data(void* context) {
    furi_check(context);

    SubGhzProtocolDecoderFordV3* instance = context;
    uint8_t hash = 0;

    for(size_t i = 0; i < FORD_V3_DATA_BYTES; i++) {
        hash ^= instance->raw_bytes[i];
    }

    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_ford_v3_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);

    SubGhzProtocolDecoderFordV3* instance = context;

    instance->generic.data = ((uint64_t)instance->raw_bytes[0] << 56) |
                             ((uint64_t)instance->raw_bytes[1] << 48) |
                             ((uint64_t)instance->raw_bytes[2] << 40) |
                             ((uint64_t)instance->raw_bytes[3] << 32) |
                             ((uint64_t)instance->raw_bytes[4] << 24) |
                             ((uint64_t)instance->raw_bytes[5] << 16) |
                             ((uint64_t)instance->raw_bytes[6] << 8) |
                             (uint64_t)instance->raw_bytes[7];
    instance->generic.data_count_bit = FORD_V3_DATA_BITS;

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);

    if(ret == SubGhzProtocolStatusOk) {
        flipper_format_rewind(flipper_format);
        flipper_format_insert_or_update_hex(
            flipper_format, "Raw", instance->raw_bytes, FORD_V3_DATA_BYTES);

        pp_flipper_update_or_insert_u32(flipper_format, FF_SERIAL, instance->generic.serial);
        pp_flipper_update_or_insert_u32(flipper_format, FF_BTN, instance->generic.btn);
        pp_flipper_update_or_insert_u32(flipper_format, FF_CNT, instance->counter);
    }

    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_ford_v3_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);

    SubGhzProtocolDecoderFordV3* instance = context;

    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_ford_v3_const.min_count_bit_for_found);

    if(ret != SubGhzProtocolStatusOk) {
        return ret;
    }

    const uint64_t d = instance->generic.data;
    for(uint8_t i = 0; i < 8U; i++) {
        instance->raw_bytes[i] = (uint8_t)(d >> (56 - i * 8));
    }
    memset(&instance->raw_bytes[8], 0, FORD_V3_DATA_BYTES - 8U);

    flipper_format_rewind(flipper_format);
    flipper_format_read_hex(flipper_format, "Raw", instance->raw_bytes, FORD_V3_DATA_BYTES);

    instance->bit_count = FORD_V3_DATA_BITS;
    ford_v3_parse_fields(instance);

    return ret;
}

void subghz_protocol_decoder_ford_v3_get_string(void* context, FuriString* output) {
    furi_check(context);

    SubGhzProtocolDecoderFordV3* instance = context;
    const uint8_t* k = instance->raw_bytes;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X\r\n"
        "Sn:%08lX Btn:%02X %s\r\n"
        "Cnt:%04X Hop:%02X%02X%02X%02X\r\n",
        instance->generic.protocol_name,
        (int)instance->generic.data_count_bit,
        k[0],
        k[1],
        k[2],
        k[3],
        k[4],
        k[5],
        k[6],
        k[7],
        k[8],
        k[9],
        k[10],
        k[11],
        k[12],
        (unsigned long)instance->generic.serial,
        instance->generic.btn,
        ford_v3_button_name(instance->generic.btn),
        (unsigned)instance->counter,
        k[9],
        k[10],
        k[11],
        k[12]);
}

const SubGhzProtocolDecoder subghz_protocol_ford_v3_decoder = {
    .alloc = subghz_protocol_decoder_ford_v3_alloc,
    .free = pp_decoder_free_default,
    .feed = subghz_protocol_decoder_ford_v3_feed,
    .reset = subghz_protocol_decoder_ford_v3_reset,
    .get_hash_data = subghz_protocol_decoder_ford_v3_get_hash_data,
    .serialize = subghz_protocol_decoder_ford_v3_serialize,
    .deserialize = subghz_protocol_decoder_ford_v3_deserialize,
    .get_string = subghz_protocol_decoder_ford_v3_get_string,
};

#ifdef ENABLE_EMULATE_FEATURE

#define FORD_V3_ENC_TE_SHORT           FORD_V3_TE_SHORT
#define FORD_V3_ENC_TE_LONG            FORD_V3_TE_LONG
#define FORD_V3_ENC_PREAMBLE_PAIRS     40U
#define FORD_V3_ENC_GAP_US             22000U
#define FORD_V3_ENCODER_DEFAULT_REPEAT 10U
// Worst case: preamble pairs (2 each) + sync (1) + data bits (up to 2 each) + trailing gap (1).
#define FORD_V3_ENC_MAX_ELEMS \
    ((FORD_V3_ENC_PREAMBLE_PAIRS * 2U) + 1U + (FORD_V3_DATA_BITS * 2U) + 1U)
_Static_assert(
    FORD_V3_ENC_MAX_ELEMS <= PP_SHARED_UPLOAD_CAPACITY,
    "FORD_V3_ENC_MAX_ELEMS exceeds shared upload slab");

typedef struct SubGhzProtocolEncoderFordV3 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
    uint8_t raw_bytes[FORD_V3_DATA_BYTES];
} SubGhzProtocolEncoderFordV3;

// Re-apply the editable fields onto the raw on-air bytes using the exact inverse of the
// decoder's ford_v3_parse_fields() mapping. For a faithful replay this is a no-op; it only
// matters when the user edits Serial/Btn/Cnt in the UI. All other bytes (b[0], b[5], the
// upper bits of b[6] and the hop bytes b[9..12]) are preserved from the captured frame.
static void ford_v3_encoder_apply_fields(SubGhzProtocolEncoderFordV3* instance) {
    uint8_t* b = instance->raw_bytes;

    b[1] = (uint8_t)(instance->generic.serial >> 24);
    b[2] = (uint8_t)(instance->generic.serial >> 16);
    b[3] = (uint8_t)(instance->generic.serial >> 8);
    b[4] = (uint8_t)(instance->generic.serial);

    if(instance->generic.btn == FORD_V3_BTN_UNLOCK) {
        b[6] |= 0x01U;
    } else {
        b[6] = (uint8_t)(b[6] & ~0x01U);
    }

    b[7] = (uint8_t)~(uint8_t)(instance->generic.cnt >> 8);
    b[8] = (uint8_t)~(uint8_t)(instance->generic.cnt);
}

// Build the on-air waveform the ford_v3 decoder inverts: an OOK short-pulse preamble, one
// LONG-low sync pulse (aligns the Manchester decoder to Mid1 without producing a bit), then
// the 104 data bits MSB-first as differential Manchester. Rule (cur_high tracks the decoder's
// Mid1/Mid0 state): from Mid1 (high) bit0 = one LONG-high, bit1 = SHORT-high + SHORT-low;
// from Mid0 (low) bit1 = one LONG-low, bit0 = SHORT-low + SHORT-high.
static void ford_v3_encoder_build_upload(SubGhzProtocolEncoderFordV3* instance) {
    LevelDuration* const up = instance->encoder.upload;
    const size_t cap = FORD_V3_ENC_MAX_ELEMS;
    size_t i = 0;

    for(size_t p = 0; p < FORD_V3_ENC_PREAMBLE_PAIRS; p++) {
        i = pp_emit(up, i, cap, false, FORD_V3_ENC_TE_SHORT);
        i = pp_emit(up, i, cap, true, FORD_V3_ENC_TE_SHORT);
    }

    i = pp_emit(up, i, cap, false, FORD_V3_ENC_TE_LONG);

    bool cur_high = true;
    for(uint16_t bit_pos = 0; bit_pos < FORD_V3_DATA_BITS; bit_pos++) {
        const uint8_t byte_idx = (uint8_t)(bit_pos >> 3);
        const uint8_t bit =
            (uint8_t)((instance->raw_bytes[byte_idx] >> (7U - (bit_pos & 7U))) & 1U);

        if(cur_high) {
            if(bit == 0U) {
                i = pp_emit(up, i, cap, true, FORD_V3_ENC_TE_LONG);
                cur_high = false;
            } else {
                i = pp_emit(up, i, cap, true, FORD_V3_ENC_TE_SHORT);
                i = pp_emit(up, i, cap, false, FORD_V3_ENC_TE_SHORT);
            }
        } else {
            if(bit == 1U) {
                i = pp_emit(up, i, cap, false, FORD_V3_ENC_TE_LONG);
                cur_high = true;
            } else {
                i = pp_emit(up, i, cap, false, FORD_V3_ENC_TE_SHORT);
                i = pp_emit(up, i, cap, true, FORD_V3_ENC_TE_SHORT);
            }
        }
    }

    // Trailing gap at the next-expected level so it never merges with the last data pulse.
    i = pp_emit(up, i, cap, cur_high, FORD_V3_ENC_GAP_US);

    instance->encoder.size_upload = i;
    instance->encoder.front = 0;
}

static void* subghz_protocol_encoder_ford_v3_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderFordV3* instance = calloc(1, sizeof(SubGhzProtocolEncoderFordV3));
    furi_check(instance);
    instance->base.protocol = &ford_protocol_v3;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = FORD_V3_ENCODER_DEFAULT_REPEAT;
    instance->encoder.front = 0;
    instance->encoder.is_running = false;
    pp_encoder_buffer_ensure(instance, FORD_V3_ENC_MAX_ELEMS);
    return instance;
}

static SubGhzProtocolStatus
    subghz_protocol_encoder_ford_v3_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderFordV3* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    instance->encoder.is_running = false;
    instance->encoder.front = 0;
    instance->encoder.repeat = FORD_V3_ENCODER_DEFAULT_REPEAT;

    do {
        flipper_format_rewind(flipper_format);
        if(pp_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
           SubGhzProtocolStatusOk) {
            break;
        }

        SubGhzProtocolStatus g = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_ford_v3_const.min_count_bit_for_found);
        if(g != SubGhzProtocolStatusOk) {
            ret = g;
            break;
        }
        if(instance->generic.data_count_bit != FORD_V3_DATA_BITS) {
            ret = SubGhzProtocolStatusErrorValueBitCount;
            break;
        }

        // Seed the on-air bytes from the packed 8-byte key, then let the full 13-byte Raw
        // field (written by the decoder's serialize) override it when present.
        const uint64_t d = instance->generic.data;
        for(uint8_t i = 0; i < 8U; i++) {
            instance->raw_bytes[i] = (uint8_t)(d >> (56 - i * 8));
        }
        memset(&instance->raw_bytes[8], 0, FORD_V3_DATA_BYTES - 8U);

        flipper_format_rewind(flipper_format);
        flipper_format_read_hex(flipper_format, "Raw", instance->raw_bytes, FORD_V3_DATA_BYTES);

        uint32_t serial = UINT32_MAX;
        uint32_t btn = UINT32_MAX;
        uint32_t cnt = UINT32_MAX;
        pp_encoder_read_fields(flipper_format, &serial, &btn, &cnt, NULL);
        if(serial != UINT32_MAX) instance->generic.serial = serial;
        if(btn != UINT32_MAX) instance->generic.btn = (uint8_t)btn;
        if(cnt != UINT32_MAX) instance->generic.cnt = cnt;
        ford_v3_encoder_apply_fields(instance);

        instance->encoder.repeat =
            pp_encoder_read_repeat(flipper_format, FORD_V3_ENCODER_DEFAULT_REPEAT);
        ford_v3_encoder_build_upload(instance);

        instance->encoder.is_running = true;
        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

const SubGhzProtocolEncoder subghz_protocol_ford_v3_encoder = {
    .alloc = subghz_protocol_encoder_ford_v3_alloc,
    .free = pp_encoder_free,
    .deserialize = subghz_protocol_encoder_ford_v3_deserialize,
    .stop = pp_encoder_stop,
    .yield = pp_encoder_yield,
};

#else
const SubGhzProtocolEncoder subghz_protocol_ford_v3_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};
#endif

const SubGhzProtocol ford_protocol_v3 = {
    .name = FORD_PROTOCOL_V3_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save
#ifdef ENABLE_EMULATE_FEATURE
            | SubGhzProtocolFlag_Send
#endif
    ,
    .decoder = &subghz_protocol_ford_v3_decoder,
    .encoder = &subghz_protocol_ford_v3_encoder,
};
