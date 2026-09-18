#include "tpms_elantra2012.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSElantra2012"

// Port of rtl_433/src/devices/tpms_elantra2012.c (GPL-2+)
//
// Hyundai Elantra 2012 (and related) TPMS sensor.
// FSK (PCM on the wire), Manchester-encoded data, 8 byte payload.
// 315 MHz (US) / 433.92 MHz (EU).
//
// Wire format (after Manchester-decoding, MSB-first):
//   PP TT II II II II FF CC          (8 bytes / 64 bits)
//   b[0]     = Pressure raw   -> kPa = raw + 60
//   b[1]     = Temperature    -> degC = raw - 50
//   b[2..5]  = 32-bit sensor ID (b[2] = MSB)
//   b[6]     = Flags: bit2 = storage mode, bit1 = battery low, bit0 = triggered
//   b[7]     = CRC-8 (poly 0x07, init 0x00) over b[0..7]; valid when crc == 0
//
// Preamble (RAW chips, before Manchester decode): 0x71 0x55  (16 chips).
// The 0x71 half contains runs of three identical chips (Manchester code
// violations), so the preamble CANNOT be matched in the Manchester-decoded
// domain like the Ford/Citroen 0x5556 marker. We instead reconstruct the raw
// chip stream from the pulse LevelDurations, hunt the 16-chip 0x7155 pattern,
// then Manchester-slice the following 128 chips into 64 data bits (rtl_433
// convention: the data bit is the SECOND chip of each pair, "01" -> 1).

#define ELANTRA2012_PREAMBLE 0x7155U
#define ELANTRA2012_PREAMBLE_BITS 16
#define ELANTRA2012_DATA_BITS 64

static const SubGhzBlockConst tpms_protocol_elantra2012_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = ELANTRA2012_DATA_BITS,
};

typedef enum {
    Elantra2012DecoderStepReset = 0,
    Elantra2012DecoderStepFindPreamble,
    Elantra2012DecoderStepData,
} Elantra2012DecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    uint16_t header_shift;
    bool chip_have_first;
    bool chip_first_val;
} TPMSProtocolDecoderElantra2012;

const SubGhzProtocolDecoder tpms_protocol_elantra2012_decoder = {
    .alloc = tpms_protocol_decoder_elantra2012_alloc,
    .free = tpms_protocol_decoder_elantra2012_free,
    .feed = tpms_protocol_decoder_elantra2012_feed,
    .reset = tpms_protocol_decoder_elantra2012_reset,
    .get_hash_data = tpms_protocol_decoder_elantra2012_get_hash_data,
    .serialize = tpms_protocol_decoder_elantra2012_serialize,
    .deserialize = tpms_protocol_decoder_elantra2012_deserialize,
    .get_string = tpms_protocol_decoder_elantra2012_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_elantra2012_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_elantra2012 = {
    .name = TPMS_PROTOCOL_ELANTRA2012_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,

    .decoder = &tpms_protocol_elantra2012_decoder,
    .encoder = &tpms_protocol_elantra2012_encoder,
};

void* tpms_protocol_decoder_elantra2012_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderElantra2012* instance = malloc(sizeof(TPMSProtocolDecoderElantra2012));
    instance->base.protocol = &tpms_protocol_elantra2012;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_elantra2012_free(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderElantra2012* instance = context;
    free(instance);
}

void tpms_protocol_decoder_elantra2012_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderElantra2012* instance = context;
    instance->decoder.parser_step = Elantra2012DecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->header_shift = 0;
    instance->chip_have_first = false;
    instance->chip_first_val = false;
}

// rtl_433 crc8(buf, len, poly=0x07, init=0x00): MSB-first, no reflection.
// The frame passes when the CRC over all 8 bytes equals 0.
static uint8_t elantra2012_crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0x00;
    for(size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for(int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static bool tpms_protocol_elantra2012_check_crc(TPMSProtocolDecoderElantra2012* instance) {
    uint8_t b[8];
    for(int i = 0; i < 8; i++) {
        b[i] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
    return elantra2012_crc8(b, 8) == 0;
}

static void tpms_protocol_elantra2012_analyze(TPMSBlockGeneric* g) {
    uint8_t b[8];
    for(int i = 0; i < 8; i++) {
        b[i] = (g->data >> (8 * (7 - i))) & 0xFF;
    }
    g->id = ((uint32_t)b[2] << 24) | ((uint32_t)b[3] << 16) | ((uint32_t)b[4] << 8) | b[5];

    // Pressure: raw + 60 kPa -> bar
    g->pressure = (float)((int)b[0] + 60) * 0.01f;
    // Temperature: raw - 50 -> deg C
    g->temperature = (float)((int)b[1] - 50);
    // Flags byte b[6]: bit1 = battery low
    g->battery_low = (uint8_t)((b[6] >> 1) & 0x01);
}

// Reconstruct how many raw chips a single pulse represents. On the wire the
// data is Manchester (max run of 2 identical chips), but the 0x71 preamble
// carries runs of up to 3, so accept 1..4 chip-wide pulses.
static bool tpms_protocol_elantra2012_duration_to_chips(uint32_t duration, uint8_t* nchips) {
    for(uint8_t n = 1; n <= 4; n++) {
        if(DURATION_DIFF(duration, tpms_protocol_elantra2012_const.te_short * n) <
           tpms_protocol_elantra2012_const.te_delta) {
            *nchips = n;
            return true;
        }
    }
    return false;
}

static void
    tpms_protocol_elantra2012_process_chip(TPMSProtocolDecoderElantra2012* instance, bool chip) {
    switch(instance->decoder.parser_step) {
    case Elantra2012DecoderStepReset:
        instance->header_shift = 0;
        instance->decoder.parser_step = Elantra2012DecoderStepFindPreamble;
        /* fallthrough */
    case Elantra2012DecoderStepFindPreamble:
        instance->header_shift = (uint16_t)((instance->header_shift << 1) | (chip ? 1 : 0));
        if(instance->header_shift == ELANTRA2012_PREAMBLE) {
            instance->decoder.parser_step = Elantra2012DecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->chip_have_first = false;
        }
        break;

    case Elantra2012DecoderStepData:
        if(!instance->chip_have_first) {
            instance->chip_first_val = chip;
            instance->chip_have_first = true;
            break;
        }
        instance->chip_have_first = false;
        if(instance->chip_first_val == chip) {
            // Two identical chips in a bit-cell = Manchester violation: resync.
            instance->decoder.parser_step = Elantra2012DecoderStepReset;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->header_shift = 0;
            break;
        }
        // rtl_433 convention: data bit is the second chip of the pair.
        subghz_protocol_blocks_add_bit(&instance->decoder, chip ? 1 : 0);
        if(instance->decoder.decode_count_bit == ELANTRA2012_DATA_BITS) {
            if(tpms_protocol_elantra2012_check_crc(instance)) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                tpms_protocol_elantra2012_analyze(&instance->generic);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = Elantra2012DecoderStepReset;
        }
        break;
    }
}

void tpms_protocol_decoder_elantra2012_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderElantra2012* instance = context;

    uint8_t nchips = 0;
    if(!tpms_protocol_elantra2012_duration_to_chips(duration, &nchips)) {
        instance->decoder.parser_step = Elantra2012DecoderStepReset;
        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;
        instance->header_shift = 0;
        instance->chip_have_first = false;
        return;
    }

    for(uint8_t k = 0; k < nchips; k++) {
        tpms_protocol_elantra2012_process_chip(instance, level);
    }
}

uint8_t tpms_protocol_decoder_elantra2012_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderElantra2012* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_elantra2012_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderElantra2012* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_elantra2012_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderElantra2012* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_elantra2012_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_elantra2012_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderElantra2012* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX Batt:%s\r\n"
        "Temp:%2.0f C  Bar:%2.1f",
        instance->generic.protocol_name,
        instance->generic.id,
        (instance->generic.battery_low == 1) ? "low" : "ok",
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}
