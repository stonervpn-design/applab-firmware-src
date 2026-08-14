/**
 * Jeep V0 SubGHz Protocol — 96-bit Manchester rolling code
 *
 * Physical: OOK Manchester @ 433.889 MHz
 * Manchester: te_short=200µs, te_long=400µs
 *
 * Frame: [Preamble: 0xFF bytes] [Sync: 0xFE] [Data: 96 bits]
 * Each button press sends 4 repetitions of the frame.
 *
 * Data (96 bits = 12 bytes):
 *   B0-B3:  Serial (32 bit, fixed per keyfob)
 *   B4-B5:  Button code (16 bit)
 *           Unlock=0x8104, Lock=0x8044, RemoteStart=0x8804, Panic=0x8024
 *   B6:     Fixed 0x07
 *   B7:     Counter (8 bit, incrementing)
 *   B8-B11: Hop code (32 bit rolling code)
 *
 * Preamble: Manchester-encoded 0xFF bytes (alternating short pulses).
 * Sync: 0xFE (11111110) — first 0-bit marks end of preamble.
 *
 * Source signals: Jeep Renegade 2022 keyfob @ 433.889 MHz
 */

#include "jeep_v0.h"

#include <lib/toolbox/manchester_decoder.h>
#include <lib/toolbox/manchester_encoder.h>

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include <furi.h>
#include <furi_hal.h>

#define TAG "SubGhzProtocolJeepV0"

#define JEEP_V0_DATA_BITS     96
#define JEEP_V0_MIN_PREAMBLE  16
#define JEEP_V0_PREAMBLE_BITS 20
#define JEEP_V0_TOTAL_BURSTS  4
// Preamble(20) + sync(1) + data(96) bits, up to 2 pulses/bit + finish + gap.
#define JEEP_V0_UPLOAD_CAP    400

#define JEEP_V0_BTN_UNLOCK    0x8104
#define JEEP_V0_BTN_LOCK      0x8044
#define JEEP_V0_BTN_RS        0x8804
#define JEEP_V0_BTN_PANIC     0x8024

static const SubGhzBlockConst subghz_protocol_jeep_v0_const = {
    .te_short = 200,
    .te_long = 400,
    .te_delta = 100,
    .min_count_bit_for_found = JEEP_V0_DATA_BITS,
};

// ============================================================================
// DATA STRUCTURES
// ============================================================================

typedef enum {
    JeepV0DecoderStepReset = 0,
    JeepV0DecoderStepPreamble,
    JeepV0DecoderStepData,
} JeepV0DecoderStep;

struct SubGhzProtocolDecoderJeepV0 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    ManchesterState manchester_state;
    uint16_t preamble_count;
    uint8_t sync_shift;
    uint32_t data_32; // bits 64-95
};

struct SubGhzProtocolEncoderJeepV0 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint32_t data_32;
};

// ============================================================================
// HELPERS
// ============================================================================

static const char* jeep_v0_btn_name(uint16_t btn_code) {
    switch(btn_code) {
    case JEEP_V0_BTN_UNLOCK: return "Unlock";
    case JEEP_V0_BTN_LOCK:   return "Lock";
    case JEEP_V0_BTN_RS:     return "RS";
    case JEEP_V0_BTN_PANIC:  return "Panic";
    default:                  return "??";
    }
}

static void jeep_v0_extract_fields(SubGhzProtocolDecoderJeepV0* instance) {
    uint64_t data = instance->generic.data;

    instance->generic.serial = (uint32_t)(data >> 32);           // B0-B3
    uint16_t btn_code = (uint16_t)((data >> 16) & 0xFFFF);      // B4-B5
    instance->generic.btn = btn_code >> 8;                        // B4
    instance->generic.type = btn_code & 0xFF;                     // B5
    instance->generic.cnt = (uint32_t)(data & 0xFF);             // B7
    instance->generic.seed = instance->data_32;                   // B8-B11 hop
}

// ============================================================================
// PROTOCOL DEFINITION
// ============================================================================

#ifdef SUBGHZ_DECODER_TEST
static SubGhzBlockGeneric* subghz_protocol_decoder_jeep_v0_get_generic(void* context) {
    SubGhzProtocolDecoderJeepV0* instance = context;
    return &instance->generic;
}
#endif

const SubGhzProtocolDecoder subghz_protocol_jeep_v0_decoder = {
    .alloc = subghz_protocol_decoder_jeep_v0_alloc,
    .free = subghz_protocol_decoder_jeep_v0_free,
    .feed = subghz_protocol_decoder_jeep_v0_feed,
    .reset = subghz_protocol_decoder_jeep_v0_reset,
    .get_hash_data = subghz_protocol_decoder_jeep_v0_get_hash_data,
    .serialize = subghz_protocol_decoder_jeep_v0_serialize,
    .deserialize = subghz_protocol_decoder_jeep_v0_deserialize,
    .get_string = subghz_protocol_decoder_jeep_v0_get_string,
#ifdef SUBGHZ_DECODER_TEST
    .get_generic = subghz_protocol_decoder_jeep_v0_get_generic,
#endif
};

const SubGhzProtocolEncoder subghz_protocol_jeep_v0_encoder = {
    .alloc = subghz_protocol_encoder_jeep_v0_alloc,
    .free = subghz_protocol_encoder_jeep_v0_free,
    .deserialize = subghz_protocol_encoder_jeep_v0_deserialize,
    .stop = subghz_protocol_encoder_jeep_v0_stop,
    .yield = subghz_protocol_encoder_jeep_v0_yield,
};

const SubGhzProtocol subghz_protocol_jeep_v0 = {
    .name = SUBGHZ_PROTOCOL_JEEP_V0_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_jeep_v0_decoder,
    .encoder = &subghz_protocol_jeep_v0_encoder,
};

// ============================================================================
// DECODER
// ============================================================================

void* subghz_protocol_decoder_jeep_v0_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderJeepV0* instance = malloc(sizeof(SubGhzProtocolDecoderJeepV0));
    instance->base.protocol = &subghz_protocol_jeep_v0;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_jeep_v0_free(void* context) {
    furi_assert(context);
    free(context);
}

void subghz_protocol_decoder_jeep_v0_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderJeepV0* instance = context;
    instance->decoder.parser_step = JeepV0DecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->preamble_count = 0;
    instance->sync_shift = 0;
    instance->data_32 = 0;
    manchester_advance(
        instance->manchester_state, ManchesterEventReset,
        &instance->manchester_state, NULL);
}

static bool jeep_v0_classify_timing(uint32_t duration, bool level, ManchesterEvent* event) {
    if(DURATION_DIFF(duration, 200) <= 100) {
        *event = level ? ManchesterEventShortHigh : ManchesterEventShortLow;
        return true;
    }
    if(DURATION_DIFF(duration, 400) <= 100) {
        *event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
        return true;
    }
    return false;
}

void subghz_protocol_decoder_jeep_v0_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderJeepV0* instance = context;

    switch(instance->decoder.parser_step) {
    case JeepV0DecoderStepReset:
        // Wait for short pulse to start preamble counting
        if(DURATION_DIFF(duration, 200) <= 100) {
            instance->preamble_count = 1;
            instance->decoder.te_last = duration;
            instance->decoder.parser_step = JeepV0DecoderStepPreamble;
        }
        break;

    case JeepV0DecoderStepPreamble: {
        bool is_short = DURATION_DIFF(duration, 200) <= 100;
        bool is_long = DURATION_DIFF(duration, 400) <= 100;

        if(is_short) {
            instance->preamble_count++;
        } else if(is_long && instance->preamble_count >= JEEP_V0_MIN_PREAMBLE) {
            // First LONG pulse after preamble — transition to data decode.
            // Initialize Manchester decoder and start data collection.
            ManchesterEvent event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
            manchester_advance(
                instance->manchester_state, ManchesterEventReset,
                &instance->manchester_state, NULL);
            manchester_advance(
                instance->manchester_state, event,
                &instance->manchester_state, NULL);

            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->data_32 = 0;
            instance->decoder.parser_step = JeepV0DecoderStepData;
        } else {
            instance->decoder.parser_step = JeepV0DecoderStepReset;
            instance->preamble_count = 0;
        }
    } break;

    case JeepV0DecoderStepData: {
        ManchesterEvent event;
        if(!jeep_v0_classify_timing(duration, level, &event)) {
            instance->decoder.parser_step = JeepV0DecoderStepReset;
            break;
        }

        bool bit;
        if(manchester_advance(
               instance->manchester_state, event,
               &instance->manchester_state, &bit)) {
            if(instance->decoder.decode_count_bit < 64) {
                instance->decoder.decode_data =
                    (instance->decoder.decode_data << 1) | bit;
            } else {
                instance->data_32 =
                    (instance->data_32 << 1) | bit;
            }
            instance->decoder.decode_count_bit++;

            if(instance->decoder.decode_count_bit == JEEP_V0_DATA_BITS) {
                // Invert — Manchester convention is inverted in this protocol
                instance->generic.data = ~instance->decoder.decode_data;
                instance->data_32 = ~instance->data_32;
                instance->generic.data_count_bit = JEEP_V0_DATA_BITS;

                jeep_v0_extract_fields(instance);

                FURI_LOG_I(TAG, "Decoded: Sn:%08lX Btn:%04X Cnt:%lu",
                    (unsigned long)instance->generic.serial,
                    (unsigned)((instance->generic.btn << 8) | instance->generic.type),
                    (unsigned long)instance->generic.cnt);

                if(instance->base.callback) {
                    instance->base.callback(&instance->base, instance->base.context);
                }

                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->data_32 = 0;
                instance->preamble_count = 0;
                instance->decoder.parser_step = JeepV0DecoderStepReset;
            }
        }
    } break;

    default:
        instance->decoder.parser_step = JeepV0DecoderStepReset;
        break;
    }
}

// ============================================================================
// HASH / SERIALIZATION
// ============================================================================

uint8_t subghz_protocol_decoder_jeep_v0_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderJeepV0* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_jeep_v0_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderJeepV0* instance = context;

    SubGhzProtocolStatus res =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(res != SubGhzProtocolStatusOk) return res;

    uint32_t k2 = instance->data_32;
    if(!flipper_format_rewind(flipper_format)) return SubGhzProtocolStatusErrorParserOthers;
    if(!flipper_format_insert_or_update_uint32(flipper_format, "Key2", &k2, 1))
        return SubGhzProtocolStatusErrorParserOthers;

    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_jeep_v0_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderJeepV0* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize(&instance->generic, flipper_format);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint32_t k2 = 0;
    if(!flipper_format_read_uint32(flipper_format, "Key2", &k2, 1))
        return SubGhzProtocolStatusError;

    instance->data_32 = k2;
    instance->generic.protocol_name = instance->base.protocol->name;

    return SubGhzProtocolStatusOk;
}

// ============================================================================
// DISPLAY
// ============================================================================

void subghz_protocol_decoder_jeep_v0_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderJeepV0* instance = context;

    uint16_t btn_code = ((uint16_t)instance->generic.btn << 8) | instance->generic.type;
    uint32_t hop = instance->data_32;

    subghz_block_generic_global.cnt_is_available = true;
    subghz_block_generic_global.cnt_length_bit = 8;
    subghz_block_generic_global.current_cnt = instance->generic.cnt;
    subghz_block_generic_global.btn_is_available = true;
    subghz_block_generic_global.btn_length_bit = 16;
    subghz_block_generic_global.current_btn = instance->generic.btn;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Sn:%08lX Btn:%s\r\n"
        "Cnt:%02lX Hop:%08lX",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long)instance->generic.serial,
        jeep_v0_btn_name(btn_code),
        (unsigned long)instance->generic.cnt,
        (unsigned long)hop);
}

// ============================================================================
// ENCODER (replay-only — hop code encryption unknown)
// ============================================================================

void* subghz_protocol_encoder_jeep_v0_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderJeepV0* instance = malloc(sizeof(SubGhzProtocolEncoderJeepV0));
    instance->base.protocol = &subghz_protocol_jeep_v0;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = JEEP_V0_TOTAL_BURSTS;
    instance->encoder.size_upload = JEEP_V0_UPLOAD_CAP;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_jeep_v0_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderJeepV0* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

static LevelDuration
    jeep_v0_res_to_ld(ManchesterEncoderResult result, uint32_t te_s, uint32_t te_l) {
    switch(result) {
    case ManchesterEncoderResultShortLow:
        return level_duration_make(false, te_s);
    case ManchesterEncoderResultLongLow:
        return level_duration_make(false, te_l);
    case ManchesterEncoderResultLongHigh:
        return level_duration_make(true, te_l);
    case ManchesterEncoderResultShortHigh:
    default:
        return level_duration_make(true, te_s);
    }
}

/*
 * Rebuild the on-air waveform the decoder consumes. The decoder counts short
 * preamble pulses, then keys off the FIRST long pulse to enter Manchester data
 * decode; that long only appears where the toolbox Manchester encoder MERGES two
 * same-level half-bits (a 1->0 transition), so we must drive that encoder rather
 * than emit raw half-bit pairs. The decoder also stores ~bits (generic.data =
 * ~decode_data), so the air bits are the bitwise inverse of the stored frame.
 * Layout: [PREAMBLE ones][one sync 0-bit -> long][96 data bits, inverted].
 */
static void jeep_v0_build_upload(SubGhzProtocolEncoderJeepV0* instance) {
    furi_assert(instance);
    LevelDuration* up = instance->encoder.upload;
    const size_t cap = JEEP_V0_UPLOAD_CAP;
    size_t index = 0;
    uint32_t te_s = subghz_protocol_jeep_v0_const.te_short;
    uint32_t te_l = subghz_protocol_jeep_v0_const.te_long;

    ManchesterEncoderState enc;
    manchester_encoder_reset(&enc);
    ManchesterEncoderResult result;

#define JEEP_V0_PUSH_BIT(bitv)                                             \
    do {                                                                   \
        bool _b = (bitv);                                                  \
        if(!manchester_encoder_advance(&enc, _b, &result)) {               \
            if(index < cap) up[index++] = jeep_v0_res_to_ld(result, te_s, te_l); \
            manchester_encoder_advance(&enc, _b, &result);                 \
        }                                                                  \
        if(index < cap) up[index++] = jeep_v0_res_to_ld(result, te_s, te_l); \
    } while(0)

    // Preamble: one-bits (all short pulses); >= JEEP_V0_MIN_PREAMBLE required.
    for(int i = 0; i < JEEP_V0_PREAMBLE_BITS; i++) JEEP_V0_PUSH_BIT(true);
    // Sync: a single 0-bit yields the long pulse the decoder transitions on.
    JEEP_V0_PUSH_BIT(false);

    // Data: 96 inverted bits, MSB-first. B0-B7 from generic.data, B8-B11 from hop.
    uint64_t inv_data = ~instance->generic.data;
    uint32_t inv_hop = ~instance->data_32;
    for(int i = 63; i >= 0; i--) JEEP_V0_PUSH_BIT((inv_data >> i) & 1);
    for(int i = 31; i >= 0; i--) JEEP_V0_PUSH_BIT((inv_hop >> i) & 1);

    // Flush the final Manchester half-bit.
    if(index < cap) up[index++] = jeep_v0_res_to_ld(manchester_encoder_finish(&enc), te_s, te_l);

#undef JEEP_V0_PUSH_BIT

    // End gap
    if(index < cap) up[index++] = level_duration_make(false, 78000);

    instance->encoder.size_upload = index;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_jeep_v0_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderJeepV0* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;

    do {
        if(subghz_block_generic_deserialize(&instance->generic, flipper_format) !=
           SubGhzProtocolStatusOk) {
            break;
        }

        uint32_t k2 = 0;
        if(!flipper_format_read_uint32(flipper_format, "Key2", &k2, 1)) break;
        instance->data_32 = k2;

        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        jeep_v0_build_upload(instance);
        instance->encoder.is_running = true;
        res = SubGhzProtocolStatusOk;
    } while(false);

    return res;
}

void subghz_protocol_encoder_jeep_v0_stop(void* context) {
    SubGhzProtocolEncoderJeepV0* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_jeep_v0_yield(void* context) {
    SubGhzProtocolEncoderJeepV0* instance = context;

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
