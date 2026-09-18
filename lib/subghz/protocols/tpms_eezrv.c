#include "tpms_eezrv.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSEezrv"

// Port of rtl_433/src/devices/tpms_eezrv.c (Bruno OCTAU / rtl_433, GPL-2+)
//
// EezTire E618, Carchet, TST-507 style RV/trailer TPMS sensors.
// OOK, Manchester (ZEROBIT) encoded, 433.92 MHz, packet sent twice.
//
// rtl_433 flow: manchester-decode -> invert whole bitbuffer -> search 0xffff
// preamble -> cc byte at (pos+16), 7 payload bytes b[] at (pos+24).
//
// Wire format (after Manchester-decode + invert, checksum-domain):
//   FF FF CC II II II PP TT F1 F2
//   FFFF     = 16-bit preamble (all ones post-invert / all zeros on-air)
//   CC       = checksum byte
//   b[0..2]  = 24-bit sensor ID (b0 = MSB of the printed "%02x%02x%02x")
//   b[3]     = pressure LSB (9-bit total, MSB is bit0 of b[6])
//   b[4]     = temperature, degrees C + 50
//   b[5]     = flags1: bit7 = battery low, bit5 = inflation, bit4 = fast leak
//   b[6]     = flags2: bit0 = pressure MSB
//
// Checksum: cs = SUM(b[0..6]); if(cs > 0xFF) cs |= 0x80; (cs & 0xFF) == CC
//
// We slice bits from the raw Manchester stream ourselves. Because the Flipper
// manchester decoder polarity relative to rtl_433's post-invert domain is not
// known without a capture, we accept the preamble in either polarity and gate
// on the checksum against both the decoded bytes and their bitwise inverse.
// The 8-bit checksum rejects false positives.

#define EEZRV_PREAMBLE_ONES  0xFFFFU
#define EEZRV_PREAMBLE_ZEROS 0x0000U
#define EEZRV_DATA_BITS      64 // CC (8) + 7 payload bytes (56)

static const SubGhzBlockConst tpms_protocol_eezrv_const = {
    .te_short = 50,
    .te_long = 100,
    .te_delta = 25,
    .min_count_bit_for_found = EEZRV_DATA_BITS,
};

typedef enum {
    EezrvDecoderStepReset = 0,
    EezrvDecoderStepFindPreamble,
    EezrvDecoderStepData,
} EezrvDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    ManchesterState manchester_saved_state;
    uint16_t header_shift;
} TPMSProtocolDecoderEezrv;

const SubGhzProtocolDecoder tpms_protocol_eezrv_decoder = {
    .alloc = tpms_protocol_decoder_eezrv_alloc,
    .free = tpms_protocol_decoder_eezrv_free,
    .feed = tpms_protocol_decoder_eezrv_feed,
    .reset = tpms_protocol_decoder_eezrv_reset,
    .get_hash_data = tpms_protocol_decoder_eezrv_get_hash_data,
    .serialize = tpms_protocol_decoder_eezrv_serialize,
    .deserialize = tpms_protocol_decoder_eezrv_deserialize,
    .get_string = tpms_protocol_decoder_eezrv_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_eezrv_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_eezrv = {
    .name = TPMS_PROTOCOL_EEZRV_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,
    .decoder = &tpms_protocol_eezrv_decoder,
    .encoder = &tpms_protocol_eezrv_encoder,
};

void* tpms_protocol_decoder_eezrv_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderEezrv* instance = malloc(sizeof(TPMSProtocolDecoderEezrv));
    instance->base.protocol = &tpms_protocol_eezrv;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_eezrv_free(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderEezrv* instance = context;
    free(instance);
}

void tpms_protocol_decoder_eezrv_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderEezrv* instance = context;
    instance->decoder.parser_step = EezrvDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->header_shift = 0;
}

static ManchesterEvent eezrv_level_and_duration_to_event(bool level, uint32_t duration) {
    bool is_long = false;
    if(DURATION_DIFF(duration, tpms_protocol_eezrv_const.te_long) <
       tpms_protocol_eezrv_const.te_delta * 2) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_eezrv_const.te_short) <
        tpms_protocol_eezrv_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }
    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

// Split the 64-bit decode_data into bytes: b[0] = CC, b[1..7] = payload b[0..6].
static void eezrv_get_bytes(TPMSProtocolDecoderEezrv* instance, uint8_t b[8]) {
    for(int i = 0; i < 8; i++) {
        b[i] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

// rtl_433: cs = add_bytes(b, 7); if(cs > 0xff) cs |= 0x80; (cs & 0xff) == cc[0]
static bool eezrv_check_checksum(const uint8_t b[8]) {
    int cs = 0;
    for(int i = 1; i < 8; i++) cs += b[i];
    if(cs > 0xFF) cs |= 0x80;
    return (uint8_t)(cs & 0xFF) == b[0];
}

static void eezrv_analyze(TPMSBlockGeneric* g, const uint8_t frame[8]) {
    // frame[0] = CC, frame[1..7] = payload b[0..6]
    const uint8_t* b = &frame[1];
    g->id = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];

    uint16_t pressure_raw = (uint16_t)((b[6] & 0x01) << 8) | b[3];
    float pressure_kpa = (float)pressure_raw * 2.5f;
    g->pressure = pressure_kpa * 0.01f; // kPa -> bar

    g->temperature = (float)((int)b[4] - 50);

    g->battery_low = (b[5] & 0x80) ? 1 : 0;
}

void tpms_protocol_decoder_eezrv_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderEezrv* instance = context;

    ManchesterEvent event = eezrv_level_and_duration_to_event(level, duration);
    if(event == ManchesterEventReset) {
        instance->decoder.parser_step = EezrvDecoderStepReset;
        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;
        instance->header_shift = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        return;
    }

    bool bit = false;
    bool have_bit = manchester_advance(
        instance->manchester_saved_state, event, &instance->manchester_saved_state, &bit);
    if(!have_bit) return;

    switch(instance->decoder.parser_step) {
    case EezrvDecoderStepReset:
        instance->header_shift = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        instance->decoder.parser_step = EezrvDecoderStepFindPreamble;
        /* fallthrough */
    case EezrvDecoderStepFindPreamble:
        instance->header_shift = (uint16_t)((instance->header_shift << 1) | (bit ? 1 : 0));
        if(instance->header_shift == EEZRV_PREAMBLE_ONES ||
           instance->header_shift == EEZRV_PREAMBLE_ZEROS) {
            instance->decoder.parser_step = EezrvDecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
        }
        break;

    case EezrvDecoderStepData:
        subghz_protocol_blocks_add_bit(&instance->decoder, bit ? 1 : 0);
        if(instance->decoder.decode_count_bit ==
           tpms_protocol_eezrv_const.min_count_bit_for_found) {
            uint8_t frame[8];
            eezrv_get_bytes(instance, frame);

            bool ok = eezrv_check_checksum(frame);
            if(!ok) {
                // Try the inverted polarity (Flipper manchester convention may be
                // opposite to rtl_433's post-invert domain).
                for(int i = 0; i < 8; i++) frame[i] ^= 0xFF;
                ok = eezrv_check_checksum(frame);
            }

            if(ok) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                eezrv_analyze(&instance->generic, frame);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = EezrvDecoderStepReset;
        }
        break;
    }
}

uint8_t tpms_protocol_decoder_eezrv_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderEezrv* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_eezrv_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderEezrv* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_eezrv_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderEezrv* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_eezrv_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_eezrv_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderEezrv* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%06lX Batt:%s\r\n"
        "Press:%3.2f bar Temp:%2.0f C",
        instance->generic.protocol_name,
        instance->generic.id,
        instance->generic.battery_low ? "low" : "ok",
        (double)instance->generic.pressure,
        (double)instance->generic.temperature);
}
