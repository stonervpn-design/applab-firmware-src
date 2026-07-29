/**
 * @file bmw_v2.c
 * @brief BMW V2 (Mini Keyfob) SubGHz Protocol Implementation
 *
 * Dynamic protocol with 56-bit Manchester-encoded rolling code.
 * Manchester encoding with 500us short / 1000us long timing.
 * Fixed header byte 0xCF, fixed trailer byte 0x3A,
 * 40-bit encrypted rolling code payload.
 *
 * Signal structure (7 bytes / 56 bits):
 *   Byte 0:   0xCF (fixed sync/header)
 *   Byte 1-5: 40-bit rolling code (encrypted)
 *   Byte 6:   0x3A (fixed trailer)
 *
 * Preamble: ~1000us HIGH mark before data
 * Gap between repeats: ~2450us
 * Each button press: 4-5 repeats of same message
 */
#include "bmw_v2.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#define TAG "SubGhzProtocolBmwV2"

static const SubGhzBlockConst subghz_protocol_bmw_v2_const = {
    .te_short = 500,
    .te_long = 1000,
    .te_delta = 150,
    .min_count_bit_for_found = 56,
};

#define BMW_V2_GAP_MIN    1700
#define BMW_V2_HEADER     0xCF
#define BMW_V2_TRAILER    0x3A

// Preamble(1 LL) + 56 bits Manchester (112 transitions) + gap = ~120
#define BMW_V2_ENCODER_UPLOAD_MAX_SIZE 130

// ============================================================================
// DATA STRUCTURES
// ============================================================================

struct SubGhzProtocolDecoderBmwV2 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
};

struct SubGhzProtocolEncoderBmwV2 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};

typedef enum {
    BmwV2DecoderStepReset = 0,
    BmwV2DecoderStepWaitGap,
    BmwV2DecoderStepManchester,
} BmwV2DecoderStep;

// ============================================================================
// PROTOCOL DESCRIPTOR
// ============================================================================

static SubGhzBlockGeneric* subghz_protocol_decoder_bmw_v2_get_generic(void* context) {
    SubGhzProtocolDecoderBmwV2* instance = context;
    return &instance->generic;
}

const SubGhzProtocolDecoder subghz_protocol_bmw_v2_decoder = {
    .alloc = subghz_protocol_decoder_bmw_v2_alloc,
    .free = subghz_protocol_decoder_bmw_v2_free,
    .feed = subghz_protocol_decoder_bmw_v2_feed,
    .reset = subghz_protocol_decoder_bmw_v2_reset,
    .get_hash_data = subghz_protocol_decoder_bmw_v2_get_hash_data,
    .serialize = subghz_protocol_decoder_bmw_v2_serialize,
    .deserialize = subghz_protocol_decoder_bmw_v2_deserialize,
    .get_string = subghz_protocol_decoder_bmw_v2_get_string,
    .get_generic = subghz_protocol_decoder_bmw_v2_get_generic,
};

const SubGhzProtocolEncoder subghz_protocol_bmw_v2_encoder = {
    .alloc = subghz_protocol_encoder_bmw_v2_alloc,
    .free = subghz_protocol_encoder_bmw_v2_free,
    .deserialize = subghz_protocol_encoder_bmw_v2_deserialize,
    .stop = subghz_protocol_encoder_bmw_v2_stop,
    .yield = subghz_protocol_encoder_bmw_v2_yield,
};

const SubGhzProtocol subghz_protocol_bmw_v2 = {
    .name = SUBGHZ_PROTOCOL_BMW_V2_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_bmw_v2_decoder,
    .encoder = &subghz_protocol_bmw_v2_encoder,
};

// ============================================================================
// CHECK REMOTE CONTROLLER
// ============================================================================

static void bmw_v2_check_remote_controller(SubGhzBlockGeneric* instance) {
    // 56-bit data: CF XX XX XX XX XX 3A
    // Bytes 1-5 are encrypted rolling code
    uint64_t data = instance->data;

    // Extract 40-bit rolling code (bytes 1-5)
    uint64_t rolling = (data >> 8) & 0xFFFFFFFFFFULL;

    instance->serial = (rolling >> 16) & 0xFFFFFF;  // upper 24 bits as "serial"
    instance->cnt = rolling & 0xFFFF;                // lower 16 bits as "counter"
    instance->btn = 0;  // button not distinguishable in encrypted payload
}

// ============================================================================
// DECODER
// ============================================================================

void* subghz_protocol_decoder_bmw_v2_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderBmwV2* instance = malloc(sizeof(SubGhzProtocolDecoderBmwV2));
    instance->base.protocol = &subghz_protocol_bmw_v2;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_bmw_v2_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderBmwV2* instance = context;
    free(instance);
}

void subghz_protocol_decoder_bmw_v2_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderBmwV2* instance = context;
    instance->decoder.parser_step = BmwV2DecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->decoder.te_last = 0;
}

/**
 * Manchester decoding state machine.
 *
 * Signal structure:
 *   [optional preamble] → GAP (>2000us LOW) → Manchester data (56 bits) → GAP
 *
 * Manchester encoding (IEEE 802.3 convention):
 *   HIGH→LOW  transition at bit center = 1
 *   LOW→HIGH  transition at bit center = 0
 *
 * Timing: short ~500us (half-bit), long ~1000us (full-bit)
 *
 * State machine uses te_last to accumulate half-periods.
 * A long pulse (L) completes a full bit period.
 * Two consecutive short pulses (S+S) also complete a full bit period.
 */

static void bmw_v2_try_emit(SubGhzProtocolDecoderBmwV2* instance) {
    if(instance->decoder.decode_count_bit >=
       subghz_protocol_bmw_v2_const.min_count_bit_for_found) {
        uint64_t data = instance->decoder.decode_data;

        // If we got more than 56 bits, right-align to 56
        if(instance->decoder.decode_count_bit > 56) {
            data >>= (instance->decoder.decode_count_bit - 56);
        }

        uint8_t header = (data >> 48) & 0xFF;
        uint8_t trailer = data & 0xFF;

        if(header == BMW_V2_HEADER && trailer == BMW_V2_TRAILER) {
            instance->generic.data = data;
            instance->generic.data_count_bit = 56;

            bmw_v2_check_remote_controller(&instance->generic);

            if(instance->base.callback) {
                instance->base.callback(&instance->base, instance->base.context);
            }
        }
    }
}

void subghz_protocol_decoder_bmw_v2_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderBmwV2* instance = context;

    switch(instance->decoder.parser_step) {
    case BmwV2DecoderStepReset:
        // Any state: wait for a gap (LOW > 2000us) as sync
        if(!level && duration > BMW_V2_GAP_MIN) {
            instance->decoder.parser_step = BmwV2DecoderStepWaitGap;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->decoder.te_last = 0;
        }
        break;

    case BmwV2DecoderStepWaitGap:
        // After gap, expect first HIGH pulse (short or long) to start Manchester data
        if(level) {
            bool is_short = DURATION_DIFF(duration, subghz_protocol_bmw_v2_const.te_short) <=
                            subghz_protocol_bmw_v2_const.te_delta;
            bool is_long = DURATION_DIFF(duration, subghz_protocol_bmw_v2_const.te_long) <=
                           subghz_protocol_bmw_v2_const.te_delta;

            if(is_short) {
                // First half of first bit - save as pending
                instance->decoder.te_last = duration;
                instance->decoder.parser_step = BmwV2DecoderStepManchester;
            } else if(is_long) {
                // Full bit period HIGH→ (need matching LOW next)
                instance->decoder.te_last = 0;
                // Long HIGH at start: this is half of bit + half of next, or just the first transition
                // Actually in Manchester, a long pulse means "same bit as would be decoded"
                // For the first pulse: long HIGH = bit 1 (will be followed by...)
                // We need the LOW to complete. Save as pending long.
                instance->decoder.te_last = duration;
                instance->decoder.parser_step = BmwV2DecoderStepManchester;
            } else {
                instance->decoder.parser_step = BmwV2DecoderStepReset;
            }
        } else {
            // Another LOW - could be another gap, stay in this state
            if(duration > BMW_V2_GAP_MIN) {
                // Another gap, keep waiting
            } else {
                instance->decoder.parser_step = BmwV2DecoderStepReset;
            }
        }
        break;

    case BmwV2DecoderStepManchester: {
        bool is_short = DURATION_DIFF(duration, subghz_protocol_bmw_v2_const.te_short) <=
                        subghz_protocol_bmw_v2_const.te_delta;
        bool is_long = DURATION_DIFF(duration, subghz_protocol_bmw_v2_const.te_long) <=
                       subghz_protocol_bmw_v2_const.te_delta;

        if(!is_short && !is_long) {
            // Invalid timing or gap — try to emit if we have enough bits
            bmw_v2_try_emit(instance);
            instance->decoder.parser_step = BmwV2DecoderStepReset;

            // If this was a gap, re-enter sync state
            if(!level && duration > BMW_V2_GAP_MIN) {
                instance->decoder.parser_step = BmwV2DecoderStepWaitGap;
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->decoder.te_last = 0;
            }
            break;
        }

        if(is_short) {
            if(instance->decoder.te_last == 0) {
                // First half of a short pair — save it
                instance->decoder.te_last = duration;
            } else {
                // Second short: S+S = one bit period
                // level=true (HIGH) means transition was LOW→HIGH = bit 0
                // level=false (LOW) means transition was HIGH→LOW = bit 1
                uint8_t bit = level ? 0 : 1;
                instance->decoder.decode_data = (instance->decoder.decode_data << 1) | bit;
                instance->decoder.decode_count_bit++;
                instance->decoder.te_last = 0;
            }
        } else {
            // Long pulse: 2 half-periods
            if(instance->decoder.te_last != 0) {
                // Pending short + long: S + L = 3 half-periods
                // First 2 form a bit period, third remains as pending
                uint8_t bit = level ? 0 : 1;
                instance->decoder.decode_data = (instance->decoder.decode_data << 1) | bit;
                instance->decoder.decode_count_bit++;
                // Keep te_last set — the second half of the long is now pending
                instance->decoder.te_last =
                    (uint32_t)subghz_protocol_bmw_v2_const.te_short;
            } else {
                // Long alone: completes a full bit period
                uint8_t bit = level ? 0 : 1;
                instance->decoder.decode_data = (instance->decoder.decode_data << 1) | bit;
                instance->decoder.decode_count_bit++;
                instance->decoder.te_last = 0;
            }
        }

        if(instance->decoder.decode_count_bit > 64) {
            bmw_v2_try_emit(instance);
            instance->decoder.parser_step = BmwV2DecoderStepReset;
        }
        break;
    }
    }
}

uint8_t subghz_protocol_decoder_bmw_v2_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderBmwV2* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

// ============================================================================
// DECODER - SERIALIZE / DESERIALIZE
// ============================================================================

SubGhzProtocolStatus subghz_protocol_decoder_bmw_v2_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderBmwV2* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_bmw_v2_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderBmwV2* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    do {
        ret = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_bmw_v2_const.min_count_bit_for_found);
        if(ret != SubGhzProtocolStatusOk) {
            break;
        }
        if(!flipper_format_rewind(flipper_format)) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        uint8_t key_data[sizeof(uint64_t)] = {0};
        if(!flipper_format_read_hex(flipper_format, "Key", key_data, sizeof(uint64_t))) {
            ret = SubGhzProtocolStatusErrorParserOthers;
            break;
        }
        for(uint8_t i = 0; i < sizeof(uint64_t); i++) {
            instance->generic.data = instance->generic.data << 8 | key_data[i];
        }

        bmw_v2_check_remote_controller(&instance->generic);
        ret = SubGhzProtocolStatusOk;
    } while(false);
    return ret;
}

// ============================================================================
// DECODER - GET STRING
// ============================================================================

void subghz_protocol_decoder_bmw_v2_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderBmwV2* instance = context;

    bmw_v2_check_remote_controller(&instance->generic);

    uint32_t key_hi = (instance->generic.data >> 32) & 0xFFFFFFFF;
    uint32_t key_lo = instance->generic.data & 0xFFFFFFFF;

    // Extract the 5 rolling code bytes for display
    uint8_t rc[5];
    rc[0] = (instance->generic.data >> 40) & 0xFF;
    rc[1] = (instance->generic.data >> 32) & 0xFF;
    rc[2] = (instance->generic.data >> 24) & 0xFF;
    rc[3] = (instance->generic.data >> 16) & 0xFF;
    rc[4] = (instance->generic.data >> 8) & 0xFF;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%08lX%08lX\r\n"
        "RC:%02X%02X%02X%02X%02X\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long)key_hi,
        (unsigned long)key_lo,
        rc[0],
        rc[1],
        rc[2],
        rc[3],
        rc[4]);
}

// ============================================================================
// ENCODER
// ============================================================================

void* subghz_protocol_encoder_bmw_v2_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderBmwV2* instance = malloc(sizeof(SubGhzProtocolEncoderBmwV2));

    instance->base.protocol = &subghz_protocol_bmw_v2;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = 5;
    instance->encoder.size_upload = BMW_V2_ENCODER_UPLOAD_MAX_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_bmw_v2_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderBmwV2* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

// ============================================================================
// ENCODER - BUILD UPLOAD (Manchester encoding)
// ============================================================================

static void subghz_protocol_encoder_bmw_v2_get_upload(SubGhzProtocolEncoderBmwV2* instance) {
    furi_assert(instance);
    size_t index = 0;

    uint64_t data = instance->generic.data;

    // Manchester encode 56 bits MSB first
    // IEEE 802.3: 1 = HIGH→LOW, 0 = LOW→HIGH
    // We build level-duration pairs

    bool last_level = false;  // start from LOW

    for(int bit = 55; bit >= 0; bit--) {
        uint8_t b = (data >> bit) & 1;

        if(b) {
            // Bit 1: HIGH for te_short, LOW for te_short
            if(last_level) {
                // Already HIGH, need to merge
                // Previous was HIGH end, now HIGH start = extend or add LOW gap first
                instance->encoder.upload[index++] =
                    level_duration_make(true, (uint32_t)subghz_protocol_bmw_v2_const.te_short);
                instance->encoder.upload[index++] =
                    level_duration_make(false, (uint32_t)subghz_protocol_bmw_v2_const.te_short);
            } else {
                instance->encoder.upload[index++] =
                    level_duration_make(true, (uint32_t)subghz_protocol_bmw_v2_const.te_short);
                instance->encoder.upload[index++] =
                    level_duration_make(false, (uint32_t)subghz_protocol_bmw_v2_const.te_short);
            }
            last_level = false;
        } else {
            // Bit 0: LOW for te_short, HIGH for te_short
            if(!last_level) {
                instance->encoder.upload[index++] =
                    level_duration_make(false, (uint32_t)subghz_protocol_bmw_v2_const.te_short);
                instance->encoder.upload[index++] =
                    level_duration_make(true, (uint32_t)subghz_protocol_bmw_v2_const.te_short);
            } else {
                instance->encoder.upload[index++] =
                    level_duration_make(false, (uint32_t)subghz_protocol_bmw_v2_const.te_short);
                instance->encoder.upload[index++] =
                    level_duration_make(true, (uint32_t)subghz_protocol_bmw_v2_const.te_short);
            }
            last_level = true;
        }
    }

    // Gap between repetitions
    instance->encoder.upload[index++] =
        level_duration_make(false, (uint32_t)BMW_V2_GAP_MIN + 450);

    instance->encoder.size_upload = index;
}

// ============================================================================
// ENCODER - DESERIALIZE
// ============================================================================

SubGhzProtocolStatus
    subghz_protocol_encoder_bmw_v2_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderBmwV2* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        if(subghz_block_generic_deserialize(&instance->generic, flipper_format) !=
           SubGhzProtocolStatusOk) {
            break;
        }
        if(!flipper_format_rewind(flipper_format)) {
            break;
        }
        uint8_t key_data[sizeof(uint64_t)] = {0};
        if(!flipper_format_read_hex(flipper_format, "Key", key_data, sizeof(uint64_t))) {
            break;
        }
        for(uint8_t i = 0; i < sizeof(uint64_t); i++) {
            instance->generic.data = instance->generic.data << 8 | key_data[i];
        }

        bmw_v2_check_remote_controller(&instance->generic);

        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        subghz_protocol_encoder_bmw_v2_get_upload(instance);

        if(!flipper_format_rewind(flipper_format)) {
            break;
        }
        for(size_t i = 0; i < sizeof(uint64_t); i++) {
            key_data[sizeof(uint64_t) - i - 1] = (instance->generic.data >> i * 8) & 0xFF;
        }
        if(!flipper_format_update_hex(flipper_format, "Key", key_data, sizeof(uint64_t))) {
            break;
        }

        instance->encoder.is_running = true;
        res = SubGhzProtocolStatusOk;
    } while(false);

    return res;
}

void subghz_protocol_encoder_bmw_v2_stop(void* context) {
    SubGhzProtocolEncoderBmwV2* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_bmw_v2_yield(void* context) {
    SubGhzProtocolEncoderBmwV2* instance = context;

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
