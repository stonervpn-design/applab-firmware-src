#include "tpms_abarth124.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSAbarth124"

// Port of rtl_433/src/devices/tpms_abarth124.c
//   (TTigges for VDO Type TG1C via Abarth 124 Spider; GPL-2+)
//
// FSK, Manchester encoded, 9 byte payload (72 bits) — VDO Type TG1C sensor.
// Seen on Abarth 124 Spider, some Fiat 124 Spider, Mazda MX-5 ND (VDO A2C1132410180 /
// Mazda BHB637140A). 433.92 MHz. (The related Shenzhen EGQ Q85 uses a 12-byte frame
// with an extra CRC — not decoded here; this port targets the Abarth124 TG1C frame.)
//
// Wire format (after Manchester-decoding), nibble map from rtl_433:
//   II II II II ?? PP TT SS CC          (9 bytes total)
//   b[0..3]  = 32-bit ID
//   b[4]     = flags (high nibble static, low nibble changes with status)
//   b[5]     = pressure raw, * 1.38 kPa
//   b[6]     = temperature raw, °C = raw - 50
//   b[7]     = status
//   b[8]     = Checksum8 XOR over bytes 0..8 (i.e. XOR of all 9 bytes == 0)
//
// Preamble: rtl_433 inverts the stream and hunts for 0xAA 0xAA 0xA9; the wire
// (non-inverted) pattern is 0x55 0x55 0x56, so we hunt for the trailing 16 bits
// 0x5556 exactly like the Ford/Citroen ports in this tree.

#define ABARTH124_PREAMBLE 0x5556U
#define ABARTH124_DATA_BITS 72

static const SubGhzBlockConst tpms_protocol_abarth124_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = ABARTH124_DATA_BITS,
};

typedef enum {
    Abarth124DecoderStepReset = 0,
    Abarth124DecoderStepFindPreamble,
    Abarth124DecoderStepData,
} Abarth124DecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    ManchesterState manchester_saved_state;
    uint16_t header_shift;
    uint64_t head_64_bit;
} TPMSProtocolDecoderAbarth124;

const SubGhzProtocolDecoder tpms_protocol_abarth124_decoder = {
    .alloc = tpms_protocol_decoder_abarth124_alloc,
    .free = tpms_protocol_decoder_abarth124_free,
    .feed = tpms_protocol_decoder_abarth124_feed,
    .reset = tpms_protocol_decoder_abarth124_reset,
    .get_hash_data = tpms_protocol_decoder_abarth124_get_hash_data,
    .serialize = tpms_protocol_decoder_abarth124_serialize,
    .deserialize = tpms_protocol_decoder_abarth124_deserialize,
    .get_string = tpms_protocol_decoder_abarth124_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_abarth124_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_abarth124 = {
    .name = TPMS_PROTOCOL_ABARTH124_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,
    .decoder = &tpms_protocol_abarth124_decoder,
    .encoder = &tpms_protocol_abarth124_encoder,
};

void* tpms_protocol_decoder_abarth124_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderAbarth124* instance = malloc(sizeof(TPMSProtocolDecoderAbarth124));
    instance->base.protocol = &tpms_protocol_abarth124;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_abarth124_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_abarth124_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderAbarth124* instance = context;
    instance->decoder.parser_step = Abarth124DecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->head_64_bit = 0;
    instance->header_shift = 0;
}

static ManchesterEvent abarth124_evt(bool level, uint32_t duration) {
    bool is_long = false;
    if(DURATION_DIFF(duration, tpms_protocol_abarth124_const.te_long) <
       tpms_protocol_abarth124_const.te_delta * 2) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_abarth124_const.te_short) <
        tpms_protocol_abarth124_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }
    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

static void abarth124_get_bytes(TPMSProtocolDecoderAbarth124* instance, uint8_t b[9]) {
    // 72 bits total: head_64_bit lower 8 bits = b[0] (MSB-first),
    // decode_data 64 bits = b[1..8].
    b[0] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 1] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

static bool abarth124_check_checksum(TPMSProtocolDecoderAbarth124* instance) {
    uint8_t b[9];
    abarth124_get_bytes(instance, b);
    // Checksum8 XOR over bytes 0..8 must be zero.
    uint8_t x = 0;
    for(int i = 0; i < 9; i++) x ^= b[i];
    return x == 0;
}

static void abarth124_analyze(TPMSProtocolDecoderAbarth124* instance) {
    uint8_t b[9];
    abarth124_get_bytes(instance, b);
    instance->generic.id =
        ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    // Pressure raw * 1.38 kPa → bar
    instance->generic.pressure = ((float)b[5] * 1.38f) * 0.01f;
    // Temperature °C = raw - 50
    instance->generic.temperature = (float)((int)b[6] - 50);
    // No documented battery bit; sensor frame carries none.
    instance->generic.battery_low = TPMS_NO_BATT;
}

void tpms_protocol_decoder_abarth124_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderAbarth124* instance = context;

    ManchesterEvent event = abarth124_evt(level, duration);
    if(event == ManchesterEventReset) {
        instance->decoder.parser_step = Abarth124DecoderStepReset;
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
    case Abarth124DecoderStepReset:
        instance->header_shift = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        instance->decoder.parser_step = Abarth124DecoderStepFindPreamble;
        /* fallthrough */
    case Abarth124DecoderStepFindPreamble:
        instance->header_shift = (uint16_t)((instance->header_shift << 1) | (bit ? 1 : 0));
        if(instance->header_shift == ABARTH124_PREAMBLE) {
            instance->decoder.parser_step = Abarth124DecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->head_64_bit = 0;
        }
        break;

    case Abarth124DecoderStepData:
        subghz_protocol_blocks_add_to_128_bit(
            &instance->decoder, bit ? 1 : 0, &instance->head_64_bit);
        if(instance->decoder.decode_count_bit ==
           tpms_protocol_abarth124_const.min_count_bit_for_found) {
            if(abarth124_check_checksum(instance)) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                abarth124_analyze(instance);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = Abarth124DecoderStepReset;
        }
        break;
    }
}

uint8_t tpms_protocol_decoder_abarth124_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderAbarth124* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_abarth124_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderAbarth124* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_abarth124_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderAbarth124* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_abarth124_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_abarth124_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderAbarth124* instance = context;
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
