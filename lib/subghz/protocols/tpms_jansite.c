#include "tpms_jansite.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSJansite"

// Port of rtl_433/src/devices/tpms_jansite.c
// (Andreas Spiess and Christian W. Zuckschwerdt 2019, GPL-2+)
//
// Jansite Solar TPMS (Internal/External) Model TY02S.
//   Working frequency: 433.92 MHz +-38 kHz
//   Working temperature: -40 .. 125 C
//   Pressure range: 0 .. 350 kPa +-7 kPa
//
// FSK (FSK_PULSE_PCM in rtl_433, short=long=52 samples), Manchester encoded,
// 7 byte / 56 bit payload. In the LevelDuration domain this is the same
// short=52 / long=104 Manchester-by-duration model the other in-tree TPMS
// ports use (see tpms_ford.c): a same-bit run merges two 52us halves into a
// ~104us pulse, a bit change stays ~52us.
//
// Wire format (nibbles):  II II II IS PP TT CC   (7 bytes)
//   I = 28-bit sensor ID
//   S = 4-bit status (deflation alarm / battery low / etc. — bit mapping
//       is not publicly documented; rtl_433 exposes it only as a raw int)
//   P = 8-bit pressure, ~1.7 kPa per count
//   T = 8-bit temperature, degrees C offset by 50
//   C = 8-bit checksum
//
// Preamble: raw 0xaa 0xaa 0xa9 (rtl_433 searches this in the inverted buffer),
// i.e. non-inverted 0x55 0x55 0x56 -> the 24-bit pattern 0x555556 in the
// Manchester-sliced bit stream.
//
// CHECKSUM: unknown. rtl_433 ships this device with `.disabled = 1` and the
// comment "Unknown checksum"; its decode has `// TODO: validate checksum`.
// It was investigated here against the 29 real captures in the rtl_433_tests
// corpus (tests/Jansite_TPMS/01): the checksum byte is provably NOT a GF(2)
// affine function of the payload (bits 2..7 are inconsistent with any linear
// model), which rules out every CRC-8 (all polynomials/inits/xorouts, both
// bit orders), every LFSR digest (all generators/keys, all byte orderings),
// and XOR checksums; exhaustive modular-sum / signed-subset-sum / nibble-sum
// families also failed to match. The only reproducible relations (the two
// low bits) turned out to be keyed on the sensor-ID bits present in the small
// sample set, so using them as a gate would reject frames from unseen sensors.
//
// Because no correct checksum is known, validating against a fabricated one
// would reject 100% of legitimate sensors — strictly worse than rtl_433's own
// behaviour. Instead the false-positive gate mirrors rtl_433's actual sanity
// check, hardened for standalone use: the FULL 24-bit Jansite preamble must
// match (8 bits stronger than the 16-bit hunt the checksummed ports can afford)
// AND exactly 56 bits must Manchester-decode cleanly with no timing glitch
// (any bad half-bit -> ManchesterEventReset -> frame discarded). The raw
// checksum byte is retained in generic.data for future validation.

#define JANSITE_PREAMBLE      0x555556UL
#define JANSITE_PREAMBLE_MASK 0xFFFFFFUL
#define JANSITE_PREAMBLE_BITS 24
#define JANSITE_DATA_BITS     56

static const SubGhzBlockConst tpms_protocol_jansite_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = JANSITE_DATA_BITS,
};

typedef enum {
    JansiteDecoderStepReset = 0,
    JansiteDecoderStepFindPreamble,
    JansiteDecoderStepData,
} JansiteDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    ManchesterState manchester_saved_state;
    uint32_t header_shift;
} TPMSProtocolDecoderJansite;

const SubGhzProtocolDecoder tpms_protocol_jansite_decoder = {
    .alloc = tpms_protocol_decoder_jansite_alloc,
    .free = tpms_protocol_decoder_jansite_free,
    .feed = tpms_protocol_decoder_jansite_feed,
    .reset = tpms_protocol_decoder_jansite_reset,
    .get_hash_data = tpms_protocol_decoder_jansite_get_hash_data,
    .serialize = tpms_protocol_decoder_jansite_serialize,
    .deserialize = tpms_protocol_decoder_jansite_deserialize,
    .get_string = tpms_protocol_decoder_jansite_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_jansite_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_jansite = {
    .name = TPMS_PROTOCOL_JANSITE_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,

    .decoder = &tpms_protocol_jansite_decoder,
    .encoder = &tpms_protocol_jansite_encoder,
};

void* tpms_protocol_decoder_jansite_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderJansite* instance = malloc(sizeof(TPMSProtocolDecoderJansite));
    instance->base.protocol = &tpms_protocol_jansite;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_jansite_free(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderJansite* instance = context;
    free(instance);
}

void tpms_protocol_decoder_jansite_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderJansite* instance = context;
    instance->decoder.parser_step = JansiteDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->header_shift = 0;
}

static ManchesterEvent jansite_level_and_duration_to_event(bool level, uint32_t duration) {
    bool is_long = false;
    if(DURATION_DIFF(duration, tpms_protocol_jansite_const.te_long) <
       tpms_protocol_jansite_const.te_delta * 2) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_jansite_const.te_short) <
        tpms_protocol_jansite_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }
    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

static void tpms_protocol_jansite_analyze(TPMSBlockGeneric* g) {
    uint8_t b[7];
    for(int i = 0; i < 7; i++) {
        b[i] = (g->data >> (8 * (6 - i))) & 0xFF;
    }

    // 28-bit ID: II II II I(S) — the low nibble of b[3] is the status field.
    g->id = ((uint32_t)b[0] << 20) | ((uint32_t)b[1] << 12) | ((uint32_t)b[2] << 4) |
            ((uint32_t)b[3] >> 4);

    // Pressure: raw * 1.7 kPa, stored in bar (kPa / 100).
    g->pressure = (float)b[4] * 1.7f * 0.01f;

    // Temperature: raw byte offset by 50 degrees C.
    g->temperature = (float)((int)b[5] - 50);

    // Status nibble b[3] & 0x0F carries "deflation alarm, battery low, etc."
    // but the per-bit mapping is not publicly documented, so the battery flag
    // is reported as not-available rather than guessed (b[6] is the checksum).
    g->battery_low = TPMS_NO_BATT;
}

void tpms_protocol_decoder_jansite_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderJansite* instance = context;

    ManchesterEvent event = jansite_level_and_duration_to_event(level, duration);
    if(event == ManchesterEventReset) {
        instance->decoder.parser_step = JansiteDecoderStepReset;
        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;
        instance->header_shift = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        return;
    }

    bool bit = false;
    bool have_bit = manchester_advance(
        instance->manchester_saved_state, event, &instance->manchester_saved_state, &bit);
    if(!have_bit) return;

    switch(instance->decoder.parser_step) {
    case JansiteDecoderStepReset:
        instance->header_shift = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        instance->decoder.parser_step = JansiteDecoderStepFindPreamble;
        // fall through to consume this bit as preamble candidate
        /* fallthrough */
    case JansiteDecoderStepFindPreamble:
        instance->header_shift =
            ((instance->header_shift << 1) | (bit ? 1 : 0)) & JANSITE_PREAMBLE_MASK;
        if(instance->header_shift == JANSITE_PREAMBLE) {
            instance->decoder.parser_step = JansiteDecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
        }
        break;

    case JansiteDecoderStepData:
        subghz_protocol_blocks_add_bit(&instance->decoder, bit ? 1 : 0);
        if(instance->decoder.decode_count_bit ==
           tpms_protocol_jansite_const.min_count_bit_for_found) {
            // A full 56-bit frame decoded cleanly after the exact 24-bit
            // preamble. The Jansite checksum algorithm is unknown (see the
            // header comment), so no checksum reject is applied here; the
            // 24-bit preamble + glitch-free Manchester decode is the gate.
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            tpms_protocol_jansite_analyze(&instance->generic);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
            instance->decoder.parser_step = JansiteDecoderStepReset;
        }
        break;
    }
}

uint8_t tpms_protocol_decoder_jansite_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderJansite* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_jansite_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderJansite* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_jansite_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderJansite* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_jansite_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_jansite_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderJansite* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%07lX Batt:%s\r\n"
        "Temp:%2.0f C Bar:%2.1f",
        instance->generic.protocol_name,
        instance->generic.id,
        (instance->generic.battery_low == TPMS_NO_BATT) ?
            "n/a" :
            (instance->generic.battery_low ? "low" : "ok"),
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}
