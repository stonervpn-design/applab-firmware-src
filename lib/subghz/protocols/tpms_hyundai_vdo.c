#include "tpms_hyundai_vdo.h"
#include "../blocks/generic.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSHyundaiVdo"

// Hyundai / Kia (Continental/VDO-family) TPMS sensor.
// Ported from at0m-b0mb/Odograph-FlipperZero parse_hyundai() (traces to rtl_433).
//
// FSK 433 MHz, Manchester-encoded, 10-byte payload — the SAME VDO wire format as
// Citroen/Peugeot/Fiat (preamble 0x55 0x55 0x55 0x56, 52us half-bit). It differs
// from Citroen only in the integrity check and the pressure scale, so it reuses the
// Citroen front-end and is a distinct decode-only protocol.
//
// Wire format (after Manchester-decoding):
//   b[0]     = state (covered by CRC)
//   b[1..4]  = 32-bit sensor ID
//   b[5]     = flags
//   b[6]     = pressure raw, 1.375 kPa per step
//   b[7]     = temperature in C + 50
//   b[8]     = battery / status
//   b[9]     = CRC-8 (poly 0x07, init 0xAA) over b[0..8]
//
// Decode-only: b[1] is real ID data that the 64-bit generic.data (b[2..9]) cannot
// preserve for re-TX, so no encoder is provided (reading/identify only). The TPMS
// spoofer handles transmit for the makes it supports.

#define HYUNDAI_VDO_PREAMBLE 0x5556U
#define HYUNDAI_VDO_DATA_BITS 80

static const SubGhzBlockConst tpms_protocol_hyundai_vdo_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = HYUNDAI_VDO_DATA_BITS,
};

typedef enum {
    HyundaiVdoDecoderStepReset = 0,
    HyundaiVdoDecoderStepFindPreamble,
    HyundaiVdoDecoderStepData,
} HyundaiVdoDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    ManchesterState manchester_saved_state;
    uint16_t header_shift;
    uint64_t head_64_bit;
} TPMSProtocolDecoderHyundaiVdo;

const SubGhzProtocolDecoder tpms_protocol_hyundai_vdo_decoder = {
    .alloc = tpms_protocol_decoder_hyundai_vdo_alloc,
    .free = tpms_protocol_decoder_hyundai_vdo_free,
    .feed = tpms_protocol_decoder_hyundai_vdo_feed,
    .reset = tpms_protocol_decoder_hyundai_vdo_reset,
    .get_hash_data = tpms_protocol_decoder_hyundai_vdo_get_hash_data,
    .serialize = tpms_protocol_decoder_hyundai_vdo_serialize,
    .deserialize = tpms_protocol_decoder_hyundai_vdo_deserialize,
    .get_string = tpms_protocol_decoder_hyundai_vdo_get_string,
};

const SubGhzProtocol tpms_protocol_hyundai_vdo = {
    .name = TPMS_PROTOCOL_HYUNDAI_VDO_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,
    .decoder = &tpms_protocol_hyundai_vdo_decoder,
    .encoder = NULL,
};

void* tpms_protocol_decoder_hyundai_vdo_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderHyundaiVdo* instance = calloc(1, sizeof(TPMSProtocolDecoderHyundaiVdo));
    instance->base.protocol = &tpms_protocol_hyundai_vdo;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->manchester_saved_state = ManchesterStateStart1;
    return instance;
}

void tpms_protocol_decoder_hyundai_vdo_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_hyundai_vdo_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderHyundaiVdo* instance = context;
    instance->decoder.parser_step = HyundaiVdoDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->head_64_bit = 0;
    instance->header_shift = 0;
    instance->manchester_saved_state = ManchesterStateStart1;
}

static ManchesterEvent hyundai_vdo_evt(bool level, uint32_t duration) {
    bool is_long = false;
    if(DURATION_DIFF(duration, tpms_protocol_hyundai_vdo_const.te_long) <
       tpms_protocol_hyundai_vdo_const.te_delta * 2) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_hyundai_vdo_const.te_short) <
        tpms_protocol_hyundai_vdo_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }
    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

static void hyundai_vdo_get_bytes(TPMSProtocolDecoderHyundaiVdo* instance, uint8_t b[10]) {
    // 80 bits: head_64_bit lower 16 bits = b[0..1] (MSB-first), decode_data 64 = b[2..9].
    b[0] = (uint8_t)((instance->head_64_bit >> 8) & 0xFF);
    b[1] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 2] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

// CRC-8, polynomial 0x07, init 0xAA (rtl_433 tpms_hyundai_vdo).
static uint8_t hyundai_vdo_crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0xAA;
    for(size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for(int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static bool hyundai_vdo_check_crc(TPMSProtocolDecoderHyundaiVdo* instance) {
    uint8_t b[10];
    hyundai_vdo_get_bytes(instance, b);
    return hyundai_vdo_crc8(b, 9) == b[9];
}

static void hyundai_vdo_analyze(TPMSProtocolDecoderHyundaiVdo* instance) {
    uint8_t b[10];
    hyundai_vdo_get_bytes(instance, b);
    instance->generic.id =
        ((uint32_t)b[1] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 8) | b[4];
    // Pressure raw -> kPa (1.375 per step) -> bar for display.
    instance->generic.pressure = ((float)b[6] * 1.375f) * 0.01f;
    instance->generic.temperature = (float)((int)b[7] - 50);
    instance->generic.battery_low = b[8];
}

void tpms_protocol_decoder_hyundai_vdo_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderHyundaiVdo* instance = context;

    ManchesterEvent event = hyundai_vdo_evt(level, duration);
    if(event == ManchesterEventReset) {
        instance->decoder.parser_step = HyundaiVdoDecoderStepReset;
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
    case HyundaiVdoDecoderStepReset:
        instance->header_shift = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        instance->decoder.parser_step = HyundaiVdoDecoderStepFindPreamble;
        /* fallthrough */
    case HyundaiVdoDecoderStepFindPreamble:
        instance->header_shift = (uint16_t)((instance->header_shift << 1) | (bit ? 1 : 0));
        if(instance->header_shift == HYUNDAI_VDO_PREAMBLE) {
            instance->decoder.parser_step = HyundaiVdoDecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->head_64_bit = 0;
        }
        break;

    case HyundaiVdoDecoderStepData:
        subghz_protocol_blocks_add_to_128_bit(
            &instance->decoder, bit ? 1 : 0, &instance->head_64_bit);
        if(instance->decoder.decode_count_bit ==
           tpms_protocol_hyundai_vdo_const.min_count_bit_for_found) {
            if(hyundai_vdo_check_crc(instance)) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                hyundai_vdo_analyze(instance);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = HyundaiVdoDecoderStepReset;
        }
        break;
    }
}

uint8_t tpms_protocol_decoder_hyundai_vdo_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderHyundaiVdo* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_hyundai_vdo_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderHyundaiVdo* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_hyundai_vdo_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderHyundaiVdo* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_hyundai_vdo_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_hyundai_vdo_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderHyundaiVdo* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX\r\n"
        "Temp:%2.0f C Bar:%2.1f",
        instance->generic.protocol_name,
        instance->generic.id,
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}
