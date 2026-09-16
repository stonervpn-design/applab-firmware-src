#include "six_sc_two.h"
#include <lib/toolbox/manchester_decoder.h>

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

/*
 * 6SC2 Car Remote
 *
 * Ported from the rtl_433 decoder src/devices/6sc2.c
 * (Copyright (C) 2024 Ethan Halsall, GPLv2+; rtl_433 PR #2764, protocol 282).
 * Chip marking "6SC2" (MSOP-10); used by Nutek-branded aftermarket car remotes
 * (models 6SC2 CMGU / 6SC2 CDFA). Vendor listed "Unknown" (NOT Microchip).
 *
 * RF profile:
 *   - Carrier 315.1 MHz, OOK/AM
 *   - Modulation: Manchester (rtl_433 OOK_PULSE_MANCHESTER_ZEROBIT, IEEE 802.3
 *     biphase). Half-bit TE (short_width) = 250us => full bit period = 500us.
 *   - Inter-frame reset gap ~10000us.
 *   - Frame: 88 bits = 16-bit preamble/sync (received 0x55,0x54) + 72-bit
 *     (9-byte) payload. Frame is repeated; rtl_433 requires >=48 bits, >=1 rep.
 *
 * On-air is LSB-first; the decoder collects bits MSB-first (matching rtl_433's
 * bitbuffer) so the preamble reads 0x5554 and each PAYLOAD byte must be
 * reverse8()'d to recover its logical value.
 *
 * Payload byte layout (buf[0..8] == rtl_433 bytes[2..10]):
 *   buf[0..3] = 32-bit encrypted (rolling)
 *   buf[4]    = button (low nibble) + unknown (high nibble)
 *   buf[5..6] = 16-bit plaintext sequence counter
 *   buf[7]    = unknown/unused
 *   buf[8]    = 8-bit XOR checksum
 *
 * Validation (replicating rtl_433 exactly):
 *   1) preamble gate: received 0x5554 sync (16-bit shift register).
 *   2) MIC: XOR of the 9 payload bytes (buf[0]^...^buf[8]) == 0.
 *   3) extra false-positive guard: button nibble in {1,2,3,4}.
 *
 * Decode-only: the 32-bit cipher is unknown, so future codes cannot be forged
 * and raw replay would desync RX. No encoder is wired (all-NULL, static type).
 */

#define TAG "SubGhzProtocol6SC2"

#define SIX_SC_TWO_PREAMBLE 0x5554
#define SIX_SC_TWO_PAYLOAD_BITS 72
#define SIX_SC_TWO_PAYLOAD_BYTES 9
#define SIX_SC_TWO_BIT_COUNT 56 // stored payload: encrypted(32)+btn/unk(8)+seq(16)

static const SubGhzBlockConst subghz_protocol_six_sc_two_const = {
    .te_short = 250,
    .te_long = 500,
    .te_delta = 120,
    .min_count_bit_for_found = SIX_SC_TWO_BIT_COUNT,
};

typedef enum {
    SixScTwoDecoderStepReset = 0,
    SixScTwoDecoderStepSync,
    SixScTwoDecoderStepCollect,
} SixScTwoDecoderStep;

struct SubGhzProtocolDecoderSixScTwo {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    ManchesterState manchester_state;
    uint32_t sync_reg; // MSB-first shift register for preamble detection
    uint16_t sync_bits; // Manchester bits seen in sync phase
    uint8_t buf[SIX_SC_TWO_PAYLOAD_BYTES]; // 72-bit payload, MSB-first
    uint16_t data_bits; // payload bits collected
};

typedef struct SubGhzProtocolDecoderSixScTwo SubGhzProtocolDecoderSixScTwo;

// ============================================================================
// Forward declarations
// ============================================================================

static void* subghz_protocol_decoder_six_sc_two_alloc(SubGhzEnvironment* environment);
static void subghz_protocol_decoder_six_sc_two_free(void* context);
static void subghz_protocol_decoder_six_sc_two_reset(void* context);
static void subghz_protocol_decoder_six_sc_two_feed(void* context, bool level, uint32_t duration);
static uint8_t subghz_protocol_decoder_six_sc_two_get_hash_data(void* context);
static SubGhzProtocolStatus subghz_protocol_decoder_six_sc_two_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
static SubGhzProtocolStatus
    subghz_protocol_decoder_six_sc_two_deserialize(void* context, FlipperFormat* flipper_format);
static void subghz_protocol_decoder_six_sc_two_get_string(void* context, FuriString* output);

// ============================================================================
// Protocol registration
// ============================================================================

const SubGhzProtocolDecoder subghz_protocol_six_sc_two_decoder = {
    .alloc = subghz_protocol_decoder_six_sc_two_alloc,
    .free = subghz_protocol_decoder_six_sc_two_free,
    .feed = subghz_protocol_decoder_six_sc_two_feed,
    .reset = subghz_protocol_decoder_six_sc_two_reset,
    .get_hash_data = subghz_protocol_decoder_six_sc_two_get_hash_data,
    .serialize = subghz_protocol_decoder_six_sc_two_serialize,
    .deserialize = subghz_protocol_decoder_six_sc_two_deserialize,
    .get_string = subghz_protocol_decoder_six_sc_two_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_six_sc_two_encoder = {
    // Decode-only: unknown 32-bit rolling cipher, no TX. NULL ops, no stubs.
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_six_sc_two = {
    .name = SIX_SC_TWO_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_six_sc_two_decoder,
    .encoder = &subghz_protocol_six_sc_two_encoder,
};

// ============================================================================
// Helpers
// ============================================================================

// Bit-reflect a byte (not present in this tree's bit_lib).
static uint8_t reverse8(uint8_t b) {
    b = (uint8_t)((b & 0xF0) >> 4) | (uint8_t)((b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2) | (uint8_t)((b & 0x33) << 2);
    b = (uint8_t)((b & 0xAA) >> 1) | (uint8_t)((b & 0x55) << 1);
    return b;
}

static const char* six_sc_two_button_name(uint8_t btn) {
    switch(btn) {
    case 0x1:
        return "Unlock";
    case 0x2:
        return "Lock";
    case 0x3:
        return "Trunk";
    case 0x4:
        return "Panic";
    default:
        return "?";
    }
}

// ============================================================================
// Process a completed 72-bit payload
// ============================================================================

static void subghz_protocol_decoder_six_sc_two_process(SubGhzProtocolDecoderSixScTwo* instance) {
    // (2) MIC: XOR of the 9 payload bytes must be zero (last byte is checksum).
    // XOR-to-zero is invariant under per-byte bit reflection -> compute on raw.
    uint8_t xor_check = 0;
    for(uint8_t i = 0; i < SIX_SC_TWO_PAYLOAD_BYTES; i++) {
        xor_check ^= instance->buf[i];
    }
    if(xor_check != 0) {
        return;
    }

    // Reflected assembly, little-endian, exactly as rtl_433.
    uint8_t btn_byte = reverse8(instance->buf[4]);
    uint8_t button = btn_byte & 0x0F;

    // (3) extra false-positive guard: button nibble in {1,2,3,4}.
    if(button < 0x1 || button > 0x4) {
        return;
    }

    uint32_t encrypted = ((uint32_t)reverse8(instance->buf[3]) << 24) |
                         ((uint32_t)reverse8(instance->buf[2]) << 16) |
                         ((uint32_t)reverse8(instance->buf[1]) << 8) |
                         (uint32_t)reverse8(instance->buf[0]);
    uint16_t sequence =
        (uint16_t)(((uint16_t)reverse8(instance->buf[6]) << 8) | reverse8(instance->buf[5]));

    uint64_t data = ((uint64_t)encrypted << 24) | ((uint64_t)btn_byte << 16) | (uint64_t)sequence;

    instance->generic.data = data;
    instance->generic.data_count_bit = SIX_SC_TWO_BIT_COUNT;
    instance->generic.serial = encrypted;
    instance->generic.cnt = sequence;
    instance->generic.btn = button;

    // Keep the block decoder view consistent for hashing.
    instance->decoder.decode_data = data;
    instance->decoder.decode_count_bit = SIX_SC_TWO_BIT_COUNT;

    FURI_LOG_D(TAG, "Frame: Enc:%08lX Btn:%X Seq:%04X", (unsigned long)encrypted, button, sequence);

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

// ============================================================================
// Decoder: alloc / free / reset
// ============================================================================

static void* subghz_protocol_decoder_six_sc_two_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderSixScTwo* instance = malloc(sizeof(SubGhzProtocolDecoderSixScTwo));
    instance->base.protocol = &subghz_protocol_six_sc_two;
    instance->generic.protocol_name = subghz_protocol_six_sc_two.name;
    return instance;
}

static void subghz_protocol_decoder_six_sc_two_free(void* context) {
    furi_assert(context);
    free(context);
}

static void subghz_protocol_decoder_six_sc_two_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderSixScTwo* instance = context;
    instance->decoder.parser_step = SixScTwoDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->sync_reg = 0;
    instance->sync_bits = 0;
    instance->data_bits = 0;
    manchester_advance(
        instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
}

// ============================================================================
// Decoder: feed — Manchester state machine (mirrors chevrolet_v0_feed)
// ============================================================================

static void subghz_protocol_decoder_six_sc_two_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderSixScTwo* instance = context;

    ManchesterEvent event = ManchesterEventReset;

    if(DURATION_DIFF(duration, subghz_protocol_six_sc_two_const.te_short) <=
       subghz_protocol_six_sc_two_const.te_delta) {
        event = level ? ManchesterEventShortLow : ManchesterEventShortHigh;
    } else if(
        DURATION_DIFF(duration, subghz_protocol_six_sc_two_const.te_long) <=
        subghz_protocol_six_sc_two_const.te_delta) {
        event = level ? ManchesterEventLongLow : ManchesterEventLongHigh;
    }

    switch(instance->decoder.parser_step) {
    case SixScTwoDecoderStepReset:
        if(event != ManchesterEventReset) {
            manchester_advance(
                instance->manchester_state,
                ManchesterEventReset,
                &instance->manchester_state,
                NULL);
            instance->sync_reg = 0;
            instance->sync_bits = 0;
            instance->decoder.parser_step = SixScTwoDecoderStepSync;
        } else {
            break;
        }
        /* FALLTHROUGH */

    case SixScTwoDecoderStepSync:
        if(event == ManchesterEventReset) {
            instance->decoder.parser_step = SixScTwoDecoderStepReset;
            break;
        }
        {
            bool data;
            bool data_ok = manchester_advance(
                instance->manchester_state, event, &instance->manchester_state, &data);
            if(data_ok) {
                data = !data; // IEEE 802.3 convention (matches rtl_433 bitbuffer)
                instance->sync_reg = (instance->sync_reg << 1) | (data ? 1UL : 0UL);
                instance->sync_bits++;
                if(instance->sync_bits >= 16 &&
                   (instance->sync_reg & 0xFFFF) == SIX_SC_TWO_PREAMBLE) {
                    // Preamble found — begin collecting the 72-bit payload.
                    memset(instance->buf, 0, sizeof(instance->buf));
                    instance->data_bits = 0;
                    instance->decoder.parser_step = SixScTwoDecoderStepCollect;
                }
            }
        }
        break;

    case SixScTwoDecoderStepCollect:
        if(event == ManchesterEventReset) {
            instance->decoder.parser_step = SixScTwoDecoderStepReset;
            break;
        }
        {
            bool data;
            bool data_ok = manchester_advance(
                instance->manchester_state, event, &instance->manchester_state, &data);
            if(data_ok) {
                data = !data; // IEEE 802.3 convention

                uint16_t idx = instance->data_bits >> 3;
                instance->buf[idx] = (uint8_t)((instance->buf[idx] << 1) | (data ? 1 : 0));
                instance->data_bits++;

                if(instance->data_bits >= SIX_SC_TWO_PAYLOAD_BITS) {
                    subghz_protocol_decoder_six_sc_two_process(instance);
                    instance->decoder.parser_step = SixScTwoDecoderStepReset;
                }
            }
        }
        break;
    }
}

// ============================================================================
// Decoder: hash / serialize / deserialize / get_string
// ============================================================================

static uint8_t subghz_protocol_decoder_six_sc_two_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderSixScTwo* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

static SubGhzProtocolStatus subghz_protocol_decoder_six_sc_two_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderSixScTwo* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

static SubGhzProtocolStatus
    subghz_protocol_decoder_six_sc_two_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderSixScTwo* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, SIX_SC_TWO_BIT_COUNT);
}

static void subghz_protocol_decoder_six_sc_two_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderSixScTwo* instance = context;

    // Recompute fields from the stored payload so display is correct after a
    // load-from-file as well as after a live decode.
    uint64_t data = instance->generic.data;
    uint32_t encrypted = (uint32_t)(data >> 24);
    uint8_t button = (uint8_t)((data >> 16) & 0x0F);
    uint16_t sequence = (uint16_t)(data & 0xFFFF);

    // Push button context to global for roundtrip.
    subghz_block_generic_global.cnt_is_available = true;
    subghz_block_generic_global.btn_is_available = true;
    subghz_block_generic_global.current_btn = button;
    subghz_block_generic_global.btn_length_bit = 4;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Enc:%08lX\r\n"
        "Btn:%s Seq:%04X",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long)encrypted,
        six_sc_two_button_name(button),
        sequence);
}
