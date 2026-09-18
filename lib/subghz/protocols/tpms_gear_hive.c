#include "tpms_gear_hive.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSGearHive"

// Port of rtl_433/src/devices/tpms_gear_hive.c (GPL-2+)
//
// OOK, Manchester line-coded, 9 byte (72-bit) payload, 433.92 MHz.
// Half-bit ~120us, full-bit ~224us, inter-frame gap ~800us.
//
// Frame:  <preamble zeros> <sync 0x2594> <9 payload bytes> <2 pad bits>
//
// The 9 payload bytes are additionally differentially XOR whitened, seeded
// from the low sync byte 0x94:
//   p[0] = raw[0] ^ 0x94;
//   p[i] = raw[i] ^ raw[i-1];   (i = 1..8)
//
// De-whitened layout p[0..8]:
//   p[0]        counter high (8 bits)
//   p[1] hi     counter low  (4 bits)  -> 12-bit rolling counter
//   p[1] lo     sensor class (4 bits)
//   p[2..4]     24-bit sensor ID
//   p[5]        pressure raw
//   p[6] b1:0 + p[7] b7:6   temperature nibble (4 bits)
//   p[6] b5:2  fixed 0x08   -> (p[6] & 0x3c) == 0x20   (sanity)
//   p[7] b5:0  fixed 0x35   -> (p[7] & 0x3f) == 0x35   (sanity)
//   p[8]        unknown / integrity
//
// Pressure:    base = (80 + class*64) & 0xff;
//              kPa  = ((p[5] - base + 256) & 0xff) * 6.25
// Temperature: temp_bits = (p[7] >> 6) | ((p[6] & 0x03) << 2); C = temp_bits + 21

#define GEAR_HIVE_PREAMBLE  0x2594U
#define GEAR_HIVE_XOR_SEED  0x94U
#define GEAR_HIVE_DATA_BITS 72

static const SubGhzBlockConst tpms_protocol_gear_hive_const = {
    .te_short = 120,
    .te_long = 224,
    .te_delta = 55,
    .min_count_bit_for_found = GEAR_HIVE_DATA_BITS,
};

typedef enum {
    GearHiveDecoderStepReset = 0,
    GearHiveDecoderStepFindPreamble,
    GearHiveDecoderStepData,
} GearHiveDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    ManchesterState manchester_saved_state;
    uint16_t header_shift;
    uint64_t head_64_bit;
} TPMSProtocolDecoderGearHive;

const SubGhzProtocolDecoder tpms_protocol_gear_hive_decoder = {
    .alloc = tpms_protocol_decoder_gear_hive_alloc,
    .free = tpms_protocol_decoder_gear_hive_free,
    .feed = tpms_protocol_decoder_gear_hive_feed,
    .reset = tpms_protocol_decoder_gear_hive_reset,
    .get_hash_data = tpms_protocol_decoder_gear_hive_get_hash_data,
    .serialize = tpms_protocol_decoder_gear_hive_serialize,
    .deserialize = tpms_protocol_decoder_gear_hive_deserialize,
    .get_string = tpms_protocol_decoder_gear_hive_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_gear_hive_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_gear_hive = {
    .name = TPMS_PROTOCOL_GEAR_HIVE_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,
    .decoder = &tpms_protocol_gear_hive_decoder,
    .encoder = &tpms_protocol_gear_hive_encoder,
};

void* tpms_protocol_decoder_gear_hive_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderGearHive* instance = malloc(sizeof(TPMSProtocolDecoderGearHive));
    instance->base.protocol = &tpms_protocol_gear_hive;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_gear_hive_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_gear_hive_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderGearHive* instance = context;
    instance->decoder.parser_step = GearHiveDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->head_64_bit = 0;
    instance->header_shift = 0;
}

static ManchesterEvent gear_hive_evt(bool level, uint32_t duration) {
    bool is_long = false;
    if(DURATION_DIFF(duration, tpms_protocol_gear_hive_const.te_long) <
       tpms_protocol_gear_hive_const.te_delta * 2) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_gear_hive_const.te_short) <
        tpms_protocol_gear_hive_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }
    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

// Assemble the 9 raw (still whitened) payload bytes:
// 72 bits total -> head_64_bit low 8 bits = raw[0], decode_data 64 bits = raw[1..8].
static void gear_hive_get_raw(TPMSProtocolDecoderGearHive* instance, uint8_t raw[9]) {
    raw[0] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        raw[i + 1] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

// Differential-XOR de-whitening, seeded from the low sync byte.
static void gear_hive_dewhiten(const uint8_t raw[9], uint8_t p[9]) {
    p[0] = raw[0] ^ GEAR_HIVE_XOR_SEED;
    for(int i = 1; i < 9; i++) {
        p[i] = raw[i] ^ raw[i - 1];
    }
}

static bool gear_hive_check_sanity(TPMSProtocolDecoderGearHive* instance) {
    uint8_t raw[9];
    uint8_t p[9];
    gear_hive_get_raw(instance, raw);
    gear_hive_dewhiten(raw, p);
    if((p[6] & 0x3c) != 0x20) return false;
    if((p[7] & 0x3f) != 0x35) return false;
    return true;
}

static void gear_hive_analyze(TPMSProtocolDecoderGearHive* instance) {
    uint8_t raw[9];
    uint8_t p[9];
    gear_hive_get_raw(instance, raw);
    gear_hive_dewhiten(raw, p);

    uint8_t sensor_class = p[1] & 0x0F;
    instance->generic.id = ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 8) | p[4];

    int base = (80 + sensor_class * 64) & 0xff;
    float kpa = (float)(((int)p[5] - base + 256) & 0xff) * 6.25f;
    instance->generic.pressure = kpa * 0.01f; // kPa -> bar

    int temp_bits = (p[7] >> 6) | ((p[6] & 0x03) << 2);
    instance->generic.temperature = (float)(temp_bits + 21);

    instance->generic.battery_low = TPMS_NO_BATT; // no documented battery bit
}

void tpms_protocol_decoder_gear_hive_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderGearHive* instance = context;

    ManchesterEvent event = gear_hive_evt(level, duration);
    if(event == ManchesterEventReset) {
        instance->decoder.parser_step = GearHiveDecoderStepReset;
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
    case GearHiveDecoderStepReset:
        instance->header_shift = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        instance->decoder.parser_step = GearHiveDecoderStepFindPreamble;
        /* fallthrough */
    case GearHiveDecoderStepFindPreamble:
        instance->header_shift = (uint16_t)((instance->header_shift << 1) | (bit ? 1 : 0));
        if(instance->header_shift == GEAR_HIVE_PREAMBLE) {
            instance->decoder.parser_step = GearHiveDecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->head_64_bit = 0;
        }
        break;

    case GearHiveDecoderStepData:
        subghz_protocol_blocks_add_to_128_bit(
            &instance->decoder, bit ? 1 : 0, &instance->head_64_bit);
        if(instance->decoder.decode_count_bit ==
           tpms_protocol_gear_hive_const.min_count_bit_for_found) {
            if(gear_hive_check_sanity(instance)) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                gear_hive_analyze(instance);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = GearHiveDecoderStepReset;
        }
        break;
    }
}

uint8_t tpms_protocol_decoder_gear_hive_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderGearHive* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_gear_hive_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderGearHive* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_gear_hive_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderGearHive* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_gear_hive_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_gear_hive_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderGearHive* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%06lX Batt:%s\r\n"
        "Temp:%2.0f C Bar:%2.1f",
        instance->generic.protocol_name,
        instance->generic.id,
        (instance->generic.battery_low == TPMS_NO_BATT) ? "?" : "OK",
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}
