/**
 * Peugot V0 SubGHz Protocol - 80-bit encrypted rolling code (PSA)
 *
 * 88-bit frame: [Sync:8=FE][Data:80]
 * - Sync: FE (11111110) — end-of-preamble marker (last bit = 0)
 * - Data: 80-bit, starts with FFAD header, fully encrypted rolling code
 *   - Bits 0-63: stored in generic.data (Key)
 *   - Bits 64-79: stored in data_16 (Key2)
 *
 * Physical layer: Manchester, ~220/440us, long preamble (80+ bits of 1),
 * FE byte marks end of preamble, followed by 80 data bits.
 *
 * 433.92 MHz, OOK
 *
 * Decoder uses stateless Manchester (Ansatz C):
 * - Collect all bits (preamble + sync + data)
 * - Search for FEFFAD pattern for robust sync detection
 * - Extract 80 data bits starting after FE (includes FFAD + 64 bit payload)
 */

#include "peugot_v0.h"

#include <lib/toolbox/manchester_decoder.h>
#include <lib/toolbox/manchester_encoder.h>

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include <furi.h>
#include <furi_hal.h>

#define TAG "SubGhzProtocolPeugotV0"

#define PEUGOT_V0_DATA_BITS         80
#define PEUGOT_V0_MAX_BITS          256
#define PEUGOT_V0_MIN_PREAMBLE_BITS 30
#define PEUGOT_V0_SYNC_LEN          8      // FE = 11111110 (preamble end marker)
#define PEUGOT_V0_MIN_DATA_BITS     70

// Sync pattern: FE = 11111110 (end-of-preamble marker)
// We search for the full FEFFAD (24 bits) for robustness, but data starts after FE (8 bits)
static const uint8_t peugot_v0_sync_pattern_full[] = {
    1,1,1,1,1,1,1,0,  // 0xFE
    1,1,1,1,1,1,1,1,  // 0xFF
    1,0,1,0,1,1,0,1   // 0xAD
};
#define PEUGOT_V0_SYNC_SEARCH_LEN 24

static const SubGhzBlockConst subghz_protocol_peugot_v0_const = {
    .te_short = 220,
    .te_long = 440,
    .te_delta = 100,
    .min_count_bit_for_found = PEUGOT_V0_DATA_BITS,
};

// ============================================================================
// DATA STRUCTURES
// ============================================================================

struct SubGhzProtocolDecoderPeugotV0 {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint8_t all_bits[PEUGOT_V0_MAX_BITS];
    uint16_t bit_count;
    bool half_bit;

    uint16_t data_16;  // Bits 64-79 of the 80-bit frame
};

struct SubGhzProtocolEncoderPeugotV0 {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint16_t data_16;  // Bits 64-79 for encoding
};

// ============================================================================
// PROTOCOL DEFINITION
// ============================================================================

static SubGhzBlockGeneric* subghz_protocol_decoder_peugot_v0_get_generic(void* context) {
    SubGhzProtocolDecoderPeugotV0* instance = context;
    return &instance->generic;
}

const SubGhzProtocolDecoder subghz_protocol_peugot_v0_decoder = {
    .alloc = subghz_protocol_decoder_peugot_v0_alloc,
    .free = subghz_protocol_decoder_peugot_v0_free,

    .feed = subghz_protocol_decoder_peugot_v0_feed,
    .reset = subghz_protocol_decoder_peugot_v0_reset,

    .get_hash_data = subghz_protocol_decoder_peugot_v0_get_hash_data,
    .serialize = subghz_protocol_decoder_peugot_v0_serialize,
    .deserialize = subghz_protocol_decoder_peugot_v0_deserialize,
    .get_string = subghz_protocol_decoder_peugot_v0_get_string,
    .get_generic = subghz_protocol_decoder_peugot_v0_get_generic,
};

const SubGhzProtocolEncoder subghz_protocol_peugot_v0_encoder = {
    .alloc = subghz_protocol_encoder_peugot_v0_alloc,
    .free = subghz_protocol_encoder_peugot_v0_free,

    .deserialize = subghz_protocol_encoder_peugot_v0_deserialize,
    .stop = subghz_protocol_encoder_peugot_v0_stop,
    .yield = subghz_protocol_encoder_peugot_v0_yield,
};

const SubGhzProtocol subghz_protocol_peugot_v0 = {
    .name = SUBGHZ_PROTOCOL_PEUGOT_V0_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,

    .decoder = &subghz_protocol_peugot_v0_decoder,
    .encoder = &subghz_protocol_peugot_v0_encoder,
};

// ============================================================================
// DECODER IMPLEMENTATION
// ============================================================================

void* subghz_protocol_decoder_peugot_v0_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderPeugotV0* instance = malloc(sizeof(SubGhzProtocolDecoderPeugotV0));
    instance->base.protocol = &subghz_protocol_peugot_v0;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_peugot_v0_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderPeugotV0* instance = context;
    free(instance);
}

void subghz_protocol_decoder_peugot_v0_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderPeugotV0* instance = context;
    instance->decoder.parser_step = 0;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->bit_count = 0;
    instance->half_bit = false;
    instance->data_16 = 0;
}

/**
 * Search for sync pattern and extract 80 data bits from collected bits.
 */
static void peugot_v0_process_frame(SubGhzProtocolDecoderPeugotV0* instance) {
    if(instance->bit_count < (PEUGOT_V0_SYNC_LEN + PEUGOT_V0_MIN_DATA_BITS)) return;

    // Search for full FEFFAD pattern (24 bits) for robust detection
    // Data starts at sync_pos + 8 (after FE), so constraint is:
    // sync_pos + SYNC_LEN + MIN_DATA_BITS <= bit_count AND sync_pos + SYNC_SEARCH_LEN <= bit_count
    int max_pos_data = instance->bit_count - PEUGOT_V0_SYNC_LEN - PEUGOT_V0_MIN_DATA_BITS;
    int max_pos_pattern = instance->bit_count - PEUGOT_V0_SYNC_SEARCH_LEN;
    int max_pos = (max_pos_data < max_pos_pattern) ? max_pos_data : max_pos_pattern;
    int sync_pos = -1;

    for(int i = 0; i <= max_pos; i++) {
        bool match = true;
        for(int j = 0; j < PEUGOT_V0_SYNC_SEARCH_LEN; j++) {
            if(instance->all_bits[i + j] != peugot_v0_sync_pattern_full[j]) {
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
        FURI_LOG_D(TAG, "No sync found in %u bits", instance->bit_count);
        return;
    }

    // Verify preamble: count consecutive 1-bits before sync
    int preamble_ones = 0;
    for(int i = sync_pos - 1; i >= 0; i--) {
        if(instance->all_bits[i] == 1) {
            preamble_ones++;
        } else {
            break;
        }
    }

    if(preamble_ones < PEUGOT_V0_MIN_PREAMBLE_BITS) {
        FURI_LOG_D(TAG, "Preamble too short: %d (need %d)", preamble_ones, PEUGOT_V0_MIN_PREAMBLE_BITS);
        return;
    }

    // Data starts after FE (8 bits into the sync match), includes FFAD + payload
    int data_start = sync_pos + PEUGOT_V0_SYNC_LEN;  // +8 = after FE
    int data_available = instance->bit_count - data_start;
    int data_bits = (data_available > PEUGOT_V0_DATA_BITS) ? PEUGOT_V0_DATA_BITS : data_available;

    if(data_bits < PEUGOT_V0_MIN_DATA_BITS) {
        FURI_LOG_D(TAG, "Data too short: %d bits (need %d)", data_bits, PEUGOT_V0_MIN_DATA_BITS);
        return;
    }

    // Extract first 64 bits → generic.data
    uint64_t data = 0;
    int bits_for_data = (data_bits > 64) ? 64 : data_bits;
    for(int i = 0; i < bits_for_data; i++) {
        data = (data << 1) | instance->all_bits[data_start + i];
    }
    if(bits_for_data < 64) {
        data <<= (64 - bits_for_data);
    }

    // Extract bits 64-79 → data_16
    uint16_t data_16 = 0;
    if(data_bits > 64) {
        int extra_bits = data_bits - 64;
        if(extra_bits > 16) extra_bits = 16;
        for(int i = 0; i < extra_bits; i++) {
            data_16 = (data_16 << 1) | instance->all_bits[data_start + 64 + i];
        }
        if(extra_bits < 16) {
            data_16 <<= (16 - extra_bits);
        }
    }

    instance->generic.data = data;
    instance->generic.data_count_bit = PEUGOT_V0_DATA_BITS;
    instance->data_16 = data_16;

    uint32_t key_hi = (uint32_t)(data >> 32);
    uint32_t key_lo = (uint32_t)(data & 0xFFFFFFFF);

    FURI_LOG_I(
        TAG,
        "Decoded %dbit: Key=%08lX%08lX Key2=%04X (preamble=%d, sync@%d)",
        data_bits,
        (unsigned long)key_hi,
        (unsigned long)key_lo,
        data_16,
        preamble_ones,
        sync_pos);

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

void subghz_protocol_decoder_peugot_v0_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderPeugotV0* instance = context;

    bool is_short = DURATION_DIFF(duration, subghz_protocol_peugot_v0_const.te_short) <=
                    subghz_protocol_peugot_v0_const.te_delta;
    bool is_long = DURATION_DIFF(duration, subghz_protocol_peugot_v0_const.te_long) <=
                   subghz_protocol_peugot_v0_const.te_delta;

    if(is_short) {
        if(instance->half_bit) {
            // Second SHORT completes a bit
            if(instance->bit_count < PEUGOT_V0_MAX_BITS) {
                instance->all_bits[instance->bit_count++] = level ? 1 : 0;
            }
            instance->half_bit = false;
        } else {
            // First SHORT, wait for second
            instance->half_bit = true;
        }
    } else if(is_long) {
        // LONG = one complete bit
        if(instance->bit_count < PEUGOT_V0_MAX_BITS) {
            instance->all_bits[instance->bit_count++] = level ? 1 : 0;
        }
        instance->half_bit = false;
    } else {
        // Gap or invalid timing → process collected bits and reset
        if(instance->bit_count > 0) {
            peugot_v0_process_frame(instance);
            instance->bit_count = 0;
            instance->half_bit = false;
        }
    }

    // Buffer full → process
    if(instance->bit_count >= PEUGOT_V0_MAX_BITS) {
        peugot_v0_process_frame(instance);
        instance->bit_count = 0;
        instance->half_bit = false;
    }
}

uint8_t subghz_protocol_decoder_peugot_v0_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderPeugotV0* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

// ============================================================================
// SERIALIZATION
// ============================================================================

SubGhzProtocolStatus subghz_protocol_decoder_peugot_v0_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderPeugotV0* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret != SubGhzProtocolStatusOk) return ret;

    // Write extra 16 bits as Key2
    uint32_t key2 = instance->data_16;
    if(!flipper_format_write_uint32(flipper_format, "Key2", &key2, 1)) {
        FURI_LOG_E(TAG, "Failed to write Key2");
        return SubGhzProtocolStatusError;
    }

    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_peugot_v0_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderPeugotV0* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize(&instance->generic, flipper_format);

    if(ret != SubGhzProtocolStatusOk) {
        FURI_LOG_E(TAG, "Failed to deserialize base fields");
        return ret;
    }

    if(instance->generic.data_count_bit != PEUGOT_V0_DATA_BITS) {
        FURI_LOG_E(TAG, "Invalid bit count: %lu", (unsigned long)instance->generic.data_count_bit);
        return SubGhzProtocolStatusErrorValueBitCount;
    }

    // Read Key2 (optional, for backwards compat)
    uint32_t key2 = 0;
    flipper_format_read_uint32(flipper_format, "Key2", &key2, 1);
    instance->data_16 = (uint16_t)(key2 & 0xFFFF);

    instance->generic.protocol_name = instance->base.protocol->name;
    return SubGhzProtocolStatusOk;
}

// ============================================================================
// DISPLAY
// ============================================================================

void subghz_protocol_decoder_peugot_v0_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderPeugotV0* instance = context;

    // First 16 bits of data = Serial, remaining 48 bits + Key2 (16 bits) = 64-bit Hop
    uint16_t serial = (uint16_t)(instance->generic.data >> 48);
    uint32_t hop_hi = (uint32_t)((instance->generic.data >> 16) & 0xFFFFFFFF);
    uint16_t hop_lo_part = (uint16_t)(instance->generic.data & 0xFFFF);
    // Full hop: 48 bits from generic.data (bits 47..0) + 16 bits from data_16 = 64 bits

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%08lX%08lX\r\n"
        "Key2:%04X Sn:%04X\r\n"
        "Hop:%08lX%04X%04X\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long)(instance->generic.data >> 32),
        (unsigned long)(instance->generic.data & 0xFFFFFFFF),
        instance->data_16,
        serial,
        (unsigned long)hop_hi,
        hop_lo_part,
        instance->data_16);
}

// ============================================================================
// ENCODER IMPLEMENTATION
// ============================================================================

void* subghz_protocol_encoder_peugot_v0_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderPeugotV0* instance = malloc(sizeof(SubGhzProtocolEncoderPeugotV0));

    instance->base.protocol = &subghz_protocol_peugot_v0;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = 3;
    instance->encoder.size_upload = 512;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_peugot_v0_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderPeugotV0* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

/**
 * Build one complete frame into the upload buffer.
 * Signal: [80x SHORT pairs (preamble)][Manchester FE sync + 80 data bits][END GAP]
 */
static void subghz_protocol_encoder_peugot_v0_get_upload(SubGhzProtocolEncoderPeugotV0* instance) {
    furi_assert(instance);
    size_t index = 0;

    const uint32_t te_short = (uint32_t)subghz_protocol_peugot_v0_const.te_short;
    const uint32_t te_long = (uint32_t)subghz_protocol_peugot_v0_const.te_long;

    // PREAMBLE: 80 SHORT pairs (= 80 Manchester 1-bits)
    for(int p = 0; p < 80; p++) {
        instance->encoder.upload[index++] = level_duration_make(true, te_short);
        instance->encoder.upload[index++] = level_duration_make(false, te_short);
    }

    // FE sync (8 bits) + 80 data bits = 88 bits Manchester encoded
    // After preamble of 1s, Manchester state is Mid1
    uint8_t manchester_state = ManchesterStateMid1;

    // FE = 11111110 (8 bits sync)
    uint8_t sync_byte = 0xFE;

    // Total: 8 (sync) + 80 (data) = 88 bits
    for(int i = 87; i >= 0; i--) {
        bool bit;
        if(i >= 80) {
            // Sync bits: FE byte, bits 7..0 (from MSB to LSB)
            bit = (sync_byte >> (i - 80)) & 1;
        } else if(i >= 16) {
            // Data bits 63..0 from generic.data
            bit = (instance->generic.data >> (i - 16)) & 1;
        } else {
            // Data bits 79-64 from data_16 (bits 15..0)
            bit = (instance->data_16 >> i) & 1;
        }

        switch(manchester_state) {
        case ManchesterStateMid1:
            if(bit == 0) {
                instance->encoder.upload[index++] = level_duration_make(true, te_long);
                manchester_state = ManchesterStateMid0;
            } else {
                instance->encoder.upload[index++] = level_duration_make(true, te_short);
                instance->encoder.upload[index++] = level_duration_make(false, te_short);
                manchester_state = ManchesterStateMid1;
            }
            break;
        case ManchesterStateMid0:
            if(bit == 0) {
                instance->encoder.upload[index++] = level_duration_make(false, te_short);
                instance->encoder.upload[index++] = level_duration_make(true, te_short);
                manchester_state = ManchesterStateMid0;
            } else {
                instance->encoder.upload[index++] = level_duration_make(false, te_long);
                manchester_state = ManchesterStateMid1;
            }
            break;
        default:
            manchester_state = ManchesterStateMid1;
            break;
        }
    }

    // END GAP
    bool end_level = (manchester_state == ManchesterStateMid1);
    instance->encoder.upload[index++] = level_duration_make(end_level, (uint32_t)6000);

    instance->encoder.size_upload = index;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_peugot_v0_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderPeugotV0* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        if(subghz_block_generic_deserialize(&instance->generic, flipper_format) !=
           SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }

        if(instance->generic.data_count_bit != PEUGOT_V0_DATA_BITS) {
            FURI_LOG_E(
                TAG, "Invalid bit count: %lu", (unsigned long)instance->generic.data_count_bit);
            res = SubGhzProtocolStatusErrorValueBitCount;
            break;
        }

        // Read Key2
        uint32_t key2 = 0;
        flipper_format_read_uint32(flipper_format, "Key2", &key2, 1);
        instance->data_16 = (uint16_t)(key2 & 0xFFFF);

        // Optional repeat parameter
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        subghz_protocol_encoder_peugot_v0_get_upload(instance);

        instance->encoder.is_running = true;
        res = SubGhzProtocolStatusOk;
    } while(false);

    return res;
}

void subghz_protocol_encoder_peugot_v0_stop(void* context) {
    SubGhzProtocolEncoderPeugotV0* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_peugot_v0_yield(void* context) {
    SubGhzProtocolEncoderPeugotV0* instance = context;

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
