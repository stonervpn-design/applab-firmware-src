#include "tpms_bmw.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSBmw"

// Port of rtl_433/src/devices/tpms_bmw.c (GPL-2+)
//
// FSK, Manchester encoded. Used by BMW (Gen4/Gen5) and Audi TPMS sensors.
// 315 MHz (US) / 433.92 MHz (EU).
//
// rtl_433 modulation: FSK_PULSE_PCM, short=long=25 us chips -> Manchester
// half-bit 25 us, full-bit 50 us.
//
// Wire format (after Manchester-decoding), BMW Gen4/Gen5 = 11 bytes (88 bits):
//   BB II II II II PP TT F1 F2 F3 CC
//   B  = brand / manufacturer         (b[0])
//   I  = 32-bit sensor ID             (b[1..4])
//   P  = pressure raw, * 2.45 kPa      (b[5])
//   T  = temperature, byte - 52 degC   (b[6])
//   F  = 24-bit flags                  (b[7..9])
//   C  = CRC-8 poly 0x2f init 0xaa over b[0..9] (b[10])
//
// CRC verification: crc8(b, 11, 0x2f, 0xaa) == 0 (residue over the whole
// message including the CRC byte must be zero).
//
// Preamble (raw): 0xaa 0x59 -> we hunt for the trailing 16 bits 0xAA59.
//
// Note: rtl_433 also handles an 8-byte "Audi Pressure Alert" variant; this port
// targets the standard 11-byte BMW/Audi sensor frame.

#define BMW_PREAMBLE      0xAA59U
#define BMW_PREAMBLE_BITS 16
#define BMW_DATA_BITS     88

static const SubGhzBlockConst tpms_protocol_bmw_const = {
    .te_short = 25,
    .te_long = 50,
    .te_delta = 10,
    .min_count_bit_for_found = BMW_DATA_BITS,
};

typedef enum {
    BmwDecoderStepReset = 0,
    BmwDecoderStepFindPreamble,
    BmwDecoderStepData,
} BmwDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    ManchesterState manchester_saved_state;
    uint16_t header_shift;
    uint64_t head_64_bit;
} TPMSProtocolDecoderBmw;

const SubGhzProtocolDecoder tpms_protocol_bmw_decoder = {
    .alloc = tpms_protocol_decoder_bmw_alloc,
    .free = tpms_protocol_decoder_bmw_free,
    .feed = tpms_protocol_decoder_bmw_feed,
    .reset = tpms_protocol_decoder_bmw_reset,
    .get_hash_data = tpms_protocol_decoder_bmw_get_hash_data,
    .serialize = tpms_protocol_decoder_bmw_serialize,
    .deserialize = tpms_protocol_decoder_bmw_deserialize,
    .get_string = tpms_protocol_decoder_bmw_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_bmw_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_bmw = {
    .name = TPMS_PROTOCOL_BMW_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,
    .decoder = &tpms_protocol_bmw_decoder,
    .encoder = &tpms_protocol_bmw_encoder,
};

void* tpms_protocol_decoder_bmw_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderBmw* instance = malloc(sizeof(TPMSProtocolDecoderBmw));
    instance->base.protocol = &tpms_protocol_bmw;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_bmw_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_bmw_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderBmw* instance = context;
    instance->decoder.parser_step = BmwDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->head_64_bit = 0;
    instance->header_shift = 0;
}

static ManchesterEvent bmw_evt(bool level, uint32_t duration) {
    bool is_long = false;
    if(DURATION_DIFF(duration, tpms_protocol_bmw_const.te_long) <
       tpms_protocol_bmw_const.te_delta * 2) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_bmw_const.te_short) <
        tpms_protocol_bmw_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }
    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

// rtl_433 crc8: MSB-first, no reflection. Returns residue over all bytes.
static uint8_t bmw_crc8(const uint8_t* data, size_t len, uint8_t poly, uint8_t init) {
    uint8_t crc = init;
    for(size_t i = 0; i < len; i++) {
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

static void bmw_get_bytes(TPMSProtocolDecoderBmw* instance, uint8_t b[11]) {
    // 88 bits total: head_64_bit lower 24 bits = b[0..2] (MSB-first),
    // decode_data 64 bits = b[3..10].
    b[0] = (uint8_t)((instance->head_64_bit >> 16) & 0xFF);
    b[1] = (uint8_t)((instance->head_64_bit >> 8) & 0xFF);
    b[2] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 3] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

static bool bmw_check_crc(TPMSProtocolDecoderBmw* instance) {
    uint8_t b[11];
    bmw_get_bytes(instance, b);
    return bmw_crc8(b, 11, 0x2f, 0xaa) == 0;
}

static void bmw_analyze(TPMSProtocolDecoderBmw* instance) {
    uint8_t b[11];
    bmw_get_bytes(instance, b);
    instance->generic.id =
        ((uint32_t)b[1] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 8) | b[4];
    // Pressure raw * 2.45 kPa -> bar (kPa / 100).
    instance->generic.pressure = ((float)b[5] * 2.45f) * 0.01f;
    instance->generic.temperature = (float)((int)b[6] - 52);
    instance->generic.battery_low = TPMS_NO_BATT;
}

void tpms_protocol_decoder_bmw_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderBmw* instance = context;

    ManchesterEvent event = bmw_evt(level, duration);
    if(event == ManchesterEventReset) {
        instance->decoder.parser_step = BmwDecoderStepReset;
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
    case BmwDecoderStepReset:
        instance->header_shift = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        instance->decoder.parser_step = BmwDecoderStepFindPreamble;
        /* fallthrough */
    case BmwDecoderStepFindPreamble:
        instance->header_shift = (uint16_t)((instance->header_shift << 1) | (bit ? 1 : 0));
        if(instance->header_shift == BMW_PREAMBLE) {
            instance->decoder.parser_step = BmwDecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->head_64_bit = 0;
        }
        break;

    case BmwDecoderStepData:
        subghz_protocol_blocks_add_to_128_bit(
            &instance->decoder, bit ? 1 : 0, &instance->head_64_bit);
        if(instance->decoder.decode_count_bit ==
           tpms_protocol_bmw_const.min_count_bit_for_found) {
            if(bmw_check_crc(instance)) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                bmw_analyze(instance);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = BmwDecoderStepReset;
        }
        break;
    }
}

uint8_t tpms_protocol_decoder_bmw_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderBmw* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_bmw_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderBmw* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_bmw_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderBmw* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, tpms_protocol_bmw_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_bmw_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderBmw* instance = context;
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
