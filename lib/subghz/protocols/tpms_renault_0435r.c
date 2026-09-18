#include "tpms_renault_0435r.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSRenault0435R"

// Port of rtl_433/src/devices/tpms_renault_0435r.c (GPL-2+)
//
// FSK, Manchester encoded, 9 byte payload (72 bits).
// Renault 40700 0435R / VDO S180052064Z. 433.92 MHz (EU).
//
// Wire format (after Manchester-decoding):
//   II II II fx PP TT AA CC tt          (9 bytes total)
//   b[0..2] = 24-bit ID (printed MSB-first: b0 b1 b2)
//   b[3]    = flags (observed 0xC0)
//   b[4]    = pressure raw, kPa = raw / 0.75
//   b[5]    = temperature raw, C = raw - 50
//   b[6]    = centrifugal acceleration raw, m/s^2 = raw * 5
//   b[7]    = checksum
//   b[8]    = tick counter: bit7 = has_tick flag, bits6..0 = tick (0..30)
//   CRC     = XOR(b[0..8]) == 0
//
// rtl_433 preamble raw bits are 0xAA 0xA9 (the bit-inverse of the Ford/Citroen
// 0x55 0x55 0x55 0x56 sync). The Flipper self-slicing Manchester decoder can lock
// onto either polarity depending on the leading edge, so we hunt for the 16-bit
// sync in BOTH polarities (0xAAA9 and its inverse 0x5556) and normalise the frame
// polarity afterwards using the checksum: an all-bytes-inverted decode makes the
// 9-byte XOR flip from 0x00 to 0xFF, which we detect and correct before extraction.

#define RENAULT_0435R_PREAMBLE_A 0xAAA9U
#define RENAULT_0435R_PREAMBLE_B 0x5556U
#define RENAULT_0435R_DATA_BITS  72

static const SubGhzBlockConst tpms_protocol_renault_0435r_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = RENAULT_0435R_DATA_BITS,
};

typedef enum {
    Renault0435RDecoderStepReset = 0,
    Renault0435RDecoderStepFindPreamble,
    Renault0435RDecoderStepData,
} Renault0435RDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    ManchesterState manchester_saved_state;
    uint16_t header_shift;
    uint64_t head_64_bit;
} TPMSProtocolDecoderRenault0435R;

const SubGhzProtocolDecoder tpms_protocol_renault_0435r_decoder = {
    .alloc = tpms_protocol_decoder_renault_0435r_alloc,
    .free = tpms_protocol_decoder_renault_0435r_free,
    .feed = tpms_protocol_decoder_renault_0435r_feed,
    .reset = tpms_protocol_decoder_renault_0435r_reset,
    .get_hash_data = tpms_protocol_decoder_renault_0435r_get_hash_data,
    .serialize = tpms_protocol_decoder_renault_0435r_serialize,
    .deserialize = tpms_protocol_decoder_renault_0435r_deserialize,
    .get_string = tpms_protocol_decoder_renault_0435r_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_renault_0435r_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_renault_0435r = {
    .name = TPMS_PROTOCOL_RENAULT_0435R_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,
    .decoder = &tpms_protocol_renault_0435r_decoder,
    .encoder = &tpms_protocol_renault_0435r_encoder,
};

void* tpms_protocol_decoder_renault_0435r_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderRenault0435R* instance =
        malloc(sizeof(TPMSProtocolDecoderRenault0435R));
    instance->base.protocol = &tpms_protocol_renault_0435r;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_renault_0435r_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_renault_0435r_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderRenault0435R* instance = context;
    instance->decoder.parser_step = Renault0435RDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->head_64_bit = 0;
    instance->header_shift = 0;
}

static ManchesterEvent renault_0435r_evt(bool level, uint32_t duration) {
    bool is_long = false;
    if(DURATION_DIFF(duration, tpms_protocol_renault_0435r_const.te_long) <
       tpms_protocol_renault_0435r_const.te_delta * 2) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_renault_0435r_const.te_short) <
        tpms_protocol_renault_0435r_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }
    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

static void renault_0435r_get_bytes(TPMSProtocolDecoderRenault0435R* instance, uint8_t b[9]) {
    // 72 bits total: head_64_bit lower 8 bits = b[0] (MSB-first),
    // decode_data 64 bits = b[1..8].
    b[0] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 1] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

// XOR of all 9 bytes must be 0. Normalises full-frame polarity inversion:
// an inverted decode gives XOR == 0xFF, which we correct in place. Returns
// true when the (possibly corrected) frame validates.
static bool renault_0435r_check_crc(uint8_t b[9]) {
    uint8_t x = 0;
    for(int i = 0; i < 9; i++) x ^= b[i];
    if(x == 0xFF) {
        for(int i = 0; i < 9; i++) b[i] = (uint8_t)(~b[i]);
        x = 0;
        for(int i = 0; i < 9; i++) x ^= b[i];
    }
    if(x != 0) return false;

    // rtl_433 sanity: when the tick byte is present the has_tick flag must be
    // set and the tick value must be <= 30.
    if(b[8] != 0) {
        uint8_t has_tick = b[8] >> 7;
        uint8_t tick = b[8] & 0x7F;
        if(has_tick != 1 || tick > 30) return false;
    }
    return true;
}

static void renault_0435r_analyze(TPMSProtocolDecoderRenault0435R* instance, uint8_t b[9]) {
    // 24-bit ID, printed MSB-first (b0 b1 b2).
    instance->generic.id =
        ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | (uint32_t)b[2];
    // Pressure raw -> kPa (raw / 0.75) -> bar (kPa * 0.01).
    instance->generic.pressure = ((float)b[4] / 0.75f) * 0.01f;
    // Temperature: raw - 50 (Celsius).
    instance->generic.temperature = (float)((int)b[5] - 50);
    // No battery bit in this protocol.
    instance->generic.battery_low = TPMS_NO_BATT;
}

void tpms_protocol_decoder_renault_0435r_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderRenault0435R* instance = context;

    ManchesterEvent event = renault_0435r_evt(level, duration);
    if(event == ManchesterEventReset) {
        instance->decoder.parser_step = Renault0435RDecoderStepReset;
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
    case Renault0435RDecoderStepReset:
        instance->header_shift = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        instance->decoder.parser_step = Renault0435RDecoderStepFindPreamble;
        /* fallthrough */
    case Renault0435RDecoderStepFindPreamble:
        instance->header_shift = (uint16_t)((instance->header_shift << 1) | (bit ? 1 : 0));
        if(instance->header_shift == RENAULT_0435R_PREAMBLE_A ||
           instance->header_shift == RENAULT_0435R_PREAMBLE_B) {
            instance->decoder.parser_step = Renault0435RDecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->head_64_bit = 0;
        }
        break;

    case Renault0435RDecoderStepData:
        subghz_protocol_blocks_add_to_128_bit(
            &instance->decoder, bit ? 1 : 0, &instance->head_64_bit);
        if(instance->decoder.decode_count_bit ==
           tpms_protocol_renault_0435r_const.min_count_bit_for_found) {
            uint8_t b[9];
            renault_0435r_get_bytes(instance, b);
            if(renault_0435r_check_crc(b)) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                renault_0435r_analyze(instance, b);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = Renault0435RDecoderStepReset;
        }
        break;
    }
}

uint8_t tpms_protocol_decoder_renault_0435r_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderRenault0435R* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_renault_0435r_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderRenault0435R* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_renault_0435r_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderRenault0435R* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_renault_0435r_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_renault_0435r_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderRenault0435R* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%06lX Batt:%s\r\n"
        "Temp:%2.0f C Bar:%2.1f",
        instance->generic.protocol_name,
        instance->generic.id,
        (instance->generic.battery_low == TPMS_NO_BATT) ?
            "?" :
            (instance->generic.battery_low ? "low" : "ok"),
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}
