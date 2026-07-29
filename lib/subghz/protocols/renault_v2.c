/**
 * Renault Clio 3 SubGHz Protocol — OOK Manchester
 *
 * Physical: OOK @ 433.92 MHz, standard preset (FuriHalSubGhzPresetOok650Async)
 *
 * Manchester: te_short=230us, te_long=460us
 * Preamble: ~87 alternating SHORT pairs (all 1s in stateless Manchester)
 * Data: 82 bits
 *   [Sync:16] [Encrypted:66]
 *   Bits 0-15:  0x71EA (protocol sync, constant)
 *   Bits 16-81: Fully encrypted rolling code (button/counter/serial embedded, not separable)
 *
 * Decoder uses stateless Manchester (Ansatz C):
 *   LONG = 1 bit (value = level), SHORT+SHORT = 1 bit (value = level of 2nd SHORT)
 *   Sync pattern 71EA searched in accumulated bits.
 */

#include "renault_v2.h"

#include <lib/toolbox/manchester_decoder.h>
#include <lib/toolbox/manchester_encoder.h>

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include <furi.h>
#include <furi_hal.h>

#define TAG "SubGhzProtocolRenaultV2"

#define RENAULT_V2_DATA_BITS     82
#define RENAULT_V2_SYNC_HI       0x71
#define RENAULT_V2_SYNC_LO       0xEA
#define RENAULT_V2_TOTAL_BURSTS  3
#define RENAULT_V2_PREAMBLE_PAIRS 44

static const SubGhzBlockConst subghz_protocol_renault_v2_const = {
    .te_short = 230,
    .te_long = 460,
    .te_delta = 60,
    .min_count_bit_for_found = RENAULT_V2_DATA_BITS,
};

// ============================================================================
// DATA STRUCTURES
// ============================================================================

typedef enum {
    RenaultV2DecoderStepReset = 0,
    RenaultV2DecoderStepCollect,
} RenaultV2DecoderStep;

struct SubGhzProtocolDecoderRenaultV2 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    bool half_bit;

    uint8_t all_bits[200];
    uint8_t all_bit_count;

    uint32_t data_18; // bits 64-81 (last 18 bits, stored left-aligned in uint32_t)
};

struct SubGhzProtocolEncoderRenaultV2 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint32_t data_18;
};

// ============================================================================
// PROTOCOL DEFINITION
// ============================================================================

static SubGhzBlockGeneric* subghz_protocol_decoder_renault_v2_get_generic(void* context) {
    SubGhzProtocolDecoderRenaultV2* instance = context;
    return &instance->generic;
}

const SubGhzProtocolDecoder subghz_protocol_renault_v2_decoder = {
    .alloc = subghz_protocol_decoder_renault_v2_alloc,
    .free = subghz_protocol_decoder_renault_v2_free,
    .feed = subghz_protocol_decoder_renault_v2_feed,
    .reset = subghz_protocol_decoder_renault_v2_reset,
    .get_hash_data = subghz_protocol_decoder_renault_v2_get_hash_data,
    .serialize = subghz_protocol_decoder_renault_v2_serialize,
    .deserialize = subghz_protocol_decoder_renault_v2_deserialize,
    .get_string = subghz_protocol_decoder_renault_v2_get_string,
    .get_generic = subghz_protocol_decoder_renault_v2_get_generic,
};

const SubGhzProtocolEncoder subghz_protocol_renault_v2_encoder = {
    .alloc = subghz_protocol_encoder_renault_v2_alloc,
    .free = subghz_protocol_encoder_renault_v2_free,
    .deserialize = subghz_protocol_encoder_renault_v2_deserialize,
    .stop = subghz_protocol_encoder_renault_v2_stop,
    .yield = subghz_protocol_encoder_renault_v2_yield,
};

const SubGhzProtocol subghz_protocol_renault_v2 = {
    .name = SUBGHZ_PROTOCOL_RENAULT_V2_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_renault_v2_decoder,
    .encoder = &subghz_protocol_renault_v2_encoder,
};

// ============================================================================
// DECODER
// ============================================================================

void* subghz_protocol_decoder_renault_v2_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderRenaultV2* instance = malloc(sizeof(SubGhzProtocolDecoderRenaultV2));
    instance->base.protocol = &subghz_protocol_renault_v2;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_renault_v2_free(void* context) {
    furi_assert(context);
    free(context);
}

void subghz_protocol_decoder_renault_v2_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderRenaultV2* instance = context;
    instance->decoder.parser_step = RenaultV2DecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->half_bit = false;
    instance->all_bit_count = 0;
    instance->data_18 = 0;
}

/**
 * Search for sync word 71EA in accumulated bits and extract frame.
 */
static void renault_v2_process_frame(SubGhzProtocolDecoderRenaultV2* instance) {
    if(instance->all_bit_count < 60) return;

    // Sync pattern: 71 EA = 0111 0001 1110 1010
    static const uint8_t sync_bits[16] = {0,1,1,1,0,0,0,1, 1,1,1,0,1,0,1,0};

    // Search for sync pattern
    int sync_pos = -1;
    int search_end = instance->all_bit_count - 60; // need at least 60 bits after sync start
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
//        FURI_LOG_D(TAG, "No sync found in %d bits", instance->all_bit_count);
        return;
    }

    int data_len = instance->all_bit_count - sync_pos;
    if(data_len > RENAULT_V2_DATA_BITS) data_len = RENAULT_V2_DATA_BITS;

    if(data_len < 60) {
        FURI_LOG_D(TAG, "Frame too short: %d bits", data_len);
        return;
    }

    // Extract generic.data (first 64 bits from sync)
    instance->generic.data = 0;
    for(int i = 0; i < 64 && i < data_len; i++) {
        instance->generic.data = (instance->generic.data << 1) | instance->all_bits[sync_pos + i];
    }

    // Extract data_18 (bits 64-81, left-aligned in uint32_t)
    instance->data_18 = 0;
    for(int i = 64; i < data_len; i++) {
        instance->data_18 = (instance->data_18 << 1) | instance->all_bits[sync_pos + i];
    }
    if(data_len > 64 && data_len - 64 < 18) {
        instance->data_18 <<= (18 - (data_len - 64));
    }

    instance->generic.data_count_bit = data_len;
    instance->decoder.decode_data = instance->generic.data;
    instance->decoder.decode_count_bit = data_len;

    // No visible serial — entire payload is encrypted
    instance->generic.serial = 0;
    instance->generic.btn = 0;
    instance->generic.cnt = 0;

    FURI_LOG_I(TAG, "Decoded %d bits: Key=%016llX",
        data_len, (unsigned long long)instance->generic.data);

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

void subghz_protocol_decoder_renault_v2_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderRenaultV2* instance = context;

    uint32_t te_s = subghz_protocol_renault_v2_const.te_short;
    uint32_t te_l = subghz_protocol_renault_v2_const.te_long;
    uint32_t te_d = subghz_protocol_renault_v2_const.te_delta;

    switch(instance->decoder.parser_step) {
    case RenaultV2DecoderStepReset:
        // Any SHORT starts bit collection
        if(DURATION_DIFF(duration, te_s) <= te_d) {
            instance->decoder.parser_step = RenaultV2DecoderStepCollect;
            instance->all_bit_count = 0;
            instance->half_bit = true; // this SHORT is the first of a pair
        }
        break;

    case RenaultV2DecoderStepCollect: {
        if(instance->all_bit_count >= 190) {
            renault_v2_process_frame(instance);
            instance->decoder.parser_step = RenaultV2DecoderStepReset;
            break;
        }

        bool is_short = DURATION_DIFF(duration, te_s) <= te_d;
        bool is_long = DURATION_DIFF(duration, te_l) <= te_d;

        if(is_long) {
            if(instance->half_bit) {
                // LONG after half-bit = Manchester error -> process & reset
                renault_v2_process_frame(instance);
                instance->decoder.parser_step = RenaultV2DecoderStepReset;
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
            }
        } else {
            // Invalid timing -> process frame & reset
            renault_v2_process_frame(instance);
            instance->decoder.parser_step = RenaultV2DecoderStepReset;
        }
    } break;

    default:
        instance->decoder.parser_step = RenaultV2DecoderStepReset;
        break;
    }
}

// ============================================================================
// HASH
// ============================================================================

uint8_t subghz_protocol_decoder_renault_v2_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderRenaultV2* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

// ============================================================================
// SERIALIZATION
// ============================================================================

SubGhzProtocolStatus subghz_protocol_decoder_renault_v2_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderRenaultV2* instance = context;

    SubGhzProtocolStatus res =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(res != SubGhzProtocolStatusOk) return res;

    FuriString* disp = furi_string_alloc();
    furi_string_printf(disp, "Renault Clio 3");
    flipper_format_write_string_cstr(flipper_format, "Disp", furi_string_get_cstr(disp));
    furi_string_free(disp);

    uint32_t k2 = instance->data_18;
    if(!flipper_format_rewind(flipper_format)) return SubGhzProtocolStatusErrorParserOthers;
    if(!flipper_format_insert_or_update_uint32(flipper_format, "Key2", &k2, 1))
        return SubGhzProtocolStatusErrorParserOthers;

    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_renault_v2_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderRenaultV2* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize(&instance->generic, flipper_format);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint32_t k2 = 0;
    if(!flipper_format_read_uint32(flipper_format, "Key2", &k2, 1))
        return SubGhzProtocolStatusError;

    instance->data_18 = k2;
    instance->generic.protocol_name = instance->base.protocol->name;

    return SubGhzProtocolStatusOk;
}

// ============================================================================
// DISPLAY
// ============================================================================

void subghz_protocol_decoder_renault_v2_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderRenaultV2* instance = context;

    uint32_t key_hi = (uint32_t)(instance->generic.data >> 32);
    uint32_t key_lo = (uint32_t)(instance->generic.data & 0xFFFFFFFF);

    // Sync = bits 0-15 (71EA), encrypted hop = bits 16-81
    // Hop spans key_lo lower 48 bits + data_18
    uint32_t hop_hi = (key_lo >> 16) & 0xFFFF;  // bits 16-31 of encrypted
    uint32_t hop_mid = (key_lo & 0xFFFF);         // bits 32-47 of encrypted
    // data_18 contains bits 48-65 (18 bits, left-aligned)

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%08lX%08lX\r\n"
        "Key2:%05lX\r\n"
        "Hop:%04lX%04lX%05lX",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long)key_hi,
        (unsigned long)key_lo,
        (unsigned long)(instance->data_18 >> 14),
        (unsigned long)hop_hi,
        (unsigned long)hop_mid,
        (unsigned long)(instance->data_18 >> 14));
}

// ============================================================================
// ENCODER
// ============================================================================

static bool renault_v2_get_data_bit(SubGhzProtocolEncoderRenaultV2* instance, uint8_t idx) {
    if(idx < 64) {
        return (instance->generic.data >> (63 - idx)) & 1;
    } else {
        return (instance->data_18 >> (17 - (idx - 64))) & 1;
    }
}

void* subghz_protocol_encoder_renault_v2_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderRenaultV2* instance = malloc(sizeof(SubGhzProtocolEncoderRenaultV2));
    instance->base.protocol = &subghz_protocol_renault_v2;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = RENAULT_V2_TOTAL_BURSTS;
    instance->encoder.size_upload = 512;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_renault_v2_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderRenaultV2* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

/**
 * Build one complete frame:
 * [PREAMBLE: 44x SHORT pairs] [82 Manchester bits] [END GAP]
 */
static void renault_v2_build_upload(SubGhzProtocolEncoderRenaultV2* instance) {
    furi_assert(instance);
    size_t index = 0;

    uint32_t te_s = subghz_protocol_renault_v2_const.te_short;
    uint32_t te_l = subghz_protocol_renault_v2_const.te_long;

    // PREAMBLE: alternating SHORT pairs (~87 bits of 1s = ~44 pairs)
    for(int p = 0; p < RENAULT_V2_PREAMBLE_PAIRS; p++) {
        instance->encoder.upload[index++] = level_duration_make(true, te_s);
        instance->encoder.upload[index++] = level_duration_make(false, te_s);
    }

    // Manchester-encode 82 data bits
    uint8_t manchester_state = ManchesterStateMid1;

    for(int i = 0; i < RENAULT_V2_DATA_BITS; i++) {
        bool b = renault_v2_get_data_bit(instance, i);

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
    subghz_protocol_encoder_renault_v2_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderRenaultV2* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;

    do {
        if(subghz_block_generic_deserialize(&instance->generic, flipper_format) !=
           SubGhzProtocolStatusOk) {
            break;
        }

        uint32_t k2 = 0;
        if(!flipper_format_read_uint32(flipper_format, "Key2", &k2, 1)) break;
        instance->data_18 = k2;

        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        renault_v2_build_upload(instance);
        instance->encoder.is_running = true;
        res = SubGhzProtocolStatusOk;
    } while(false);

    return res;
}

void subghz_protocol_encoder_renault_v2_stop(void* context) {
    SubGhzProtocolEncoderRenaultV2* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_renault_v2_yield(void* context) {
    SubGhzProtocolEncoderRenaultV2* instance = context;

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
