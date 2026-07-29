/**
 * Audi V2 SubGHz Protocol - 72-bit encrypted rolling code
 *
 * 72-bit protocol: [RollingCode:64][Trailer:8=0x6C]
 * - Rolling code: 64-bit, fully encrypted, no visible serial/button/counter
 * - Trailer: fixed 0x6C byte (used for frame validation)
 *
 * Physical layer: Manchester, ~280/560us, long preamble (200+ short pulses),
 * no sync/header pulse. Data starts on first long pulse after preamble.
 *
 * 433.92 MHz, OOK
 */

#include "audi_v2.h"

#include <lib/toolbox/manchester_decoder.h>
#include <lib/toolbox/manchester_encoder.h>

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include <furi.h>
#include <furi_hal.h>

#define TAG "SubGhzProtocolAudiV2"

#define AUDI_V2_DATA_BITS       72
#define AUDI_V2_MIN_PREAMBLE    40
#define AUDI_V2_TRAILER         0x93

static const SubGhzBlockConst subghz_protocol_audi_v2_const = {
    .te_short = 280,
    .te_long = 560,
    .te_delta = 100,
    .min_count_bit_for_found = AUDI_V2_DATA_BITS,
};

// ============================================================================
// DATA STRUCTURES
// ============================================================================

typedef enum {
    AudiV2DecoderStepReset = 0,
    AudiV2DecoderStepPreamble,
    AudiV2DecoderStepData,
} AudiV2DecoderStep;

struct SubGhzProtocolDecoderAudiV2 {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint16_t preamble_count;
    ManchesterState manchester_saved_state;

    uint8_t trailer;
};

struct SubGhzProtocolEncoderAudiV2 {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};

// ============================================================================
// PROTOCOL DEFINITION
// ============================================================================

static SubGhzBlockGeneric* subghz_protocol_decoder_audi_v2_get_generic(void* context) {
    SubGhzProtocolDecoderAudiV2* instance = context;
    return &instance->generic;
}

const SubGhzProtocolDecoder subghz_protocol_audi_v2_decoder = {
    .alloc = subghz_protocol_decoder_audi_v2_alloc,
    .free = subghz_protocol_decoder_audi_v2_free,

    .feed = subghz_protocol_decoder_audi_v2_feed,
    .reset = subghz_protocol_decoder_audi_v2_reset,

    .get_hash_data = subghz_protocol_decoder_audi_v2_get_hash_data,
    .serialize = subghz_protocol_decoder_audi_v2_serialize,
    .deserialize = subghz_protocol_decoder_audi_v2_deserialize,
    .get_string = subghz_protocol_decoder_audi_v2_get_string,
    .get_generic = subghz_protocol_decoder_audi_v2_get_generic,
};

const SubGhzProtocolEncoder subghz_protocol_audi_v2_encoder = {
    .alloc = subghz_protocol_encoder_audi_v2_alloc,
    .free = subghz_protocol_encoder_audi_v2_free,

    .deserialize = subghz_protocol_encoder_audi_v2_deserialize,
    .stop = subghz_protocol_encoder_audi_v2_stop,
    .yield = subghz_protocol_encoder_audi_v2_yield,
};

const SubGhzProtocol subghz_protocol_audi_v2 = {
    .name = SUBGHZ_PROTOCOL_AUDI_V2_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,

    .decoder = &subghz_protocol_audi_v2_decoder,
    .encoder = &subghz_protocol_audi_v2_encoder,
};

// ============================================================================
// DECODER IMPLEMENTATION
// ============================================================================

void* subghz_protocol_decoder_audi_v2_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderAudiV2* instance = malloc(sizeof(SubGhzProtocolDecoderAudiV2));
    instance->base.protocol = &subghz_protocol_audi_v2;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_audi_v2_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderAudiV2* instance = context;
    free(instance);
}

void subghz_protocol_decoder_audi_v2_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderAudiV2* instance = context;
    instance->decoder.parser_step = AudiV2DecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->preamble_count = 0;
    manchester_advance(
        instance->manchester_saved_state,
        ManchesterEventReset,
        &instance->manchester_saved_state,
        NULL);
}

/**
 * Complete signal processing when we have 72 bits.
 */
static void audi_v2_decoder_check_complete(SubGhzProtocolDecoderAudiV2* instance) {
    if(instance->decoder.decode_count_bit < AUDI_V2_DATA_BITS) return;

    // generic.data holds bits 0-63 (the rolling code, saved at bit 64)
    // decode_data holds bits 64-71 (the trailer byte)
    instance->trailer = instance->decoder.decode_data & 0xFF;

    // Validate trailer
    if(instance->trailer != AUDI_V2_TRAILER) {
        FURI_LOG_D(
            TAG,
            "Invalid trailer: 0x%02X (expected 0x%02X)",
            instance->trailer,
            AUDI_V2_TRAILER);
        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;
        instance->decoder.parser_step = AudiV2DecoderStepReset;
        return;
    }

    instance->generic.data_count_bit = AUDI_V2_DATA_BITS;

    uint32_t key_hi = (uint32_t)(instance->generic.data >> 32);
    uint32_t key_lo = (uint32_t)(instance->generic.data & 0xFFFFFFFF);

    FURI_LOG_I(
        TAG,
        "Decoded 72-bit: Key=%08lX%08lX Trailer=0x%02X",
        (unsigned long)key_hi,
        (unsigned long)key_lo,
        instance->trailer);

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }

    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->decoder.parser_step = AudiV2DecoderStepReset;
}

void subghz_protocol_decoder_audi_v2_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderAudiV2* instance = context;

    switch(instance->decoder.parser_step) {
    case AudiV2DecoderStepReset:
        // Any short pulse starts preamble counting
        if(DURATION_DIFF(duration, subghz_protocol_audi_v2_const.te_short) <=
           subghz_protocol_audi_v2_const.te_delta) {
            instance->preamble_count = 1;
            instance->decoder.parser_step = AudiV2DecoderStepPreamble;
        }
        break;

    case AudiV2DecoderStepPreamble:
        // Count short preamble pulses
        if(DURATION_DIFF(duration, subghz_protocol_audi_v2_const.te_short) <=
           subghz_protocol_audi_v2_const.te_delta) {
            instance->preamble_count++;
        } else if(
            DURATION_DIFF(duration, subghz_protocol_audi_v2_const.te_long) <=
                subghz_protocol_audi_v2_const.te_delta &&
            instance->preamble_count >= AUDI_V2_MIN_PREAMBLE) {
            // First long pulse after sufficient preamble = start of data
            instance->decoder.parser_step = AudiV2DecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->generic.data = 0;

            manchester_advance(
                instance->manchester_saved_state,
                ManchesterEventReset,
                &instance->manchester_saved_state,
                NULL);

            // Process trigger LONG as separator (advance state, don't count as data bit)
            ManchesterEvent event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
            bool data;
            manchester_advance(
                instance->manchester_saved_state, event, &instance->manchester_saved_state, &data);
        } else {
            instance->decoder.parser_step = AudiV2DecoderStepReset;
            instance->preamble_count = 0;
        }
        break;

    case AudiV2DecoderStepData: {
        ManchesterEvent event = ManchesterEventReset;

        if(DURATION_DIFF(duration, subghz_protocol_audi_v2_const.te_short) <=
           subghz_protocol_audi_v2_const.te_delta) {
            event = level ? ManchesterEventShortHigh : ManchesterEventShortLow;
        } else if(
            DURATION_DIFF(duration, subghz_protocol_audi_v2_const.te_long) <=
            subghz_protocol_audi_v2_const.te_delta) {
            event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
        }

        if(event != ManchesterEventReset) {
            bool data;
            bool data_ok = manchester_advance(
                instance->manchester_saved_state, event, &instance->manchester_saved_state, &data);

            if(data_ok) {
                instance->decoder.decode_data = (instance->decoder.decode_data << 1) | data;
                instance->decoder.decode_count_bit++;

                // At 64 bits, save key and reset decode_data for trailer
                if(instance->decoder.decode_count_bit == 64) {
                    instance->generic.data = instance->decoder.decode_data;
                    instance->decoder.decode_data = 0;
                }
            }
        }

        // Frame completed at 72 bits
        if(instance->decoder.decode_count_bit >= AUDI_V2_DATA_BITS) {
            audi_v2_decoder_check_complete(instance);
            break;
        }

        // Invalid timing ends data phase
        if(event == ManchesterEventReset) {
            if(instance->decoder.decode_count_bit >= 40) {
                FURI_LOG_D(
                    TAG,
                    "Partial frame: %lu bits (need 72)",
                    (unsigned long)instance->decoder.decode_count_bit);
            }
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->decoder.parser_step = AudiV2DecoderStepReset;
        }
    } break;

    default:
        instance->decoder.parser_step = AudiV2DecoderStepReset;
        break;
    }
}

uint8_t subghz_protocol_decoder_audi_v2_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderAudiV2* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

// ============================================================================
// SERIALIZATION
// ============================================================================

SubGhzProtocolStatus subghz_protocol_decoder_audi_v2_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderAudiV2* instance = context;

    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_audi_v2_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderAudiV2* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize(&instance->generic, flipper_format);

    if(ret != SubGhzProtocolStatusOk) {
        FURI_LOG_E(TAG, "Failed to deserialize base fields");
        return ret;
    }

    if(instance->generic.data_count_bit != AUDI_V2_DATA_BITS) {
        FURI_LOG_E(TAG, "Invalid bit count: %lu", (unsigned long)instance->generic.data_count_bit);
        return SubGhzProtocolStatusErrorValueBitCount;
    }

    instance->generic.protocol_name = instance->base.protocol->name;
    return SubGhzProtocolStatusOk;
}

// ============================================================================
// DISPLAY
// ============================================================================

void subghz_protocol_decoder_audi_v2_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderAudiV2* instance = context;

    uint32_t key_hi = (uint32_t)(instance->generic.data >> 32);
    uint32_t key_lo = (uint32_t)(instance->generic.data & 0xFFFFFFFF);

    furi_string_cat_printf(
        output,
        "%s 72bit\r\n"
        "Key:%08lX%08lX\r\n",
        instance->generic.protocol_name,
        (unsigned long)key_hi,
        (unsigned long)key_lo);
}

// ============================================================================
// ENCODER IMPLEMENTATION
// ============================================================================

void* subghz_protocol_encoder_audi_v2_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderAudiV2* instance = malloc(sizeof(SubGhzProtocolEncoderAudiV2));

    instance->base.protocol = &subghz_protocol_audi_v2;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = 3;
    // 100 preamble pairs (200) + trigger (1) + up to 2 entries/data-bit (~144) + end
    // gap (1) can reach ~346 entries; size the buffer above the worst case to avoid a
    // heap overflow while get_upload writes (size_upload is set to the real count below).
    instance->encoder.size_upload = 384;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_audi_v2_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderAudiV2* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

/**
 * Build one complete frame into the upload buffer.
 * Signal: [100x SHORT pairs (preamble)][TRIGGER LONG]
 *         [Manchester 72 data bits][END GAP]
 */
static void subghz_protocol_encoder_audi_v2_get_upload(SubGhzProtocolEncoderAudiV2* instance) {
    furi_assert(instance);
    size_t index = 0;
    const size_t enc_cap = 384;

    // PREAMBLE: 100 SHORT pairs (HIGH-LOW)
    for(int p = 0; p < 100; p++) {
        if(index < enc_cap)
            instance->encoder.upload[index++] =
                level_duration_make(true, (uint32_t)subghz_protocol_audi_v2_const.te_short);
        if(index < enc_cap)
            instance->encoder.upload[index++] =
                level_duration_make(false, (uint32_t)subghz_protocol_audi_v2_const.te_short);
    }

    // TRIGGER: LONG LOW (first long pulse transitions to data)
    if(index < enc_cap)
        instance->encoder.upload[index++] =
            level_duration_make(false, (uint32_t)subghz_protocol_audi_v2_const.te_long);

    // MANCHESTER DATA: 72 bits
    // [64 bits from generic.data] + [8 bits trailer = 0x6C]
    uint8_t manchester_state = ManchesterStateMid1;

    for(int i = 71; i >= 0; i--) {
        bool b;
        if(i >= 8) {
            // Bits 71..8: from generic.data (bits 63..0)
            b = (instance->generic.data >> (i - 8)) & 1;
        } else {
            // Bits 7..0: trailer byte 0x6C
            b = (AUDI_V2_TRAILER >> i) & 1;
        }

        switch(manchester_state) {
        case ManchesterStateMid1:
            if(b == 0) {
                if(index < enc_cap)
                    instance->encoder.upload[index++] =
                        level_duration_make(true, (uint32_t)subghz_protocol_audi_v2_const.te_long);
                manchester_state = ManchesterStateMid0;
            } else {
                if(index < enc_cap)
                    instance->encoder.upload[index++] =
                        level_duration_make(true, (uint32_t)subghz_protocol_audi_v2_const.te_short);
                if(index < enc_cap)
                    instance->encoder.upload[index++] =
                        level_duration_make(false, (uint32_t)subghz_protocol_audi_v2_const.te_short);
                manchester_state = ManchesterStateMid1;
            }
            break;
        case ManchesterStateMid0:
            if(b == 0) {
                if(index < enc_cap)
                    instance->encoder.upload[index++] =
                        level_duration_make(false, (uint32_t)subghz_protocol_audi_v2_const.te_short);
                if(index < enc_cap)
                    instance->encoder.upload[index++] =
                        level_duration_make(true, (uint32_t)subghz_protocol_audi_v2_const.te_short);
                manchester_state = ManchesterStateMid0;
            } else {
                if(index < enc_cap)
                    instance->encoder.upload[index++] =
                        level_duration_make(false, (uint32_t)subghz_protocol_audi_v2_const.te_long);
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
    if(index < enc_cap)
        instance->encoder.upload[index++] = level_duration_make(end_level, (uint32_t)6000);

    instance->encoder.size_upload = index;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_audi_v2_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderAudiV2* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        if(subghz_block_generic_deserialize(&instance->generic, flipper_format) !=
           SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }

        if(instance->generic.data_count_bit != AUDI_V2_DATA_BITS) {
            FURI_LOG_E(
                TAG, "Invalid bit count: %lu", (unsigned long)instance->generic.data_count_bit);
            res = SubGhzProtocolStatusErrorValueBitCount;
            break;
        }

        // Optional repeat parameter
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        // Build upload (simple replay, no roundtrip)
        subghz_protocol_encoder_audi_v2_get_upload(instance);

        instance->encoder.is_running = true;
        res = SubGhzProtocolStatusOk;
    } while(false);

    return res;
}

void subghz_protocol_encoder_audi_v2_stop(void* context) {
    SubGhzProtocolEncoderAudiV2* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_audi_v2_yield(void* context) {
    SubGhzProtocolEncoderAudiV2* instance = context;

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
