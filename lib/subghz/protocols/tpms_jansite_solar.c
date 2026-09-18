#include "tpms_jansite_solar.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSJansiteSolar"

// Port of rtl_433/src/devices/tpms_jansite_solar.c (GPL-2+)
//
// FSK, Manchester encoded, 433.92 MHz, 11 byte payload (88 bits after Manchester).
// Jansite Solar TPMS (aftermarket solar-powered sensors).
//
// rtl_433 samples the raw stream (FSK_PULSE_PCM, 51 us/cell), searches the raw
// preamble 0xA6 0xA6 0x5A, then Manchester-decodes 88 bits. The decoded payload
// begins with the 16-bit sync word 0xDD33, so on the Flipper we Manchester-decode
// continuously and lock onto that sync word (exactly the way tpms_ford hunts its
// decoded 0x5556 preamble).
//
// Wire format (after Manchester-decoding):
//   YY YY II II II RR TT PP RR CC CC          (11 bytes total)
//   b[0..1]  = sync word 0xDD33 (used as preamble, not stored)
//   b[2..4]  = 24-bit sensor ID
//   b[5]     = reserved
//   b[6]     = temperature, degrees C = byte - 55
//   b[7]     = pressure raw, 1.6 kPa per step
//   b[8]     = reserved
//   b[9..10] = CRC-16/BUYPASS over b[2..8] (poly 0x8005, init 0x0000)
//
// After the 0xDD33 preamble we collect the remaining 72 bits (b[2..10]).

#define JANSITE_SOLAR_PREAMBLE      0xDD33U
#define JANSITE_SOLAR_PREAMBLE_BITS 16
#define JANSITE_SOLAR_DATA_BITS     72 // b[2..10], 9 bytes

static const SubGhzBlockConst tpms_protocol_jansite_solar_const = {
    .te_short = 51,
    .te_long = 102,
    .te_delta = 20,
    .min_count_bit_for_found = JANSITE_SOLAR_DATA_BITS,
};

typedef enum {
    JansiteSolarDecoderStepReset = 0,
    JansiteSolarDecoderStepFindPreamble,
    JansiteSolarDecoderStepData,
} JansiteSolarDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    ManchesterState manchester_saved_state;
    uint16_t header_shift;
    uint64_t head_64_bit;
} TPMSProtocolDecoderJansiteSolar;

const SubGhzProtocolDecoder tpms_protocol_jansite_solar_decoder = {
    .alloc = tpms_protocol_decoder_jansite_solar_alloc,
    .free = tpms_protocol_decoder_jansite_solar_free,
    .feed = tpms_protocol_decoder_jansite_solar_feed,
    .reset = tpms_protocol_decoder_jansite_solar_reset,
    .get_hash_data = tpms_protocol_decoder_jansite_solar_get_hash_data,
    .serialize = tpms_protocol_decoder_jansite_solar_serialize,
    .deserialize = tpms_protocol_decoder_jansite_solar_deserialize,
    .get_string = tpms_protocol_decoder_jansite_solar_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_jansite_solar_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_jansite_solar = {
    .name = TPMS_PROTOCOL_JANSITE_SOLAR_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,
    .decoder = &tpms_protocol_jansite_solar_decoder,
    .encoder = &tpms_protocol_jansite_solar_encoder,
};

void* tpms_protocol_decoder_jansite_solar_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderJansiteSolar* instance = malloc(sizeof(TPMSProtocolDecoderJansiteSolar));
    instance->base.protocol = &tpms_protocol_jansite_solar;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_jansite_solar_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_jansite_solar_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteSolar* instance = context;
    instance->decoder.parser_step = JansiteSolarDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->head_64_bit = 0;
    instance->header_shift = 0;
}

static ManchesterEvent jansite_solar_evt(bool level, uint32_t duration) {
    bool is_long = false;
    if(DURATION_DIFF(duration, tpms_protocol_jansite_solar_const.te_long) <
       tpms_protocol_jansite_solar_const.te_delta * 2) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_jansite_solar_const.te_short) <
        tpms_protocol_jansite_solar_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }
    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

static void jansite_solar_get_bytes(TPMSProtocolDecoderJansiteSolar* instance, uint8_t b[11]) {
    // b[0..1] = fixed sync word (the preamble we locked onto).
    // 72 collected bits: head_64_bit lower 8 bits = b[2], decode_data 64 bits = b[3..10].
    b[0] = 0xDD;
    b[1] = 0x33;
    b[2] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 3] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

// CRC-16/BUYPASS: poly 0x8005, init 0x0000, no reflection, no final xor (rtl_433 crc16()).
static uint16_t jansite_solar_crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0x0000;
    for(size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for(int j = 0; j < 8; j++) {
            if(crc & 0x8000)
                crc = (uint16_t)((crc << 1) ^ 0x8005);
            else
                crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static bool jansite_solar_check_crc(TPMSProtocolDecoderJansiteSolar* instance) {
    uint8_t b[11];
    jansite_solar_get_bytes(instance, b);
    uint16_t crc = jansite_solar_crc16(&b[2], 7); // covers b[2..8]
    uint16_t rx = ((uint16_t)b[9] << 8) | b[10];
    return crc == rx;
}

static void jansite_solar_analyze(TPMSProtocolDecoderJansiteSolar* instance) {
    uint8_t b[11];
    jansite_solar_get_bytes(instance, b);
    instance->generic.id = ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 8) | b[4];
    // Temperature: raw byte - 55 (deg C).
    instance->generic.temperature = (float)((int)b[6] - 55);
    // Pressure raw, 1.6 kPa per step -> bar (kPa * 0.01).
    instance->generic.pressure = ((float)b[7] * 1.6f) * 0.01f;
    // No documented battery bit in this payload.
    instance->generic.battery_low = TPMS_NO_BATT;
}

void tpms_protocol_decoder_jansite_solar_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteSolar* instance = context;

    ManchesterEvent event = jansite_solar_evt(level, duration);
    if(event == ManchesterEventReset) {
        instance->decoder.parser_step = JansiteSolarDecoderStepReset;
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
    case JansiteSolarDecoderStepReset:
        instance->header_shift = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        instance->decoder.parser_step = JansiteSolarDecoderStepFindPreamble;
        /* fallthrough */
    case JansiteSolarDecoderStepFindPreamble:
        instance->header_shift = (uint16_t)((instance->header_shift << 1) | (bit ? 1 : 0));
        if(instance->header_shift == JANSITE_SOLAR_PREAMBLE) {
            instance->decoder.parser_step = JansiteSolarDecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->head_64_bit = 0;
        }
        break;

    case JansiteSolarDecoderStepData:
        subghz_protocol_blocks_add_to_128_bit(
            &instance->decoder, bit ? 1 : 0, &instance->head_64_bit);
        if(instance->decoder.decode_count_bit ==
           tpms_protocol_jansite_solar_const.min_count_bit_for_found) {
            if(jansite_solar_check_crc(instance)) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                jansite_solar_analyze(instance);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = JansiteSolarDecoderStepReset;
        }
        break;
    }
}

uint8_t tpms_protocol_decoder_jansite_solar_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteSolar* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_jansite_solar_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteSolar* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_jansite_solar_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteSolar* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_jansite_solar_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_jansite_solar_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteSolar* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%06lX\r\n"
        "Batt:%s\r\n"
        "Temp:%2.0f C Bar:%2.1f",
        instance->generic.protocol_name,
        instance->generic.id,
        instance->generic.battery_low == TPMS_NO_BATT ?
            "?" :
            (instance->generic.battery_low ? "low" : "ok"),
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}
