#include "tpms_jansite_ty588.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSJansiteTy588"

// Port of rtl_433/src/devices/tpms_jansite_ty588.c (Benjamin Larsson 2026, GPL-2+)
//
// FSK, PCM cells, Manchester-encoded data, 8 byte payload.
// Jansite TPMS TY588-EU2 (a different wire format from the older Jansite Solar,
// see rtl_433 issue #2320). 315 MHz (US) / 433.92 MHz (EU).
//
// 174 bit packet on the wire:
//   44 raw (pre-Manchester) preamble bits: 0x99 0xAA 0x5A 0x6A 0x9A 0xA...
//   128 Manchester bits -> 8 decoded bytes B0..B7
//   2 padding bits
//
// Decoded byte layout (after Manchester decode):
//   B0 B1 B2 B3 B4 B5 B6 B7
//   B7 repeats B0                      (tail integrity check == this decoder's "mic")
//   B3 = (B0 - 0x1e) & 0xFF            (structural, so (B3 + B4) & 0xFF == 0x30)
//   B4 = (0x4e - B0) & 0xFF            (structural)
//   (B0 & 0x0F) == (B1 & 0x0F)         (structural)
//   temperature_C = ((B2 + B5) & 0xFF) - 139
//   pressure_kPa  = (((B5 + B6) & 0xFF) - 90) * 2.5
//
// Sensor ID / battery are not resolvable from a single sensor's captures; rtl_433
// reports the raw 7 byte payload as a "code". Here B0..B3 are packed into the
// generic id so distinct sensors can be told apart.
//
// PCM cell = 51 us. Manchester over the cells: a half-bit is one cell (te_short),
// a full-bit is two cells (te_long). We reconstruct the raw cell bitstream from
// LevelDuration pulses, hunt the 24-bit preamble head, skip the rest of the
// preamble, then Manchester-decode 128 raw bits into 64 data bits.

#define JANSITE_TY588_PREAMBLE      0x99AA5AUL
#define JANSITE_TY588_PREAMBLE_MASK 0xFFFFFFUL
#define JANSITE_TY588_PREAMBLE_BITS 24
#define JANSITE_TY588_PREAMBLE_SKIP 20 // remaining raw preamble bits (44 - 24)
#define JANSITE_TY588_DATA_BITS     64

static const SubGhzBlockConst tpms_protocol_jansite_ty588_const = {
    .te_short = 51,
    .te_long = 102,
    .te_delta = 25,
    .min_count_bit_for_found = JANSITE_TY588_DATA_BITS,
};

typedef enum {
    JansiteTy588DecoderStepReset = 0,
    JansiteTy588DecoderStepFindPreamble,
    JansiteTy588DecoderStepSkipPreamble,
    JansiteTy588DecoderStepData,
} JansiteTy588DecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    uint32_t header_shift;
    uint8_t preamble_skip;
    uint8_t half;
    uint8_t first_bit;
} TPMSProtocolDecoderJansiteTy588;

const SubGhzProtocolDecoder tpms_protocol_jansite_ty588_decoder = {
    .alloc = tpms_protocol_decoder_jansite_ty588_alloc,
    .free = tpms_protocol_decoder_jansite_ty588_free,
    .feed = tpms_protocol_decoder_jansite_ty588_feed,
    .reset = tpms_protocol_decoder_jansite_ty588_reset,
    .get_hash_data = tpms_protocol_decoder_jansite_ty588_get_hash_data,
    .serialize = tpms_protocol_decoder_jansite_ty588_serialize,
    .deserialize = tpms_protocol_decoder_jansite_ty588_deserialize,
    .get_string = tpms_protocol_decoder_jansite_ty588_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_jansite_ty588_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_jansite_ty588 = {
    .name = TPMS_PROTOCOL_JANSITE_TY588_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,

    .decoder = &tpms_protocol_jansite_ty588_decoder,
    .encoder = &tpms_protocol_jansite_ty588_encoder,
};

void* tpms_protocol_decoder_jansite_ty588_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderJansiteTy588* instance =
        malloc(sizeof(TPMSProtocolDecoderJansiteTy588));
    instance->base.protocol = &tpms_protocol_jansite_ty588;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_jansite_ty588_free(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteTy588* instance = context;
    free(instance);
}

void tpms_protocol_decoder_jansite_ty588_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteTy588* instance = context;
    instance->decoder.parser_step = JansiteTy588DecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->header_shift = 0;
    instance->preamble_skip = 0;
    instance->half = 0;
    instance->first_bit = 0;
}

static bool tpms_protocol_jansite_ty588_check(TPMSProtocolDecoderJansiteTy588* instance) {
    uint8_t b[8];
    for(int i = 0; i < 8; i++) {
        b[i] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
    // Tail byte must equal B0 (the only "mic" this encoding has).
    if(b[7] != b[0]) return false;
    // Structural checks that always hold for a genuine frame; reject garbled decodes.
    if(((b[3] + b[4]) & 0xFF) != 0x30) return false;
    if((b[0] & 0x0F) != (b[1] & 0x0F)) return false;

    int temperature = ((b[2] + b[5]) & 0xFF) - 139;
    int pressure_raw = ((b[5] + b[6]) & 0xFF) - 90;
    if(pressure_raw < 0 || temperature < -40 || temperature > 120) return false;
    return true;
}

static void tpms_protocol_jansite_ty588_analyze(TPMSBlockGeneric* g) {
    uint8_t b[8];
    for(int i = 0; i < 8; i++) {
        b[i] = (g->data >> (8 * (7 - i))) & 0xFF;
    }
    // No fixed sensor ID is resolvable from a single sensor; pack B0..B3 so distinct
    // sensors can at least be told apart (matches rtl_433's raw "code" intent).
    g->id = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];

    int pressure_raw = ((b[5] + b[6]) & 0xFF) - 90;
    float pressure_kpa = (float)pressure_raw * 2.5f;
    g->pressure = pressure_kpa * 0.01f; // kPa -> bar

    g->temperature = (float)(((b[2] + b[5]) & 0xFF) - 139);
    g->battery_low = TPMS_NO_BATT;
}

static void tpms_protocol_jansite_ty588_process_raw_bit(
    TPMSProtocolDecoderJansiteTy588* instance,
    uint8_t bit) {
    switch(instance->decoder.parser_step) {
    case JansiteTy588DecoderStepReset:
        instance->header_shift = 0;
        instance->decoder.parser_step = JansiteTy588DecoderStepFindPreamble;
        /* fallthrough */
    case JansiteTy588DecoderStepFindPreamble:
        instance->header_shift =
            ((instance->header_shift << 1) | (bit ? 1 : 0)) & JANSITE_TY588_PREAMBLE_MASK;
        if(instance->header_shift == JANSITE_TY588_PREAMBLE) {
            instance->decoder.parser_step = JansiteTy588DecoderStepSkipPreamble;
            instance->preamble_skip = JANSITE_TY588_PREAMBLE_SKIP;
        }
        break;

    case JansiteTy588DecoderStepSkipPreamble:
        if(instance->preamble_skip > 0) instance->preamble_skip--;
        if(instance->preamble_skip == 0) {
            instance->decoder.parser_step = JansiteTy588DecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->half = 0;
        }
        break;

    case JansiteTy588DecoderStepData:
        if(instance->half == 0) {
            instance->first_bit = bit ? 1 : 0;
            instance->half = 1;
        } else {
            instance->half = 0;
            uint8_t second_bit = bit ? 1 : 0;
            if(instance->first_bit == second_bit) {
                // Not a valid Manchester pair -> abandon this frame.
                instance->decoder.parser_step = JansiteTy588DecoderStepReset;
                break;
            }
            // rtl_433 bitbuffer_manchester_decode emits the second bit of each pair.
            subghz_protocol_blocks_add_bit(&instance->decoder, second_bit);
            if(instance->decoder.decode_count_bit ==
               tpms_protocol_jansite_ty588_const.min_count_bit_for_found) {
                if(tpms_protocol_jansite_ty588_check(instance)) {
                    instance->generic.data = instance->decoder.decode_data;
                    instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                    tpms_protocol_jansite_ty588_analyze(&instance->generic);
                    if(instance->base.callback)
                        instance->base.callback(&instance->base, instance->base.context);
                }
                instance->decoder.parser_step = JansiteTy588DecoderStepReset;
            }
        }
        break;
    }
}

void tpms_protocol_decoder_jansite_ty588_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteTy588* instance = context;

    // Slice the pulse into 51 us PCM cells (1 or 2 cells per run in this protocol).
    uint8_t cells;
    if(DURATION_DIFF(duration, tpms_protocol_jansite_ty588_const.te_short) <
       tpms_protocol_jansite_ty588_const.te_delta) {
        cells = 1;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_jansite_ty588_const.te_long) <
        tpms_protocol_jansite_ty588_const.te_delta) {
        cells = 2;
    } else {
        // Out-of-spec run (gap / noise) -> resync.
        instance->decoder.parser_step = JansiteTy588DecoderStepReset;
        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;
        instance->header_shift = 0;
        instance->half = 0;
        return;
    }

    for(uint8_t i = 0; i < cells; i++) {
        tpms_protocol_jansite_ty588_process_raw_bit(instance, level ? 1 : 0);
    }
}

uint8_t tpms_protocol_decoder_jansite_ty588_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteTy588* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_jansite_ty588_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteTy588* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus tpms_protocol_decoder_jansite_ty588_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteTy588* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_jansite_ty588_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_jansite_ty588_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteTy588* instance = context;
    const char* batt = (instance->generic.battery_low == TPMS_NO_BATT) ?
                           "?" :
                           (instance->generic.battery_low ? "low" : "ok");
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX Batt:%s\r\n"
        "Temp:%2.0f C Bar:%2.1f",
        instance->generic.protocol_name,
        (unsigned long)instance->generic.id,
        batt,
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}
