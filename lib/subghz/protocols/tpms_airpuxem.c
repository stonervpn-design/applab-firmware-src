#include "tpms_airpuxem.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSAirpuxem"

// Port of rtl_433/src/devices/tpms_airpuxem.c
// (Alexander Grau, Bruno Octau, Christian W. Zuckschwerdt 2019, GPL-2+)
//
// Airpuxem TYH11_EU6_ZQ, FSK, Manchester encoded, based on SENASIC SNP739D.
// Likely a white-labeled Jansite sensor. 433.92 MHz +-30 kHz.
//
// Data layout (nibbles):
//   F  II II II II  M N  PP  TT  BB  CC  CC
//   F = 4 bit sync (constant 0x5)
//   I = 32 bit ID
//   M = high nibble of byte[4]: bit3 = pressure MSB_ONE (weight 1<<9), bits2..0 = flags
//   N = low nibble of byte[4]:  bit3 -> combined into byte[4] bit7 handling, bits2..0 = position
//       (rtl_433 slices byte[4] directly: flags=(b4>>4)&7, position=b4&7,
//        pressure MSBs = b4 bit7 (weight 1<<8) and b4 bit3 (weight 1<<9))
//   P = 8 bit pressure LSB (kPa, value = raw - 100)
//   T = 8 bit temperature (signed, deg C)
//   B = 8 bit battery level (volts = raw * 0.02)
//   C = 16 bit checksum (only the first CRC byte is verified)
//
// CRC-8, poly 0x2F, init 0xAA, over the 8 CRC'd bytes (ID..battery), == CRC byte 0.
//
// Preamble (raw bits): 0x55 0x55 0x56  (we hunt for the trailing 16 bits 0x5556),
// followed by the 4-bit sync nibble 0x5, then the 80-bit payload+checksum.

#define AIRPUXEM_PREAMBLE 0x5556U
#define AIRPUXEM_PREAMBLE_BITS 16
#define AIRPUXEM_SYNC_NIBBLE 0x5U
#define AIRPUXEM_SYNC_BITS 4
#define AIRPUXEM_DATA_BITS 80 // 10 bytes: ID(4) MN P T B CRC0 CRC1

static const SubGhzBlockConst tpms_protocol_airpuxem_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = AIRPUXEM_DATA_BITS,
};

typedef enum {
    AirpuxemDecoderStepReset = 0,
    AirpuxemDecoderStepFindPreamble,
    AirpuxemDecoderStepSync,
    AirpuxemDecoderStepData,
} AirpuxemDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    ManchesterState manchester_saved_state;
    uint16_t header_shift;
    uint8_t sync_shift;
    uint8_t sync_count;
    uint64_t head_64_bit;
} TPMSProtocolDecoderAirpuxem;

const SubGhzProtocolDecoder tpms_protocol_airpuxem_decoder = {
    .alloc = tpms_protocol_decoder_airpuxem_alloc,
    .free = tpms_protocol_decoder_airpuxem_free,
    .feed = tpms_protocol_decoder_airpuxem_feed,
    .reset = tpms_protocol_decoder_airpuxem_reset,
    .get_hash_data = tpms_protocol_decoder_airpuxem_get_hash_data,
    .serialize = tpms_protocol_decoder_airpuxem_serialize,
    .deserialize = tpms_protocol_decoder_airpuxem_deserialize,
    .get_string = tpms_protocol_decoder_airpuxem_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_airpuxem_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_airpuxem = {
    .name = TPMS_PROTOCOL_AIRPUXEM_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,
    .decoder = &tpms_protocol_airpuxem_decoder,
    .encoder = &tpms_protocol_airpuxem_encoder,
};

void* tpms_protocol_decoder_airpuxem_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderAirpuxem* instance = malloc(sizeof(TPMSProtocolDecoderAirpuxem));
    instance->base.protocol = &tpms_protocol_airpuxem;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_airpuxem_free(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderAirpuxem* instance = context;
    free(instance);
}

void tpms_protocol_decoder_airpuxem_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderAirpuxem* instance = context;
    instance->decoder.parser_step = AirpuxemDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->head_64_bit = 0;
    instance->header_shift = 0;
    instance->sync_shift = 0;
    instance->sync_count = 0;
}

static ManchesterEvent airpuxem_evt(bool level, uint32_t duration) {
    bool is_long = false;
    if(DURATION_DIFF(duration, tpms_protocol_airpuxem_const.te_long) <
       tpms_protocol_airpuxem_const.te_delta * 2) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_airpuxem_const.te_short) <
        tpms_protocol_airpuxem_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }
    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

// rtl_433 crc8: MSB-first, no reflection, no final xor.
static uint8_t airpuxem_crc8(const uint8_t* message, size_t n_bytes, uint8_t poly, uint8_t init) {
    uint8_t remainder = init;
    for(size_t byte = 0; byte < n_bytes; ++byte) {
        remainder ^= message[byte];
        for(uint8_t bit = 0; bit < 8; ++bit) {
            if(remainder & 0x80) {
                remainder = (uint8_t)((remainder << 1) ^ poly);
            } else {
                remainder = (uint8_t)(remainder << 1);
            }
        }
    }
    return remainder;
}

static void airpuxem_get_bytes(TPMSProtocolDecoderAirpuxem* instance, uint8_t b[10]) {
    // 80 bits total: head_64_bit low 16 bits = b[0..1] (MSB-first),
    // decode_data 64 bits = b[2..9].
    b[0] = (uint8_t)((instance->head_64_bit >> 8) & 0xFF);
    b[1] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 2] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

static bool airpuxem_check_crc(TPMSProtocolDecoderAirpuxem* instance) {
    uint8_t b[10];
    airpuxem_get_bytes(instance, b);
    // CRC over the 8 CRC'd bytes (ID0..ID3, MN, P, T, B), compared to CRC byte 0.
    uint8_t crc = airpuxem_crc8(b, 8, 0x2F, 0xAA);
    return crc == b[8];
}

static void airpuxem_analyze(TPMSProtocolDecoderAirpuxem* instance) {
    uint8_t b[10];
    airpuxem_get_bytes(instance, b);

    instance->generic.id =
        ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];

    // Pressure: LSB byte b[5], plus two MSBs from b[4]: bit7 -> weight 1<<8,
    // bit3 -> weight 1<<9. Value in kPa is (raw - 100).
    int pressure_kpa =
        (int)(b[5] | (((b[4] >> 7) & 1) << 8) | (((b[4] >> 3) & 1) << 9)) - 100;
    instance->generic.pressure = (float)pressure_kpa * 0.01f; // kPa -> bar

    instance->generic.temperature = (float)((int8_t)b[6]);

    // Battery byte (volts = raw * 0.02); keep the raw byte, get_string scales it.
    instance->generic.battery_low = b[7];
}

void tpms_protocol_decoder_airpuxem_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderAirpuxem* instance = context;

    ManchesterEvent event = airpuxem_evt(level, duration);
    if(event == ManchesterEventReset) {
        instance->decoder.parser_step = AirpuxemDecoderStepReset;
        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;
        instance->head_64_bit = 0;
        instance->header_shift = 0;
        instance->sync_shift = 0;
        instance->sync_count = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        return;
    }

    bool bit = false;
    bool have_bit = manchester_advance(
        instance->manchester_saved_state, event, &instance->manchester_saved_state, &bit);
    if(!have_bit) return;

    switch(instance->decoder.parser_step) {
    case AirpuxemDecoderStepReset:
        instance->header_shift = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        instance->decoder.parser_step = AirpuxemDecoderStepFindPreamble;
        /* fallthrough */
    case AirpuxemDecoderStepFindPreamble:
        instance->header_shift = (uint16_t)((instance->header_shift << 1) | (bit ? 1 : 0));
        if(instance->header_shift == AIRPUXEM_PREAMBLE) {
            instance->decoder.parser_step = AirpuxemDecoderStepSync;
            instance->sync_shift = 0;
            instance->sync_count = 0;
        }
        break;

    case AirpuxemDecoderStepSync:
        instance->sync_shift = (uint8_t)((instance->sync_shift << 1) | (bit ? 1 : 0));
        instance->sync_count++;
        if(instance->sync_count == AIRPUXEM_SYNC_BITS) {
            if((instance->sync_shift & 0x0F) == AIRPUXEM_SYNC_NIBBLE) {
                instance->decoder.parser_step = AirpuxemDecoderStepData;
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->head_64_bit = 0;
            } else {
                // Not the expected sync nibble; resync from scratch.
                instance->decoder.parser_step = AirpuxemDecoderStepFindPreamble;
                instance->header_shift = 0;
            }
        }
        break;

    case AirpuxemDecoderStepData:
        subghz_protocol_blocks_add_to_128_bit(
            &instance->decoder, bit ? 1 : 0, &instance->head_64_bit);
        if(instance->decoder.decode_count_bit ==
           tpms_protocol_airpuxem_const.min_count_bit_for_found) {
            if(airpuxem_check_crc(instance)) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                airpuxem_analyze(instance);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = AirpuxemDecoderStepReset;
        }
        break;
    }
}

uint8_t tpms_protocol_decoder_airpuxem_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderAirpuxem* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_airpuxem_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderAirpuxem* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_airpuxem_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderAirpuxem* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_airpuxem_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_airpuxem_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderAirpuxem* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX\r\n"
        "Batt:%1.2fV\r\n"
        "Temp:%2.0f C Bar:%2.1f",
        instance->generic.protocol_name,
        instance->generic.id,
        (double)((float)instance->generic.battery_low * 0.02f),
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}
