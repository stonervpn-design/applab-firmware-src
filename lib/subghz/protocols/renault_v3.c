/**
 * Renault Megane SubGHz Protocol — 2-FSK NRZ
 *
 * Physical: 2-FSK @ 433.92 MHz, custom CC1101 preset (15.4 kbps)
 * CC1101 demodulates to baseband → raw timings = NRZ level durations.
 *
 * NRZ encoding: each timing = N consecutive bits of same level,
 *   where N = round(duration / te_short).
 *   te_short ≈ 52us (1 bit period from preamble)
 *
 * Preamble: alternating SHORT pulses (010101... in NRZ)
 * Frame: ~140 bits NRZ data after preamble
 *   Sync: 0x56 0x4B (16 bits, constant — observed in multiple bursts)
 *   Data: ~120+ bits (encrypted rolling code, fields not separable)
 *
 * Signal is very noisy (2-FSK weak reception). Multiple repeated bursts
 * per keypress, separated by short gaps.
 */

#include "renault_v3.h"

#include <lib/toolbox/manchester_decoder.h>
#include <lib/toolbox/manchester_encoder.h>

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include <furi.h>
#include <furi_hal.h>

#define TAG "SubGhzProtocolRenaultV3"

#define RENAULT_V3_MIN_DATA_BITS  64
#define RENAULT_V3_MAX_DATA_BITS  160
// Two alternating frame types observed:
// Type A: starts with C9 69 (sync A)
// Type B: starts with CB 2D (sync B)
// Both have same preamble structure (10+ SHORTs → NRZ data)
#define RENAULT_V3_SYNC_A_HI      0xC9
#define RENAULT_V3_SYNC_A_LO      0x69
#define RENAULT_V3_SYNC_B_HI      0xCB
#define RENAULT_V3_SYNC_B_LO      0x2D
#define RENAULT_V3_MIN_PREAMBLE   10
#define RENAULT_V3_TOTAL_BURSTS   3
#define RENAULT_V3_PREAMBLE_PAIRS 20

static const SubGhzBlockConst subghz_protocol_renault_v3_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 22,
    .min_count_bit_for_found = RENAULT_V3_MIN_DATA_BITS,
};

// ============================================================================
// DATA STRUCTURES
// ============================================================================

typedef enum {
    RenaultV3DecoderStepReset = 0,
    RenaultV3DecoderStepPreamble,
    RenaultV3DecoderStepCollectNRZ,
} RenaultV3DecoderStep;

struct SubGhzProtocolDecoderRenaultV3 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint16_t preamble_count;

    uint8_t all_bits[256];
    uint16_t all_bit_count;

    uint32_t data_32; // bits 64-95 (extra bytes beyond generic.data)
};

struct SubGhzProtocolEncoderRenaultV3 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint32_t data_32;
};

// ============================================================================
// PROTOCOL DEFINITION
// ============================================================================

static SubGhzBlockGeneric* subghz_protocol_decoder_renault_v3_get_generic(void* context) {
    SubGhzProtocolDecoderRenaultV3* instance = context;
    return &instance->generic;
}

const SubGhzProtocolDecoder subghz_protocol_renault_v3_decoder = {
    .alloc = subghz_protocol_decoder_renault_v3_alloc,
    .free = subghz_protocol_decoder_renault_v3_free,
    .feed = subghz_protocol_decoder_renault_v3_feed,
    .reset = subghz_protocol_decoder_renault_v3_reset,
    .get_hash_data = subghz_protocol_decoder_renault_v3_get_hash_data,
    .serialize = subghz_protocol_decoder_renault_v3_serialize,
    .deserialize = subghz_protocol_decoder_renault_v3_deserialize,
    .get_string = subghz_protocol_decoder_renault_v3_get_string,
    .get_generic = subghz_protocol_decoder_renault_v3_get_generic,
};

const SubGhzProtocolEncoder subghz_protocol_renault_v3_encoder = {
    .alloc = subghz_protocol_encoder_renault_v3_alloc,
    .free = subghz_protocol_encoder_renault_v3_free,
    .deserialize = subghz_protocol_encoder_renault_v3_deserialize,
    .stop = subghz_protocol_encoder_renault_v3_stop,
    .yield = subghz_protocol_encoder_renault_v3_yield,
};

const SubGhzProtocol subghz_protocol_renault_v3 = {
    .name = SUBGHZ_PROTOCOL_RENAULT_V3_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_renault_v3_decoder,
    .encoder = &subghz_protocol_renault_v3_encoder,
};

// ============================================================================
// DECODER
// ============================================================================

void* subghz_protocol_decoder_renault_v3_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderRenaultV3* instance = malloc(sizeof(SubGhzProtocolDecoderRenaultV3));
    instance->base.protocol = &subghz_protocol_renault_v3;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_renault_v3_free(void* context) {
    furi_assert(context);
    free(context);
}

void subghz_protocol_decoder_renault_v3_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderRenaultV3* instance = context;
    instance->decoder.parser_step = RenaultV3DecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->preamble_count = 0;
    instance->all_bit_count = 0;
    instance->data_32 = 0;
}

/**
 * Search for sync word 564B in accumulated NRZ bits and extract frame.
 */
static void renault_v3_process_frame(SubGhzProtocolDecoderRenaultV3* instance) {
    if(instance->all_bit_count < RENAULT_V3_MIN_DATA_BITS) return;

    // Sync pattern A: C9 69 = 1100 1001 0110 1001
    static const uint8_t sync_a[16] = {1,1,0,0,1,0,0,1, 0,1,1,0,1,0,0,1};
    // Sync pattern B: CB 2D = 1100 1011 0010 1101
    static const uint8_t sync_b[16] = {1,1,0,0,1,0,1,1, 0,0,1,0,1,1,0,1};

    // Fixed-position sync: V3 resets the bit buffer at the data boundary, so the
    // 16-bit sync (either variant) sits at the very start of the collected NRZ
    // data. Pin BOTH syncs to offset 0 (tolerating up to 2 bits of NRZ boundary
    // quantization jitter) instead of scanning every offset across the buffer —
    // a free scan of a 16-bit pattern over ~160 NRZ bits is the false-positive
    // source.
    int sync_pos = -1;
    for(int off = 0; off <= 2; off++) {
        bool match_a = true, match_b = true;
        for(int j = 0; j < 16; j++) {
            if(instance->all_bits[off + j] != sync_a[j]) match_a = false;
            if(instance->all_bits[off + j] != sync_b[j]) match_b = false;
            if(!match_a && !match_b) break;
        }
        if(match_a || match_b) {
            sync_pos = off;
            break;
        }
    }

    if(sync_pos < 0) {
        // Debug: dump first bytes
        char hex_dump[42] = {0};
        int dump_bits = instance->all_bit_count < 160 ? instance->all_bit_count : 160;
        for(int i = 0; i < dump_bits / 8 && i < 20; i++) {
            uint8_t byte = 0;
            for(int j = 0; j < 8; j++) {
                byte = (byte << 1) | instance->all_bits[i * 8 + j];
            }
            snprintf(hex_dump + i * 2, 3, "%02X", byte);
        }
        FURI_LOG_D(TAG, "No sync in %d bits: %s", instance->all_bit_count, hex_dump);
        return;
    }

    int data_len = instance->all_bit_count - sync_pos;
    if(data_len > RENAULT_V3_MAX_DATA_BITS) data_len = RENAULT_V3_MAX_DATA_BITS;

    // Extract generic.data (first 64 bits from sync)
    instance->generic.data = 0;
    for(int i = 0; i < 64 && i < data_len; i++) {
        instance->generic.data = (instance->generic.data << 1) | instance->all_bits[sync_pos + i];
    }

    // Extract data_32 (bits 64-95)
    instance->data_32 = 0;
    for(int i = 64; i < data_len && i < 96; i++) {
        instance->data_32 = (instance->data_32 << 1) | instance->all_bits[sync_pos + i];
    }
    if(data_len > 64 && data_len - 64 < 32) {
        instance->data_32 <<= (32 - (data_len - 64));
    }

    // Payload invariant: the bits after the sync are an encrypted rolling code,
    // never all-zero or all-one. Reject degenerate buffers that satisfy a sync by
    // chance. (Sync bits live in the high 16 of generic.data, so mask them off.)
    {
        uint64_t payload = instance->generic.data & 0xFFFFFFFFFFFFULL; // bits 16..63
        if((payload == 0 && instance->data_32 == 0) ||
           (payload == 0xFFFFFFFFFFFFULL && instance->data_32 == 0xFFFFFFFFUL)) {
            return;
        }
    }

    instance->generic.data_count_bit = data_len;
    instance->decoder.decode_data = instance->generic.data;
    instance->decoder.decode_count_bit = data_len;

    // Bytes 0-1: Sync 564B, Bytes 2-5: probable serial/data
    instance->generic.serial = (uint32_t)((instance->generic.data >> 32) & 0xFFFF);

    FURI_LOG_I(TAG, "Decoded %d bits: Sn=%04lX Key=%08lX%08lX",
        data_len, (unsigned long)instance->generic.serial,
        (unsigned long)(instance->generic.data >> 32),
        (unsigned long)(instance->generic.data & 0xFFFFFFFF));

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

void subghz_protocol_decoder_renault_v3_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderRenaultV3* instance = context;

    uint32_t te_s = subghz_protocol_renault_v3_const.te_short;
    uint32_t te_d = subghz_protocol_renault_v3_const.te_delta;

    switch(instance->decoder.parser_step) {
    case RenaultV3DecoderStepReset:
        // Any SHORT starts preamble detection
        if(DURATION_DIFF(duration, te_s) <= te_d) {
            instance->decoder.parser_step = RenaultV3DecoderStepPreamble;
            instance->preamble_count = 1;
        }
        break;

    case RenaultV3DecoderStepPreamble: {
        // Count SHORT pulses in preamble. When we see a non-SHORT timing
        // after enough preamble, transition to NRZ data collection.
        bool is_short = DURATION_DIFF(duration, te_s) <= te_d;

        if(is_short) {
            instance->preamble_count++;
            if(instance->preamble_count > 5000) {
                instance->decoder.parser_step = RenaultV3DecoderStepReset;
            }
        } else if(instance->preamble_count >= RENAULT_V3_MIN_PREAMBLE) {
            // Non-SHORT after sufficient preamble = data starts
            // NRZ-decode this timing as the first data bits
            FURI_LOG_D(TAG, "Preamble: %d SHORTs, NRZ data starts (dur=%lu)",
                instance->preamble_count, (unsigned long)duration);

            instance->all_bit_count = 0;

            // Quantize this first non-SHORT timing to N bits
            uint32_t n = (duration + te_s / 2) / te_s;
            if(n > 8) n = 8; // cap single timing to 8 bits
            uint8_t bit_val = level ? 1 : 0;
            for(uint32_t b = 0; b < n && instance->all_bit_count < sizeof(instance->all_bits); b++) {
                instance->all_bits[instance->all_bit_count++] = bit_val;
            }

            instance->decoder.parser_step = RenaultV3DecoderStepCollectNRZ;
        } else {
            // Not enough preamble → reset
            instance->decoder.parser_step = RenaultV3DecoderStepReset;
        }
    } break;

    case RenaultV3DecoderStepCollectNRZ: {
        // NRZ decode: quantize each timing to N = round(duration / te_short)
        // Each produces N bits of the current level
        uint32_t n = (duration + te_s / 2) / te_s;

        if(n == 0 || duration > te_s * 8) {
            // Gap or noise → process collected frame and reset
            renault_v3_process_frame(instance);
            instance->decoder.parser_step = RenaultV3DecoderStepReset;
            break;
        }

        uint8_t bit_val = level ? 1 : 0;
        for(uint32_t b = 0; b < n && instance->all_bit_count < sizeof(instance->all_bits); b++) {
            instance->all_bits[instance->all_bit_count++] = bit_val;
        }

        if(instance->all_bit_count >= 250) {
            renault_v3_process_frame(instance);
            instance->decoder.parser_step = RenaultV3DecoderStepReset;
        }
    } break;

    default:
        instance->decoder.parser_step = RenaultV3DecoderStepReset;
        break;
    }
}

// ============================================================================
// HASH
// ============================================================================

uint8_t subghz_protocol_decoder_renault_v3_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderRenaultV3* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

// ============================================================================
// SERIALIZATION
// ============================================================================

SubGhzProtocolStatus subghz_protocol_decoder_renault_v3_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderRenaultV3* instance = context;

    SubGhzProtocolStatus res =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(res != SubGhzProtocolStatusOk) return res;

    FuriString* disp = furi_string_alloc();
    furi_string_printf(disp, "Renault Megane - Sn:%04lX", (unsigned long)instance->generic.serial);
    flipper_format_write_string_cstr(flipper_format, "Disp", furi_string_get_cstr(disp));
    furi_string_free(disp);

    uint32_t k2 = instance->data_32;
    if(!flipper_format_rewind(flipper_format)) return SubGhzProtocolStatusErrorParserOthers;
    if(!flipper_format_insert_or_update_uint32(flipper_format, "Key2", &k2, 1))
        return SubGhzProtocolStatusErrorParserOthers;

    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_renault_v3_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderRenaultV3* instance = context;

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

void subghz_protocol_decoder_renault_v3_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderRenaultV3* instance = context;

    uint32_t key_hi = (uint32_t)(instance->generic.data >> 32);
    uint32_t key_lo = (uint32_t)(instance->generic.data & 0xFFFFFFFF);

    // Everything after sync is encrypted — output as Hop
    uint32_t hop_hi = key_lo;
    uint32_t hop_lo = instance->data_32;

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

static bool renault_v3_get_data_bit(SubGhzProtocolEncoderRenaultV3* instance, uint8_t idx) {
    if(idx < 64) {
        return (instance->generic.data >> (63 - idx)) & 1;
    } else {
        return (instance->data_32 >> (31 - (idx - 64))) & 1;
    }
}

void* subghz_protocol_encoder_renault_v3_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderRenaultV3* instance = malloc(sizeof(SubGhzProtocolEncoderRenaultV3));
    instance->base.protocol = &subghz_protocol_renault_v3;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = RENAULT_V3_TOTAL_BURSTS;
    instance->encoder.size_upload = 512;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_renault_v3_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderRenaultV3* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

/**
 * Build one complete NRZ frame:
 * [PREAMBLE: 20x alternating SHORT] [NRZ data bits] [END GAP]
 */
static void renault_v3_build_upload(SubGhzProtocolEncoderRenaultV3* instance) {
    furi_assert(instance);
    size_t index = 0;

    uint32_t te_s = subghz_protocol_renault_v3_const.te_short;

    // PREAMBLE: alternating SHORT pulses (010101...)
    for(int p = 0; p < RENAULT_V3_PREAMBLE_PAIRS; p++) {
        instance->encoder.upload[index++] = level_duration_make(true, te_s);
        instance->encoder.upload[index++] = level_duration_make(false, te_s);
    }

    // NRZ-encode data bits: merge consecutive same-level bits into one timing
    int total_bits = instance->generic.data_count_bit;
    if(total_bits > 96) total_bits = 96;

    bool prev_level = false; // preamble ends LOW
    uint32_t run_len = 0;

    for(int i = 0; i < total_bits; i++) {
        bool b = renault_v3_get_data_bit(instance, i);

        if(i == 0) {
            prev_level = b;
            run_len = 1;
        } else if(b == prev_level) {
            run_len++;
        } else {
            // Emit the completed run
            instance->encoder.upload[index++] =
                level_duration_make(prev_level, te_s * run_len);
            prev_level = b;
            run_len = 1;
        }
    }
    // Emit last run
    if(run_len > 0) {
        instance->encoder.upload[index++] =
            level_duration_make(prev_level, te_s * run_len);
    }

    // END: gap
    instance->encoder.upload[index++] =
        level_duration_make(!prev_level, 2000);

    instance->encoder.size_upload = index;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_renault_v3_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderRenaultV3* instance = context;
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

        renault_v3_build_upload(instance);
        instance->encoder.is_running = true;
        res = SubGhzProtocolStatusOk;
    } while(false);

    return res;
}

void subghz_protocol_encoder_renault_v3_stop(void* context) {
    SubGhzProtocolEncoderRenaultV3* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_renault_v3_yield(void* context) {
    SubGhzProtocolEncoderRenaultV3* instance = context;

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
