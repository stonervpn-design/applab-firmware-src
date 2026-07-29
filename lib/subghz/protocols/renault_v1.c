/**
 * Renault Clio 2 SubGHz Protocol — 2-FSK Manchester
 *
 * Physical: 2-FSK @ 433.92 MHz, custom CC1101 preset (38.4 kbps)
 * CC1101 demodulates to baseband → raw timings = Manchester edges.
 *
 * Manchester: te_short=52us, te_long=104us
 * Preamble: 10+ alternating SHORT pulses
 * Data: 96 bits (12 bytes)
 *   [Sync:16] [Serial:16] [EncryptedRolling:64]
 *   Bytes 0-1: 0x4C 0x64 (protocol sync, constant)
 *   Bytes 2-3: Serial number (per remote)
 *   Bytes 4-11: Encrypted rolling code (button/counter embedded, not separable)
 */

#include "renault_v1.h"

#include <lib/toolbox/manchester_decoder.h>
#include <lib/toolbox/manchester_encoder.h>

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include <furi.h>
#include <furi_hal.h>

#define TAG "SubGhzProtocolRenaultV1"

#define RENAULT_V1_DATA_BITS     96
#define RENAULT_V1_MIN_PREAMBLE  10
#define RENAULT_V1_SYNC_HI       0x4C
#define RENAULT_V1_SYNC_LO       0x64
#define RENAULT_V1_TOTAL_BURSTS  3
#define RENAULT_V1_PREAMBLE_PAIRS 20

static const SubGhzBlockConst subghz_protocol_renault_v1_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = RENAULT_V1_DATA_BITS,
};

// ============================================================================
// DATA STRUCTURES
// ============================================================================

typedef enum {
    RenaultV1DecoderStepReset = 0,
    RenaultV1DecoderStepCollect,
} RenaultV1DecoderStep;

struct SubGhzProtocolDecoderRenaultV1 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    bool half_bit;        // waiting for second SHORT
    bool half_bit_level;  // level of first SHORT (unused, kept for struct alignment)

    uint8_t all_bits[160];
    uint8_t all_bit_count;

    uint32_t data_32; // bits 64-95 (last 4 bytes)
};

struct SubGhzProtocolEncoderRenaultV1 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint32_t data_32;
};

// ============================================================================
// PROTOCOL DEFINITION
// ============================================================================

static SubGhzBlockGeneric* subghz_protocol_decoder_renault_v1_get_generic(void* context) {
    SubGhzProtocolDecoderRenaultV1* instance = context;
    return &instance->generic;
}

const SubGhzProtocolDecoder subghz_protocol_renault_v1_decoder = {
    .alloc = subghz_protocol_decoder_renault_v1_alloc,
    .free = subghz_protocol_decoder_renault_v1_free,
    .feed = subghz_protocol_decoder_renault_v1_feed,
    .reset = subghz_protocol_decoder_renault_v1_reset,
    .get_hash_data = subghz_protocol_decoder_renault_v1_get_hash_data,
    .serialize = subghz_protocol_decoder_renault_v1_serialize,
    .deserialize = subghz_protocol_decoder_renault_v1_deserialize,
    .get_string = subghz_protocol_decoder_renault_v1_get_string,
    .get_generic = subghz_protocol_decoder_renault_v1_get_generic,
};

const SubGhzProtocolEncoder subghz_protocol_renault_v1_encoder = {
    .alloc = subghz_protocol_encoder_renault_v1_alloc,
    .free = subghz_protocol_encoder_renault_v1_free,
    .deserialize = subghz_protocol_encoder_renault_v1_deserialize,
    .stop = subghz_protocol_encoder_renault_v1_stop,
    .yield = subghz_protocol_encoder_renault_v1_yield,
};

const SubGhzProtocol subghz_protocol_renault_v1 = {
    .name = SUBGHZ_PROTOCOL_RENAULT_V1_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_renault_v1_decoder,
    .encoder = &subghz_protocol_renault_v1_encoder,
};

// ============================================================================
// DECODER
// ============================================================================

void* subghz_protocol_decoder_renault_v1_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderRenaultV1* instance = malloc(sizeof(SubGhzProtocolDecoderRenaultV1));
    instance->base.protocol = &subghz_protocol_renault_v1;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_renault_v1_free(void* context) {
    furi_assert(context);
    free(context);
}

void subghz_protocol_decoder_renault_v1_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderRenaultV1* instance = context;
    instance->decoder.parser_step = RenaultV1DecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->half_bit = false;
    instance->all_bit_count = 0;
    instance->data_32 = 0;
}

/**
 * Search for sync word 4C64 in accumulated bits and extract frame.
 */
static void renault_v1_process_frame(SubGhzProtocolDecoderRenaultV1* instance) {
    if(instance->all_bit_count < 80) return;

    // Sync pattern: 4C 64 = 0100 1100 0110 0100
    static const uint8_t sync_bits[16] = {0,1,0,0,1,1,0,0, 0,1,1,0,0,1,0,0};

    // Search for sync pattern
    int sync_pos = -1;
    int search_end = instance->all_bit_count - 80; // need at least 80 bits after sync
    for(int i = 0; i <= search_end; i++) {
        bool match = true;
        for(int j = 0; j < 16; j++) {
            if(instance->all_bits[i + j] != sync_bits[j]) {
                match = false;
                break;
            }
        }
        if(match) {
            sync_pos = i;
            break;
        }
    }

    if(sync_pos < 0) {
        FURI_LOG_D(TAG, "No sync found in %d bits", instance->all_bit_count);
        return;
    }

    int data_len = instance->all_bit_count - sync_pos;
    if(data_len > RENAULT_V1_DATA_BITS) data_len = RENAULT_V1_DATA_BITS;

    // Extract generic.data (first 64 bits from sync)
    instance->generic.data = 0;
    for(int i = 0; i < 64 && i < data_len; i++) {
        instance->generic.data = (instance->generic.data << 1) | instance->all_bits[sync_pos + i];
    }

    // Extract data_32 (bits 64-95)
    instance->data_32 = 0;
    for(int i = 64; i < data_len; i++) {
        instance->data_32 = (instance->data_32 << 1) | instance->all_bits[sync_pos + i];
    }
    if(data_len > 64 && data_len - 64 < 32) {
        instance->data_32 <<= (32 - (data_len - 64));
    }

    instance->generic.data_count_bit = data_len;
    instance->decoder.decode_data = instance->generic.data;
    instance->decoder.decode_count_bit = data_len;

    // Bytes 0-1: Sync 4C64, Bytes 2-3: Serial, Bytes 4-11: Encrypted rolling code
    instance->generic.serial = (uint32_t)((instance->generic.data >> 32) & 0xFFFF);

    FURI_LOG_I(TAG, "Decoded %d bits: Sn=%04lX",
        data_len, (unsigned long)instance->generic.serial);

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

void subghz_protocol_decoder_renault_v1_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderRenaultV1* instance = context;

    uint32_t te_s = subghz_protocol_renault_v1_const.te_short;
    uint32_t te_l = subghz_protocol_renault_v1_const.te_long;
    uint32_t te_d = subghz_protocol_renault_v1_const.te_delta;

    switch(instance->decoder.parser_step) {
    case RenaultV1DecoderStepReset:
        // Any SHORT starts bit collection
        if(DURATION_DIFF(duration, te_s) <= te_d) {
            instance->decoder.parser_step = RenaultV1DecoderStepCollect;
            instance->all_bit_count = 0;
            instance->half_bit = true; // this SHORT is the first of a pair
            instance->half_bit_level = level;
        }
        break;

    case RenaultV1DecoderStepCollect: {
        if(instance->all_bit_count >= 150) {
            renault_v1_process_frame(instance);
            instance->decoder.parser_step = RenaultV1DecoderStepReset;
            break;
        }

        bool is_short = DURATION_DIFF(duration, te_s) <= te_d;
        bool is_long = DURATION_DIFF(duration, te_l) <= te_d;

        if(is_long) {
            if(instance->half_bit) {
                // LONG after half-bit = Manchester error → process & reset
                renault_v1_process_frame(instance);
                instance->decoder.parser_step = RenaultV1DecoderStepReset;
            } else {
                // LONG = complete bit, value = level
                if(instance->all_bit_count < sizeof(instance->all_bits)) {
                    instance->all_bits[instance->all_bit_count++] = level ? 1 : 0;
                }
            }
        } else if(is_short) {
            if(instance->half_bit) {
                // Second SHORT completes a bit, value = level of second SHORT
                if(instance->all_bit_count < sizeof(instance->all_bits)) {
                    instance->all_bits[instance->all_bit_count++] = level ? 1 : 0;
                }
                instance->half_bit = false;
            } else {
                instance->half_bit = true;
                instance->half_bit_level = level;
            }
        } else {
            // Invalid timing → process frame & reset
            renault_v1_process_frame(instance);
            instance->decoder.parser_step = RenaultV1DecoderStepReset;
        }
    } break;

    default:
        instance->decoder.parser_step = RenaultV1DecoderStepReset;
        break;
    }
}

// ============================================================================
// HASH
// ============================================================================

uint8_t subghz_protocol_decoder_renault_v1_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderRenaultV1* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

// ============================================================================
// SERIALIZATION
// ============================================================================

SubGhzProtocolStatus subghz_protocol_decoder_renault_v1_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderRenaultV1* instance = context;

    SubGhzProtocolStatus res =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(res != SubGhzProtocolStatusOk) return res;

    FuriString* disp = furi_string_alloc();
    furi_string_printf(disp, "Renault Clio 2 - Sn:%04lX", (unsigned long)instance->generic.serial);
    flipper_format_write_string_cstr(flipper_format, "Disp", furi_string_get_cstr(disp));
    furi_string_free(disp);

    uint32_t k2 = instance->data_32;
    if(!flipper_format_rewind(flipper_format)) return SubGhzProtocolStatusErrorParserOthers;
    if(!flipper_format_insert_or_update_uint32(flipper_format, "Key2", &k2, 1))
        return SubGhzProtocolStatusErrorParserOthers;

    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_renault_v1_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderRenaultV1* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize(&instance->generic, flipper_format);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint32_t k2 = 0;
    if(!flipper_format_read_uint32(flipper_format, "Key2", &k2, 1))
        return SubGhzProtocolStatusError;

    instance->data_32 = k2;

    instance->generic.serial = (uint32_t)((instance->generic.data >> 32) & 0xFFFF);
    instance->generic.protocol_name = instance->base.protocol->name;

    return SubGhzProtocolStatusOk;
}

// ============================================================================
// DISPLAY
// ============================================================================

void subghz_protocol_decoder_renault_v1_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderRenaultV1* instance = context;

    uint32_t key_hi = (uint32_t)(instance->generic.data >> 32);
    uint32_t key_lo = (uint32_t)(instance->generic.data & 0xFFFFFFFF);

    // Bytes 4-11 = encrypted rolling code (64 bit across key_lo lower 32 + data_32)
    uint32_t hop_hi = key_lo;             // bytes 4-7
    uint32_t hop_lo = instance->data_32;  // bytes 8-11

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%08lX%08lX\r\n"
        "Key2:%08lX Sn:%04lX\r\n"
        "Hop:%08lX%08lX",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long)key_hi,
        (unsigned long)key_lo,
        (unsigned long)instance->data_32,
        (unsigned long)instance->generic.serial,
        (unsigned long)hop_hi,
        (unsigned long)hop_lo);
}

// ============================================================================
// ENCODER
// ============================================================================

static bool renault_v1_get_data_bit(SubGhzProtocolEncoderRenaultV1* instance, uint8_t idx) {
    if(idx < 64) {
        return (instance->generic.data >> (63 - idx)) & 1;
    } else {
        return (instance->data_32 >> (31 - (idx - 64))) & 1;
    }
}

void* subghz_protocol_encoder_renault_v1_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderRenaultV1* instance = malloc(sizeof(SubGhzProtocolEncoderRenaultV1));
    instance->base.protocol = &subghz_protocol_renault_v1;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = RENAULT_V1_TOTAL_BURSTS;
    instance->encoder.size_upload = 512;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_renault_v1_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderRenaultV1* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

/**
 * Build one complete frame:
 * [PREAMBLE: 20x SHORT pairs] [TRIGGER LONG] [96 Manchester bits] [END GAP]
 */
static void renault_v1_build_upload(SubGhzProtocolEncoderRenaultV1* instance) {
    furi_assert(instance);
    size_t index = 0;

    uint32_t te_s = subghz_protocol_renault_v1_const.te_short;
    uint32_t te_l = subghz_protocol_renault_v1_const.te_long;

    // PREAMBLE: alternating SHORT pairs
    for(int p = 0; p < RENAULT_V1_PREAMBLE_PAIRS; p++) {
        instance->encoder.upload[index++] = level_duration_make(true, te_s);
        instance->encoder.upload[index++] = level_duration_make(false, te_s);
    }

    // TRIGGER: first LONG pulse transitions preamble -> data
    instance->encoder.upload[index++] = level_duration_make(false, te_l);

    // Manchester-encode 96 data bits
    uint8_t manchester_state = ManchesterStateMid1;

    for(int i = 0; i < RENAULT_V1_DATA_BITS; i++) {
        bool b = renault_v1_get_data_bit(instance, i);

        switch(manchester_state) {
        case ManchesterStateMid1:
            if(b == 0) {
                instance->encoder.upload[index++] = level_duration_make(true, te_l);
                manchester_state = ManchesterStateMid0;
            } else {
                instance->encoder.upload[index++] = level_duration_make(true, te_s);
                instance->encoder.upload[index++] = level_duration_make(false, te_s);
                manchester_state = ManchesterStateMid1;
            }
            break;
        case ManchesterStateMid0:
            if(b == 0) {
                instance->encoder.upload[index++] = level_duration_make(false, te_s);
                instance->encoder.upload[index++] = level_duration_make(true, te_s);
                manchester_state = ManchesterStateMid0;
            } else {
                instance->encoder.upload[index++] = level_duration_make(false, te_l);
                manchester_state = ManchesterStateMid1;
            }
            break;
        default:
            manchester_state = ManchesterStateMid1;
            break;
        }
    }

    // END: guard HIGH + gap
    bool end_level = (manchester_state == ManchesterStateMid1);
    instance->encoder.upload[index++] = level_duration_make(end_level, 2000);

    instance->encoder.size_upload = index;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_renault_v1_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderRenaultV1* instance = context;
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

        renault_v1_build_upload(instance);
        instance->encoder.is_running = true;
        res = SubGhzProtocolStatusOk;
    } while(false);

    return res;
}

void subghz_protocol_encoder_renault_v1_stop(void* context) {
    SubGhzProtocolEncoderRenaultV1* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_renault_v1_yield(void* context) {
    SubGhzProtocolEncoderRenaultV1* instance = context;

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
