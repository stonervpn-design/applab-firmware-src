#include "tpms_ave.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSAve"

// Port of rtl_433/src/devices/tpms_ave.c (GPL-2+)
//
// FSK, DIFFERENTIAL Manchester Coded (DMC), PCM half-bit, 8 byte payload.
// 433.92 MHz (EU) / 315 MHz (US).
//
// rtl_433 r_device: FSK_PULSE_PCM, short_width = long_width = 100, reset_limit = 400.
// The raw preamble is {0xcc, 0xcc, 0xcc, 0xcd}; in the pulse domain that is a run of
// ~100us half-bits paired into full DMC bits that all decode to '1', terminated by a
// single '0' (the trailing ...0d nibble), after which the 64 payload bits begin. This
// matches the Porsche DMC family: equal PCM pair (one LONG ~200us pulse) => '1',
// differing PCM pair (two SHORT ~100us pulses, a mid-bit transition) => '0'
// (rtl_433 bitbuffer_differential_manchester_decode).
//
// Wire format (after DMC-decoding, MSB-first, 8 bytes):
//   II II II II PP TT FF CC
//   I = ID (32 bit)
//   P = Pressure raw. mode = FF>>6: kPa = (raw - offset) * ratio
//       mode 0: ratio 2.352, offset 47.0   mode 1: ratio 2.352, offset 0.0
//       mode 2: ratio 5.491, offset 18.2   mode 3: ratio 5.491, offset 0.0
//   T = Temperature: degC = raw - 50
//   F = mode (bit7..6) | battery (bit5..3) | flags (bit2..0)
//       battery_raw == 7 => low (25%), == 6 => 75%, else full (100%)
//   C = Checksum: CRC-8 poly 0x31 init 0xff over all 8 bytes, == 0 when valid

#define AVE_DATA_BITS         64
#define AVE_PREAMBLE_MIN_ONES 10

static const SubGhzBlockConst tpms_protocol_ave_const = {
    .te_short = 100,
    .te_long = 200,
    .te_delta = 30,
    .min_count_bit_for_found = AVE_DATA_BITS,
};

typedef enum {
    AveDecoderStepReset = 0,
    AveDecoderStepFindPreamble,
    AveDecoderStepData,
} AveDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    uint16_t preamble_ones;
    bool half_pending; // one SHORT half-bit seen, awaiting its pair
} TPMSProtocolDecoderAve;

const SubGhzProtocolDecoder tpms_protocol_ave_decoder = {
    .alloc = tpms_protocol_decoder_ave_alloc,
    .free = tpms_protocol_decoder_ave_free,
    .feed = tpms_protocol_decoder_ave_feed,
    .reset = tpms_protocol_decoder_ave_reset,
    .get_hash_data = tpms_protocol_decoder_ave_get_hash_data,
    .serialize = tpms_protocol_decoder_ave_serialize,
    .deserialize = tpms_protocol_decoder_ave_deserialize,
    .get_string = tpms_protocol_decoder_ave_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_ave_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_ave = {
    .name = TPMS_PROTOCOL_AVE_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,
    .decoder = &tpms_protocol_ave_decoder,
    .encoder = &tpms_protocol_ave_encoder,
};

void* tpms_protocol_decoder_ave_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderAve* instance = calloc(1, sizeof(TPMSProtocolDecoderAve));
    instance->base.protocol = &tpms_protocol_ave;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_ave_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_ave_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderAve* instance = context;
    instance->decoder.parser_step = AveDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->preamble_ones = 0;
    instance->half_pending = false;
}

typedef enum {
    AvePulseShort = 0,
    AvePulseLong,
    AvePulseReset,
} AvePulse;

static AvePulse ave_classify(uint32_t duration) {
    if(DURATION_DIFF(duration, tpms_protocol_ave_const.te_long) <
       tpms_protocol_ave_const.te_delta * 2) {
        return AvePulseLong;
    }
    if(DURATION_DIFF(duration, tpms_protocol_ave_const.te_short) <
       tpms_protocol_ave_const.te_delta) {
        return AvePulseShort;
    }
    return AvePulseReset;
}

static void ave_get_bytes(TPMSProtocolDecoderAve* instance, uint8_t b[8]) {
    for(int i = 0; i < 8; i++) {
        b[i] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

// rtl_433 crc8(message, nBytes, polynomial, init): MSB-first, no reflection, no final
// xor. AVE uses crc8(b, 8, 0x31, 0xff); a valid frame yields 0.
static uint8_t ave_crc8(const uint8_t* data, size_t len, uint8_t polynomial, uint8_t init) {
    uint8_t remainder = init;
    for(size_t byte = 0; byte < len; byte++) {
        remainder ^= data[byte];
        for(int bit = 0; bit < 8; bit++) {
            if(remainder & 0x80) {
                remainder = (uint8_t)((remainder << 1) ^ polynomial);
            } else {
                remainder = (uint8_t)(remainder << 1);
            }
        }
    }
    return remainder;
}

static bool ave_check_crc(TPMSProtocolDecoderAve* instance) {
    uint8_t b[8];
    ave_get_bytes(instance, b);
    return ave_crc8(b, 8, 0x31, 0xff) == 0;
}

static void ave_analyze(TPMSProtocolDecoderAve* instance) {
    uint8_t b[8];
    ave_get_bytes(instance, b);
    instance->generic.id =
        ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];

    int pressure_raw = b[4];
    int mode = (b[6] >> 6) & 0x3;
    int battery_raw = (b[6] >> 3) & 0x7;

    float ratio;
    float offset;
    switch(mode) {
    case 0:
        ratio = 2.352f;
        offset = 47.0f;
        break;
    case 1:
    default:
        ratio = 2.352f;
        offset = 0.0f;
        break;
    case 2:
        ratio = 5.491f;
        offset = 18.2f;
        break;
    case 3:
        ratio = 5.491f;
        offset = 0.0f;
        break;
    }
    float pressure_kpa = ((float)pressure_raw - offset) * ratio;
    instance->generic.pressure = pressure_kpa * 0.01f; // kPa -> bar for display

    instance->generic.temperature = (float)((int)b[5] - 50);

    // battery_raw == 7 signals a low battery (rtl_433 battery_ok = battery_raw != 7).
    instance->generic.battery_low = (battery_raw == 7) ? 1 : 0;
}

void tpms_protocol_decoder_ave_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderAve* instance = context;
    UNUSED(level); // DMC slicing is purely pulse-width based

    AvePulse pulse = ave_classify(duration);
    if(pulse == AvePulseReset) {
        tpms_protocol_decoder_ave_reset(instance);
        return;
    }

    // Differential-Manchester slice: LONG => '1', pair of SHORTs => '0'.
    bool have_bit = false;
    bool bit = false;
    if(pulse == AvePulseLong) {
        if(instance->half_pending) {
            // LONG in mid-bit => phase lost, resync from scratch.
            tpms_protocol_decoder_ave_reset(instance);
            return;
        }
        have_bit = true;
        bit = true;
    } else { // AvePulseShort
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
    case AveDecoderStepReset:
        instance->preamble_ones = 0;
        instance->decoder.parser_step = AveDecoderStepFindPreamble;
        /* fallthrough */
    case AveDecoderStepFindPreamble:
        if(bit) {
            if(instance->preamble_ones < 0xFFFF) instance->preamble_ones++;
        } else {
            if(instance->preamble_ones >= AVE_PREAMBLE_MIN_ONES) {
                // Terminating '0' of the preamble; payload starts next bit.
                instance->decoder.parser_step = AveDecoderStepData;
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
            } else {
                instance->preamble_ones = 0;
            }
        }
        break;

    case AveDecoderStepData:
        subghz_protocol_blocks_add_bit(&instance->decoder, bit ? 1 : 0);
        if(instance->decoder.decode_count_bit ==
           tpms_protocol_ave_const.min_count_bit_for_found) {
            if(ave_check_crc(instance)) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                ave_analyze(instance);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = AveDecoderStepReset;
            instance->preamble_ones = 0;
        }
        break;
    }
}

uint8_t tpms_protocol_decoder_ave_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderAve* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_ave_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderAve* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_ave_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderAve* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, tpms_protocol_ave_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_ave_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderAve* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX Batt:%s\r\n"
        "Temp:%2.0f C Bar:%2.1f",
        instance->generic.protocol_name,
        instance->generic.id,
        instance->generic.battery_low ? "low" : "ok",
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}
