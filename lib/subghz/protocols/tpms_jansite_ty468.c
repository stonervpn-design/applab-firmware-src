#include "tpms_jansite_ty468.h"

#define TAG "TPMSJansiteTY468"

// Port of rtl_433/src/devices/tpms_jansite_ty468.c
//   (Jansite TPMS TY-468-eu2 / KKMOON TPMS; GPL-2+)
//
// OOK (AM), PCM/Manchester encoded, 8 byte payload (64 data bits).
// short_width == long_width == 50 us (one 50 us raw slot per half-bit; a data
// bit is two half-bits, so a Manchester symbol is 100 us). 433.92 MHz / 315 MHz.
//
// On the wire (OOK PCM): mark(RF present) = raw 1, gap = raw 0. rtl_433 hunts the
// raw 32-bit clock preamble 0xAAAAAAAA (searching the first 24 bits {0xAA,0xAA,0xAA}),
// then Manchester-decodes the 128 raw bits that follow the 32-bit preamble into
// 64 data bits and inverts them. rtl_433's bitbuffer_manchester_decode emits the
// SECOND raw bit of each (differing) pair; the subsequent bitbuffer_invert flips it,
// so the net decoded data bit equals the FIRST raw bit of each pair (bit1), with the
// hard requirement bit1 != bit2 (a Manchester violation aborts the frame). This port
// slices the raw 50/100 us pulses itself and reproduces that net mapping exactly.
//
// Wire format (after Manchester-decoding + invert), byte map from rtl_433:
//   b[0]     = data byte; low nibble mirrors b[1] low nibble; b[7] repeats it
//   b[1]     = data byte (low nibble matches b[0])
//   b[2]     = temperature component
//   b[3]     = checksum component (b3 + b4 selects the per-unit calibration)
//   b[4]     = checksum component
//   b[5]     = temperature + pressure component
//   b[6]     = pressure component
//   b[7]     = tail integrity, must equal b[0]
//
//   checksum = (b[3] + b[4]) & 0xFF -> 0xFB : temp_off=224, press_off=273
//                                   -> 0x64 : temp_off=153, press_off=201
//                                   -> else : reject (fail MIC)
//   temperature_C = temp_offset  - ((b[2] + b[5]) & 0xFF)
//   pressure_kPa  = (pressure_offset - ((b[5] + b[6]) & 0xFF)) * 2.5
//
// Sanity checks (all required, no false positives): b[7] == b[0],
// (b[0] & 0x0F) == (b[1] & 0x0F), and the checksum must be 0xFB or 0x64.

#define JANSITE_TY468_PREAMBLE 0xAAAAAAUL // raw 24-bit clock 1010...10
#define JANSITE_TY468_PREAMBLE_MASK 0xFFFFFFUL
#define JANSITE_TY468_PREAMBLE_SKIP 8 // skip to pos+32 after the 24-bit match (pos+24)
#define JANSITE_TY468_DATA_BITS 64

static const SubGhzBlockConst tpms_protocol_jansite_ty468_const = {
    .te_short = 50,
    .te_long = 100,
    .te_delta = 25,
    .min_count_bit_for_found = JANSITE_TY468_DATA_BITS,
};

typedef enum {
    JansiteTY468DecoderStepReset = 0,
    JansiteTY468DecoderStepFindPreamble,
    JansiteTY468DecoderStepSkipPreamble,
    JansiteTY468DecoderStepData,
} JansiteTY468DecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    uint32_t header_shift; // raw-bit preamble hunt shift register (24 bits)
    uint8_t skip_count; // raw bits left to skip to reach data start (pos+32)
    bool pair_have_first; // Manchester pair phase
    bool pair_first_val; // first raw half-bit of the current pair (== decoded bit)
} TPMSProtocolDecoderJansiteTY468;

const SubGhzProtocolDecoder tpms_protocol_jansite_ty468_decoder = {
    .alloc = tpms_protocol_decoder_jansite_ty468_alloc,
    .free = tpms_protocol_decoder_jansite_ty468_free,
    .feed = tpms_protocol_decoder_jansite_ty468_feed,
    .reset = tpms_protocol_decoder_jansite_ty468_reset,
    .get_hash_data = tpms_protocol_decoder_jansite_ty468_get_hash_data,
    .serialize = tpms_protocol_decoder_jansite_ty468_serialize,
    .deserialize = tpms_protocol_decoder_jansite_ty468_deserialize,
    .get_string = tpms_protocol_decoder_jansite_ty468_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_jansite_ty468_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_jansite_ty468 = {
    .name = TPMS_PROTOCOL_JANSITE_TY468_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,
    .decoder = &tpms_protocol_jansite_ty468_decoder,
    .encoder = &tpms_protocol_jansite_ty468_encoder,
};

void* tpms_protocol_decoder_jansite_ty468_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderJansiteTY468* instance =
        malloc(sizeof(TPMSProtocolDecoderJansiteTY468));
    instance->base.protocol = &tpms_protocol_jansite_ty468;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_jansite_ty468_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_jansite_ty468_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteTY468* instance = context;
    instance->decoder.parser_step = JansiteTY468DecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->header_shift = 0;
    instance->skip_count = 0;
    instance->pair_have_first = false;
    instance->pair_first_val = false;
}

static bool jansite_ty468_check(TPMSProtocolDecoderJansiteTY468* instance) {
    uint8_t b[8];
    for(int i = 0; i < 8; i++) {
        b[i] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
    if(b[7] != b[0]) return false;
    if((b[0] & 0x0F) != (b[1] & 0x0F)) return false;
    uint8_t checksum = (uint8_t)((b[3] + b[4]) & 0xFF);
    return (checksum == 0xFB) || (checksum == 0x64);
}

static void jansite_ty468_analyze(TPMSBlockGeneric* g) {
    uint8_t b[8];
    for(int i = 0; i < 8; i++) {
        b[i] = (g->data >> (8 * (7 - i))) & 0xFF;
    }
    // No dedicated serial field in the frame; use the leading identifying bytes.
    g->id = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];

    int temp_offset = 0;
    int pressure_offset = 0;
    uint8_t checksum = (uint8_t)((b[3] + b[4]) & 0xFF);
    if(checksum == 0xFB) {
        temp_offset = 224;
        pressure_offset = 273;
    } else if(checksum == 0x64) {
        temp_offset = 153;
        pressure_offset = 201;
    }

    g->temperature = (float)(temp_offset - ((b[2] + b[5]) & 0xFF));
    float pressure_kPa = (float)(pressure_offset - ((b[5] + b[6]) & 0xFF)) * 2.5f;
    g->pressure = pressure_kPa * 0.01f; // kPa -> bar
    g->battery_low = TPMS_NO_BATT;
}

// Slice one raw 50 us half-bit slot (level = mark/gap) through the framing machine.
static void jansite_ty468_process_raw_bit(
    TPMSProtocolDecoderJansiteTY468* instance,
    bool raw) {
    switch(instance->decoder.parser_step) {
    case JansiteTY468DecoderStepReset:
        instance->header_shift = 0;
        instance->decoder.parser_step = JansiteTY468DecoderStepFindPreamble;
        /* fallthrough */
    case JansiteTY468DecoderStepFindPreamble:
        instance->header_shift =
            ((instance->header_shift << 1) | (raw ? 1 : 0)) & JANSITE_TY468_PREAMBLE_MASK;
        if(instance->header_shift == JANSITE_TY468_PREAMBLE) {
            instance->decoder.parser_step = JansiteTY468DecoderStepSkipPreamble;
            instance->skip_count = JANSITE_TY468_PREAMBLE_SKIP;
        }
        break;

    case JansiteTY468DecoderStepSkipPreamble:
        if(instance->skip_count > 0) instance->skip_count--;
        if(instance->skip_count == 0) {
            instance->decoder.parser_step = JansiteTY468DecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->pair_have_first = false;
        }
        break;

    case JansiteTY468DecoderStepData:
        if(!instance->pair_have_first) {
            instance->pair_first_val = raw;
            instance->pair_have_first = true;
        } else {
            instance->pair_have_first = false;
            if(raw == instance->pair_first_val) {
                // Manchester violation: rtl_433 stops the decode -> frame too short.
                instance->decoder.parser_step = JansiteTY468DecoderStepReset;
                instance->header_shift = 0;
                break;
            }
            // Net decoded bit == first raw half-bit (bit1), per manchester_decode+invert.
            subghz_protocol_blocks_add_bit(
                &instance->decoder, instance->pair_first_val ? 1 : 0);
            if(instance->decoder.decode_count_bit ==
               tpms_protocol_jansite_ty468_const.min_count_bit_for_found) {
                if(jansite_ty468_check(instance)) {
                    instance->generic.data = instance->decoder.decode_data;
                    instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                    jansite_ty468_analyze(&instance->generic);
                    if(instance->base.callback)
                        instance->base.callback(&instance->base, instance->base.context);
                }
                instance->decoder.parser_step = JansiteTY468DecoderStepReset;
                instance->header_shift = 0;
            }
        }
        break;
    }
}

void tpms_protocol_decoder_jansite_ty468_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteTY468* instance = context;

    // OOK PCM: a pulse is an integer number of 50 us half-bit slots (1 or 2 in-spec).
    uint8_t slots;
    if(DURATION_DIFF(duration, tpms_protocol_jansite_ty468_const.te_short) <
       tpms_protocol_jansite_ty468_const.te_delta) {
        slots = 1;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_jansite_ty468_const.te_long) <
        tpms_protocol_jansite_ty468_const.te_delta) {
        slots = 2;
    } else {
        // Out-of-spec gap/pulse: abort any in-progress frame and re-hunt preamble.
        instance->decoder.parser_step = JansiteTY468DecoderStepReset;
        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;
        instance->header_shift = 0;
        instance->skip_count = 0;
        instance->pair_have_first = false;
        return;
    }

    for(uint8_t i = 0; i < slots; i++) {
        jansite_ty468_process_raw_bit(instance, level);
    }
}

uint8_t tpms_protocol_decoder_jansite_ty468_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteTY468* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_jansite_ty468_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteTY468* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus tpms_protocol_decoder_jansite_ty468_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteTY468* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_jansite_ty468_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_jansite_ty468_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteTY468* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX Batt:%s\r\n"
        "Temp:%2.0f C Bar:%2.1f",
        instance->generic.protocol_name,
        instance->generic.id,
        (instance->generic.battery_low == TPMS_NO_BATT) ?
            "n/a" :
            (instance->generic.battery_low ? "low" : "ok"),
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}
