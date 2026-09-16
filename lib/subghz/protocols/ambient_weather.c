#include "ambient_weather.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include <lib/toolbox/manchester_decoder.h>

/*
 * Ambient Weather F007TH / F012TH Thermo-Hygrometer (also TFA 30.3208.02,
 * SwitchDoc F016TH, Suomen Lampomittari 7411).
 *
 * Ported from rtl_433 src/devices/ambient_weather.c
 *   (contributed by David Ediger, discovered by Ron C. Lewis, GPLv2+).
 *
 * RF profile:
 *   - 433.92 MHz OOK, Manchester-encoded (rtl_433 OOK_PULSE_MANCHESTER_ZEROBIT)
 *   - Manchester half-bit ~500 us  (=> ~1 ms per data bit)
 *   - Three repeats without gap; reset_limit ~2400 us
 *
 * Wire format (6 data bytes, MSB-first):
 *   Byte0   Byte1   Byte2   Byte3   Byte4   Byte5
 *   xxxxMMMM IIIIIIII BCCCTTTT TTTTTTTT HHHHHHHH DDDDDDDD
 *     x : unknown (0x04 on F007TH/F012TH)
 *     M : model nibble (0x05 on F007TH/F012TH/F016TH)  -> b[0] = 0x45
 *     I : 8-bit device id (volatile, changes at power-up)
 *     B : battery-low flag
 *     C : channel (3 bits, 1-8)
 *     T : temperature, 12 bits = Fahrenheit*10 + 400
 *     H : humidity, 8 bits
 *     D : digest-8 MIC, LFSR gen 0x98, key 0x3e, init 0x64, over b[0..4]
 *
 * Preamble (rtl_433): 12-bit 0x0145 pattern, decode begins 8 bits in so the
 * first data byte is 0x45. Both Manchester phases resolve to the SAME data
 * bytes (rtl_433 also matches the 0xfd45 variant, which differs only in the
 * leading DC-balance run); we therefore hunt the common 12-bit lead-in for
 * either phase and seed b[0]'s high nibble (0x4).
 *
 * MIC + sanity (humidity <= 100, -40F <= temp < 344F) are enforced before the
 * callback, exactly as rtl_433 does, to keep noise from false-positiving.
 *
 * Receive/decode-only (passive sensor); the encoder is an all-NULL stub.
 */

#define TAG "AmbientF007TH"

#define AMBIENT_HALF_SHORT 500 /* Manchester half-bit */
#define AMBIENT_HALF_LONG  1000 /* two half-bits */

#define AMBIENT_DATA_BITS 48 /* 6 bytes */

/* 12-bit lead-in into b[0] high nibble for the two Manchester phases.
 * normal   : 0000 0001 0100 = 0x014  (from rtl_433 preamble 0x0145)
 * inverted : 1111 1101 0100 = 0xFD4  (from rtl_433 preamble 0xfd45)
 * Both are immediately followed by the same non-inverted data; the trailing
 * nibble 0100 is b[0]'s high nibble, which we seed on match. */
#define AMBIENT_PREAMBLE_A 0x014U
#define AMBIENT_PREAMBLE_B 0xFD4U
#define AMBIENT_PREAMBLE_MASK 0xFFFU

static const SubGhzBlockConst subghz_protocol_ambient_weather_const = {
    .te_short = AMBIENT_HALF_SHORT,
    .te_long = AMBIENT_HALF_LONG,
    .te_delta = 150,
    .min_count_bit_for_found = AMBIENT_DATA_BITS,
};

typedef enum {
    AmbientDecoderStepReset = 0,
    AmbientDecoderStepFindPreamble,
    AmbientDecoderStepData,
} AmbientDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    ManchesterState manchester_saved_state;
    uint16_t header_shift;
} SubGhzProtocolDecoderAmbientWeather;

// ============================================================================
// Forward declarations
// ============================================================================

static void* subghz_protocol_decoder_ambient_weather_alloc(SubGhzEnvironment* environment);
static void subghz_protocol_decoder_ambient_weather_free(void* context);
static void subghz_protocol_decoder_ambient_weather_reset(void* context);
static void
    subghz_protocol_decoder_ambient_weather_feed(void* context, bool level, uint32_t duration);
static uint8_t subghz_protocol_decoder_ambient_weather_get_hash_data(void* context);
static SubGhzProtocolStatus subghz_protocol_decoder_ambient_weather_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
static SubGhzProtocolStatus subghz_protocol_decoder_ambient_weather_deserialize(
    void* context,
    FlipperFormat* flipper_format);
static void subghz_protocol_decoder_ambient_weather_get_string(void* context, FuriString* output);

// ============================================================================
// Registration
// ============================================================================

const SubGhzProtocolDecoder subghz_protocol_ambient_weather_decoder = {
    .alloc = subghz_protocol_decoder_ambient_weather_alloc,
    .free = subghz_protocol_decoder_ambient_weather_free,
    .feed = subghz_protocol_decoder_ambient_weather_feed,
    .reset = subghz_protocol_decoder_ambient_weather_reset,
    .get_hash_data = subghz_protocol_decoder_ambient_weather_get_hash_data,
    .serialize = subghz_protocol_decoder_ambient_weather_serialize,
    .deserialize = subghz_protocol_decoder_ambient_weather_deserialize,
    .get_string = subghz_protocol_decoder_ambient_weather_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_ambient_weather_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

const SubGhzProtocol subghz_protocol_ambient_weather = {
    .name = AMBIENT_WEATHER_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_ambient_weather_decoder,
    .encoder = &subghz_protocol_ambient_weather_encoder,
};

// ============================================================================
// Decode helpers
// ============================================================================

static void
    subghz_protocol_ambient_weather_get_bytes(SubGhzBlockGeneric* generic, uint8_t b[6]) {
    for(int i = 0; i < 6; i++) {
        b[i] = (uint8_t)((generic->data >> (8 * (5 - i))) & 0xFF);
    }
}

static bool subghz_protocol_ambient_weather_check(uint64_t data) {
    uint8_t b[6];
    for(int i = 0; i < 6; i++) {
        b[i] = (uint8_t)((data >> (8 * (5 - i))) & 0xFF);
    }
    /* MIC: LFSR digest-8 over b[0..4], gen 0x98, key 0x3e, xor init 0x64. */
    uint8_t calc = subghz_protocol_blocks_lfsr_digest8(b, 5, 0x98, 0x3e) ^ 0x64;
    if(calc != b[5]) return false;

    /* Sanity checks (rtl_433) to reject bad data that still passes the MIC. */
    int humidity = b[4];
    if(humidity > 100) return false;
    int temp_raw = ((b[2] & 0x0f) << 8) | b[3];
    float temp_f = (temp_raw - 400) * 0.1f;
    if(temp_f < -40.0f || temp_f >= 344.0f) return false;
    return true;
}

static void subghz_protocol_decoder_ambient_weather_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderAmbientWeather* instance = context;
    instance->decoder.parser_step = AmbientDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->header_shift = 0;
    instance->manchester_saved_state = ManchesterStateStart1;
}

static ManchesterEvent subghz_protocol_ambient_weather_evt(bool level, uint32_t duration) {
    bool is_long = false;
    if(DURATION_DIFF(duration, AMBIENT_HALF_LONG) <
       subghz_protocol_ambient_weather_const.te_delta * 2) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, AMBIENT_HALF_SHORT) <
        subghz_protocol_ambient_weather_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }
    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

static void
    subghz_protocol_decoder_ambient_weather_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderAmbientWeather* instance = context;

    ManchesterEvent event = subghz_protocol_ambient_weather_evt(level, duration);
    if(event == ManchesterEventReset) {
        instance->decoder.parser_step = AmbientDecoderStepReset;
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
    case AmbientDecoderStepReset:
        instance->header_shift = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        instance->decoder.parser_step = AmbientDecoderStepFindPreamble;
        /* fallthrough */
    case AmbientDecoderStepFindPreamble:
        instance->header_shift =
            (uint16_t)(((instance->header_shift << 1) | (bit ? 1 : 0)) & AMBIENT_PREAMBLE_MASK);
        if(instance->header_shift == AMBIENT_PREAMBLE_A ||
           instance->header_shift == AMBIENT_PREAMBLE_B) {
            /* Seed b[0] high nibble (0100) already consumed in the lead-in. */
            instance->decoder.parser_step = AmbientDecoderStepData;
            instance->decoder.decode_data = 0x4;
            instance->decoder.decode_count_bit = 4;
        }
        break;

    case AmbientDecoderStepData:
        subghz_protocol_blocks_add_bit(&instance->decoder, bit ? 1 : 0);
        if(instance->decoder.decode_count_bit == AMBIENT_DATA_BITS) {
            if(subghz_protocol_ambient_weather_check(instance->decoder.decode_data)) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                uint8_t b[6];
                subghz_protocol_ambient_weather_get_bytes(&instance->generic, b);
                instance->generic.serial = b[1];
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            /* Restart the hunt (three no-gap repeats follow). */
            instance->decoder.parser_step = AmbientDecoderStepReset;
        }
        break;
    }
}

// ============================================================================
// alloc / free / hash / serialize
// ============================================================================

static void* subghz_protocol_decoder_ambient_weather_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderAmbientWeather* instance =
        calloc(1, sizeof(SubGhzProtocolDecoderAmbientWeather));
    instance->base.protocol = &subghz_protocol_ambient_weather;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->manchester_saved_state = ManchesterStateStart1;
    return instance;
}

static void subghz_protocol_decoder_ambient_weather_free(void* context) {
    furi_assert(context);
    free(context);
}

static uint8_t subghz_protocol_decoder_ambient_weather_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderAmbientWeather* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

static SubGhzProtocolStatus subghz_protocol_decoder_ambient_weather_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderAmbientWeather* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

static SubGhzProtocolStatus subghz_protocol_decoder_ambient_weather_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderAmbientWeather* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_ambient_weather_const.min_count_bit_for_found);
}

static void subghz_protocol_decoder_ambient_weather_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderAmbientWeather* instance = context;

    uint8_t b[6];
    subghz_protocol_ambient_weather_get_bytes(&instance->generic, b);

    int device_id = b[1];
    int battery_low = (b[2] & 0x80) != 0;
    int channel = ((b[2] & 0x70) >> 4) + 1;
    int temp_raw = ((b[2] & 0x0f) << 8) | b[3];
    float temp_f = (temp_raw - 400) * 0.1f;
    float temp_c = (temp_f - 32.0f) * 5.0f / 9.0f;
    int humidity = b[4];

    furi_string_printf(
        output,
        "%s %dbit\r\n"
        "Id:0x%02X Ch:%d Batt:%s\r\n"
        "Temp:%.1f F (%.1f C)\r\n"
        "Hum:%d%%",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        device_id,
        channel,
        battery_low ? "LOW" : "OK",
        (double)temp_f,
        (double)temp_c,
        humidity);
}
