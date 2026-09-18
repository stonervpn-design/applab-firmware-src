#include "tpms_nissan.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSNissan"

// Port of rtl_433/src/devices/tpms_nissan.c (Alex Wilson 2021, GPL-2+)
//
// FSK, Manchester (G.E. Thomas) encoded, 37-bit checksummed payload.
// rtl_433 decodes standard Manchester then bitbuffer_invert()s the result,
// so we collect the sliced bits and invert them before parsing (see below).
// 315 MHz (US) / 433.92 MHz (EU).
//
// Wire format (5 bytes, MSB-first, after Manchester-decode + invert):
//   MODE:3  ID:24  PRESSURE:8  UNKNOWN:2      (37 bits)
//   b[0] = MODE(3) | ID[23:19]
//   b[1] = ID[18:11]
//   b[2] = ID[10:3]
//   b[3] = ID[2:0] | PRESSURE[7:3]
//   b[4] = PRESSURE[2:0] | UNKNOWN(2) | pad   (top 5 bits used)
//   id        = ((b[0]&0x1F)<<19)|(b[1]<<11)|(b[2]<<3)|(b[3]>>5)
//   pressure  = (((b[3]&0x1F)<<3)|(b[4]>>5)) / 4.0 - 3.0   [PSI]
//   checksum  = sum2N over b[0..4], valid when (~chk & 0x03) == 0
//   (no temperature and no battery field in this protocol)
//
// Preamble (raw): f5 55 55 55 e0 (36 bits). Mirroring tpms_ford.c we hunt the
// trailing 16 bits of the preamble (0x555E) in the Manchester-sliced stream.

#define NISSAN_PREAMBLE 0x555EU
#define NISSAN_PREAMBLE_BITS 16
#define NISSAN_DATA_BITS 37

static const SubGhzBlockConst tpms_protocol_nissan_const = {
    .te_short = 120,
    .te_long = 240,
    .te_delta = 50,
    .min_count_bit_for_found = NISSAN_DATA_BITS,
};

typedef enum {
    NissanDecoderStepReset = 0,
    NissanDecoderStepFindPreamble,
    NissanDecoderStepData,
} NissanDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    ManchesterState manchester_saved_state;
    uint16_t header_shift;
} TPMSProtocolDecoderNissan;

const SubGhzProtocolDecoder tpms_protocol_nissan_decoder = {
    .alloc = tpms_protocol_decoder_nissan_alloc,
    .free = tpms_protocol_decoder_nissan_free,
    .feed = tpms_protocol_decoder_nissan_feed,
    .reset = tpms_protocol_decoder_nissan_reset,
    .get_hash_data = tpms_protocol_decoder_nissan_get_hash_data,
    .serialize = tpms_protocol_decoder_nissan_serialize,
    .deserialize = tpms_protocol_decoder_nissan_deserialize,
    .get_string = tpms_protocol_decoder_nissan_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_nissan_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_nissan = {
    .name = TPMS_PROTOCOL_NISSAN_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,

    .decoder = &tpms_protocol_nissan_decoder,
    .encoder = &tpms_protocol_nissan_encoder,
};

void* tpms_protocol_decoder_nissan_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderNissan* instance = malloc(sizeof(TPMSProtocolDecoderNissan));
    instance->base.protocol = &tpms_protocol_nissan;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_nissan_free(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderNissan* instance = context;
    free(instance);
}

void tpms_protocol_decoder_nissan_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderNissan* instance = context;
    instance->decoder.parser_step = NissanDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->header_shift = 0;
}

static ManchesterEvent nissan_level_and_duration_to_event(bool level, uint32_t duration) {
    bool is_long = false;
    if(DURATION_DIFF(duration, tpms_protocol_nissan_const.te_long) <
       tpms_protocol_nissan_const.te_delta * 2) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_nissan_const.te_short) <
        tpms_protocol_nissan_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }
    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

// sum2N checksum, verbatim from rtl_433 tpms_nissan.c. Expects (~chk & 0x03) == 0.
static bool tpms_protocol_nissan_check_checksum(const uint8_t b[5]) {
    uint8_t chk = 0;
    for(int i = 0; i < 4; i++) {
        chk += b[i] >> 7;
        chk += b[i] >> 5;
        chk += b[i] >> 3;
        chk += b[i] >> 1;
        chk += (uint8_t)(b[i] << 1);
    }
    chk += b[4] >> 7;
    chk += b[4] >> 5;
    chk += b[4] >> 3;
    return (uint8_t)(~chk & 0x03) == 0;
}

// Split the (already inverted) 37-bit value into rtl_433's MSB-first b[0..4].
static void tpms_protocol_nissan_get_bytes(uint64_t data37, uint8_t b[5]) {
    b[0] = (uint8_t)((data37 >> 29) & 0xFF);
    b[1] = (uint8_t)((data37 >> 21) & 0xFF);
    b[2] = (uint8_t)((data37 >> 13) & 0xFF);
    b[3] = (uint8_t)((data37 >> 5) & 0xFF);
    b[4] = (uint8_t)((data37 & 0x1F) << 3); // 5 real bits, left-aligned like the bitbuffer
}

static void tpms_protocol_nissan_analyze(TPMSBlockGeneric* g) {
    uint8_t b[5];
    tpms_protocol_nissan_get_bytes(g->data, b);

    // TPMS_ID:24h
    g->id = (uint32_t)(((uint32_t)(b[0] & 0x1F) << 19) | ((uint32_t)b[1] << 11) |
                       ((uint32_t)b[2] << 3) | (b[3] >> 5));

    // (PSI+3)*4 = 8d
    int pressure_raw = ((b[3] & 0x1F) << 3) | (b[4] >> 5);
    float pressure_psi = (float)(pressure_raw / 4.0f - 3.0f);
    g->pressure = pressure_psi * 0.0689476f; // PSI -> bar (TPMSBlockGeneric stores bar)

    // Nissan carries no temperature or battery status.
    g->temperature = -100.0f; // sentinel: not present
    g->battery_low = TPMS_NO_BATT;
}

void tpms_protocol_decoder_nissan_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderNissan* instance = context;

    ManchesterEvent event = nissan_level_and_duration_to_event(level, duration);
    if(event == ManchesterEventReset) {
        instance->decoder.parser_step = NissanDecoderStepReset;
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
    case NissanDecoderStepReset:
        instance->header_shift = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        instance->decoder.parser_step = NissanDecoderStepFindPreamble;
        // fall through to consume this bit as preamble candidate
        /* fallthrough */
    case NissanDecoderStepFindPreamble:
        instance->header_shift = (uint16_t)((instance->header_shift << 1) | (bit ? 1 : 0));
        if(instance->header_shift == NISSAN_PREAMBLE) {
            instance->decoder.parser_step = NissanDecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
        }
        break;

    case NissanDecoderStepData:
        subghz_protocol_blocks_add_bit(&instance->decoder, bit ? 1 : 0);
        if(instance->decoder.decode_count_bit ==
           tpms_protocol_nissan_const.min_count_bit_for_found) {
            // rtl_433 inverts the Manchester (G.E. Thomas) decoded payload.
            uint64_t inverted = (~instance->decoder.decode_data) & 0x1FFFFFFFFFULL; // 37-bit mask
            uint8_t b[5];
            tpms_protocol_nissan_get_bytes(inverted, b);
            if(tpms_protocol_nissan_check_checksum(b)) {
                instance->generic.data = inverted;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                tpms_protocol_nissan_analyze(&instance->generic);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = NissanDecoderStepReset;
        }
        break;
    }
}

uint8_t tpms_protocol_decoder_nissan_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderNissan* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_nissan_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderNissan* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_nissan_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderNissan* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_nissan_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_nissan_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderNissan* instance = context;
    uint8_t mode = (uint8_t)((instance->generic.data >> 34) & 0x07);
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%06lX Mode:%u\r\n"
        "Batt:n/a Bar:%2.1f Temp:n/a",
        instance->generic.protocol_name,
        (unsigned long)instance->generic.id,
        mode,
        (double)instance->generic.pressure);
}
