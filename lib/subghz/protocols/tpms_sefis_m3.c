#include "tpms_sefis_m3.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSSefisM3"

// Port of rtl_433/src/devices/tpms_sefis_m3.c (Benjamin Larsson 2026, GPL-2+)
//
// Sefis M3 / Careud / Sykik SRTP300 TPMS, Infineon SP400 (8051) transceiver.
// FSK_PCM at 52 us/bit, Manchester (IEEE 802.3) coded, long 0xAA preamble
// and a 16-bit sync word 0x5A9D, followed by a 9-byte payload:
//
//   B0 B1 B2 B3 B4 B5 B6 CRC:16
//
//   B0..B1  opaque (used here as an Id surrogate; no true per-sensor ID exists)
//   B2      combines with B5 for temperature
//   B3      undecoded application-level state
//   B4      top 3 bits = pressure page selector, low 5 bits = pressure data hi
//   B5      pressure data lo (also combines with B2 for temperature)
//   B6      undecoded application-level state
//   B7..B8  CRC-16, poly 0x1021, init 0x0000, over B0..B6 (big-endian)
//
// rtl_433 searches the RAW (pre-Manchester) sync bytes {0x66,0x99,0x96,0xA6}
// which Manchester-decode to 0xA562, then inverts every payload byte (^0xFF);
// the inverted sync is 0x5A9D. Flipper's manchester_advance uses the same
// polarity as rtl_433's bitbuffer_manchester_decode, so in the Manchester-
// sliced stream this decoder hunts the demanchestered sync 0xA562 and then
// inverts each collected payload byte to recover B0..B8.
//
// Pressure (kPa):
//   page = {7:0, 4:1, 5:2, 2:3}[B4 >> 5]  (invalid otherwise)
//   code = page << 13 | (B4 & 0x1f) << 8 | B5
//   pressure_kPa = (code - 0x0e00) / 102.4   (clamped >= 0)
// Temperature (C):
//   temp_code = (B2 + B5) & 0xff
//   temperature_C = 14 + (temp_code & 0x0f)

#define SEFIS_M3_PREAMBLE 0xA562U
#define SEFIS_M3_DATA_BITS 72

static const SubGhzBlockConst tpms_protocol_sefis_m3_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = SEFIS_M3_DATA_BITS,
};

typedef enum {
    SefisM3DecoderStepReset = 0,
    SefisM3DecoderStepFindPreamble,
    SefisM3DecoderStepData,
} SefisM3DecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    ManchesterState manchester_saved_state;
    uint16_t header_shift;
    uint64_t head_64_bit;
} TPMSProtocolDecoderSefisM3;

const SubGhzProtocolDecoder tpms_protocol_sefis_m3_decoder = {
    .alloc = tpms_protocol_decoder_sefis_m3_alloc,
    .free = tpms_protocol_decoder_sefis_m3_free,
    .feed = tpms_protocol_decoder_sefis_m3_feed,
    .reset = tpms_protocol_decoder_sefis_m3_reset,
    .get_hash_data = tpms_protocol_decoder_sefis_m3_get_hash_data,
    .serialize = tpms_protocol_decoder_sefis_m3_serialize,
    .deserialize = tpms_protocol_decoder_sefis_m3_deserialize,
    .get_string = tpms_protocol_decoder_sefis_m3_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_sefis_m3_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_sefis_m3 = {
    .name = TPMS_PROTOCOL_SEFIS_M3_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,
    .decoder = &tpms_protocol_sefis_m3_decoder,
    .encoder = &tpms_protocol_sefis_m3_encoder,
};

void* tpms_protocol_decoder_sefis_m3_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderSefisM3* instance = malloc(sizeof(TPMSProtocolDecoderSefisM3));
    instance->base.protocol = &tpms_protocol_sefis_m3;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_sefis_m3_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_sefis_m3_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderSefisM3* instance = context;
    instance->decoder.parser_step = SefisM3DecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->head_64_bit = 0;
    instance->header_shift = 0;
}

static ManchesterEvent sefis_m3_evt(bool level, uint32_t duration) {
    bool is_long = false;
    if(DURATION_DIFF(duration, tpms_protocol_sefis_m3_const.te_long) <
       tpms_protocol_sefis_m3_const.te_delta * 2) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_sefis_m3_const.te_short) <
        tpms_protocol_sefis_m3_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }
    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

static void sefis_m3_get_bytes(TPMSProtocolDecoderSefisM3* instance, uint8_t b[9]) {
    // 72 bits total: head_64_bit lower 8 bits = b[0] (MSB-first overflow),
    // decode_data 64 bits = b[1..8]. rtl_433 inverts every payload byte.
    b[0] = (uint8_t)(instance->head_64_bit & 0xFF) ^ 0xFF;
    for(int i = 0; i < 8; i++) {
        b[i + 1] = ((instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF) ^ 0xFF;
    }
}

static bool sefis_m3_check_crc(TPMSProtocolDecoderSefisM3* instance) {
    uint8_t b[9];
    sefis_m3_get_bytes(instance, b);
    uint16_t crc = subghz_protocol_blocks_crc16(b, 7, 0x1021, 0x0000);
    uint16_t crc_recv = ((uint16_t)b[7] << 8) | b[8];
    return crc == crc_recv;
}

static void sefis_m3_analyze(TPMSProtocolDecoderSefisM3* instance) {
    uint8_t b[9];
    sefis_m3_get_bytes(instance, b);

    // No true per-sensor ID exists; surface the opaque B0..B3 as an Id.
    instance->generic.id =
        ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];

    int pressure_page = -1;
    switch(b[4] >> 5) {
    case 7:
        pressure_page = 0;
        break;
    case 4:
        pressure_page = 1;
        break;
    case 5:
        pressure_page = 2;
        break;
    case 2:
        pressure_page = 3;
        break;
    }
    if(pressure_page >= 0) {
        int code = (pressure_page << 13) | ((b[4] & 0x1f) << 8) | b[5];
        float pressure_kpa = (float)(code - 0x0e00) / 102.4f;
        if(pressure_kpa < 0.0f) pressure_kpa = 0.0f;
        instance->generic.pressure = pressure_kpa * 0.01f; // kPa -> bar
    } else {
        instance->generic.pressure = 0.0f;
    }

    int temp_code = (b[2] + b[5]) & 0xff;
    instance->generic.temperature = (float)(14 + (temp_code & 0x0f));

    instance->generic.battery_low = TPMS_NO_BATT;
}

void tpms_protocol_decoder_sefis_m3_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderSefisM3* instance = context;

    ManchesterEvent event = sefis_m3_evt(level, duration);
    if(event == ManchesterEventReset) {
        instance->decoder.parser_step = SefisM3DecoderStepReset;
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
    case SefisM3DecoderStepReset:
        instance->header_shift = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        instance->decoder.parser_step = SefisM3DecoderStepFindPreamble;
        /* fallthrough */
    case SefisM3DecoderStepFindPreamble:
        instance->header_shift = (uint16_t)((instance->header_shift << 1) | (bit ? 1 : 0));
        if(instance->header_shift == SEFIS_M3_PREAMBLE) {
            instance->decoder.parser_step = SefisM3DecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->head_64_bit = 0;
        }
        break;

    case SefisM3DecoderStepData:
        subghz_protocol_blocks_add_to_128_bit(
            &instance->decoder, bit ? 1 : 0, &instance->head_64_bit);
        if(instance->decoder.decode_count_bit ==
           tpms_protocol_sefis_m3_const.min_count_bit_for_found) {
            if(sefis_m3_check_crc(instance)) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                sefis_m3_analyze(instance);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = SefisM3DecoderStepReset;
        }
        break;
    }
}

uint8_t tpms_protocol_decoder_sefis_m3_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderSefisM3* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_sefis_m3_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderSefisM3* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_sefis_m3_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderSefisM3* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_sefis_m3_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_sefis_m3_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderSefisM3* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX\r\n"
        "Batt:%s\r\n"
        "Pressure:%2.1f bar\r\n"
        "Temp:%2.0f C",
        instance->generic.protocol_name,
        instance->generic.id,
        (instance->generic.battery_low == TPMS_NO_BATT) ?
            "?" :
            (instance->generic.battery_low ? "low" : "ok"),
        (double)instance->generic.pressure,
        (double)instance->generic.temperature);
}
