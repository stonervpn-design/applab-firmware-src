#include "tpms_imars_t240.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSiMarsT240"

// Port of rtl_433/src/devices/tpms_imars_t240.c (GPL-2+)
//
// iMars T240 aftermarket TPMS sensor.
// OOK, Manchester encoded, ~50 us half-bit, 8 byte payload (64 bits).
// 433.92 MHz (some clones also on 315 MHz).
//
// Wire format (raw bitbuffer): a 32-bit alternating preamble 0xaaaaaaaa
// followed by 128 raw chips that Manchester-decode to 8 bytes B0..B7.
// rtl_433 hunts the 24-bit pattern 0xaaaaaa, then Manchester-decodes 64 bits
// starting 32 chips after the match.
//
// Decoded byte layout (after Manchester-decoding), 8 bytes / 64 bits:
//   B0 B1 B2 B3 B4 B5 B6 B7
//   B0        = message head / status; low nibble mirrors B1 low nibble
//   B1        = message head; (B0 & 0x0f) == (B1 & 0x0f) must hold
//   B2        = candidate temperature/pressure (mapping undetermined upstream)
//   B3,B4     = checksum bytes: (B3 + B4) & 0xff must equal 0x41 or 0x3c
//   B5,B6     = candidate temperature/pressure (mapping undetermined upstream)
//   B7        = tail, must repeat B0 (B7 == B0)
//
// Integrity (matches rtl_433 exactly, rejects false positives):
//   B7 == B0
//   (B0 & 0x0f) == (B1 & 0x0f)
//   ((B3 + B4) & 0xff) == 0x41  OR  == 0x3c
//
// NOTE: rtl_433 states the mapping of B2/B5/B6 to temperature and pressure has
// not been determined (no ground-truth calibration). We therefore surface the
// sensor ID (B0..B3) and gate on the checksum, but leave pressure/temperature
// unset rather than fabricate a scale/offset.
//
// The iMars preamble is a pure alternating 0xaa run with NO distinct sync word,
// so (unlike the Ford/Citroen ports) there is no unique bit pattern marking the
// preamble->data boundary. We arm on a run of decoded preamble bits, then slide
// a rolling 64-bit window and accept the first alignment that passes the triple
// integrity check. The check is strong enough (>= 19 constrained bits) that a
// bad alignment cannot pass.

#define IMARS_DATA_BITS         64
#define IMARS_PREAMBLE_MIN_ONES 8U // consecutive decoded preamble bits to arm
#define IMARS_SLIDE_BUDGET      64U // extra bits to slide while re-aligning data

static const SubGhzBlockConst tpms_protocol_imars_t240_const = {
    .te_short = 50,
    .te_long = 100,
    .te_delta = 20,
    .min_count_bit_for_found = IMARS_DATA_BITS,
};

typedef enum {
    IMarsT240DecoderStepReset = 0,
    IMarsT240DecoderStepFindPreamble,
    IMarsT240DecoderStepData,
} IMarsT240DecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    ManchesterState manchester_saved_state;
    uint16_t header_shift; // reused as a consecutive-preamble-ones counter
} TPMSProtocolDecoderIMarsT240;

const SubGhzProtocolDecoder tpms_protocol_imars_t240_decoder = {
    .alloc = tpms_protocol_decoder_imars_t240_alloc,
    .free = tpms_protocol_decoder_imars_t240_free,
    .feed = tpms_protocol_decoder_imars_t240_feed,
    .reset = tpms_protocol_decoder_imars_t240_reset,
    .get_hash_data = tpms_protocol_decoder_imars_t240_get_hash_data,
    .serialize = tpms_protocol_decoder_imars_t240_serialize,
    .deserialize = tpms_protocol_decoder_imars_t240_deserialize,
    .get_string = tpms_protocol_decoder_imars_t240_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_imars_t240_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_imars_t240 = {
    .name = TPMS_PROTOCOL_IMARS_T240_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,

    .decoder = &tpms_protocol_imars_t240_decoder,
    .encoder = &tpms_protocol_imars_t240_encoder,
};

void* tpms_protocol_decoder_imars_t240_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderIMarsT240* instance = malloc(sizeof(TPMSProtocolDecoderIMarsT240));
    instance->base.protocol = &tpms_protocol_imars_t240;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_imars_t240_free(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderIMarsT240* instance = context;
    free(instance);
}

void tpms_protocol_decoder_imars_t240_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderIMarsT240* instance = context;
    instance->decoder.parser_step = IMarsT240DecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->header_shift = 0;
}

static ManchesterEvent imars_t240_evt(bool level, uint32_t duration) {
    bool is_long = false;
    if(DURATION_DIFF(duration, tpms_protocol_imars_t240_const.te_long) <
       tpms_protocol_imars_t240_const.te_delta * 2) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_imars_t240_const.te_short) <
        tpms_protocol_imars_t240_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }
    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

static void imars_t240_get_bytes(TPMSProtocolDecoderIMarsT240* instance, uint8_t b[8]) {
    for(int i = 0; i < 8; i++) {
        b[i] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

static bool tpms_protocol_imars_t240_check_checksum(TPMSProtocolDecoderIMarsT240* instance) {
    uint8_t b[8];
    imars_t240_get_bytes(instance, b);
    // Tail repeats head.
    if(b[7] != b[0]) return false;
    // Head low nibbles must match.
    if((b[0] & 0x0f) != (b[1] & 0x0f)) return false;
    // Unit-specific checksum: (B3 + B4) & 0xff is one of two constants.
    uint8_t checksum = (uint8_t)(b[3] + b[4]);
    if(checksum != 0x41 && checksum != 0x3c) return false;
    return true;
}

static void tpms_protocol_imars_t240_analyze(TPMSBlockGeneric* g) {
    uint8_t b[8];
    for(int i = 0; i < 8; i++) {
        b[i] = (g->data >> (8 * (7 - i))) & 0xFF;
    }
    // rtl_433 reports "code" = B0..B6; we surface the first 32 bits as the id.
    g->id = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];

    // Temperature and pressure byte mapping is undetermined upstream (rtl_433),
    // so we do not fabricate a scale/offset here.
    g->pressure = 0.0f;
    g->temperature = 0.0f;
    g->battery_low = TPMS_NO_BATT;
}

void tpms_protocol_decoder_imars_t240_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderIMarsT240* instance = context;

    ManchesterEvent event = imars_t240_evt(level, duration);
    if(event == ManchesterEventReset) {
        instance->decoder.parser_step = IMarsT240DecoderStepReset;
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
    case IMarsT240DecoderStepReset:
        instance->header_shift = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        instance->decoder.parser_step = IMarsT240DecoderStepFindPreamble;
        /* fallthrough */
    case IMarsT240DecoderStepFindPreamble:
        // The alternating 0xaa preamble Manchester-decodes to a run of identical
        // bits. Count that run; once we are confidently inside the preamble, arm
        // the sliding data collector.
        if(bit) {
            if(instance->header_shift < 0xFFFF) instance->header_shift++;
        } else {
            instance->header_shift = 0;
        }
        if(instance->header_shift >= IMARS_PREAMBLE_MIN_ONES) {
            instance->decoder.parser_step = IMarsT240DecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
        }
        break;

    case IMarsT240DecoderStepData:
        // Rolling 64-bit window (decode_data keeps the most recent 64 bits).
        subghz_protocol_blocks_add_bit(&instance->decoder, bit ? 1 : 0);
        if(instance->decoder.decode_count_bit >=
           tpms_protocol_imars_t240_const.min_count_bit_for_found) {
            if(tpms_protocol_imars_t240_check_checksum(instance)) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = IMARS_DATA_BITS;
                // Stabilise the hash length regardless of how far we slid.
                instance->decoder.decode_count_bit = IMARS_DATA_BITS;
                tpms_protocol_imars_t240_analyze(&instance->generic);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
                instance->decoder.parser_step = IMarsT240DecoderStepReset;
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->header_shift = 0;
            } else if(
                instance->decoder.decode_count_bit >= IMARS_DATA_BITS + IMARS_SLIDE_BUDGET) {
                // Slid past the residual preamble without a match; re-hunt.
                instance->decoder.parser_step = IMarsT240DecoderStepReset;
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->header_shift = 0;
            }
            // Otherwise keep sliding: the window advances one bit and re-checks.
        }
        break;
    }
}

uint8_t tpms_protocol_decoder_imars_t240_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderIMarsT240* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_imars_t240_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderIMarsT240* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_imars_t240_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderIMarsT240* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_imars_t240_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_imars_t240_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderIMarsT240* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX Batt:%s\r\n"
        "Temp:%2.0f C Bar:%2.1f",
        instance->generic.protocol_name,
        instance->generic.id,
        (instance->generic.battery_low == TPMS_NO_BATT) ?
            "n/a" :
            (instance->generic.battery_low ? "low" : "ok"),
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}
