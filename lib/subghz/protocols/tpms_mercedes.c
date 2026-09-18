#include "tpms_mercedes.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSMercedes"

// Port of rtl_433/src/devices/tpms_mercedes_benz.c (GPL-2+)
//
// FSK, Manchester encoded (FSK_PULSE_MANCHESTER_ZEROBIT, ~25us half-bit),
// 80-bit / 10-byte payload. Seen on Mercedes-Benz Sprinter and related.
// 433.92 MHz (EU) / 315 MHz (US).
//
// Wire format (after Manchester-decoding, MSB-first bytes):
//   ST II II II II PP TT CC FF XX          (10 bytes total)
//   b[0]     = ST  state/family (0x83 stationary, 0xa3 moving)
//   b[1..4]  = II  32-bit sensor ID
//   b[5]     = PP  pressure raw, PSI = raw / 2.75
//   b[6]     = TT  temperature, degC = raw - 51
//   b[7]     = CC  counter (low 5 bits) + flags (high 3 bits)
//   b[8]     = FF  status flags (observed 0x6b/0xe9/0xea)
//   b[9]     = XX  CRC-8, poly 0x2f, init 0xaa, over b[0..9] must equal 0
//
// Preamble (decoded bits): 12-bit 0x002 (or inverted 0xff2). We hunt the
// trailing 12 bits and then collect the 80 data bits. The CRC rejects any
// false lock (including a wrong-polarity Manchester slice).

#define MERCEDES_PREAMBLE 0x002U
#define MERCEDES_PREAMBLE_INV 0xFF2U
#define MERCEDES_PREAMBLE_MASK 0xFFFU
#define MERCEDES_DATA_BITS 80

static const SubGhzBlockConst tpms_protocol_mercedes_const = {
    .te_short = 25,
    .te_long = 50,
    .te_delta = 12,
    .min_count_bit_for_found = MERCEDES_DATA_BITS,
};

typedef enum {
    MercedesDecoderStepReset = 0,
    MercedesDecoderStepFindPreamble,
    MercedesDecoderStepData,
} MercedesDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    ManchesterState manchester_saved_state;
    uint16_t header_shift;
    uint64_t head_64_bit;
} TPMSProtocolDecoderMercedes;

const SubGhzProtocolDecoder tpms_protocol_mercedes_decoder = {
    .alloc = tpms_protocol_decoder_mercedes_alloc,
    .free = tpms_protocol_decoder_mercedes_free,
    .feed = tpms_protocol_decoder_mercedes_feed,
    .reset = tpms_protocol_decoder_mercedes_reset,
    .get_hash_data = tpms_protocol_decoder_mercedes_get_hash_data,
    .serialize = tpms_protocol_decoder_mercedes_serialize,
    .deserialize = tpms_protocol_decoder_mercedes_deserialize,
    .get_string = tpms_protocol_decoder_mercedes_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_mercedes_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_mercedes = {
    .name = TPMS_PROTOCOL_MERCEDES_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,
    .decoder = &tpms_protocol_mercedes_decoder,
    .encoder = &tpms_protocol_mercedes_encoder,
};

void* tpms_protocol_decoder_mercedes_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderMercedes* instance = malloc(sizeof(TPMSProtocolDecoderMercedes));
    instance->base.protocol = &tpms_protocol_mercedes;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_mercedes_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_mercedes_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderMercedes* instance = context;
    instance->decoder.parser_step = MercedesDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->head_64_bit = 0;
    instance->header_shift = 0;
}

static ManchesterEvent mercedes_evt(bool level, uint32_t duration) {
    bool is_long = false;
    if(DURATION_DIFF(duration, tpms_protocol_mercedes_const.te_long) <
       tpms_protocol_mercedes_const.te_delta * 2) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_mercedes_const.te_short) <
        tpms_protocol_mercedes_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }
    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

static void mercedes_get_bytes(TPMSProtocolDecoderMercedes* instance, uint8_t b[10]) {
    // 80 bits total: head_64_bit lower 16 bits = b[0..1] (MSB-first),
    // decode_data 64 bits = b[2..9].
    b[0] = (uint8_t)((instance->head_64_bit >> 8) & 0xFF);
    b[1] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 2] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

// rtl_433 crc8: MSB-first, poly in normal form, no final xor.
static uint8_t mercedes_crc8(const uint8_t* message, unsigned n_bytes, uint8_t poly, uint8_t init) {
    uint8_t remainder = init;
    for(unsigned byte = 0; byte < n_bytes; ++byte) {
        remainder ^= message[byte];
        for(unsigned bit = 0; bit < 8; ++bit) {
            if(remainder & 0x80) {
                remainder = (uint8_t)((remainder << 1) ^ poly);
            } else {
                remainder = (uint8_t)(remainder << 1);
            }
        }
    }
    return remainder;
}

static bool mercedes_check_crc(TPMSProtocolDecoderMercedes* instance) {
    uint8_t b[10];
    mercedes_get_bytes(instance, b);
    // Valid frame: CRC-8 over all 10 bytes (b[9] is the checksum) equals 0.
    return mercedes_crc8(b, 10, 0x2f, 0xaa) == 0;
}

static void mercedes_analyze(TPMSProtocolDecoderMercedes* instance) {
    uint8_t b[10];
    mercedes_get_bytes(instance, b);
    instance->generic.id =
        ((uint32_t)b[1] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 8) | b[4];
    // Pressure raw → PSI (raw / 2.75) → bar
    float psi = (float)b[5] / 2.75f;
    instance->generic.pressure = psi * 0.06895f;
    instance->generic.temperature = (float)((int)b[6] - 51);
    // No confirmed battery bit in this protocol.
    instance->generic.battery_low = TPMS_NO_BATT;
}

void tpms_protocol_decoder_mercedes_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderMercedes* instance = context;

    ManchesterEvent event = mercedes_evt(level, duration);
    if(event == ManchesterEventReset) {
        instance->decoder.parser_step = MercedesDecoderStepReset;
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
    case MercedesDecoderStepReset:
        instance->header_shift = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        instance->decoder.parser_step = MercedesDecoderStepFindPreamble;
        /* fallthrough */
    case MercedesDecoderStepFindPreamble:
        instance->header_shift =
            (uint16_t)(((instance->header_shift << 1) | (bit ? 1 : 0)) & MERCEDES_PREAMBLE_MASK);
        if(instance->header_shift == MERCEDES_PREAMBLE ||
           instance->header_shift == MERCEDES_PREAMBLE_INV) {
            instance->decoder.parser_step = MercedesDecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->head_64_bit = 0;
        }
        break;

    case MercedesDecoderStepData:
        subghz_protocol_blocks_add_to_128_bit(
            &instance->decoder, bit ? 1 : 0, &instance->head_64_bit);
        if(instance->decoder.decode_count_bit ==
           tpms_protocol_mercedes_const.min_count_bit_for_found) {
            if(mercedes_check_crc(instance)) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                mercedes_analyze(instance);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = MercedesDecoderStepReset;
        }
        break;
    }
}

uint8_t tpms_protocol_decoder_mercedes_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderMercedes* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_mercedes_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderMercedes* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_mercedes_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderMercedes* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_mercedes_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_mercedes_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderMercedes* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX Batt:0x%02X\r\n"
        "Temp:%2.0f C Bar:%2.1f",
        instance->generic.protocol_name,
        instance->generic.id,
        instance->generic.battery_low,
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}
