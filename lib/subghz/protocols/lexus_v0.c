/**
 * Lexus / Toyota Smart-Key SubGHz Protocol — 2-FSK Manchester rolling code
 *
 * Physical: 2-FSK @ ~434.39 MHz, FuriHalSubGhzPreset2FSKDev476Async
 * Manchester: te_short (half-bit) ~350us, te_long (full bit) ~700us.
 *   (FSK-demodulated baseband: marks read ~305us / spaces ~395us short,
 *    ~655us / ~750us long; a single 500us split cleanly separates short/long.)
 *
 * Capture characteristics: the FSK discriminator emits continuous noise between
 * bursts, so most of the recording is garbage. The real packet is found by a
 * NARROW preamble gate: >=16 consecutive pulses in [250,470]us (the clean
 * alternating preamble), after which the first LONG pulse triggers data.
 *
 * Frame (224 Manchester bits collected after the trigger LONG):
 *   bit   0.. 31 : Header / preamble-tail (phase-jittered: FF00FF30 / 80FF00CE
 *                  / FF00FF33 / 80FF00CD across captures) — DISCARDED.
 *   bit  32..191 : DATA      — 160-bit rolling code (encrypted).
 *   bit 192..223 : SERIAL    — 32-bit keyfob id (e.g. 0x353B58EA).
 *
 * Toyota complement signature: each rolling code is transmitted as a frame and
 * its bitwise complement on alternating frames, so the serial reads either
 * 0x353B58EA (data frame) or its complement 0xCAC4A715 (complement frame).
 * Each (data, complement) pair is repeated ~4-5x per button press.
 *
 * The 160-bit payload is a fully encrypted rolling code — no cleartext button
 * or counter is separable — so only Serial + Hop are reported (decode-only dev).
 *
 * Source signals: Lexus 350H (2020) keyfob, Lock/Unlock/Trunk captures.
 */

#include "lexus_v0.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include <furi.h>
#include <furi_hal.h>

#define TAG "SubGhzProtocolLexusV0"

// Timing bands (us)
#define LEXUS_V0_PRE_LO     250 // preamble short: lower bound
#define LEXUS_V0_PRE_HI     470 // preamble short: upper bound
#define LEXUS_V0_LONG_LO    500 // full-bit (LONG): lower bound
#define LEXUS_V0_LONG_HI    980 // full-bit (LONG): upper bound
#define LEXUS_V0_GAP        980 // >= this ends a burst
#define LEXUS_V0_MIN_PRE    16  // minimum clean preamble pulses before trigger

// Frame layout
#define LEXUS_V0_HEADER_BITS 32
#define LEXUS_V0_DATA_BITS   160
#define LEXUS_V0_SERIAL_BITS 32
#define LEXUS_V0_FRAME_BITS \
    (LEXUS_V0_HEADER_BITS + LEXUS_V0_DATA_BITS + LEXUS_V0_SERIAL_BITS) // 224
#define LEXUS_V0_PAYLOAD_BITS (LEXUS_V0_DATA_BITS + LEXUS_V0_SERIAL_BITS) // 192
#define LEXUS_V0_DATA_BYTES   (LEXUS_V0_DATA_BITS / 8) // 20

static const SubGhzBlockConst subghz_protocol_lexus_v0_const __attribute__((unused)) = {
    .te_short = 350,
    .te_long = 700,
    .te_delta = 150,
    .min_count_bit_for_found = LEXUS_V0_PAYLOAD_BITS,
};

// ============================================================================
// DATA STRUCTURES
// ============================================================================

typedef enum {
    LexusV0StepReset = 0,
    LexusV0StepPreamble,
    LexusV0StepData,
} LexusV0Step;

struct SubGhzProtocolDecoderLexusV0 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    LexusV0Step step;
    uint16_t preamble_count;
    bool half_bit;
    uint8_t all_bits[LEXUS_V0_FRAME_BITS + 8];
    uint16_t bit_count;

    uint32_t serial;                       // bits 192..223
    uint8_t rolling[LEXUS_V0_DATA_BYTES];   // bits 32..191 (160-bit rolling code)
};

// ============================================================================
// PROTOCOL DEFINITION
// ============================================================================

static SubGhzBlockGeneric* subghz_protocol_decoder_lexus_v0_get_generic(void* context) {
    SubGhzProtocolDecoderLexusV0* instance = context;
    return &instance->generic;
}

const SubGhzProtocolDecoder subghz_protocol_lexus_v0_decoder = {
    .alloc = subghz_protocol_decoder_lexus_v0_alloc,
    .free = subghz_protocol_decoder_lexus_v0_free,
    .feed = subghz_protocol_decoder_lexus_v0_feed,
    .reset = subghz_protocol_decoder_lexus_v0_reset,
    .get_hash_data = subghz_protocol_decoder_lexus_v0_get_hash_data,
    .serialize = subghz_protocol_decoder_lexus_v0_serialize,
    .deserialize = subghz_protocol_decoder_lexus_v0_deserialize,
    .get_string = subghz_protocol_decoder_lexus_v0_get_string,
    .get_generic = subghz_protocol_decoder_lexus_v0_get_generic,
};

const SubGhzProtocol subghz_protocol_lexus_v0 = {
    .name = SUBGHZ_PROTOCOL_LEXUS_V0_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_lexus_v0_decoder,
    .encoder = NULL,
};

// ============================================================================
// DECODER
// ============================================================================

void* subghz_protocol_decoder_lexus_v0_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderLexusV0* instance = malloc(sizeof(SubGhzProtocolDecoderLexusV0));
    instance->base.protocol = &subghz_protocol_lexus_v0;
    instance->generic.protocol_name = instance->base.protocol->name;
    subghz_protocol_decoder_lexus_v0_reset(instance);
    return instance;
}

void subghz_protocol_decoder_lexus_v0_free(void* context) {
    furi_assert(context);
    free(context);
}

void subghz_protocol_decoder_lexus_v0_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderLexusV0* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->step = LexusV0StepReset;
    instance->preamble_count = 0;
    instance->half_bit = false;
    instance->bit_count = 0;
    instance->serial = 0;
}

/**
 * Validate and emit a completed 224-bit frame.
 * Extracts the 160-bit rolling code (bits 32..191) and 32-bit serial (192..223),
 * dropping the phase-jittered 32-bit header. Degenerate (all-0 / all-1) serials
 * are noise from a false trigger and are rejected.
 */
static void lexus_v0_process_frame(SubGhzProtocolDecoderLexusV0* instance) {
    if(instance->bit_count < LEXUS_V0_FRAME_BITS) return;

    // Serial: bits 192..223
    uint32_t serial = 0;
    for(int i = 0; i < LEXUS_V0_SERIAL_BITS; i++) {
        serial = (serial << 1) | instance->all_bits[LEXUS_V0_HEADER_BITS + LEXUS_V0_DATA_BITS + i];
    }
    // Reject obvious noise frames (false trigger inside FSK noise)
    if(serial == 0x00000000u || serial == 0xFFFFFFFFu) {
        FURI_LOG_D(TAG, "Rejected degenerate serial %08lX", (unsigned long)serial);
        return;
    }
    instance->serial = serial;

    // Rolling code: bits 32..191 -> 20 bytes, MSB first
    for(int b = 0; b < LEXUS_V0_DATA_BYTES; b++) {
        uint8_t v = 0;
        for(int k = 0; k < 8; k++) {
            v = (v << 1) | instance->all_bits[LEXUS_V0_HEADER_BITS + b * 8 + k];
        }
        instance->rolling[b] = v;
    }

    // generic.data = first 64 bits of the rolling code (for hashing / dedup)
    uint64_t data = 0;
    for(int b = 0; b < 8; b++) {
        data = (data << 8) | instance->rolling[b];
    }
    instance->generic.data = data;
    instance->generic.data_count_bit = LEXUS_V0_PAYLOAD_BITS;
    instance->generic.serial = serial;
    instance->generic.btn = 0;
    instance->generic.cnt = 0;
    instance->decoder.decode_data = data;
    instance->decoder.decode_count_bit = LEXUS_V0_PAYLOAD_BITS;

    FURI_LOG_I(TAG, "Decoded Sn:%08lX rolling %02X%02X%02X%02X...",
        (unsigned long)serial, instance->rolling[0], instance->rolling[1],
        instance->rolling[2], instance->rolling[3]);

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

static void lexus_v0_restart(SubGhzProtocolDecoderLexusV0* instance) {
    instance->step = LexusV0StepReset;
    instance->preamble_count = 0;
    instance->half_bit = false;
    instance->bit_count = 0;
}

void subghz_protocol_decoder_lexus_v0_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderLexusV0* instance = context;

    bool in_pre = (duration >= LEXUS_V0_PRE_LO && duration <= LEXUS_V0_PRE_HI);
    bool is_long = (duration >= LEXUS_V0_LONG_LO && duration <= LEXUS_V0_LONG_HI);
    bool is_short = (duration < LEXUS_V0_LONG_LO);

    switch(instance->step) {
    case LexusV0StepReset:
        if(in_pre) {
            instance->preamble_count = 1;
            instance->step = LexusV0StepPreamble;
        }
        break;

    case LexusV0StepPreamble:
        if(in_pre) {
            instance->preamble_count++;
        } else if(is_long && instance->preamble_count >= LEXUS_V0_MIN_PRE) {
            // First LONG after a clean preamble = first data bit (value = level)
            instance->step = LexusV0StepData;
            instance->bit_count = 0;
            instance->half_bit = false;
            instance->all_bits[instance->bit_count++] = level ? 1 : 0;
        } else {
            lexus_v0_restart(instance);
        }
        break;

    case LexusV0StepData:
        if(duration >= LEXUS_V0_GAP) {
            // burst ended -> emit what we have, restart
            lexus_v0_process_frame(instance);
            lexus_v0_restart(instance);
            break;
        }
        if(is_long) {
            if(instance->half_bit) {
                // Manchester violation -> frame boundary
                lexus_v0_process_frame(instance);
                lexus_v0_restart(instance);
            } else {
                instance->all_bits[instance->bit_count++] = level ? 1 : 0;
            }
        } else if(is_short) {
            if(instance->half_bit) {
                instance->all_bits[instance->bit_count++] = level ? 1 : 0;
                instance->half_bit = false;
            } else {
                instance->half_bit = true;
            }
        }

        if(instance->bit_count >= LEXUS_V0_FRAME_BITS) {
            lexus_v0_process_frame(instance);
            lexus_v0_restart(instance);
        }
        break;

    default:
        lexus_v0_restart(instance);
        break;
    }
}

// ============================================================================
// HASH
// ============================================================================

uint8_t subghz_protocol_decoder_lexus_v0_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderLexusV0* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

// ============================================================================
// SERIALIZATION
// ============================================================================

SubGhzProtocolStatus subghz_protocol_decoder_lexus_v0_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderLexusV0* instance = context;

    SubGhzProtocolStatus res =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(res != SubGhzProtocolStatusOk) return res;

    // Store the full 160-bit rolling code (beyond generic.data's 64 bits)
    if(!flipper_format_write_hex(flipper_format, "Rolling", instance->rolling, LEXUS_V0_DATA_BYTES))
        return SubGhzProtocolStatusErrorParserOthers;

    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_lexus_v0_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderLexusV0* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize(&instance->generic, flipper_format);
    if(ret != SubGhzProtocolStatusOk) return ret;

    flipper_format_read_hex(flipper_format, "Rolling", instance->rolling, LEXUS_V0_DATA_BYTES);
    instance->serial = instance->generic.serial;
    instance->generic.protocol_name = instance->base.protocol->name;
    return SubGhzProtocolStatusOk;
}

// ============================================================================
// DISPLAY
// ============================================================================

void subghz_protocol_decoder_lexus_v0_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderLexusV0* instance = context;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Sn:%08lX\r\n"
        "Hop:",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long)instance->serial);

    for(int b = 0; b < LEXUS_V0_DATA_BYTES; b++) {
        furi_string_cat_printf(output, "%02X", instance->rolling[b]);
    }
}
