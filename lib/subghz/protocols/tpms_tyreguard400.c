#include "tpms_tyreguard400.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSTyreGuard400"

// Port of rtl_433/src/devices/tpms_tyreguard400.c (GPL-2+).
//
// TyreGuard 400 (Fit2Go / iTPMS style) tyre pressure monitor.
// 434.1 MHz, OOK (ASK) Manchester encoded (OOK_MC_ZEROBIT), 100 us symbol.
// 88-bit message (11 bytes) follows a 28-bit frame sync 0xfd5fd5f.
//
// Wire format (after Manchester-decoding, 11 bytes b[0..10]):
//   b[0..2]  = header / manufacturer (not decoded, covered by CRC)
//   b[3]     = low nibble is the top 4 bits of the 28-bit ID
//   b[4..6]  = remaining 24 bits of the ID
//   b[7]     = pressure LSB (combined with flags 0x70 → +256/512/1024 kPa)
//   b[8]     = temperature in °C + 40
//   b[9]     = flags (bit0 batt_low?, bits0..1 leaking/peering, bit3 ack_leaking)
//   b[10]    = CRC-8, poly 0x31, init 0xdd, final 0x00 over b[0..9]
//
// CRC check: crc8(b, 11, 0x31, 0xdd) == 0.
//
// Frame sync raw bits 0xfd5fd5f (28 bits) → we hunt for the trailing 28 bits.

#define TYREGUARD400_PREAMBLE      0x0FD5FD5FUL
#define TYREGUARD400_PREAMBLE_MASK 0x0FFFFFFFUL
#define TYREGUARD400_DATA_BITS     88

static const SubGhzBlockConst tpms_protocol_tyreguard400_const = {
    .te_short = 100,
    .te_long = 200,
    .te_delta = 50,
    .min_count_bit_for_found = TYREGUARD400_DATA_BITS,
};

typedef enum {
    TyreGuard400DecoderStepReset = 0,
    TyreGuard400DecoderStepFindPreamble,
    TyreGuard400DecoderStepData,
} TyreGuard400DecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    ManchesterState manchester_saved_state;
    uint32_t header_shift;
    uint64_t head_64_bit;
} TPMSProtocolDecoderTyreGuard400;

const SubGhzProtocolDecoder tpms_protocol_tyreguard400_decoder = {
    .alloc = tpms_protocol_decoder_tyreguard400_alloc,
    .free = tpms_protocol_decoder_tyreguard400_free,
    .feed = tpms_protocol_decoder_tyreguard400_feed,
    .reset = tpms_protocol_decoder_tyreguard400_reset,
    .get_hash_data = tpms_protocol_decoder_tyreguard400_get_hash_data,
    .serialize = tpms_protocol_decoder_tyreguard400_serialize,
    .deserialize = tpms_protocol_decoder_tyreguard400_deserialize,
    .get_string = tpms_protocol_decoder_tyreguard400_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_tyreguard400_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_tyreguard400 = {
    .name = TPMS_PROTOCOL_TYREGUARD400_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,
    .decoder = &tpms_protocol_tyreguard400_decoder,
    .encoder = &tpms_protocol_tyreguard400_encoder,
};

void* tpms_protocol_decoder_tyreguard400_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderTyreGuard400* instance = malloc(sizeof(TPMSProtocolDecoderTyreGuard400));
    instance->base.protocol = &tpms_protocol_tyreguard400;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_tyreguard400_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_tyreguard400_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderTyreGuard400* instance = context;
    instance->decoder.parser_step = TyreGuard400DecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->head_64_bit = 0;
    instance->header_shift = 0;
}

static ManchesterEvent tyreguard400_evt(bool level, uint32_t duration) {
    bool is_long = false;
    if(DURATION_DIFF(duration, tpms_protocol_tyreguard400_const.te_long) <
       tpms_protocol_tyreguard400_const.te_delta) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_tyreguard400_const.te_short) <
        tpms_protocol_tyreguard400_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }
    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

// rtl_433 crc8: MSB-first, no reflection.
static uint8_t tyreguard400_crc8(const uint8_t* msg, unsigned len, uint8_t poly, uint8_t init) {
    uint8_t remainder = init;
    for(unsigned b = 0; b < len; b++) {
        remainder ^= msg[b];
        for(int bit = 0; bit < 8; bit++) {
            if(remainder & 0x80)
                remainder = (uint8_t)((remainder << 1) ^ poly);
            else
                remainder = (uint8_t)(remainder << 1);
        }
    }
    return remainder;
}

static void tyreguard400_get_bytes(TPMSProtocolDecoderTyreGuard400* instance, uint8_t b[11]) {
    // 88 bits total: head_64_bit lower 24 bits = b[0..2] (MSB-first),
    // decode_data 64 bits = b[3..10].
    b[0] = (uint8_t)((instance->head_64_bit >> 16) & 0xFF);
    b[1] = (uint8_t)((instance->head_64_bit >> 8) & 0xFF);
    b[2] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 3] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

static bool tyreguard400_check_crc(TPMSProtocolDecoderTyreGuard400* instance) {
    uint8_t b[11];
    tyreguard400_get_bytes(instance, b);
    return tyreguard400_crc8(b, 11, 0x31, 0xdd) == 0;
}

static void tyreguard400_analyze(TPMSProtocolDecoderTyreGuard400* instance) {
    uint8_t b[11];
    tyreguard400_get_bytes(instance, b);

    // 28-bit sensor ID.
    instance->generic.id =
        (uint32_t)(((b[3] & 0x0F) << 24) | (b[4] << 16) | (b[5] << 8) | b[6]);

    uint8_t flags = b[9];
    // Pressure in kPa: byte 7 plus top-bit expansion from flags 0x70.
    int pressure_kpa = b[7] | ((flags & 0x70) << 4);
    instance->generic.pressure = (float)pressure_kpa * 0.01f; // kPa → bar
    instance->generic.temperature = (float)((int)b[8] - 40);
    // flags bit0 suspected battery-low indicator (TBC in rtl_433).
    instance->generic.battery_low = (uint8_t)(flags & 0x01);
}

void tpms_protocol_decoder_tyreguard400_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderTyreGuard400* instance = context;

    ManchesterEvent event = tyreguard400_evt(level, duration);
    if(event == ManchesterEventReset) {
        instance->decoder.parser_step = TyreGuard400DecoderStepReset;
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
    case TyreGuard400DecoderStepReset:
        instance->header_shift = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        instance->decoder.parser_step = TyreGuard400DecoderStepFindPreamble;
        /* fallthrough */
    case TyreGuard400DecoderStepFindPreamble:
        instance->header_shift =
            ((instance->header_shift << 1) | (bit ? 1 : 0)) & TYREGUARD400_PREAMBLE_MASK;
        if(instance->header_shift == TYREGUARD400_PREAMBLE) {
            instance->decoder.parser_step = TyreGuard400DecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->head_64_bit = 0;
        }
        break;

    case TyreGuard400DecoderStepData:
        subghz_protocol_blocks_add_to_128_bit(
            &instance->decoder, bit ? 1 : 0, &instance->head_64_bit);
        if(instance->decoder.decode_count_bit ==
           tpms_protocol_tyreguard400_const.min_count_bit_for_found) {
            if(tyreguard400_check_crc(instance)) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                tyreguard400_analyze(instance);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = TyreGuard400DecoderStepReset;
        }
        break;
    }
}

uint8_t tpms_protocol_decoder_tyreguard400_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderTyreGuard400* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_tyreguard400_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderTyreGuard400* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_tyreguard400_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderTyreGuard400* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_tyreguard400_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_tyreguard400_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderTyreGuard400* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%07lX Batt:%d\r\n"
        "Temp:%2.0f C Bar:%2.1f",
        instance->generic.protocol_name,
        instance->generic.id,
        (int)instance->generic.battery_low,
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}
