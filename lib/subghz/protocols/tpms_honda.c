#include "tpms_honda.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSHonda"

// Port of rtl_433/src/devices/tpms_honda.c (Honda TRW PPA-GF33, GPL-2+)
//
// FSK, IEEE 802.3 Manchester encoded (PCM half-bits ~50 us), 8 byte payload.
// 315 MHz (US) / 433.92 MHz (EU).
//
// Wire format (after Manchester-decoding, before CRC check):
//   PP TT II II II II FF CC
//   P = Pressure raw (byte 0)   -> raw * 0.2 PSI
//   T = Temperature raw (byte 1)-> raw - 50 C
//   I = ID (32 bit, bytes 2..5)
//   F = Flags (byte 6, e.g. 0xE1 stationary; bit meanings unknown)
//   C = CRC-8/SMBUS (byte 7): crc8(b[0..6], poly 0x07, init 0x00) == b[7]
//
// Sync: a 23-bit raw marker {0xDA, 0xE3, 0x54} (= 0x6D71AA) that is a
// deliberately INVALID Manchester sequence used as a desync marker. rtl_433
// finds it with bitbuffer_search(marker, 23) then Manchester-decodes 64 bits
// from bitpos 23. We reproduce that exactly with a chip-level pulse state
// machine: slice each LevelDuration into 50 us chips, hunt the 23-bit marker
// (and its polarity inverse) on the raw chip stream, then decode Manchester
// pairs (decoded bit = 2nd chip, per rtl_433) into the 64-bit frame.

#define HONDA_MARKER23     0x6D71AAU // {0xDA,0xE3,0x54} top 23 bits
#define HONDA_MARKER23_INV 0x128E55U // polarity-inverted marker
#define HONDA_MARKER_MASK  0x7FFFFFU // 23 bits
#define HONDA_DATA_BITS    64
#define HONDA_MAX_CHIPS_PER_PULSE 8U

static const SubGhzBlockConst tpms_protocol_honda_const = {
    .te_short = 50,
    .te_long = 100,
    .te_delta = 20,
    .min_count_bit_for_found = HONDA_DATA_BITS,
};

typedef enum {
    HondaDecoderStepFindMarker = 0,
    HondaDecoderStepData,
} HondaDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    uint32_t marker_shift;
    bool chip_invert;
    bool pair_have_first;
    uint8_t pair_first;
} TPMSProtocolDecoderHonda;

const SubGhzProtocolDecoder tpms_protocol_honda_decoder = {
    .alloc = tpms_protocol_decoder_honda_alloc,
    .free = tpms_protocol_decoder_honda_free,
    .feed = tpms_protocol_decoder_honda_feed,
    .reset = tpms_protocol_decoder_honda_reset,
    .get_hash_data = tpms_protocol_decoder_honda_get_hash_data,
    .serialize = tpms_protocol_decoder_honda_serialize,
    .deserialize = tpms_protocol_decoder_honda_deserialize,
    .get_string = tpms_protocol_decoder_honda_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_honda_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_honda = {
    .name = TPMS_PROTOCOL_HONDA_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,

    .decoder = &tpms_protocol_honda_decoder,
    .encoder = &tpms_protocol_honda_encoder,
};

void* tpms_protocol_decoder_honda_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderHonda* instance = malloc(sizeof(TPMSProtocolDecoderHonda));
    instance->base.protocol = &tpms_protocol_honda;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_honda_free(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderHonda* instance = context;
    free(instance);
}

void tpms_protocol_decoder_honda_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderHonda* instance = context;
    instance->decoder.parser_step = HondaDecoderStepFindMarker;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->marker_shift = 0;
    instance->chip_invert = false;
    instance->pair_have_first = false;
    instance->pair_first = 0;
}

// CRC-8/SMBUS: MSB-first, poly 0x07, init 0x00, no reflection/xorout.
static uint8_t honda_crc8(const uint8_t* data, size_t len, uint8_t poly, uint8_t init) {
    uint8_t crc = init;
    for(size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for(int b = 0; b < 8; b++) {
            if(crc & 0x80)
                crc = (uint8_t)((crc << 1) ^ poly);
            else
                crc = (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static bool honda_check_crc(TPMSProtocolDecoderHonda* instance) {
    uint8_t b[8];
    for(int i = 0; i < 8; i++) {
        b[i] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
    return honda_crc8(b, 7, 0x07, 0x00) == b[7];
}

static void honda_analyze(TPMSBlockGeneric* g) {
    uint8_t b[8];
    for(int i = 0; i < 8; i++) {
        b[i] = (g->data >> (8 * (7 - i))) & 0xFF;
    }
    g->id = ((uint32_t)b[2] << 24) | ((uint32_t)b[3] << 16) | ((uint32_t)b[4] << 8) | b[5];

    // Pressure: raw * 0.2 PSI, stored as bar to match the other TPMS ports.
    float psi = (float)b[0] * 0.2f;
    g->pressure = psi * 0.06895f; // PSI -> bar

    // Temperature: raw - 50 C.
    g->temperature = (float)((int)b[1] - 50);

    // Flags byte b[6] bit meanings unknown; battery not decoded by rtl_433.
    g->battery_low = TPMS_NO_BATT;
}

static void honda_start_data(TPMSProtocolDecoderHonda* instance) {
    instance->decoder.parser_step = HondaDecoderStepData;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->pair_have_first = false;
}

static void honda_feed_chip(TPMSProtocolDecoderHonda* instance, uint8_t chip) {
    // Marker search always runs on the raw (pre-Manchester) chip stream.
    instance->marker_shift =
        ((instance->marker_shift << 1) | (chip & 1U)) & HONDA_MARKER_MASK;

    switch(instance->decoder.parser_step) {
    case HondaDecoderStepFindMarker:
        if(instance->marker_shift == HONDA_MARKER23) {
            instance->chip_invert = false;
            honda_start_data(instance);
        } else if(instance->marker_shift == HONDA_MARKER23_INV) {
            instance->chip_invert = true;
            honda_start_data(instance);
        }
        break;

    case HondaDecoderStepData: {
        uint8_t c = chip ^ (instance->chip_invert ? 1U : 0U);
        if(!instance->pair_have_first) {
            instance->pair_first = c;
            instance->pair_have_first = true;
            break;
        }
        instance->pair_have_first = false;
        uint8_t c1 = instance->pair_first;
        uint8_t c2 = c;
        if(c1 == c2) {
            // Invalid Manchester pair -> abort this frame, keep hunting.
            instance->decoder.parser_step = HondaDecoderStepFindMarker;
            break;
        }
        // rtl_433 bitbuffer_manchester_decode: decoded bit = second chip.
        subghz_protocol_blocks_add_bit(&instance->decoder, c2 & 1U);
        if(instance->decoder.decode_count_bit ==
           tpms_protocol_honda_const.min_count_bit_for_found) {
            if(honda_check_crc(instance)) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                honda_analyze(&instance->generic);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = HondaDecoderStepFindMarker;
        }
        break;
    }

    default:
        instance->decoder.parser_step = HondaDecoderStepFindMarker;
        break;
    }
}

void tpms_protocol_decoder_honda_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderHonda* instance = context;

    // Slice the FSK PCM pulse into te_short (~50 us) half-bit chips.
    uint32_t te = tpms_protocol_honda_const.te_short;
    uint32_t n = (duration + te / 2) / te;
    if(n == 0) return;
    if(n > HONDA_MAX_CHIPS_PER_PULSE) {
        // Long idle / gap: desynchronise and resume hunting.
        tpms_protocol_decoder_honda_reset(instance);
        return;
    }

    uint8_t chip = level ? 1U : 0U;
    for(uint32_t k = 0; k < n; k++) {
        honda_feed_chip(instance, chip);
    }
}

uint8_t tpms_protocol_decoder_honda_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderHonda* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_honda_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderHonda* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_honda_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderHonda* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_honda_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_honda_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderHonda* instance = context;
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
