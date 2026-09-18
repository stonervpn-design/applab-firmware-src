#include "tpms_gm.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSGm"

// Port of rtl_433/src/devices/tpms_gm.c (GM aftermarket TPMS, GPL-2+).
//
// Manchester (OOK_PULSE_MANCHESTER_ZEROBIT), short_width ~120us.
// 315 MHz (US) / 433.92 MHz. 130 bit frame.
//
// Wire format (16 bytes b[0..15] + 2 trailing guard bits):
//   b[0..5]  = preamble, all 0x00 (48 bits)
//   b[6..7]  = flags (16 bit)
//   b[8..12] = sensor id (5 bytes, 40 bit)
//   b[13]    = pressure raw   (kPa = raw * 2.75)
//   b[14]    = temperature raw (C  = raw - 60)
//   b[15]    = checksum = SUM(b[6..14]) & 0xFF
//
// Flags: bit5 = battery low (battery_ok = !bit5).
//
// The preamble carries no unique sync word (all zero), so instead of hunting a
// pattern we slide a 128-bit window (b[0..15]) and, at every bit, accept the
// alignment where b[0..5]==0 AND the checksum over b[6..14] matches b[15].
// The checksum + the 48 required zero bits reject false alignments/noise.

#define GM_DATA_BITS 64 // informative payload we persist (b[8..15])

static const SubGhzBlockConst tpms_protocol_gm_const = {
    .te_short = 120,
    .te_long = 240,
    .te_delta = 55,
    .min_count_bit_for_found = GM_DATA_BITS,
};

typedef enum {
    GmDecoderStepReset = 0,
    GmDecoderStepData,
} GmDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    ManchesterState manchester_saved_state;
    uint64_t win_hi; // window bits 127..64 -> frame bytes b[0..7]
    uint64_t win_lo; // window bits  63..0  -> frame bytes b[8..15]
    uint16_t bit_count;
} TPMSProtocolDecoderGm;

const SubGhzProtocolDecoder tpms_protocol_gm_decoder = {
    .alloc = tpms_protocol_decoder_gm_alloc,
    .free = tpms_protocol_decoder_gm_free,
    .feed = tpms_protocol_decoder_gm_feed,
    .reset = tpms_protocol_decoder_gm_reset,
    .get_hash_data = tpms_protocol_decoder_gm_get_hash_data,
    .serialize = tpms_protocol_decoder_gm_serialize,
    .deserialize = tpms_protocol_decoder_gm_deserialize,
    .get_string = tpms_protocol_decoder_gm_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_gm_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol tpms_protocol_gm = {
    .name = TPMS_PROTOCOL_GM_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load,
    .decoder = &tpms_protocol_gm_decoder,
    .encoder = &tpms_protocol_gm_encoder,
};

void* tpms_protocol_decoder_gm_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderGm* instance = malloc(sizeof(TPMSProtocolDecoderGm));
    instance->base.protocol = &tpms_protocol_gm;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_gm_free(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderGm* instance = context;
    free(instance);
}

void tpms_protocol_decoder_gm_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderGm* instance = context;
    instance->decoder.parser_step = GmDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->win_hi = 0;
    instance->win_lo = 0;
    instance->bit_count = 0;
}

static ManchesterEvent gm_evt(bool level, uint32_t duration) {
    bool is_long = false;
    if(DURATION_DIFF(duration, tpms_protocol_gm_const.te_long) <
       tpms_protocol_gm_const.te_delta * 2) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_gm_const.te_short) <
        tpms_protocol_gm_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }
    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

// Verify checksum for the current window alignment; also reject all-zero payload.
static bool gm_check_frame(TPMSProtocolDecoderGm* instance) {
    // b[0..5] must be zero (48 preamble bits) -> top 48 bits of win_hi.
    if((instance->win_hi >> 16) != 0) return false;

    uint8_t b6 = (uint8_t)((instance->win_hi >> 8) & 0xFF);
    uint8_t b7 = (uint8_t)(instance->win_hi & 0xFF);
    uint8_t p[8]; // frame bytes b[8..15]
    for(int i = 0; i < 8; i++) {
        p[i] = (uint8_t)((instance->win_lo >> (8 * (7 - i))) & 0xFF);
    }

    // checksum covers b[6..14] = b6, b7, p[0..6]; result is b[15] = p[7].
    uint16_t sum = (uint16_t)b6 + b7;
    for(int i = 0; i < 7; i++) sum += p[i];
    if((uint8_t)(sum & 0xFF) != p[7]) return false;

    // Sanity: reject the all-zero payload rtl_433 treats as bogus.
    bool all_zero = (b6 == 0) && (b7 == 0) && (p[7] == 0);
    for(int i = 0; i < 7 && all_zero; i++) all_zero &= (p[i] == 0);
    if(all_zero) return false;

    return true;
}

static void gm_analyze(TPMSProtocolDecoderGm* instance) {
    uint8_t b6 = (uint8_t)((instance->win_hi >> 8) & 0xFF);
    uint8_t b7 = (uint8_t)(instance->win_hi & 0xFF);
    uint8_t p[8]; // b[8..15]: id0..id4, pressure, temp, crc
    for(int i = 0; i < 8; i++) {
        p[i] = (uint8_t)((instance->win_lo >> (8 * (7 - i))) & 0xFF);
    }
    uint16_t flags = ((uint16_t)b6 << 8) | b7;

    // Persist the informative 64-bit payload (b[8..15]) as the raw frame.
    instance->generic.data = instance->win_lo;
    instance->generic.data_count_bit = GM_DATA_BITS;
    instance->decoder.decode_data = instance->win_lo;
    instance->decoder.decode_count_bit = GM_DATA_BITS;

    // 40-bit sensor id -> keep lower 32 bits in generic.id (full id printed from data).
    instance->generic.id =
        ((uint32_t)p[1] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 8) | p[4];

    instance->generic.pressure = (float)p[5] * 2.75f * 0.01f; // kPa -> bar
    instance->generic.temperature = (float)((int)p[6] - 60);
    instance->generic.battery_low = ((flags >> 5) & 1) ? 1 : TPMS_NO_BATT;
}

void tpms_protocol_decoder_gm_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderGm* instance = context;

    ManchesterEvent event = gm_evt(level, duration);
    if(event == ManchesterEventReset) {
        instance->decoder.parser_step = GmDecoderStepData;
        instance->win_hi = 0;
        instance->win_lo = 0;
        instance->bit_count = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        return;
    }

    bool bit = false;
    bool have_bit = manchester_advance(
        instance->manchester_saved_state, event, &instance->manchester_saved_state, &bit);
    if(!have_bit) return;

    // Slide the 128-bit window and validate every alignment.
    instance->win_hi = (instance->win_hi << 1) | (instance->win_lo >> 63);
    instance->win_lo = (instance->win_lo << 1) | (bit ? 1 : 0);
    if(instance->bit_count < 0xFFFF) instance->bit_count++;

    if(instance->bit_count >= 128 && gm_check_frame(instance)) {
        gm_analyze(instance);
        if(instance->base.callback)
            instance->base.callback(&instance->base, instance->base.context);
        // Restart the window so the same frame is not re-emitted bit-by-bit.
        instance->win_hi = 0;
        instance->win_lo = 0;
        instance->bit_count = 0;
    }
}

uint8_t tpms_protocol_decoder_gm_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderGm* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_gm_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderGm* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_gm_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderGm* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, tpms_protocol_gm_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_gm_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderGm* instance = context;
    // Full 40-bit sensor id lives in the top 5 bytes of the stored payload.
    uint64_t id40 = (instance->generic.data >> 24) & 0xFFFFFFFFFFULL;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%010llX\r\n"
        "Batt:%s\r\n"
        "Temp:%2.0f C Bar:%2.1f",
        instance->generic.protocol_name,
        (unsigned long long)id40,
        (instance->generic.battery_low == TPMS_NO_BATT) ? "ok" : "low",
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}
