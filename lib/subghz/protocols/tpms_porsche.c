#include "tpms_porsche.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSPorsche"

// Port of rtl_433/src/devices/tpms_porsche.c (Christian W. Zuckschwerdt 2021, GPL-2+)
//
// Porsche second generation (Typ 987) Boxster/Cayman TPMS.
// FSK, DIFFERENTIAL Manchester Coded (DMC), 10 byte payload.
// 433.92 MHz (EU) / 315 MHz (US).
//
// Full preamble is {30}ccccccca (PCM bits 0x33333332). The PCM half-bit is ~52us,
// so — decoded to DMC bits — the preamble is a long run of '1' bits terminated by a
// '0', after which the 80 payload bits begin.
//
// Wire format (after DMC-decoding, MSB-first, 10 bytes):
//   II II II II PP TT SS SS CC CC
//   I = ID (32 bit)
//   P = Pressure  : kPa = raw * 5 / 2 - 100   (raw*2.5 - 100)
//   T = Temperature: degC = raw - 40
//   S = Status/flags (16 bit, meaning undefined)
//   C = Checksum : CRC-16 poly 0x1021 init 0xffff over all 10 bytes, == 0 when valid
//
// DMC slicing in the pulse domain: a full bit spans two 52us half-bits with a
// guaranteed transition at every bit boundary. No mid-bit transition -> the level
// runs the whole bit (one LONG ~104us pulse) -> decoded '1'. A mid-bit transition
// splits the bit into two SHORT ~52us pulses -> decoded '0'. This matches
// rtl_433's bitbuffer_differential_manchester_decode() (equal PCM pair => 1,
// differing PCM pair => 0).

#define PORSCHE_DATA_BITS         80
#define PORSCHE_PREAMBLE_MIN_ONES 10

static const SubGhzBlockConst tpms_protocol_porsche_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = PORSCHE_DATA_BITS,
};

typedef enum {
    PorscheDecoderStepReset = 0,
    PorscheDecoderStepFindPreamble,
    PorscheDecoderStepData,
} PorscheDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    uint64_t head_64_bit;
    uint16_t preamble_ones;
    bool half_pending; // one SHORT half-bit seen, awaiting its pair
} TPMSProtocolDecoderPorsche;

const SubGhzProtocolDecoder tpms_protocol_porsche_decoder = {
    .alloc = tpms_protocol_decoder_porsche_alloc,
    .free = tpms_protocol_decoder_porsche_free,
    .feed = tpms_protocol_decoder_porsche_feed,
    .reset = tpms_protocol_decoder_porsche_reset,
    .get_hash_data = tpms_protocol_decoder_porsche_get_hash_data,
    .serialize = tpms_protocol_decoder_porsche_serialize,
    .deserialize = tpms_protocol_decoder_porsche_deserialize,
    .get_string = tpms_protocol_decoder_porsche_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_porsche_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_porsche = {
    .name = TPMS_PROTOCOL_PORSCHE_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,
    .decoder = &tpms_protocol_porsche_decoder,
    .encoder = &tpms_protocol_porsche_encoder,
};

void* tpms_protocol_decoder_porsche_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderPorsche* instance = calloc(1, sizeof(TPMSProtocolDecoderPorsche));
    instance->base.protocol = &tpms_protocol_porsche;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_porsche_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_porsche_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderPorsche* instance = context;
    instance->decoder.parser_step = PorscheDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->head_64_bit = 0;
    instance->preamble_ones = 0;
    instance->half_pending = false;
}

typedef enum {
    PorschePulseShort = 0,
    PorschePulseLong,
    PorschePulseReset,
} PorschePulse;

static PorschePulse porsche_classify(uint32_t duration) {
    if(DURATION_DIFF(duration, tpms_protocol_porsche_const.te_long) <
       tpms_protocol_porsche_const.te_delta * 2) {
        return PorschePulseLong;
    }
    if(DURATION_DIFF(duration, tpms_protocol_porsche_const.te_short) <
       tpms_protocol_porsche_const.te_delta) {
        return PorschePulseShort;
    }
    return PorschePulseReset;
}

static void porsche_get_bytes(TPMSProtocolDecoderPorsche* instance, uint8_t b[10]) {
    // 80 bits total: head_64_bit lower 16 bits = b[0..1] (MSB-first),
    // decode_data 64 bits = b[2..9].
    b[0] = (uint8_t)((instance->head_64_bit >> 8) & 0xFF);
    b[1] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 2] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

// CRC-16/CCITT-FALSE: poly 0x1021, init 0xffff, no reflection, no final xor
// (rtl_433 crc16()). Computed over all 10 bytes; a valid frame yields 0.
static uint16_t porsche_crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for(size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for(int bit = 0; bit < 8; bit++) {
            if(crc & 0x8000) {
                crc = (uint16_t)((crc << 1) ^ 0x1021);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

static bool porsche_check_crc(TPMSProtocolDecoderPorsche* instance) {
    uint8_t b[10];
    porsche_get_bytes(instance, b);
    return porsche_crc16(b, 10) == 0;
}

static void porsche_analyze(TPMSProtocolDecoderPorsche* instance) {
    uint8_t b[10];
    porsche_get_bytes(instance, b);
    instance->generic.id =
        ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    // Pressure raw -> kPa (raw*2.5 - 100) -> bar for display.
    instance->generic.pressure = ((float)b[4] * 2.5f - 100.0f) * 0.01f;
    instance->generic.temperature = (float)((int)b[5] - 40);
    // b[6..7] are status flags (undefined). No battery bit is documented.
    instance->generic.battery_low = TPMS_NO_BATT;
}

void tpms_protocol_decoder_porsche_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderPorsche* instance = context;
    UNUSED(level); // DMC slicing is purely pulse-width based

    PorschePulse pulse = porsche_classify(duration);
    if(pulse == PorschePulseReset) {
        tpms_protocol_decoder_porsche_reset(instance);
        return;
    }

    // Differential-Manchester slice: LONG => '1', pair of SHORTs => '0'.
    bool have_bit = false;
    bool bit = false;
    if(pulse == PorschePulseLong) {
        if(instance->half_pending) {
            // LONG in mid-bit => phase lost, resync from scratch.
            tpms_protocol_decoder_porsche_reset(instance);
            return;
        }
        have_bit = true;
        bit = true;
    } else { // PorschePulseShort
        if(instance->half_pending) {
            instance->half_pending = false;
            have_bit = true;
            bit = false;
        } else {
            instance->half_pending = true;
        }
    }
    if(!have_bit) return;

    switch(instance->decoder.parser_step) {
    case PorscheDecoderStepReset:
        instance->preamble_ones = 0;
        instance->decoder.parser_step = PorscheDecoderStepFindPreamble;
        /* fallthrough */
    case PorscheDecoderStepFindPreamble:
        if(bit) {
            if(instance->preamble_ones < 0xFFFF) instance->preamble_ones++;
        } else {
            if(instance->preamble_ones >= PORSCHE_PREAMBLE_MIN_ONES) {
                // Terminating '0' of the preamble; payload starts next bit.
                instance->decoder.parser_step = PorscheDecoderStepData;
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                instance->head_64_bit = 0;
            } else {
                instance->preamble_ones = 0;
            }
        }
        break;

    case PorscheDecoderStepData:
        subghz_protocol_blocks_add_to_128_bit(
            &instance->decoder, bit ? 1 : 0, &instance->head_64_bit);
        if(instance->decoder.decode_count_bit ==
           tpms_protocol_porsche_const.min_count_bit_for_found) {
            if(porsche_check_crc(instance)) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                porsche_analyze(instance);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = PorscheDecoderStepReset;
            instance->preamble_ones = 0;
        }
        break;
    }
}

uint8_t tpms_protocol_decoder_porsche_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderPorsche* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_porsche_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderPorsche* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_porsche_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderPorsche* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_porsche_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_porsche_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderPorsche* instance = context;
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
