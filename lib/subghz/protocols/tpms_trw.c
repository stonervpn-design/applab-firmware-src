#include "tpms_trw.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSTrw"

// Port of rtl_433/src/devices/tpms_trw.c (Bruno OCTAU / rtl_433, GPL-2+)
//
// FSK, Manchester-encoded (MANCHESTER_ZEROBIT), 52 us half-bit, 11 byte payload.
// TRW OEM and Clone TPMS sensors. 315 MHz (US) / 433.92 MHz (EU).
// (rtl_433 also defines an OOK variant with preamble 0x0001; this port mirrors
//  the FSK variant used on 315/433 FM.)
//
// Wire format (after Manchester-decoding, 11 bytes = 88 bits):
//   MM II II II II FS PP TT NN CC OO
//   b[0]      = mode/model (0x5c, 0x5d, 0x5e)
//   b[1..4]   = 32-bit sensor ID
//   b[5]      = flags (high nibble) + sequence (low nibble)
//   b[6]      = pressure raw, * 0.4 PSI
//   b[7]      = temperature raw, - 50 °C
//   b[8]      = motion flags (0x0e == parked)
//   b[9]      = CRC-8/SMBUS over b[0..8]  (poly 0x07, init 0x00)
//   b[10]     = OEM model (high nibble: 0x4 OEM, 0x0 Clone) — NOT CRC-covered
//
// Preamble (FSK, in the Manchester-decoded bitstream): raw 0x7f 0xff → 0x7FFF.
// rtl_433 validates by running crc8(b, 10, 0x07, 0x00) == 0, i.e. b[9] is the
// CRC of b[0..8].

#define TRW_PREAMBLE 0x7FFFU
#define TRW_DATA_BITS 88

static const SubGhzBlockConst tpms_protocol_trw_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = TRW_DATA_BITS,
};

typedef enum {
    TrwDecoderStepReset = 0,
    TrwDecoderStepFindPreamble,
    TrwDecoderStepData,
} TrwDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    ManchesterState manchester_saved_state;
    uint16_t header_shift;
    uint64_t head_64_bit;
} TPMSProtocolDecoderTrw;

const SubGhzProtocolDecoder tpms_protocol_trw_decoder = {
    .alloc = tpms_protocol_decoder_trw_alloc,
    .free = tpms_protocol_decoder_trw_free,
    .feed = tpms_protocol_decoder_trw_feed,
    .reset = tpms_protocol_decoder_trw_reset,
    .get_hash_data = tpms_protocol_decoder_trw_get_hash_data,
    .serialize = tpms_protocol_decoder_trw_serialize,
    .deserialize = tpms_protocol_decoder_trw_deserialize,
    .get_string = tpms_protocol_decoder_trw_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_trw_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_trw = {
    .name = TPMS_PROTOCOL_TRW_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,
    .decoder = &tpms_protocol_trw_decoder,
    .encoder = &tpms_protocol_trw_encoder,
};

void* tpms_protocol_decoder_trw_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderTrw* instance = malloc(sizeof(TPMSProtocolDecoderTrw));
    instance->base.protocol = &tpms_protocol_trw;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_trw_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_trw_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderTrw* instance = context;
    instance->decoder.parser_step = TrwDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->head_64_bit = 0;
    instance->header_shift = 0;
}

static ManchesterEvent trw_evt(bool level, uint32_t duration) {
    bool is_long = false;
    if(DURATION_DIFF(duration, tpms_protocol_trw_const.te_long) <
       tpms_protocol_trw_const.te_delta * 2) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_trw_const.te_short) <
        tpms_protocol_trw_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }
    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

static void trw_get_bytes(TPMSProtocolDecoderTrw* instance, uint8_t b[11]) {
    // 88 bits total: head_64_bit lower 24 bits = b[0..2] (MSB-first),
    // decode_data 64 bits = b[3..10].
    b[0] = (uint8_t)((instance->head_64_bit >> 16) & 0xFF);
    b[1] = (uint8_t)((instance->head_64_bit >> 8) & 0xFF);
    b[2] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 3] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

// CRC-8/SMBUS: poly 0x07, init 0x00, MSB-first, no reflection, no final xor.
static uint8_t trw_crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0x00;
    for(size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for(uint8_t bit = 0; bit < 8; bit++) {
            if(crc & 0x80)
                crc = (uint8_t)((crc << 1) ^ 0x07);
            else
                crc = (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static bool trw_check_crc(TPMSProtocolDecoderTrw* instance) {
    uint8_t b[11];
    trw_get_bytes(instance, b);
    // b[9] is CRC-8 over b[0..8]; equivalent to crc8(b, 10) == 0 in rtl_433.
    return trw_crc8(b, 9) == b[9];
}

static void trw_analyze(TPMSProtocolDecoderTrw* instance) {
    uint8_t b[11];
    trw_get_bytes(instance, b);
    instance->generic.id =
        ((uint32_t)b[1] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 8) | b[4];
    // Pressure: raw * 0.4 PSI → bar (1 PSI = 0.06895 bar).
    float psi = (float)b[6] * 0.4f;
    instance->generic.pressure = psi * 0.06895f;
    instance->generic.temperature = (float)((int)b[7] - 50);
    // No dedicated battery field in the TRW frame.
    instance->generic.battery_low = TPMS_NO_BATT;
}

void tpms_protocol_decoder_trw_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderTrw* instance = context;

    ManchesterEvent event = trw_evt(level, duration);
    if(event == ManchesterEventReset) {
        instance->decoder.parser_step = TrwDecoderStepReset;
        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;
        instance->head_64_bit = 0;
        instance->header_shift = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        return;
    }

    bool bit = false;
    bool have_bit = manchester_advance(
        instance->manchester_saved_state, event, &instance->manchester_saved_state, &bit);
    if(!have_bit) return;

    switch(instance->decoder.parser_step) {
    case TrwDecoderStepReset:
        instance->header_shift = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        instance->decoder.parser_step = TrwDecoderStepFindPreamble;
        /* fallthrough */
    case TrwDecoderStepFindPreamble:
        instance->header_shift = (uint16_t)((instance->header_shift << 1) | (bit ? 1 : 0));
        if(instance->header_shift == TRW_PREAMBLE) {
            instance->decoder.parser_step = TrwDecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->head_64_bit = 0;
        }
        break;

    case TrwDecoderStepData:
        subghz_protocol_blocks_add_to_128_bit(
            &instance->decoder, bit ? 1 : 0, &instance->head_64_bit);
        if(instance->decoder.decode_count_bit ==
           tpms_protocol_trw_const.min_count_bit_for_found) {
            if(trw_check_crc(instance)) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                trw_analyze(instance);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = TrwDecoderStepReset;
        }
        break;
    }
}

uint8_t tpms_protocol_decoder_trw_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderTrw* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_trw_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderTrw* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_trw_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderTrw* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, tpms_protocol_trw_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_trw_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderTrw* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX Batt:%d\r\n"
        "Temp:%2.0f C Bar:%2.1f",
        instance->generic.protocol_name,
        instance->generic.id,
        instance->generic.battery_low,
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}
