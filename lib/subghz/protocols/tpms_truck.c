#include "tpms_truck.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSTruck"

// Port of rtl_433/src/devices/tpms_truck.c ("Unbranded SolarTPMS for trucks", GPL-2+)
//
// FSK (FSK_PULSE_PCM), Manchester encoded, 9 byte payload after a leading
// nibble is discarded. Seen on generic solar truck/trailer TPMS sensors.
// 433.92 MHz (also used on 315 MHz clones).
//
// Wire format (after Manchester-decoding, MSB-first):
//   U II II II II WW FP PP TT CC          (76 decoded bits total)
//   U        = leading 4-bit nibble (discarded, not part of the 9 data bytes)
//   b[0..3]  = 32-bit sensor ID
//   b[4]     = wheel / position (0..255)
//   b[5]     = flags (high nibble) + pressure MSBs (low nibble)
//              flags bit2 (0x4) = pressure alert, flags low bits (0x3)==0x3 = battery ok
//   b[6]     = pressure LSBs; pressure = ((b[5]&0x0F)<<8)|b[6]  → kPa
//   b[7]     = temperature (°C, raw byte)
//   b[8]     = checksum: XOR(b[0..8]) == 0
//
// Preamble raw 0x55 0x55 0x56 → trailing 16-bit pattern 0x5556 (rtl_433 inverts
// and hunts 0xAA 0xAA 0xA9; the non-inverted sync is 0x5556).

#define TRUCK_PREAMBLE 0x5556U
#define TRUCK_DATA_BITS 76

static const SubGhzBlockConst tpms_protocol_truck_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = TRUCK_DATA_BITS,
};

typedef enum {
    TruckDecoderStepReset = 0,
    TruckDecoderStepFindPreamble,
    TruckDecoderStepData,
} TruckDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    ManchesterState manchester_saved_state;
    uint16_t header_shift;
    uint64_t head_64_bit;
} TPMSProtocolDecoderTruck;

const SubGhzProtocolDecoder tpms_protocol_truck_decoder = {
    .alloc = tpms_protocol_decoder_truck_alloc,
    .free = tpms_protocol_decoder_truck_free,
    .feed = tpms_protocol_decoder_truck_feed,
    .reset = tpms_protocol_decoder_truck_reset,
    .get_hash_data = tpms_protocol_decoder_truck_get_hash_data,
    .serialize = tpms_protocol_decoder_truck_serialize,
    .deserialize = tpms_protocol_decoder_truck_deserialize,
    .get_string = tpms_protocol_decoder_truck_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_truck_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_truck = {
    .name = TPMS_PROTOCOL_TRUCK_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,
    .decoder = &tpms_protocol_truck_decoder,
    .encoder = &tpms_protocol_truck_encoder,
};

void* tpms_protocol_decoder_truck_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderTruck* instance = malloc(sizeof(TPMSProtocolDecoderTruck));
    instance->base.protocol = &tpms_protocol_truck;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_truck_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_truck_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderTruck* instance = context;
    instance->decoder.parser_step = TruckDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->head_64_bit = 0;
    instance->header_shift = 0;
}

static ManchesterEvent truck_evt(bool level, uint32_t duration) {
    bool is_long = false;
    if(DURATION_DIFF(duration, tpms_protocol_truck_const.te_long) <
       tpms_protocol_truck_const.te_delta * 2) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_truck_const.te_short) <
        tpms_protocol_truck_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }
    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

static void truck_get_bytes(TPMSProtocolDecoderTruck* instance, uint8_t b[9]) {
    // 76 decoded bits, MSB-first. The leading 4-bit nibble occupies the top of
    // head_64_bit and is discarded; the 9 data bytes are the 72 bits that follow
    // (rtl_433 extracts from bit offset 4).
    //   b[0] = bits 4..11   → head_64_bit & 0xFF
    //   b[k] = (decode_data >> (8*(8-k))) & 0xFF   for k = 1..8
    b[0] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int k = 1; k < 9; k++) {
        b[k] = (uint8_t)((instance->decoder.decode_data >> (8 * (8 - k))) & 0xFF);
    }
}

static bool truck_check_crc(TPMSProtocolDecoderTruck* instance) {
    uint8_t b[9];
    truck_get_bytes(instance, b);
    // Reject all-zero ID (rtl_433 does the same before checksum).
    if(!b[0] && !b[1] && !b[2] && !b[3]) return false;
    uint8_t x = 0;
    for(int i = 0; i < 9; i++) x ^= b[i];
    return x == 0;
}

static void truck_analyze(TPMSProtocolDecoderTruck* instance) {
    uint8_t b[9];
    truck_get_bytes(instance, b);
    instance->generic.id =
        ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];

    int flags = b[5] >> 4;
    int pressure_kpa = ((int)(b[5] & 0x0F) << 8) | b[6];
    // kPa → bar for the generic block (which stores pressure in bar).
    instance->generic.pressure = (float)pressure_kpa * 0.01f;
    // rtl_433 reports the raw temperature byte (no offset/sign).
    instance->generic.temperature = (float)b[7];

    bool battery_ok = ((flags & 0x3) == 0x3);
    instance->generic.battery_low = battery_ok ? 0 : 1;
}

void tpms_protocol_decoder_truck_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderTruck* instance = context;

    ManchesterEvent event = truck_evt(level, duration);
    if(event == ManchesterEventReset) {
        instance->decoder.parser_step = TruckDecoderStepReset;
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
    case TruckDecoderStepReset:
        instance->header_shift = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        instance->decoder.parser_step = TruckDecoderStepFindPreamble;
        /* fallthrough */
    case TruckDecoderStepFindPreamble:
        instance->header_shift = (uint16_t)((instance->header_shift << 1) | (bit ? 1 : 0));
        if(instance->header_shift == TRUCK_PREAMBLE) {
            instance->decoder.parser_step = TruckDecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->head_64_bit = 0;
        }
        break;

    case TruckDecoderStepData:
        subghz_protocol_blocks_add_to_128_bit(
            &instance->decoder, bit ? 1 : 0, &instance->head_64_bit);
        if(instance->decoder.decode_count_bit ==
           tpms_protocol_truck_const.min_count_bit_for_found) {
            if(truck_check_crc(instance)) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                truck_analyze(instance);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = TruckDecoderStepReset;
        }
        break;
    }
}

uint8_t tpms_protocol_decoder_truck_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderTruck* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_truck_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderTruck* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_truck_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderTruck* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, tpms_protocol_truck_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_truck_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderTruck* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX\r\n"
        "Batt:%s\r\n"
        "Pres:%2.1f bar Temp:%2.0f C",
        instance->generic.protocol_name,
        instance->generic.id,
        instance->generic.battery_low ? "low" : "ok",
        (double)instance->generic.pressure,
        (double)instance->generic.temperature);
}
