#include "tpms_bmw_g3.h"

#define TAG "TPMSBmwG3"

// Port of rtl_433/src/devices/tpms_bmw_g3.c
//   (Copyright (C) 2024 Bruno OCTAU (ProfBoc75), @Billymazze, GPL-2+)
//
// BMW Gen2 / Gen3 TPMS sensor.
// RF: FSK, PCM, s = l = 52 us, DIFFERENTIAL Manchester encoded.
// 433.92 MHz (EU) / 315 MHz (US).
//
// Unlike the Ford/Citroen ports (plain Manchester via manchester_decoder.h),
// this signal is differential-Manchester over a PCM bit clock. We therefore
// mirror rtl_433's two stages directly on the LevelDuration pulse stream:
//   1. Slice each pulse into 52 us PCM raw bits (value = RF level).
//   2. Hunt the 16-bit raw preamble 0xCCCD, then differential-Manchester
//      decode the trailing raw bits into the data bitstream, exactly as
//      bitbuffer_differential_manchester_decode() does.
//
// Wire format after DMC decode:
//   Gen3 {89} 11 bytes: [II II II II PP TT F1 F2 F3] CC CC (+1 trailing bit)
//   Gen2 {81} 10 bytes: [II II II II PP TT F1 F2]       CC CC (+1 trailing bit)
//     I  = 32-bit sensor ID           (b[0..3], big-endian)
//     PP = pressure   kPa = (PP - 43) * 2.5   (b[4])
//     TT = temperature C = TT - 40             (b[5])
//     F1 = flags1 (b[6], often 0xF8 = brand?)
//     F2 = flags2 (b[7], battery / pressure warning?)
//     F3 = flags3 (b[8], Gen3 only, often 0x03)
//     CC = CRC-16, poly 0x1021, init 0x0000, over the whole frame incl. CRC
//          (crc16(b, nbytes) == 0 when valid).

#define BMW_TE       52 // us per PCM half-bit (s = l)
#define BMW_TE_HALF  26
#define BMW_PREAMBLE 0xCCCDU
#define BMW_G3_BITS  88 // 11 bytes
#define BMW_G2_BITS  80 // 10 bytes
#define BMW_RAW_MAX  192 // raw PCM bits after preamble (88 data bits -> 176 raw + slack)
#define BMW_DEC_MAX  96 // max decoded data bits (12 bytes)

static const SubGhzBlockConst tpms_protocol_bmw_g3_const = {
    .te_short = BMW_TE,
    .te_long = BMW_TE,
    .te_delta = 25,
    .min_count_bit_for_found = BMW_G3_BITS,
};

typedef enum {
    BmwG3DecoderStepFindPreamble = 0,
    BmwG3DecoderStepData,
} BmwG3DecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    uint16_t header_shift;
    uint16_t raw_count;
    uint8_t raw[BMW_RAW_MAX];
} TPMSProtocolDecoderBmwG3;

const SubGhzProtocolDecoder tpms_protocol_bmw_g3_decoder = {
    .alloc = tpms_protocol_decoder_bmw_g3_alloc,
    .free = tpms_protocol_decoder_bmw_g3_free,
    .feed = tpms_protocol_decoder_bmw_g3_feed,
    .reset = tpms_protocol_decoder_bmw_g3_reset,
    .get_hash_data = tpms_protocol_decoder_bmw_g3_get_hash_data,
    .serialize = tpms_protocol_decoder_bmw_g3_serialize,
    .deserialize = tpms_protocol_decoder_bmw_g3_deserialize,
    .get_string = tpms_protocol_decoder_bmw_g3_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_bmw_g3_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_bmw_g3 = {
    .name = TPMS_PROTOCOL_BMW_G3_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,
    .decoder = &tpms_protocol_bmw_g3_decoder,
    .encoder = &tpms_protocol_bmw_g3_encoder,
};

void* tpms_protocol_decoder_bmw_g3_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderBmwG3* instance = malloc(sizeof(TPMSProtocolDecoderBmwG3));
    instance->base.protocol = &tpms_protocol_bmw_g3;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_bmw_g3_free(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderBmwG3* instance = context;
    free(instance);
}

void tpms_protocol_decoder_bmw_g3_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderBmwG3* instance = context;
    instance->decoder.parser_step = BmwG3DecoderStepFindPreamble;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->header_shift = 0;
    instance->raw_count = 0;
}

// rtl_433 crc16: MSB-first, poly/init parametric. Returns 0 over a valid frame
// (the two CRC bytes are included in the message).
static uint16_t bmw_crc16(const uint8_t* message, unsigned n_bytes) {
    uint16_t remainder = 0x0000;
    for(unsigned byte = 0; byte < n_bytes; ++byte) {
        remainder ^= (uint16_t)message[byte] << 8;
        for(int bit = 0; bit < 8; ++bit) {
            if(remainder & 0x8000)
                remainder = (uint16_t)((remainder << 1) ^ 0x1021);
            else
                remainder = (uint16_t)(remainder << 1);
        }
    }
    return remainder;
}

// Faithful port of bitbuffer_differential_manchester_decode() operating on a
// flat array of raw PCM bits. Returns the number of decoded data bits in out[].
static unsigned bmw_dmc_decode(const uint8_t* bits, unsigned len, uint8_t* out, unsigned max) {
    unsigned ipos = 0;
    unsigned opos = 0;
    uint8_t bit1;
    uint8_t bit2 = 0;

    if(max && len > max * 2) len = max * 2;

#define BMW_BIT_AT(i) (((i) < len) ? bits[(i)] : 0)

    // Establish clock phase; skip one short pulse if needed to get in sync.
    while(ipos < len) {
        bit1 = BMW_BIT_AT(ipos);
        ipos++;
        bit2 = BMW_BIT_AT(ipos);
        ipos++;
        uint8_t bit3 = BMW_BIT_AT(ipos);

        if(bit1 != bit2) {
            if(bit2 != bit3) {
                if(opos < max) out[opos++] = 0;
            } else {
                bit2 = bit1;
                ipos -= 1;
                break;
            }
        } else {
            bit2 = (uint8_t)(1 - bit1);
            ipos -= 2;
            break;
        }
    }

    while(ipos < len && opos < max) {
        bit1 = BMW_BIT_AT(ipos);
        ipos++;
        if(bit1 == bit2) break; // clock missing, abort
        bit2 = BMW_BIT_AT(ipos);
        ipos++;
        if(bit1 == bit2)
            out[opos++] = 1;
        else
            out[opos++] = 0;
    }

#undef BMW_BIT_AT
    return opos;
}

static void bmw_analyze(TPMSProtocolDecoderBmwG3* instance, const uint8_t* b, unsigned n_bytes) {
    instance->generic.id =
        ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];

    // kPa = (PP - 43) * 2.5 ; TPMSBlockGeneric.pressure is in bar.
    float pressure_kpa = ((float)b[4] - 43.0f) * 2.5f;
    instance->generic.pressure = pressure_kpa * 0.01f;
    instance->generic.temperature = (float)((int)b[5] - 40);
    // Battery bit mapping in the flag bytes is unconfirmed upstream; report as
    // unknown rather than guessing a false low-battery warning.
    instance->generic.battery_low = TPMS_NO_BATT;

    // Store the low 64 bits of the frame for hashing / serialization, mirroring
    // how the template stashes decode_data. (id/pressure/temp are persisted as
    // their own key-file fields, so reload does not depend on this.)
    unsigned start = (n_bytes > 8) ? (n_bytes - 8) : 0;
    uint64_t d = 0;
    for(unsigned i = start; i < n_bytes; i++) {
        d = (d << 8) | b[i];
    }
    instance->decoder.decode_data = d;
    instance->decoder.decode_count_bit = (uint8_t)(n_bytes * 8);
    instance->generic.data = d;
    instance->generic.data_count_bit = (uint8_t)(n_bytes * 8);
}

static void bmw_try_decode(TPMSProtocolDecoderBmwG3* instance) {
    if(instance->raw_count < BMW_G2_BITS * 2) return; // not enough for even Gen2

    uint8_t out[BMW_DEC_MAX];
    unsigned nbits = bmw_dmc_decode(instance->raw, instance->raw_count, out, BMW_DEC_MAX);

    uint8_t b[BMW_DEC_MAX / 8] = {0};
    for(unsigned i = 0; i < nbits && i < BMW_DEC_MAX; i++) {
        if(out[i]) b[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
    }

    unsigned n_bytes = 0;
    if(nbits >= BMW_G3_BITS && bmw_crc16(b, 11) == 0) {
        n_bytes = 11; // Gen3
    } else if(nbits >= BMW_G2_BITS && bmw_crc16(b, 10) == 0) {
        n_bytes = 10; // Gen2
    } else {
        return; // CRC failed -> reject (guards against false positives)
    }

    bmw_analyze(instance, b, n_bytes);
    if(instance->base.callback)
        instance->base.callback(&instance->base, instance->base.context);
}

void tpms_protocol_decoder_bmw_g3_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderBmwG3* instance = context;

    // Number of 52 us PCM slices this pulse represents (rounded).
    unsigned slices = (duration + BMW_TE_HALF) / BMW_TE;

    // In differential Manchester the longest run without a transition is two
    // half-bits (one "long"), so 3+ equal slices (or a glitch) ends the frame.
    if(slices == 0 || slices >= 3) {
        if(instance->decoder.parser_step == BmwG3DecoderStepData) {
            bmw_try_decode(instance);
        }
        instance->decoder.parser_step = BmwG3DecoderStepFindPreamble;
        instance->header_shift = 0;
        instance->raw_count = 0;
        return;
    }

    uint8_t bit = level ? 1 : 0;
    for(unsigned k = 0; k < slices; k++) {
        if(instance->decoder.parser_step == BmwG3DecoderStepFindPreamble) {
            instance->header_shift = (uint16_t)((instance->header_shift << 1) | bit);
            if(instance->header_shift == BMW_PREAMBLE) {
                instance->decoder.parser_step = BmwG3DecoderStepData;
                instance->raw_count = 0;
            }
        } else { // BmwG3DecoderStepData
            if(instance->raw_count < BMW_RAW_MAX) {
                instance->raw[instance->raw_count++] = bit;
            } else {
                bmw_try_decode(instance);
                instance->decoder.parser_step = BmwG3DecoderStepFindPreamble;
                instance->header_shift = 0;
                instance->raw_count = 0;
            }
        }
    }
}

uint8_t tpms_protocol_decoder_bmw_g3_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderBmwG3* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_bmw_g3_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderBmwG3* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_bmw_g3_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderBmwG3* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, tpms_protocol_bmw_g3_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_bmw_g3_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderBmwG3* instance = context;
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
