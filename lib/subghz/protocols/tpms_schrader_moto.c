#include "tpms_schrader_moto.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSSchraderMoto"

// Port of rtl_433/src/devices/tpms_schrader_motorcycle.c (GPL-2+)
//
// OOK, Manchester encoded (with fixed leading zero bit), 7 byte payload.
// Schrader motorcycle TPMS sensor. 433.92 MHz (EU) / 315 MHz.
//
// Wire format (after Manchester-decoding, 56 bits = 7 bytes):
//   b[0..3] = 24-bit sensor ID, packed:
//             id = ((b[0] & 0x03) << 22) | (b[1] << 14) | (b[2] << 6) | (b[3] >> 2)
//   b[3..4] = 10-bit pressure raw = ((b[3] & 0x03) << 8) | b[4];  kPa = raw * 0.5
//   b[5]    = temperature = b[5] - 50  (°C)
//   b[6]    = CRC-8, poly 0x07, init 0xE0, covers b[0..6] → valid when crc == 0
//
// Preamble (decoded bits): 0x7ff8 searched as 13 bits = 0b0_1111_1111_1111
// (one fixed leading zero bit followed by twelve ones). We hunt the trailing
// 13-bit pattern 0x0FFF in the decoded bit stream.

#define SCHRADER_MOTO_PREAMBLE      0x0FFFU
#define SCHRADER_MOTO_PREAMBLE_MASK 0x1FFFU
#define SCHRADER_MOTO_DATA_BITS     56

static const SubGhzBlockConst tpms_protocol_schrader_moto_const = {
    .te_short = 122,
    .te_long = 244,
    .te_delta = 50,
    .min_count_bit_for_found = SCHRADER_MOTO_DATA_BITS,
};

typedef enum {
    SchraderMotoDecoderStepReset = 0,
    SchraderMotoDecoderStepFindPreamble,
    SchraderMotoDecoderStepData,
} SchraderMotoDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    ManchesterState manchester_saved_state;
    uint16_t header_shift;
} TPMSProtocolDecoderSchraderMoto;

const SubGhzProtocolDecoder tpms_protocol_schrader_moto_decoder = {
    .alloc = tpms_protocol_decoder_schrader_moto_alloc,
    .free = tpms_protocol_decoder_schrader_moto_free,
    .feed = tpms_protocol_decoder_schrader_moto_feed,
    .reset = tpms_protocol_decoder_schrader_moto_reset,
    .get_hash_data = tpms_protocol_decoder_schrader_moto_get_hash_data,
    .serialize = tpms_protocol_decoder_schrader_moto_serialize,
    .deserialize = tpms_protocol_decoder_schrader_moto_deserialize,
    .get_string = tpms_protocol_decoder_schrader_moto_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_schrader_moto_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_schrader_moto = {
    .name = TPMS_PROTOCOL_SCHRADER_MOTO_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,

    .decoder = &tpms_protocol_schrader_moto_decoder,
    .encoder = &tpms_protocol_schrader_moto_encoder,
};

void* tpms_protocol_decoder_schrader_moto_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderSchraderMoto* instance = malloc(sizeof(TPMSProtocolDecoderSchraderMoto));
    instance->base.protocol = &tpms_protocol_schrader_moto;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_schrader_moto_free(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderSchraderMoto* instance = context;
    free(instance);
}

void tpms_protocol_decoder_schrader_moto_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderSchraderMoto* instance = context;
    instance->decoder.parser_step = SchraderMotoDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->header_shift = 0;
}

static ManchesterEvent schrader_moto_level_and_duration_to_event(bool level, uint32_t duration) {
    bool is_long = false;
    if(DURATION_DIFF(duration, tpms_protocol_schrader_moto_const.te_long) <
       tpms_protocol_schrader_moto_const.te_delta * 2) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_schrader_moto_const.te_short) <
        tpms_protocol_schrader_moto_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }
    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

// rtl_433 crc8(): poly 0x07, MSB-first, init 0xE0, no final XOR.
static uint8_t schrader_moto_crc8(const uint8_t* data, int len, uint8_t poly, uint8_t init) {
    uint8_t crc = init;
    for(int i = 0; i < len; i++) {
        crc ^= data[i];
        for(int b = 0; b < 8; b++) {
            if(crc & 0x80) {
                crc = (uint8_t)((crc << 1) ^ poly);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

static void schrader_moto_get_bytes(TPMSProtocolDecoderSchraderMoto* instance, uint8_t b[7]) {
    // 56 bits in decode_data, MSB-first: b[0] is the most-significant byte.
    for(int i = 0; i < 7; i++) {
        b[i] = (instance->decoder.decode_data >> (8 * (6 - i))) & 0xFF;
    }
}

static bool tpms_protocol_schrader_moto_check_crc(TPMSProtocolDecoderSchraderMoto* instance) {
    uint8_t b[7];
    schrader_moto_get_bytes(instance, b);
    // crc8 over all 7 bytes (including trailing CRC) must be zero.
    return schrader_moto_crc8(b, 7, 0x07, 0xE0) == 0;
}

static void tpms_protocol_schrader_moto_analyze(TPMSBlockGeneric* g) {
    uint8_t b[7];
    for(int i = 0; i < 7; i++) {
        b[i] = (g->data >> (8 * (6 - i))) & 0xFF;
    }
    g->id = ((uint32_t)(b[0] & 0x03) << 22) | ((uint32_t)b[1] << 14) | ((uint32_t)b[2] << 6) |
            (uint32_t)(b[3] >> 2);

    uint16_t pressure_raw = (uint16_t)(((b[3] & 0x03) << 8) | b[4]);
    float pressure_kpa = (float)pressure_raw * 0.5f;
    g->pressure = pressure_kpa * 0.01f; // kPa → bar

    g->temperature = (float)((int)b[5] - 50);

    g->battery_low = TPMS_NO_BATT;
}

void tpms_protocol_decoder_schrader_moto_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderSchraderMoto* instance = context;

    ManchesterEvent event = schrader_moto_level_and_duration_to_event(level, duration);
    if(event == ManchesterEventReset) {
        instance->decoder.parser_step = SchraderMotoDecoderStepReset;
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
    case SchraderMotoDecoderStepReset:
        instance->header_shift = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        instance->decoder.parser_step = SchraderMotoDecoderStepFindPreamble;
        // fall through to consume this bit as preamble candidate
        /* fallthrough */
    case SchraderMotoDecoderStepFindPreamble:
        instance->header_shift = (uint16_t)((instance->header_shift << 1) | (bit ? 1 : 0));
        if((instance->header_shift & SCHRADER_MOTO_PREAMBLE_MASK) == SCHRADER_MOTO_PREAMBLE) {
            instance->decoder.parser_step = SchraderMotoDecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
        }
        break;

    case SchraderMotoDecoderStepData:
        subghz_protocol_blocks_add_bit(&instance->decoder, bit ? 1 : 0);
        if(instance->decoder.decode_count_bit ==
           tpms_protocol_schrader_moto_const.min_count_bit_for_found) {
            if(tpms_protocol_schrader_moto_check_crc(instance)) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                tpms_protocol_schrader_moto_analyze(&instance->generic);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = SchraderMotoDecoderStepReset;
        }
        break;
    }
}

uint8_t tpms_protocol_decoder_schrader_moto_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderSchraderMoto* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_schrader_moto_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderSchraderMoto* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_schrader_moto_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderSchraderMoto* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_schrader_moto_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_schrader_moto_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderSchraderMoto* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX\r\n"
        "Batt:%s\r\n"
        "Temp:%2.0f C Bar:%2.1f",
        instance->generic.protocol_name,
        instance->generic.id,
        (instance->generic.battery_low == TPMS_NO_BATT) ?
            "n/a" :
            (instance->generic.battery_low ? "low" : "ok"),
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}
