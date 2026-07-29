/**
 * @file honda_v4.c
 * @brief Honda V4 SubGHz Protocol - Decoder and Encoder
 *
 * PWM-encoded protocol found in Pandora 2.4++ firmware (FUN_0000c510).
 * Uses pulse-width modulation (not Manchester) at 433.92 MHz.
 *
 * Signal: 433.92 MHz, AM (OOK 650), PWM encoded
 * Timing: bit 0 = 750us LOW + 750us HIGH, bit 1 = 1100us LOW + 1100us HIGH
 *
 * Transmission: [PREAMBLE 3x(1500us LOW + 1500us HIGH)][DATA 51bit PWM][GAP 10000us] x5
 *
 * Data format (51 bits, MSB-first, right-aligned in 64-bit):
 *   Bits 50..32: Serial (19 bits)
 *   Bits 31..24: Button (8 bits)
 *   Bits 23..16: Inverse of button (8 bits, must equal 0xFF - button)
 *   Bits 15..0:  Counter (16 bits, Big Endian, +1 per press)
 */

#include "honda_v4.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include "../blocks/custom_btn_i.h"

#define TAG "SubGhzProtocolHondaV4"

static const SubGhzBlockConst subghz_protocol_honda_v4_const = {
    .te_short = 750,
    .te_long = 1100,
    .te_delta = 200,
    .min_count_bit_for_found = 48,
};

#define HONDA_V4_PREAMBLE_DURATION 1500
#define HONDA_V4_PREAMBLE_DELTA    300
#define HONDA_V4_PREAMBLE_COUNT    3
#define HONDA_V4_DATA_BITS         51
#define HONDA_V4_TOTAL_REPEATS     5
#define HONDA_V4_INTER_FRAME_GAP   10000

// ============================================================================
// DATA STRUCTURES
// ============================================================================

typedef enum {
    HondaV4DecoderStepReset = 0,
    HondaV4DecoderStepPreambleHigh,
    HondaV4DecoderStepPreambleOrDataLow,
    HondaV4DecoderStepDataHigh,
    HondaV4DecoderStepDataLow,
} HondaV4DecoderStep;

struct SubGhzProtocolDecoderHondaV4 {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint16_t preamble_count;
    uint32_t te_last;
};

struct SubGhzProtocolEncoderHondaV4 {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};

// ============================================================================
// PROTOCOL DEFINITION
// ============================================================================

static SubGhzBlockGeneric* subghz_protocol_decoder_honda_v4_get_generic(void* context) {
    SubGhzProtocolDecoderHondaV4* instance = context;
    return &instance->generic;
}

const SubGhzProtocolDecoder subghz_protocol_honda_v4_decoder = {
    .alloc = subghz_protocol_decoder_honda_v4_alloc,
    .free = subghz_protocol_decoder_honda_v4_free,

    .feed = subghz_protocol_decoder_honda_v4_feed,
    .reset = subghz_protocol_decoder_honda_v4_reset,

    .get_hash_data = subghz_protocol_decoder_honda_v4_get_hash_data,
    .serialize = subghz_protocol_decoder_honda_v4_serialize,
    .deserialize = subghz_protocol_decoder_honda_v4_deserialize,
    .get_string = subghz_protocol_decoder_honda_v4_get_string,
    .get_generic = subghz_protocol_decoder_honda_v4_get_generic,
};

const SubGhzProtocolEncoder subghz_protocol_honda_v4_encoder = {
    .alloc = subghz_protocol_encoder_honda_v4_alloc,
    .free = subghz_protocol_encoder_honda_v4_free,

    .deserialize = subghz_protocol_encoder_honda_v4_deserialize,
    .stop = subghz_protocol_encoder_honda_v4_stop,
    .yield = subghz_protocol_encoder_honda_v4_yield,
};

const SubGhzProtocol subghz_protocol_honda_v4 = {
    .name = SUBGHZ_PROTOCOL_HONDA_V4_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,

    .decoder = &subghz_protocol_honda_v4_decoder,
    .encoder = &subghz_protocol_honda_v4_encoder,
};

// ============================================================================
// DATA EXTRACTION
// ============================================================================

static void honda_v4_extract_data(SubGhzBlockGeneric* generic) {
    generic->serial = (generic->data >> 32) & 0x7FFFF;
    generic->btn = (generic->data >> 24) & 0xFF;
    generic->cnt = generic->data & 0xFFFF;
}

static bool honda_v4_validate(uint64_t data, uint16_t bit_count) {
    if(bit_count < subghz_protocol_honda_v4_const.min_count_bit_for_found) return false;
    uint8_t button = (data >> 24) & 0xFF;
    uint8_t inverse = (data >> 16) & 0xFF;
    return (inverse == (uint8_t)(0xFF - button));
}

static const char* honda_v4_get_button_name(uint8_t btn) {
    switch(btn) {
    case 0x11:
        return "LOCK";
    case 0x12:
        return "UNLOCK";
    default:
        return "?";
    }
}

static uint8_t honda_v4_get_btn_code(void) {
    uint8_t original_btn = subghz_custom_btn_get_original();
    uint8_t c = subghz_custom_btn_get();

    if(c == SUBGHZ_CUSTOM_BTN_OK) return original_btn;

    switch(c) {
    case SUBGHZ_CUSTOM_BTN_UP:
        return 0x12; // UNLOCK
    case SUBGHZ_CUSTOM_BTN_DOWN:
        return 0x11; // LOCK
    default:
        break;
    }

    return original_btn;
}

// ============================================================================
// DECODER
// ============================================================================

void* subghz_protocol_decoder_honda_v4_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderHondaV4* instance = malloc(sizeof(SubGhzProtocolDecoderHondaV4));
    instance->base.protocol = &subghz_protocol_honda_v4;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_honda_v4_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaV4* instance = context;
    free(instance);
}

void subghz_protocol_decoder_honda_v4_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaV4* instance = context;
    instance->decoder.parser_step = HondaV4DecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->preamble_count = 0;
    instance->te_last = 0;
}

/**
 * Honda V4 decoder feed function.
 *
 * PWM state machine:
 *   1. StepReset: detect LOW ~1500us (preamble start)
 *   2. StepPreambleHigh: HIGH ~1500us completes one preamble cycle
 *   3. StepPreambleOrDataLow: LOW ~1500us = more preamble, LOW ~750/~1100 = data start
 *   4. StepDataHigh: HIGH pulse after data LOW, emit bit based on te_last
 *   5. StepDataLow: next LOW pulse (~750/~1100 = data, ~1500 = new frame)
 */
void subghz_protocol_decoder_honda_v4_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaV4* instance = context;

    bool is_preamble =
        DURATION_DIFF(duration, HONDA_V4_PREAMBLE_DURATION) < HONDA_V4_PREAMBLE_DELTA;
    bool is_short = DURATION_DIFF(duration, subghz_protocol_honda_v4_const.te_short) <
                    subghz_protocol_honda_v4_const.te_delta;
    bool is_long = DURATION_DIFF(duration, subghz_protocol_honda_v4_const.te_long) <
                   subghz_protocol_honda_v4_const.te_delta;

    switch(instance->decoder.parser_step) {
    case HondaV4DecoderStepReset:
        if(!level && is_preamble) {
            instance->preamble_count = 0;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->decoder.parser_step = HondaV4DecoderStepPreambleHigh;
        }
        break;

    case HondaV4DecoderStepPreambleHigh:
        if(level && is_preamble) {
            instance->preamble_count++;
            instance->decoder.parser_step = HondaV4DecoderStepPreambleOrDataLow;
        } else {
            instance->decoder.parser_step = HondaV4DecoderStepReset;
        }
        break;

    case HondaV4DecoderStepPreambleOrDataLow:
        if(!level) {
            if(is_preamble) {
                // Another preamble cycle
                instance->decoder.parser_step = HondaV4DecoderStepPreambleHigh;
            } else if(instance->preamble_count >= HONDA_V4_PREAMBLE_COUNT && (is_short || is_long)) {
                // First data bit LOW
                instance->te_last = duration;
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->decoder.parser_step = HondaV4DecoderStepDataHigh;
            } else {
                instance->decoder.parser_step = HondaV4DecoderStepReset;
            }
        } else {
            instance->decoder.parser_step = HondaV4DecoderStepReset;
        }
        break;

    case HondaV4DecoderStepDataHigh:
        if(level) {
            // Bit value determined by the LOW duration
            bool bit = DURATION_DIFF(instance->te_last, subghz_protocol_honda_v4_const.te_long) <
                       subghz_protocol_honda_v4_const.te_delta;
            instance->decoder.decode_data = (instance->decoder.decode_data << 1) | bit;
            instance->decoder.decode_count_bit++;

            if(instance->decoder.decode_count_bit >= HONDA_V4_DATA_BITS) {
                if(honda_v4_validate(
                       instance->decoder.decode_data, instance->decoder.decode_count_bit)) {
                    instance->generic.data = instance->decoder.decode_data;
                    instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                    honda_v4_extract_data(&instance->generic);

                    subghz_custom_btn_set_original(instance->generic.btn);
                    subghz_custom_btn_set_max(2);

                    if(instance->base.callback) {
                        instance->base.callback(&instance->base, instance->base.context);
                    }
                }
                instance->decoder.parser_step = HondaV4DecoderStepReset;
            } else {
                instance->decoder.parser_step = HondaV4DecoderStepDataLow;
            }
        } else {
            instance->decoder.parser_step = HondaV4DecoderStepReset;
        }
        break;

    case HondaV4DecoderStepDataLow:
        if(!level) {
            if(is_short || is_long) {
                instance->te_last = duration;
                instance->decoder.parser_step = HondaV4DecoderStepDataHigh;
            } else if(is_preamble) {
                // New preamble = frame boundary, validate what we have
                if(honda_v4_validate(
                       instance->decoder.decode_data, instance->decoder.decode_count_bit)) {
                    instance->generic.data = instance->decoder.decode_data;
                    instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                    honda_v4_extract_data(&instance->generic);

                    subghz_custom_btn_set_original(instance->generic.btn);
                    subghz_custom_btn_set_max(2);

                    if(instance->base.callback) {
                        instance->base.callback(&instance->base, instance->base.context);
                    }
                }
                instance->preamble_count = 0;
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->decoder.parser_step = HondaV4DecoderStepPreambleHigh;
            } else {
                // Invalid duration - try to validate and reset
                if(honda_v4_validate(
                       instance->decoder.decode_data, instance->decoder.decode_count_bit)) {
                    instance->generic.data = instance->decoder.decode_data;
                    instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                    honda_v4_extract_data(&instance->generic);

                    subghz_custom_btn_set_original(instance->generic.btn);
                    subghz_custom_btn_set_max(2);

                    if(instance->base.callback) {
                        instance->base.callback(&instance->base, instance->base.context);
                    }
                }
                instance->decoder.parser_step = HondaV4DecoderStepReset;
            }
        } else {
            instance->decoder.parser_step = HondaV4DecoderStepReset;
        }
        break;
    }
}

uint8_t subghz_protocol_decoder_honda_v4_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaV4* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

// ============================================================================
// DECODER - SERIALIZE / DESERIALIZE
// ============================================================================

SubGhzProtocolStatus subghz_protocol_decoder_honda_v4_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaV4* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint32_t cnt_val = instance->generic.cnt;
    if(!flipper_format_write_uint32(flipper_format, "Cnt", &cnt_val, 1)) {
        return SubGhzProtocolStatusError;
    }

    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_honda_v4_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaV4* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    do {
        ret = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_honda_v4_const.min_count_bit_for_found);
        if(ret != SubGhzProtocolStatusOk) break;

        honda_v4_extract_data(&instance->generic);

        uint32_t cnt_val = 0;
        if(!flipper_format_rewind(flipper_format)) {
            ret = SubGhzProtocolStatusError;
            break;
        }
        if(flipper_format_read_uint32(flipper_format, "Cnt", &cnt_val, 1)) {
            instance->generic.cnt = cnt_val & 0xFFFF;
        }

        subghz_custom_btn_set_original(instance->generic.btn);
        subghz_custom_btn_set_max(2);

    } while(false);

    return ret;
}

// ============================================================================
// DECODER - GET STRING
// ============================================================================

void subghz_protocol_decoder_honda_v4_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaV4* instance = context;

    honda_v4_extract_data(&instance->generic);

    subghz_block_generic_global.btn_is_available = true;
    subghz_block_generic_global.btn_length_bit = 8;
    subghz_block_generic_global.current_btn = instance->generic.btn;
    subghz_block_generic_global.cnt_is_available = true;
    subghz_block_generic_global.cnt_length_bit = 16;
    subghz_block_generic_global.current_cnt = instance->generic.cnt;

    furi_string_cat_printf(
        output,
        "%s %lubit\r\n"
        "Key:%013llX\r\n"
        "Sn:%05lX Btn:%s\r\n"
        "Cnt:%lu\r\n",
        instance->generic.protocol_name,
        (unsigned long)instance->generic.data_count_bit,
        instance->generic.data,
        (unsigned long)instance->generic.serial,
        honda_v4_get_button_name(instance->generic.btn),
        (unsigned long)instance->generic.cnt);
}

// ============================================================================
// ENCODER
// ============================================================================

void* subghz_protocol_encoder_honda_v4_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderHondaV4* instance = malloc(sizeof(SubGhzProtocolEncoderHondaV4));

    instance->base.protocol = &subghz_protocol_honda_v4;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = HONDA_V4_TOTAL_REPEATS;
    // Preamble 6 + data 102 (51*2) + gap 1 = 109
    instance->encoder.size_upload = 256;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_honda_v4_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderHondaV4* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

// ============================================================================
// ENCODER - BUILD UPLOAD
// ============================================================================

static void subghz_protocol_encoder_honda_v4_get_upload(SubGhzProtocolEncoderHondaV4* instance) {
    furi_assert(instance);
    size_t index = 0;

    uint32_t te_short = subghz_protocol_honda_v4_const.te_short;
    uint32_t te_long = subghz_protocol_honda_v4_const.te_long;

    // Button override
    {
        uint8_t btn_override = instance->generic.btn;
        if(!subghz_block_generic_global_button_override_get(&btn_override)) {
            btn_override = honda_v4_get_btn_code();
        }
        instance->generic.btn = btn_override;
    }

    // Counter increment
    {
        uint32_t cnt = instance->generic.cnt;
        if(furi_hal_subghz_get_rolling_counter_mult() != -0x7FFFFFFF) {
            if(!subghz_block_generic_global_counter_override_get(&cnt)) {
                if((cnt + furi_hal_subghz_get_rolling_counter_mult()) > 0xFFFF) {
                    cnt = 0;
                } else {
                    cnt += furi_hal_subghz_get_rolling_counter_mult();
                }
            }
        } else {
            cnt = (cnt + 1) & 0xFFFF;
        }
        instance->generic.cnt = cnt & 0xFFFF;
    }

    // Rebuild data: serial(19) | button(8) | inverse(8) | counter(16)
    uint8_t inv = 0xFF - (instance->generic.btn & 0xFF);
    instance->generic.data = ((uint64_t)(instance->generic.serial & 0x7FFFF) << 32) |
                             ((uint64_t)(instance->generic.btn & 0xFF) << 24) |
                             ((uint64_t)inv << 16) | (instance->generic.cnt & 0xFFFF);
    instance->generic.data_count_bit = HONDA_V4_DATA_BITS;

    uint64_t data = instance->generic.data;

    // Preamble: 3x (LOW 1500 + HIGH 1500)
    for(int p = 0; p < HONDA_V4_PREAMBLE_COUNT; p++) {
        instance->encoder.upload[index++] = level_duration_make(false, HONDA_V4_PREAMBLE_DURATION);
        instance->encoder.upload[index++] = level_duration_make(true, HONDA_V4_PREAMBLE_DURATION);
    }

    // Data: 51 bits MSB-first
    for(int bit_num = HONDA_V4_DATA_BITS - 1; bit_num >= 0; bit_num--) {
        bool bit = (data >> bit_num) & 1;
        uint32_t te = bit ? te_long : te_short;
        instance->encoder.upload[index++] = level_duration_make(false, te);
        instance->encoder.upload[index++] = level_duration_make(true, te);
    }

    // Inter-frame gap
    instance->encoder.upload[index++] = level_duration_make(false, HONDA_V4_INTER_FRAME_GAP);

    instance->encoder.size_upload = index;
}

// ============================================================================
// ENCODER - DESERIALIZE
// ============================================================================

SubGhzProtocolStatus
    subghz_protocol_encoder_honda_v4_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderHondaV4* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;

    do {
        if(subghz_block_generic_deserialize(&instance->generic, flipper_format) !=
           SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize generic failed");
            break;
        }

        if(!flipper_format_rewind(flipper_format)) {
            FURI_LOG_E(TAG, "Rewind failed");
            break;
        }

        uint8_t key_data[sizeof(uint64_t)] = {0};
        if(!flipper_format_read_hex(flipper_format, "Key", key_data, sizeof(uint64_t))) {
            FURI_LOG_E(TAG, "Missing Key");
            break;
        }

        instance->generic.data = 0;
        for(uint8_t i = 0; i < sizeof(uint64_t); i++) {
            instance->generic.data = instance->generic.data << 8 | key_data[i];
        }

        honda_v4_extract_data(&instance->generic);

        // Read Cnt field
        uint32_t cnt_val = 0;
        if(!flipper_format_rewind(flipper_format)) {
            FURI_LOG_E(TAG, "Rewind for Cnt failed");
            break;
        }
        if(flipper_format_read_uint32(flipper_format, "Cnt", &cnt_val, 1)) {
            instance->generic.cnt = cnt_val & 0xFFFF;
        }

        if(subghz_custom_btn_get_original() == 0) {
            subghz_custom_btn_set_original(instance->generic.btn);
        }
        subghz_custom_btn_set_max(2);

        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        subghz_protocol_encoder_honda_v4_get_upload(instance);

        // Write back updated key
        if(!flipper_format_rewind(flipper_format)) {
            FURI_LOG_E(TAG, "Rewind for update failed");
            break;
        }
        for(size_t i = 0; i < sizeof(uint64_t); i++) {
            key_data[sizeof(uint64_t) - i - 1] = (instance->generic.data >> i * 8) & 0xFF;
        }
        if(!flipper_format_update_hex(flipper_format, "Key", key_data, sizeof(uint64_t))) {
            FURI_LOG_E(TAG, "Update Key failed");
            break;
        }

        // Write back updated Cnt
        {
            uint32_t cnt_write = instance->generic.cnt;
            if(!flipper_format_rewind(flipper_format)) {
                FURI_LOG_E(TAG, "Rewind for Cnt update failed");
                break;
            }
            if(!flipper_format_update_uint32(flipper_format, "Cnt", &cnt_write, 1)) {
                FURI_LOG_E(TAG, "Update Cnt failed");
                break;
            }
        }

        instance->encoder.is_running = true;
        res = SubGhzProtocolStatusOk;

    } while(false);

    return res;
}

void subghz_protocol_encoder_honda_v4_stop(void* context) {
    SubGhzProtocolEncoderHondaV4* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_honda_v4_yield(void* context) {
    SubGhzProtocolEncoderHondaV4* instance = context;

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
