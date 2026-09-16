/**
 * @file bmw_v3.c
 * @brief BMW V3 (Mini Keyfob Gen2) SubGHz Protocol Implementation
 *
 * Dynamic protocol with 72-bit Manchester-encoded rolling code.
 * Manchester encoding with 95us short / 190us long timing.
 * 2FSK modulation at 434.190 MHz.
 *
 * Signal structure (9 bytes / 72 bits total, 64 bits stored):
 *   Byte 0:   0xCC (fixed header, validated but not stored)
 *   Byte 1:   Button code (02=Trunk, 04=Unlock, 08=Lock)
 *   Byte 2-3: Device serial (e.g. 4EFE, DA9B - varies per keyfob)
 *   Byte 4-8: 40-bit rolling code (encrypted)
 *
 * Stored in generic.data (64 bits): Bytes 1-8
 *
 * Gap between repeats: ~500us (noise region in FSK demod)
 * Each button press: multiple repeats of same message
 */
#include "bmw_v3.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#define TAG "SubGhzProtocolBmwV3"

static const SubGhzBlockConst subghz_protocol_bmw_v3_const = {
    .te_short = 95,
    .te_long = 190,
    .te_delta = 40,
    .min_count_bit_for_found = 64,
};

#define BMW_V3_TOTAL_BITS 72
#define BMW_V3_GAP_MIN    350
#define BMW_V3_HEADER     0xCC
#define BMW_V3_FIXED_HI   0x4E
#define BMW_V3_FIXED_LO   0xFE

#define BMW_V3_BTN_TRUNK  0x02
#define BMW_V3_BTN_UNLOCK 0x04
#define BMW_V3_BTN_LOCK   0x08

// 72 bits Manchester (144 transitions) + gap = ~160
#define BMW_V3_ENCODER_UPLOAD_MAX_SIZE 160

// ============================================================================
// DATA STRUCTURES
// ============================================================================

struct SubGhzProtocolDecoderBmwV3 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
};

struct SubGhzProtocolEncoderBmwV3 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};

typedef enum {
    BmwV3DecoderStepReset = 0,
    BmwV3DecoderStepWaitGap,
    BmwV3DecoderStepFindHeader,
    BmwV3DecoderStepCollectData,
} BmwV3DecoderStep;

// ============================================================================
// PROTOCOL DESCRIPTOR
// ============================================================================

static SubGhzBlockGeneric* subghz_protocol_decoder_bmw_v3_get_generic(void* context) {
    SubGhzProtocolDecoderBmwV3* instance = context;
    return &instance->generic;
}

const SubGhzProtocolDecoder subghz_protocol_bmw_v3_decoder = {
    .alloc = subghz_protocol_decoder_bmw_v3_alloc,
    .free = subghz_protocol_decoder_bmw_v3_free,
    .feed = subghz_protocol_decoder_bmw_v3_feed,
    .reset = subghz_protocol_decoder_bmw_v3_reset,
    .get_hash_data = subghz_protocol_decoder_bmw_v3_get_hash_data,
    .serialize = subghz_protocol_decoder_bmw_v3_serialize,
    .deserialize = subghz_protocol_decoder_bmw_v3_deserialize,
    .get_string = subghz_protocol_decoder_bmw_v3_get_string,
    .get_generic = subghz_protocol_decoder_bmw_v3_get_generic,
};

const SubGhzProtocolEncoder subghz_protocol_bmw_v3_encoder = {
    .alloc = subghz_protocol_encoder_bmw_v3_alloc,
    .free = subghz_protocol_encoder_bmw_v3_free,
    .deserialize = subghz_protocol_encoder_bmw_v3_deserialize,
    .stop = subghz_protocol_encoder_bmw_v3_stop,
    .yield = subghz_protocol_encoder_bmw_v3_yield,
};

const SubGhzProtocol subghz_protocol_bmw_v3 = {
    .name = SUBGHZ_PROTOCOL_BMW_V3_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_bmw_v3_decoder,
    .encoder = &subghz_protocol_bmw_v3_encoder,
};

// ============================================================================
// CHECK REMOTE CONTROLLER
// ============================================================================

static const char* bmw_v3_get_button_name(uint8_t btn) {
    switch(btn) {
    case BMW_V3_BTN_LOCK:
        return "Lock";
    case BMW_V3_BTN_UNLOCK:
        return "Unlock";
    case BMW_V3_BTN_TRUNK:
        return "Trunk";
    default:
        return "Unknown";
    }
}

static void bmw_v3_check_remote_controller(SubGhzBlockGeneric* instance) {
    // Stored 64-bit data: BB SS SS RR RR RR RR RR
    // (Header CC was validated during decode but not stored)
    uint64_t data = instance->data;

    // Byte 0 of stored data = button
    instance->btn = (data >> 56) & 0xFF;

    // Bytes 1-2 = device serial (varies per keyfob, e.g. 4EFE, DA9B)
    instance->serial = ((data >> 48) & 0xFF) << 8 | ((data >> 40) & 0xFF);

    // Bytes 3-7 = rolling code (40 bits)
    instance->cnt = (data >> 16) & 0xFFFF;   // middle 16 bits as counter
    instance->seed = (data >> 32) & 0xFF;     // upper 8 bits of RC
}

// ============================================================================
// DECODER
// ============================================================================

void* subghz_protocol_decoder_bmw_v3_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderBmwV3* instance = malloc(sizeof(SubGhzProtocolDecoderBmwV3));
    instance->base.protocol = &subghz_protocol_bmw_v3;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_bmw_v3_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderBmwV3* instance = context;
    free(instance);
}

void subghz_protocol_decoder_bmw_v3_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderBmwV3* instance = context;
    instance->decoder.parser_step = BmwV3DecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->decoder.te_last = 0;
}

/**
 * Manchester decoding state machine.
 *
 * Timing: short ~95us (half-bit), long ~190us (full-bit)
 *
 * Manchester encoding (IEEE 802.3 convention):
 *   HIGH->LOW  transition at bit center = 1
 *   LOW->HIGH  transition at bit center = 0
 *
 * Two-phase approach:
 *   1. FindHeader: Manchester-decode bits into sliding window, look for CC header byte
 *   2. CollectData: Once CC found, collect exactly 64 more bits for payload
 */

static void bmw_v3_manchester_bit(SubGhzProtocolDecoderBmwV3* instance, uint8_t bit) {
    instance->decoder.decode_data = (instance->decoder.decode_data << 1) | bit;
    instance->decoder.decode_count_bit++;

    switch(instance->decoder.parser_step) {
    case BmwV3DecoderStepFindHeader:
        // Check if the last 8 bits form the CC header
        if(instance->decoder.decode_count_bit >= 8 &&
           (instance->decoder.decode_data & 0xFF) == BMW_V3_HEADER) {
            // Found CC header — now collect 64 data bits
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->decoder.parser_step = BmwV3DecoderStepCollectData;
        } else if(instance->decoder.decode_count_bit > 32) {
            // Too many bits without finding CC — reset
            instance->decoder.parser_step = BmwV3DecoderStepReset;
        }
        break;

    case BmwV3DecoderStepCollectData:
        if(instance->decoder.decode_count_bit == 64) {
            uint64_t data = instance->decoder.decode_data;
            uint8_t btn = (data >> 56) & 0xFF;

            if(btn == BMW_V3_BTN_LOCK || btn == BMW_V3_BTN_UNLOCK ||
               btn == BMW_V3_BTN_TRUNK || btn == 0x10) {
                instance->generic.data = data;
                instance->generic.data_count_bit = 64;
                bmw_v3_check_remote_controller(&instance->generic);

                if(instance->base.callback) {
                    instance->base.callback(&instance->base, instance->base.context);
                }
            }
            instance->decoder.parser_step = BmwV3DecoderStepReset;
        }
        break;

    default:
        break;
    }
}

void subghz_protocol_decoder_bmw_v3_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderBmwV3* instance = context;

    switch(instance->decoder.parser_step) {
    case BmwV3DecoderStepReset:
        // Wait for a gap (LOW > GAP_MIN) as sync
        if(!level && duration > BMW_V3_GAP_MIN) {
            instance->decoder.parser_step = BmwV3DecoderStepWaitGap;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->decoder.te_last = 0;
        }
        break;

    case BmwV3DecoderStepWaitGap:
        // After gap, expect first HIGH pulse to start Manchester data
        if(level) {
            bool is_short = DURATION_DIFF(duration, subghz_protocol_bmw_v3_const.te_short) <=
                            subghz_protocol_bmw_v3_const.te_delta;
            bool is_long = DURATION_DIFF(duration, subghz_protocol_bmw_v3_const.te_long) <=
                           subghz_protocol_bmw_v3_const.te_delta;

            if(is_short || is_long) {
                instance->decoder.te_last = duration;
                instance->decoder.parser_step = BmwV3DecoderStepFindHeader;
            } else {
                instance->decoder.parser_step = BmwV3DecoderStepReset;
            }
        } else {
            if(duration > BMW_V3_GAP_MIN) {
                // Another gap, keep waiting
            } else {
                instance->decoder.parser_step = BmwV3DecoderStepReset;
            }
        }
        break;

    case BmwV3DecoderStepFindHeader:
    case BmwV3DecoderStepCollectData: {
        bool is_short = DURATION_DIFF(duration, subghz_protocol_bmw_v3_const.te_short) <=
                        subghz_protocol_bmw_v3_const.te_delta;
        bool is_long = DURATION_DIFF(duration, subghz_protocol_bmw_v3_const.te_long) <=
                       subghz_protocol_bmw_v3_const.te_delta;

        if(!is_short && !is_long) {
            instance->decoder.parser_step = BmwV3DecoderStepReset;

            // If this was a gap, re-enter sync state
            if(!level && duration > BMW_V3_GAP_MIN) {
                instance->decoder.parser_step = BmwV3DecoderStepWaitGap;
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->decoder.te_last = 0;
            }
            break;
        }

        if(is_short) {
            if(instance->decoder.te_last == 0) {
                instance->decoder.te_last = duration;
            } else {
                uint8_t bit = level ? 0 : 1;
                bmw_v3_manchester_bit(instance, bit);
                instance->decoder.te_last = 0;
            }
        } else {
            if(instance->decoder.te_last != 0) {
                uint8_t bit = level ? 0 : 1;
                bmw_v3_manchester_bit(instance, bit);
                instance->decoder.te_last =
                    (uint32_t)subghz_protocol_bmw_v3_const.te_short;
            } else {
                uint8_t bit = level ? 0 : 1;
                bmw_v3_manchester_bit(instance, bit);
                instance->decoder.te_last = 0;
            }
        }
        break;
    }
    }
}

uint8_t subghz_protocol_decoder_bmw_v3_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderBmwV3* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

// ============================================================================
// DECODER - SERIALIZE / DESERIALIZE
// ============================================================================

SubGhzProtocolStatus subghz_protocol_decoder_bmw_v3_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderBmwV3* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_bmw_v3_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderBmwV3* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    do {
        ret = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_bmw_v3_const.min_count_bit_for_found);
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

        bmw_v3_check_remote_controller(&instance->generic);
        ret = SubGhzProtocolStatusOk;
    } while(false);
    return ret;
}

// ============================================================================
// DECODER - GET STRING
// ============================================================================

void subghz_protocol_decoder_bmw_v3_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderBmwV3* instance = context;

    bmw_v3_check_remote_controller(&instance->generic);

    uint32_t key_hi = (instance->generic.data >> 32) & 0xFFFFFFFF;
    uint32_t key_lo = instance->generic.data & 0xFFFFFFFF;

    uint8_t btn = (instance->generic.data >> 56) & 0xFF;

    // 40-bit rolling code (bytes 3-7 of stored data)
    uint8_t rc[5];
    rc[0] = (instance->generic.data >> 32) & 0xFF;
    rc[1] = (instance->generic.data >> 24) & 0xFF;
    rc[2] = (instance->generic.data >> 16) & 0xFF;
    rc[3] = (instance->generic.data >> 8) & 0xFF;
    rc[4] = instance->generic.data & 0xFF;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%08lX%08lX\r\n"
        "Btn:%02X (%s)\r\n"
        "RC:%02X%02X%02X%02X%02X\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long)key_hi,
        (unsigned long)key_lo,
        btn,
        bmw_v3_get_button_name(btn),
        rc[0],
        rc[1],
        rc[2],
        rc[3],
        rc[4]);
}

// ============================================================================
// ENCODER
// ============================================================================

void* subghz_protocol_encoder_bmw_v3_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderBmwV3* instance = malloc(sizeof(SubGhzProtocolEncoderBmwV3));

    instance->base.protocol = &subghz_protocol_bmw_v3;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = 5;
    instance->encoder.size_upload = BMW_V3_ENCODER_UPLOAD_MAX_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_bmw_v3_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderBmwV3* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

// ============================================================================
// ENCODER - BUILD UPLOAD (Manchester encoding)
// ============================================================================

static void subghz_protocol_encoder_bmw_v3_get_upload(SubGhzProtocolEncoderBmwV3* instance) {
    furi_assert(instance);
    size_t index = 0;

    // First encode the CC header (8 bits), then the 64-bit stored data
    uint8_t header = BMW_V3_HEADER;
    uint64_t data = instance->generic.data;

    // Manchester encode header (8 bits)
    for(int bit = 7; bit >= 0; bit--) {
        uint8_t b = (header >> bit) & 1;
        if(b) {
            instance->encoder.upload[index++] =
                level_duration_make(true, (uint32_t)subghz_protocol_bmw_v3_const.te_short);
            instance->encoder.upload[index++] =
                level_duration_make(false, (uint32_t)subghz_protocol_bmw_v3_const.te_short);
        } else {
            instance->encoder.upload[index++] =
                level_duration_make(false, (uint32_t)subghz_protocol_bmw_v3_const.te_short);
            instance->encoder.upload[index++] =
                level_duration_make(true, (uint32_t)subghz_protocol_bmw_v3_const.te_short);
        }
    }

    // Manchester encode data (64 bits)
    for(int bit = 63; bit >= 0; bit--) {
        uint8_t b = (data >> bit) & 1;
        if(b) {
            instance->encoder.upload[index++] =
                level_duration_make(true, (uint32_t)subghz_protocol_bmw_v3_const.te_short);
            instance->encoder.upload[index++] =
                level_duration_make(false, (uint32_t)subghz_protocol_bmw_v3_const.te_short);
        } else {
            instance->encoder.upload[index++] =
                level_duration_make(false, (uint32_t)subghz_protocol_bmw_v3_const.te_short);
            instance->encoder.upload[index++] =
                level_duration_make(true, (uint32_t)subghz_protocol_bmw_v3_const.te_short);
        }
    }

    // The decoder emits a data bit only after seeing BOTH te_short halves of its
    // Manchester pair (and fires the callback the instant bit 64 completes). When
    // the last data bit is 1 the pair ends on a LOW half, which on-air (and in the
    // coalesce) merges with the LOW inter-frame gap and swallows the 64th bit ->
    // the frame decodes as 63 bits and is rejected. Emit a HIGH te_short terminator
    // so that trailing LOW half stays a distinct pulse before the (LOW) gap.
    if((data & 1ULL) != 0ULL) {
        instance->encoder.upload[index++] =
            level_duration_make(true, (uint32_t)subghz_protocol_bmw_v3_const.te_short);
    }

    // Gap between repetitions (LOW so the decoder can re-sync on the next frame)
    instance->encoder.upload[index++] =
        level_duration_make(false, (uint32_t)BMW_V3_GAP_MIN + 150);

    instance->encoder.size_upload = index;
}

// ============================================================================
// ENCODER - DESERIALIZE
// ============================================================================

SubGhzProtocolStatus
    subghz_protocol_encoder_bmw_v3_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderBmwV3* instance = context;
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

        bmw_v3_check_remote_controller(&instance->generic);

        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        subghz_protocol_encoder_bmw_v3_get_upload(instance);

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

void subghz_protocol_encoder_bmw_v3_stop(void* context) {
    SubGhzProtocolEncoderBmwV3* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_bmw_v3_yield(void* context) {
    SubGhzProtocolEncoderBmwV3* instance = context;

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
