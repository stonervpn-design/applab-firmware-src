#include "tpms_hyundai_ev.h"
#include "../blocks/generic.h"

#define TAG "TPMSHyundaiEV"

// Hyundai / Kia EV-platform TPMS sensor (part 52940-BV100 / 52940-CG100), used by
// Kona EV, EV6, EV9, Ioniq, Niro, Tucson, Sportage, Creta (~2021+). Reverse-
// engineered from the raw frames in rtl_433 issue #3097 (2023 Kia EV6), which is
// unsolved upstream. Same VDO-family wire front-end as tpms_hyundai_vdo (433.92
// MHz FSK, Manchester, raw preamble 0x55 55 55 56, 52us half-symbol) BUT a
// **72-bit / 9-byte** payload instead of VDO's 80-bit — which is exactly why
// tpms_hyundai_vdo rejects it (it only fires at 80 bits).
//
// Decoded reference frame (three matching captures from #3097):
//   payload = 86 7A 05 89 45 80 5C 26 D5   (ID in the leading bytes)
//
// STATUS: ID + raw payload are solid (validated against the real EV6 frame).
// The pressure/temperature scaling and the trailing checksum could NOT be pinned
// from the single available online reading (34.5 PSI / 91F): no CRC-8 matched and
// the scaling is ambiguous with one data point. So this decode-only decoder shows
// the ID + full raw payload + a PROVISIONAL pressure (byte6 * 2.5 kPa, the closest
// candidate). Finalize pressure/temp once two dashboard readings are captured.

#define HYUNDAI_EV_PREAMBLE 0x5556U
#define HYUNDAI_EV_DATA_BITS 72

static const SubGhzBlockConst tpms_protocol_hyundai_ev_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = HYUNDAI_EV_DATA_BITS,
};

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    uint16_t raw_shift; // rolling raw half-symbol bits for the preamble hunt
    bool in_data;
    bool data_second; // pair convention: false=1st half (0x5556), true=2nd (0xAAA9)
    bool have_half;
    uint8_t first_half;
    uint64_t head_64_bit; // payload > 64 bits accumulates here
} TPMSProtocolDecoderHyundaiEv;

const SubGhzProtocolDecoder tpms_protocol_hyundai_ev_decoder = {
    .alloc = tpms_protocol_decoder_hyundai_ev_alloc,
    .free = tpms_protocol_decoder_hyundai_ev_free,
    .feed = tpms_protocol_decoder_hyundai_ev_feed,
    .reset = tpms_protocol_decoder_hyundai_ev_reset,
    .get_hash_data = tpms_protocol_decoder_hyundai_ev_get_hash_data,
    .serialize = tpms_protocol_decoder_hyundai_ev_serialize,
    .deserialize = tpms_protocol_decoder_hyundai_ev_deserialize,
    .get_string = tpms_protocol_decoder_hyundai_ev_get_string,
};

// Decode-only: no encoder (pressure/temp/CRC not yet solved, so no faithful TX).
const SubGhzProtocolEncoder tpms_protocol_hyundai_ev_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_hyundai_ev = {
    .name = TPMS_PROTOCOL_HYUNDAI_EV_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,
    .decoder = &tpms_protocol_hyundai_ev_decoder,
    .encoder = &tpms_protocol_hyundai_ev_encoder,
};

void* tpms_protocol_decoder_hyundai_ev_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderHyundaiEv* instance = calloc(1, sizeof(TPMSProtocolDecoderHyundaiEv));
    instance->base.protocol = &tpms_protocol_hyundai_ev;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_hyundai_ev_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_hyundai_ev_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderHyundaiEv* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->head_64_bit = 0;
    instance->raw_shift = 0;
    instance->in_data = false;
    instance->data_second = false;
    instance->have_half = false;
    instance->first_half = 0;
}

// 72 bits: head_64_bit low 8 bits = b[0], decode_data 64 = b[1..8].
static void hyundai_ev_get_bytes(TPMSProtocolDecoderHyundaiEv* instance, uint8_t b[9]) {
    b[0] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 1] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

static void hyundai_ev_analyze(TPMSProtocolDecoderHyundaiEv* instance) {
    uint8_t b[9];
    hyundai_ev_get_bytes(instance, b);
    instance->generic.id =
        ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    // PROVISIONAL pressure: byte6 * 2.5 kPa -> bar (closest candidate to 34.5 psi).
    instance->generic.pressure = ((float)b[6] * 2.5f) * 0.01f;
    // PROVISIONAL temperature: raw byte7 (scaling unconfirmed).
    instance->generic.temperature = (float)b[7];
    instance->generic.battery_low = b[5];
}

// Emit one raw half-symbol bit: hunt the raw preamble, then Manchester-decode the
// payload. Fires only when the burst terminates at EXACTLY 72 data bits, so an
// 80-bit VDO frame (which passes 72 mid-stream) is never mistaken for an EV frame.
static void hyundai_ev_emit_raw_bit(TPMSProtocolDecoderHyundaiEv* instance, uint8_t bit) {
    bit &= 1;
    if(!instance->in_data) {
        instance->raw_shift = (uint16_t)((instance->raw_shift << 1) | bit);
        if(instance->raw_shift == 0x5556 || instance->raw_shift == 0xAAA9) {
            instance->in_data = true;
            instance->data_second = (instance->raw_shift == 0xAAA9);
            instance->have_half = false;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->head_64_bit = 0;
        }
        return;
    }
    if(!instance->have_half) {
        instance->first_half = bit;
        instance->have_half = true;
        return;
    }
    instance->have_half = false;
    if(instance->first_half == bit) {
        // Non-transition = end of burst. Accept only a complete 72-bit frame.
        if(instance->decoder.decode_count_bit == HYUNDAI_EV_DATA_BITS) {
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            hyundai_ev_analyze(instance);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        instance->in_data = false;
        instance->raw_shift = 0;
        return;
    }
    uint8_t data_bit = instance->data_second ? bit : instance->first_half;
    subghz_protocol_blocks_add_to_128_bit(&instance->decoder, data_bit, &instance->head_64_bit);
    // Guard: never run past the frame length (a longer frame is not an EV frame).
    if(instance->decoder.decode_count_bit > HYUNDAI_EV_DATA_BITS) {
        instance->in_data = false;
        instance->raw_shift = 0;
    }
}

void tpms_protocol_decoder_hyundai_ev_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderHyundaiEv* instance = context;

    const uint32_t te = tpms_protocol_hyundai_ev_const.te_short; // 52us half-symbol
    if(duration >= te * 3) {
        // The last Manchester bit's second half is the transition INTO this gap,
        // so a dangling half pairs with the gap's leading low to finish bit 72.
        if(instance->in_data && instance->have_half) {
            hyundai_ev_emit_raw_bit(instance, 0);
        }
        // Gap ends the burst: accept a complete 72-bit frame captured so far.
        if(instance->in_data)
        if(instance->in_data && instance->decoder.decode_count_bit == HYUNDAI_EV_DATA_BITS) {
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            hyundai_ev_analyze(instance);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        tpms_protocol_decoder_hyundai_ev_reset(instance);
        return;
    }
    uint32_t n = (duration + te / 2) / te;
    if(n < 1) n = 1;
    if(n > 2) n = 2;
    for(uint32_t k = 0; k < n; k++) {
        hyundai_ev_emit_raw_bit(instance, level ? 1 : 0);
    }
}

uint8_t tpms_protocol_decoder_hyundai_ev_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderHyundaiEv* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_hyundai_ev_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderHyundaiEv* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_hyundai_ev_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderHyundaiEv* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_hyundai_ev_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_hyundai_ev_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderHyundaiEv* instance = context;
    // Reconstruct the 9 payload bytes from the persistent generic fields (the live
    // decoder state is cleared right after the frame fires): b[0] = id top byte,
    // b[1..8] = generic.data. Byte 7 is the provisional temperature raw.
    uint8_t b0 = (uint8_t)((instance->generic.id >> 24) & 0xFF);
    uint64_t data = instance->generic.data;
    uint8_t t_raw = (uint8_t)((data >> 8) & 0xFF);
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX\r\n"
        "Raw:%02X%016llX\r\n"
        "Bar~%2.1f (prov) T?:%02X",
        instance->generic.protocol_name,
        instance->generic.id,
        b0,
        (unsigned long long)data,
        (double)instance->generic.pressure,
        t_raw);
}
