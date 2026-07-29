/*
 * Chevrolet V0 SubGHz Protocol (e.g. Chevrolet Orlando)
 *
 * Signal: 433.92 MHz, OOK
 * Encoding: Standard Manchester (IEEE 802.3)
 * Timing: te_short=120us, te_long=240us
 *
 * Frame structure (after Manchester decoding):
 *   Preamble: ~44 bits of alternating 10, ending with sync (0xAAAAAAAB)
 *   Data:     96 bits (12 bytes):
 *             Bytes 0-2: Serial (24 bits, fixed per remote)
 *             Byte 3 hi: Button (4 bits, 0xA=Open, 0xB=Close)
 *             Byte 3 lo + Bytes 4-7: Encrypted hop code (36 bits)
 *             Bytes 8-11: Seed (32 bits, encrypted)
 *   Total:    ~140 Manchester bits
 *
 * Decoder uses a two-stage approach:
 *   Stage 1: Feeds pulses to manchester_advance(), tracks 32-bit register
 *            for preamble pattern 0xAAAAAAAB detection
 *   Stage 2: After preamble, collects exactly 96 data bits into
 *            data_header(32) + decode_data(64) registers
 */

#include "chevrolet_v0.h"
#include <lib/toolbox/manchester_decoder.h>

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#define TAG "SubGhzProtocolChevroletV0"

#define CHEVROLET_V0_BIT_COUNT     96
#define CHEVROLET_V0_PREAMBLE_BITS 44
#define CHEVROLET_V0_DATA_BITS     96

static const SubGhzBlockConst subghz_protocol_chevrolet_v0_const = {
    .te_short = 120,
    .te_long = 240,
    .te_delta = 70,
    .min_count_bit_for_found = CHEVROLET_V0_BIT_COUNT,
};

// ============================================================================
// DATA STRUCTURES
// ============================================================================

typedef enum {
    ChevroletV0DecoderStepReset = 0,
    ChevroletV0DecoderStepDecodeManchesterPreamble,
    ChevroletV0DecoderStepCollectData,
} ChevroletV0DecoderStep;

struct SubGhzProtocolDecoderChevroletV0 {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    ManchesterState manchester_state;
    uint32_t data_header; // upper 32 bits of 96-bit data (or preamble detect register)
    uint16_t bit_count;   // Manchester bits received (preamble phase)
    uint16_t data_count;  // data bits collected after preamble
};

struct SubGhzProtocolEncoderChevroletV0 {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};

// ============================================================================
// PROTOCOL DEFINITION
// ============================================================================

static SubGhzBlockGeneric* subghz_protocol_decoder_chevrolet_v0_get_generic(void* context) {
    SubGhzProtocolDecoderChevroletV0* instance = context;
    return &instance->generic;
}

const SubGhzProtocolDecoder subghz_protocol_chevrolet_v0_decoder = {
    .alloc = subghz_protocol_decoder_chevrolet_v0_alloc,
    .free = subghz_protocol_decoder_chevrolet_v0_free,

    .feed = subghz_protocol_decoder_chevrolet_v0_feed,
    .reset = subghz_protocol_decoder_chevrolet_v0_reset,

    .get_hash_data = subghz_protocol_decoder_chevrolet_v0_get_hash_data,
    .serialize = subghz_protocol_decoder_chevrolet_v0_serialize,
    .deserialize = subghz_protocol_decoder_chevrolet_v0_deserialize,
    .get_string = subghz_protocol_decoder_chevrolet_v0_get_string,
    .get_generic = subghz_protocol_decoder_chevrolet_v0_get_generic,
};

const SubGhzProtocolEncoder subghz_protocol_chevrolet_v0_encoder = {
    .alloc = subghz_protocol_encoder_chevrolet_v0_alloc,
    .free = subghz_protocol_encoder_chevrolet_v0_free,

    .deserialize = subghz_protocol_encoder_chevrolet_v0_deserialize,
    .stop = subghz_protocol_encoder_chevrolet_v0_stop,
    .yield = subghz_protocol_encoder_chevrolet_v0_yield,
};

const SubGhzProtocol subghz_protocol_chevrolet_v0 = {
    .name = SUBGHZ_PROTOCOL_CHEVROLET_V0_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,

    .decoder = &subghz_protocol_chevrolet_v0_decoder,
    .encoder = &subghz_protocol_chevrolet_v0_encoder,
};

// ============================================================================
// DATA EXTRACTION
// ============================================================================

static void chevrolet_v0_extract_data(SubGhzBlockGeneric* generic) {
    // 96-bit payload stored as: data(64) = bytes 0-7, seed(32) = bytes 8-11
    // Bytes 0-2: serial (24 bits)
    // Byte 3 hi nibble: button (4 bits, 0xA=Open, 0xB=Close)
    // Byte 3 lo + Bytes 4-7: encrypted hop code (36 bits)
    generic->serial = (uint32_t)(generic->data >> 40) & 0xFFFFFF;
    generic->btn = (generic->data >> 36) & 0x0F;
    generic->cnt = 0; // no plaintext counter — hop code is fully encrypted
}

static const char* chevrolet_v0_get_button_name(uint8_t btn) {
    switch(btn) {
    case 0xA:
        return "Open";
    case 0xB:
        return "Close";
    default: {
        static char buf[8];
        snprintf(buf, sizeof(buf), "0x%02X", btn);
        return buf;
    }
    }
}

// ============================================================================
// DECODER
// ============================================================================

void* subghz_protocol_decoder_chevrolet_v0_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderChevroletV0* instance =
        malloc(sizeof(SubGhzProtocolDecoderChevroletV0));
    instance->base.protocol = &subghz_protocol_chevrolet_v0;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_chevrolet_v0_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderChevroletV0* instance = context;
    free(instance);
}

void subghz_protocol_decoder_chevrolet_v0_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderChevroletV0* instance = context;
    instance->decoder.parser_step = ChevroletV0DecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->data_header = 0;
    instance->bit_count = 0;
    instance->data_count = 0;
    manchester_advance(
        instance->manchester_state,
        ManchesterEventReset,
        &instance->manchester_state,
        NULL);
}

void subghz_protocol_decoder_chevrolet_v0_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderChevroletV0* instance = context;

    ManchesterEvent event = ManchesterEventReset;

    if(DURATION_DIFF(duration, subghz_protocol_chevrolet_v0_const.te_short) <=
       subghz_protocol_chevrolet_v0_const.te_delta) {
        event = level ? ManchesterEventShortLow : ManchesterEventShortHigh;
    } else if(
        DURATION_DIFF(duration, subghz_protocol_chevrolet_v0_const.te_long) <=
        subghz_protocol_chevrolet_v0_const.te_delta) {
        event = level ? ManchesterEventLongLow : ManchesterEventLongHigh;
    }

    switch(instance->decoder.parser_step) {
    case ChevroletV0DecoderStepReset:
        if(event != ManchesterEventReset) {
            manchester_advance(
                instance->manchester_state,
                ManchesterEventReset,
                &instance->manchester_state,
                NULL);
            instance->data_header = 0;
            instance->bit_count = 0;
            instance->decoder.parser_step = ChevroletV0DecoderStepDecodeManchesterPreamble;
        } else {
            break;
        }
        /* FALLTHROUGH */

    case ChevroletV0DecoderStepDecodeManchesterPreamble:
        if(event == ManchesterEventReset) {
            instance->decoder.parser_step = ChevroletV0DecoderStepReset;
            break;
        }

        {
            bool data;
            bool data_ok = manchester_advance(
                instance->manchester_state,
                event,
                &instance->manchester_state,
                &data);

            if(data_ok) {
                data = !data; // Invert for IEEE 802.3 convention

                // Shift into 32-bit preamble detection register
                instance->data_header =
                    (instance->data_header << 1) | (data ? 1UL : 0UL);
                instance->bit_count++;

                // Check for preamble sync pattern
                if(instance->bit_count >= 32 &&
                   instance->data_header == 0xAAAAAAAB) {
                    // Preamble found — start collecting 96 data bits
                    instance->data_header = 0;
                    instance->decoder.decode_data = 0;
                    instance->data_count = 0;
                    instance->decoder.parser_step = ChevroletV0DecoderStepCollectData;
                }
            }
        }
        break;

    case ChevroletV0DecoderStepCollectData:
        if(event == ManchesterEventReset) {
            instance->decoder.parser_step = ChevroletV0DecoderStepReset;
            break;
        }

        {
            bool data;
            bool data_ok = manchester_advance(
                instance->manchester_state,
                event,
                &instance->manchester_state,
                &data);

            if(data_ok) {
                data = !data; // Invert for IEEE 802.3 convention

                // Shift into 96-bit register: data_header(32) + decode_data(64)
                instance->data_header =
                    (instance->data_header << 1) |
                    (uint32_t)((instance->decoder.decode_data >> 63) & 1);
                instance->decoder.decode_data =
                    (instance->decoder.decode_data << 1) | (data ? 1ULL : 0ULL);
                instance->data_count++;

                if(instance->data_count >= CHEVROLET_V0_DATA_BITS) {
                    // Frame complete
                    // data_header = payload bytes 0-3 (first 32 bits)
                    // decode_data = payload bytes 4-11 (last 64 bits)
                    instance->generic.data =
                        ((uint64_t)instance->data_header << 32) |
                        (instance->decoder.decode_data >> 32);
                    instance->generic.seed =
                        (uint32_t)(instance->decoder.decode_data & 0xFFFFFFFF);
                    instance->generic.data_count_bit = CHEVROLET_V0_BIT_COUNT;
                    instance->generic.serial =
                        (uint32_t)(instance->generic.data >> 40) & 0xFFFFFF;

                    if(instance->base.callback) {
                        instance->base.callback(
                            &instance->base, instance->base.context);
                    }

                    instance->decoder.parser_step = ChevroletV0DecoderStepReset;
                }
            }
        }
        break;
    }
}

uint8_t subghz_protocol_decoder_chevrolet_v0_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderChevroletV0* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

// ============================================================================
// DECODER - SERIALIZE
// ============================================================================

SubGhzProtocolStatus subghz_protocol_decoder_chevrolet_v0_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderChevroletV0* instance = context;

    chevrolet_v0_extract_data(&instance->generic);

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret != SubGhzProtocolStatusOk) return ret;

    // Store the extra 32 bits (bytes 8-11) as Seed
    uint8_t seed_data[4];
    seed_data[0] = (instance->generic.seed >> 24) & 0xFF;
    seed_data[1] = (instance->generic.seed >> 16) & 0xFF;
    seed_data[2] = (instance->generic.seed >> 8) & 0xFF;
    seed_data[3] = instance->generic.seed & 0xFF;
    if(!flipper_format_write_hex(flipper_format, "Seed", seed_data, 4)) {
        return SubGhzProtocolStatusError;
    }

    return SubGhzProtocolStatusOk;
}

// ============================================================================
// DECODER - DESERIALIZE
// ============================================================================

SubGhzProtocolStatus subghz_protocol_decoder_chevrolet_v0_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderChevroletV0* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    do {
        ret = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic, flipper_format, CHEVROLET_V0_BIT_COUNT);
        if(ret != SubGhzProtocolStatusOk) {
            break;
        }

        // Read Seed (extra 32 bits: bytes 8-11)
        if(!flipper_format_rewind(flipper_format)) {
            break;
        }
        uint8_t seed_data[4] = {0};
        if(flipper_format_read_hex(flipper_format, "Seed", seed_data, 4)) {
            instance->generic.seed =
                ((uint32_t)seed_data[0] << 24) | ((uint32_t)seed_data[1] << 16) |
                ((uint32_t)seed_data[2] << 8) | seed_data[3];
        }

        chevrolet_v0_extract_data(&instance->generic);

    } while(false);

    return ret;
}

// ============================================================================
// DECODER - GET STRING
// ============================================================================

void subghz_protocol_decoder_chevrolet_v0_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderChevroletV0* instance = context;

    chevrolet_v0_extract_data(&instance->generic);

    const char* btn_name = chevrolet_v0_get_button_name(instance->generic.btn);

    // Push protocol data to global for roundtrip
    subghz_block_generic_global.cnt_is_available = false;
    subghz_block_generic_global.btn_is_available = true;
    subghz_block_generic_global.current_btn = instance->generic.btn;
    subghz_block_generic_global.btn_length_bit = 4;

    uint32_t code_found_hi = instance->generic.data >> 32;
    uint32_t code_found_lo = instance->generic.data & 0xFFFFFFFF;

    // Encrypted hop code: byte 3 lo nibble + bytes 4-7 (36 bits)
    uint64_t hop = instance->generic.data & 0xFFFFFFFFFULL;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%08lX%08lX%08lX\r\n"
        "Sn:%06lX Btn:%s Hop:%09llX\r\n",
        SUBGHZ_PROTOCOL_CHEVROLET_V0_NAME,
        instance->generic.data_count_bit,
        (unsigned long)code_found_hi,
        (unsigned long)code_found_lo,
        (unsigned long)instance->generic.seed,
        (unsigned long)instance->generic.serial,
        btn_name,
        (unsigned long long)hop);
}

// ============================================================================
// ENCODER
// ============================================================================

void* subghz_protocol_encoder_chevrolet_v0_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderChevroletV0* instance =
        malloc(sizeof(SubGhzProtocolEncoderChevroletV0));

    instance->base.protocol = &subghz_protocol_chevrolet_v0;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = 1;
    instance->encoder.size_upload = 1024;
    instance->encoder.upload =
        malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
    return instance;
}

void subghz_protocol_encoder_chevrolet_v0_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderChevroletV0* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

// ============================================================================
// ENCODER - BUILD UPLOAD
// ============================================================================

static void subghz_protocol_encoder_chevrolet_v0_get_upload(
    SubGhzProtocolEncoderChevroletV0* instance) {
    furi_assert(instance);
    size_t index = 0;

    uint32_t te_s = subghz_protocol_chevrolet_v0_const.te_short;
    uint32_t te_l = subghz_protocol_chevrolet_v0_const.te_long;

    // Manual Manchester encoder matched to the decoder's inverted level mapping.
    // The decoder uses: level=true->ShortLow, level=false->ShortHigh, with !data inversion.
    //
    // Decoder transition rules (after !data):
    //   From Mid1: bit 1 -> L:long -> Mid0
    //              bit 0 -> L:short, H:short -> Mid1
    //   From Mid0: bit 0 -> H:long -> Mid1
    //              bit 1 -> H:short, L:short -> Mid0
    //
    // Initial half-bit: H:short (gets "wasted" by decoder, resets Mid1->Mid1)

    // 96-bit payload: data(64) = bytes 0-7, seed(32) = bytes 8-11
    uint64_t data = instance->generic.data;
    uint32_t seed = instance->generic.seed;

    // Initial startup half-bit (HIGH, te_short) — decoder will discard this
    instance->encoder.upload[index++] = level_duration_make(true, te_s);

    // Encoder state: tracks decoder's internal Manchester state
    // After the initial H:short, decoder resets to Mid1
    bool in_mid1 = true; // true=Mid1, false=Mid0

    int total_bits = CHEVROLET_V0_PREAMBLE_BITS + CHEVROLET_V0_DATA_BITS;
    for(int i = 0; i < total_bits; i++) {
        bool bit;
        if(i < CHEVROLET_V0_PREAMBLE_BITS - 2) {
            // Alternating preamble: 1,0,1,0,... (42 bits)
            bit = (i % 2 == 0);
        } else if(i < CHEVROLET_V0_PREAMBLE_BITS) {
            // Sync: two 1-bits to create 0xAAAAAAAB pattern
            bit = true;
        } else {
            // Data: 96 bits MSB first — data(64) then seed(32)
            int data_idx = i - CHEVROLET_V0_PREAMBLE_BITS;
            if(data_idx < 64) {
                bit = (data >> (63 - data_idx)) & 1;
            } else {
                bit = (seed >> (31 - (data_idx - 64))) & 1;
            }
        }

        if(in_mid1) {
            if(bit) {
                // Mid1, bit 1: L:long -> Mid0
                instance->encoder.upload[index++] = level_duration_make(false, te_l);
                in_mid1 = false;
            } else {
                // Mid1, bit 0: L:short, H:short -> Mid1
                instance->encoder.upload[index++] = level_duration_make(false, te_s);
                instance->encoder.upload[index++] = level_duration_make(true, te_s);
            }
        } else {
            if(bit) {
                // Mid0, bit 1: H:short, L:short -> Mid0
                instance->encoder.upload[index++] = level_duration_make(true, te_s);
                instance->encoder.upload[index++] = level_duration_make(false, te_s);
            } else {
                // Mid0, bit 0: H:long -> Mid1
                instance->encoder.upload[index++] = level_duration_make(true, te_l);
                in_mid1 = true;
            }
        }
    }

    // Gap to terminate frame — use opposite level from last entry to prevent merging
    bool last_level = level_duration_get_level(instance->encoder.upload[index - 1]);
    instance->encoder.upload[index++] = level_duration_make(!last_level, te_l * 40);

    instance->encoder.size_upload = index;
    instance->encoder.front = 0;
}

// ============================================================================
// ENCODER - DESERIALIZE
// ============================================================================

SubGhzProtocolStatus subghz_protocol_encoder_chevrolet_v0_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderChevroletV0* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;

    do {
        if(subghz_block_generic_deserialize(&instance->generic, flipper_format) !=
           SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "EGF");
            break;
        }

        if(instance->generic.data_count_bit != CHEVROLET_V0_BIT_COUNT) {
            FURI_LOG_E(
                TAG,
                "Invalid bit count: %lu",
                (unsigned long)instance->generic.data_count_bit);
            res = SubGhzProtocolStatusErrorValueBitCount;
            break;
        }

        if(!flipper_format_rewind(flipper_format)) {
            FURI_LOG_E(TAG, "ERE");
            break;
        }

        // Read Key (bytes 0-7)
        uint8_t key_data[sizeof(uint64_t)] = {0};
        if(!flipper_format_read_hex(flipper_format, "Key", key_data, sizeof(uint64_t))) {
            FURI_LOG_E(TAG, "EMK");
            break;
        }

        instance->generic.data = 0;
        for(uint8_t i = 0; i < sizeof(uint64_t); i++) {
            instance->generic.data = instance->generic.data << 8 | key_data[i];
        }

        // Read Seed (bytes 8-11)
        if(!flipper_format_rewind(flipper_format)) {
            FURI_LOG_E(TAG, "ERE2");
            break;
        }
        uint8_t seed_data[4] = {0};
        if(flipper_format_read_hex(flipper_format, "Seed", seed_data, 4)) {
            instance->generic.seed =
                ((uint32_t)seed_data[0] << 24) | ((uint32_t)seed_data[1] << 16) |
                ((uint32_t)seed_data[2] << 8) | seed_data[3];
        }

        chevrolet_v0_extract_data(&instance->generic);

        // No counter advance — hop code is fully encrypted, replay as-is

        // Optional repeat
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        // Build upload
        subghz_protocol_encoder_chevrolet_v0_get_upload(instance);

        // Write back updated key
        if(!flipper_format_rewind(flipper_format)) {
            FURI_LOG_E(TAG, "ERE3");
            break;
        }
        for(size_t i = 0; i < sizeof(uint64_t); i++) {
            key_data[sizeof(uint64_t) - i - 1] = (instance->generic.data >> i * 8) & 0xFF;
        }
        if(!flipper_format_update_hex(flipper_format, "Key", key_data, sizeof(uint64_t))) {
            FURI_LOG_E(TAG, "EUUK");
            break;
        }

        instance->encoder.is_running = true;
        res = SubGhzProtocolStatusOk;

    } while(false);

    return res;
}

void subghz_protocol_encoder_chevrolet_v0_stop(void* context) {
    SubGhzProtocolEncoderChevroletV0* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_chevrolet_v0_yield(void* context) {
    SubGhzProtocolEncoderChevroletV0* instance = context;

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
