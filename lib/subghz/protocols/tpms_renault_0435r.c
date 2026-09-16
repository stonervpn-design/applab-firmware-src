#include "tpms_renault_0435r.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSRenault0435R"

// Port of rtl_433/src/devices/tpms_renault_0435r.c
//   (Copyright (C) 2021 Tomas Ebenlendr <ebik@ucw.cz>, GPL-2+)
//
// FSK 433 MHz, PCM/NRZ raw half-symbols carrying a Manchester-encoded 9-byte
// payload. Compatible sensors: Renault 40700 0435R / VDO S180052064Z (Fluence,
// Laguna III, Latitude, Megane III, Scenic III, ZOE, ...).
//
// Wire format (rtl_433 nibbles):  II II II fx PP TT AA CC tt   (9 bytes, MSB first)
//   b[0..2] = 24-bit ID (rtl_433 prints "%02x%02x%02x", b[0], b[1], b[2])
//   b[3]    = flags (f = high nibble, seen 0xc; x = low nibble, seen 0x0 -> 0xc0)
//   b[4]    = pressure raw, 4/3 kPa per step  (kPa = raw / 0.75)
//   b[5]    = temperature in degC, offset -50  (C = raw - 50)
//   b[6]    = centrifugal acceleration, 5 m/s^2 per step (255 = overflow)
//   b[7]    = checksum: 8-bit XOR of b[0..8] == 0
//   b[8]    = 0x80 + measurement count (has_tick in bit7, tick in bits6..0)
//
// Modulation detail: rtl_433 is FSK_PULSE_PCM (short=long=52us), inverts the raw
// bitbuffer, searches the 16-bit preamble 0xaa 0xa9 (the tail of the inverted
// full preamble 55 55 55 56 -> aa aa aa a9), then bitbuffer_manchester_decode of
// the following bits. After the invert, manchester decode yields the FIRST half
// of each original raw pair as the data bit -- identical modulation and
// half-symbol convention to tpms_citroen (raw preamble 0x5556, data = 1st half).
// So this decoder mirrors tpms_citroen: hunt raw 0x5556 (or inverted 0xAAA9),
// then Manchester-decode a 72-bit payload (its top 8 bits, b[0], spill into
// head_64_bit via subghz_protocol_blocks_add_to_128_bit).

#define RENAULT_0435R_PREAMBLE 0x5556U
#define RENAULT_0435R_DATA_BITS 72

static const SubGhzBlockConst tpms_protocol_renault_0435r_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = RENAULT_0435R_DATA_BITS,
};

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    uint16_t raw_shift; // rolling window of raw half-symbol bits for preamble hunt
    bool in_data; // true once the preamble matched, collecting manchester data
    bool data_second; // pair convention: false=data is 1st half (0x5556), true=2nd (0xAAA9)
    bool have_half; // holding the first half of a manchester pair
    uint8_t first_half; // the first half-symbol of the current pair
    uint64_t head_64_bit; // payload bits above 64 (72-bit frame -> holds b[0])
} TPMSProtocolDecoderRenault0435R;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderRenault0435R;

const SubGhzProtocolDecoder tpms_protocol_renault_0435r_decoder = {
    .alloc = tpms_protocol_decoder_renault_0435r_alloc,
    .free = tpms_protocol_decoder_renault_0435r_free,
    .feed = tpms_protocol_decoder_renault_0435r_feed,
    .reset = tpms_protocol_decoder_renault_0435r_reset,
    .get_hash_data = tpms_protocol_decoder_renault_0435r_get_hash_data,
    .serialize = tpms_protocol_decoder_renault_0435r_serialize,
    .deserialize = tpms_protocol_decoder_renault_0435r_deserialize,
    .get_string = tpms_protocol_decoder_renault_0435r_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_renault_0435r_encoder = {
    .alloc = tpms_protocol_encoder_renault_0435r_alloc,
    .free = tpms_protocol_encoder_renault_0435r_free,
    .deserialize = tpms_protocol_encoder_renault_0435r_deserialize,
    .stop = tpms_protocol_encoder_renault_0435r_stop,
    .yield = tpms_protocol_encoder_renault_0435r_yield,
};

const SubGhzProtocol tpms_protocol_tpms_renault_0435r = {
    .name = TPMS_PROTOCOL_RENAULT_0435R_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_renault_0435r_decoder,
    .encoder = &tpms_protocol_renault_0435r_encoder,
};

void* tpms_protocol_decoder_renault_0435r_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderRenault0435R* instance =
        calloc(1, sizeof(TPMSProtocolDecoderRenault0435R));
    instance->base.protocol = &tpms_protocol_tpms_renault_0435r;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_renault_0435r_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_renault_0435r_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderRenault0435R* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->raw_shift = 0;
    instance->in_data = false;
    instance->data_second = false;
    instance->have_half = false;
    instance->first_half = 0;
    instance->head_64_bit = 0;
}

// 72 bits total: head_64_bit low 8 bits = b[0]; decode_data 64 bits = b[1..8].
static void renault_0435r_get_bytes(TPMSProtocolDecoderRenault0435R* instance, uint8_t b[9]) {
    b[0] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 1] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

static bool renault_0435r_check_crc(TPMSProtocolDecoderRenault0435R* instance) {
    uint8_t b[9];
    renault_0435r_get_bytes(instance, b);
    // rtl_433: chk = xor_bytes(b, 9); chk == 0.
    uint8_t x = 0;
    for(int i = 0; i < 9; i++) x ^= b[i];
    if(x != 0) return false;
    // rtl_433 sanity check on the tick byte b[8] (reject clearly bogus frames).
    uint8_t tick = b[8] & 0x7F;
    uint8_t has_tick = b[8] >> 7;
    if(b[8] && (!has_tick || tick > 30)) return false;
    return true;
}

static void renault_0435r_analyze(TPMSProtocolDecoderRenault0435R* instance) {
    uint8_t b[9];
    renault_0435r_get_bytes(instance, b);
    // rtl_433 prints the ID as "%02x%02x%02x", b[0], b[1], b[2]; mirror that
    // ordering into a single 24-bit value so the Flipper display matches.
    instance->generic.id =
        ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | (uint32_t)b[2];
    // Pressure raw -> kPa (raw / 0.75) -> bar (kPa / 100).
    instance->generic.pressure = ((float)b[4] / 0.75f) * 0.01f;
    // Temperature: raw - 50 degC.
    instance->generic.temperature = (float)((int)b[5] - 50);
    // Renault frames carry no battery bit; keep flags byte for reference.
    instance->generic.battery_low = b[3];
}

void tpms_protocol_tpms_renault_0435r_pack(TPMSBlockGeneric* g) {
    furi_assert(g);
    // Renault ID is 24-bit; drop any high byte (matches rtl_433's 3-byte ID).
    uint32_t id24 = g->id & 0x00FFFFFFU;
    g->id = id24;

    // pressure: kPa = raw / 0.75, bar = kPa / 100  ->  raw = bar * 75.
    int p_raw = (int)(g->pressure * 75.0f + 0.5f);
    if(p_raw < 0) p_raw = 0;
    if(p_raw > 0xFF) p_raw = 0xFF;
    int t = (int)(g->temperature + (g->temperature >= 0 ? 0.5f : -0.5f)) + 50; // b[5] = C + 50
    if(t < 0) t = 0;
    if(t > 0xFF) t = 0xFF;

    uint8_t b[9];
    b[0] = (uint8_t)((id24 >> 16) & 0xFF);
    b[1] = (uint8_t)((id24 >> 8) & 0xFF);
    b[2] = (uint8_t)(id24 & 0xFF);
    b[3] = 0xC0; // flags (observed 0xc0)
    b[4] = (uint8_t)p_raw;
    b[5] = (uint8_t)t;
    b[6] = 0x00; // centrifugal acceleration (stationary)
    b[7] = 0x00; // checksum placeholder, computed below
    b[8] = 0x80; // has_tick = 1, tick = 0 (start of a measurement session)
    // 8-bit XOR of all 9 bytes must be 0 -> b[7] = XOR of the other eight.
    uint8_t x = 0;
    for(int i = 0; i < 9; i++)
        if(i != 7) x ^= b[i];
    b[7] = x;

    // generic.data holds b[1..8] (low 64 bits); b[0] is recovered from generic.id
    // by the encoder (it lives in head_64_bit on the decode side).
    uint64_t data = 0;
    for(int i = 1; i < 9; i++) data = (data << 8) | b[i];
    g->data = data;
    g->data_count_bit = RENAULT_0435R_DATA_BITS;
}

// Consume one raw half-symbol bit: hunt the raw preamble (0x5556, or the
// inverted-polarity 0xAAA9), then Manchester-decode the 72-bit payload. Mirrors
// rtl_433 tpms_renault_0435r (preamble tail 0xaaa9 in the inverted raw bits,
// only the data is Manchester). The 72-bit payload spills its top 8 bits (b[0])
// into head_64_bit.
static void
    renault_0435r_emit_raw_bit(TPMSProtocolDecoderRenault0435R* instance, uint8_t bit) {
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
        // No transition -> Manchester violation: end of burst, resync.
        instance->in_data = false;
        instance->raw_shift = 0;
        return;
    }
    uint8_t data_bit = instance->data_second ? bit : instance->first_half;
    subghz_protocol_blocks_add_to_128_bit(&instance->decoder, data_bit, &instance->head_64_bit);
    if(instance->decoder.decode_count_bit == RENAULT_0435R_DATA_BITS) {
        if(renault_0435r_check_crc(instance)) {
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            renault_0435r_analyze(instance);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        instance->in_data = false;
        instance->raw_shift = 0;
    }
}

void tpms_protocol_decoder_renault_0435r_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderRenault0435R* instance = context;

    const uint32_t te = tpms_protocol_renault_0435r_const.te_short; // 52us half-symbol
    if(duration >= te * 3) {
        tpms_protocol_decoder_renault_0435r_reset(instance);
        return;
    }
    uint32_t n = (duration + te / 2) / te;
    if(n < 1) n = 1;
    if(n > 2) n = 2;
    for(uint32_t k = 0; k < n; k++) {
        renault_0435r_emit_raw_bit(instance, level ? 1 : 0);
    }
}

uint8_t tpms_protocol_decoder_renault_0435r_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderRenault0435R* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_renault_0435r_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderRenault0435R* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus tpms_protocol_decoder_renault_0435r_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderRenault0435R* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_renault_0435r_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_renault_0435r_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderRenault0435R* instance = context;
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

// -----------------------------------------------------------------------------
// Encoder
// -----------------------------------------------------------------------------
//
// FSK Manchester-II, non-inverting (matches the decoder's 0x5556 convention:
// data bit = 1st half of each transitioning pair):
//   bit 1 -> first half HIGH, second half LOW
//   bit 0 -> first half LOW,  second half HIGH
// Frame: gap (reset) + raw 0x55 0x55 0x55 0x56 preamble + 72 Manchester data bits.
//
// The 72-bit frame is b[0..8]; the serialized generic.data holds only the low 64
// bits (b[1..8]). b[0] is the top ID byte, recovered from generic.id. b[7] is the
// XOR checksum, recomputed here so the frame is always self-consistent.
//
// Worst-case entries: 1 gap + 32 preamble halves + 2*72 data halves + 1 = 178.
#define TPMS_RENAULT_0435R_ENCODER_UPLOAD_SIZE 200

void* tpms_protocol_encoder_renault_0435r_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderRenault0435R* instance =
        malloc(sizeof(TPMSProtocolEncoderRenault0435R));
    instance->base.protocol = &tpms_protocol_tpms_renault_0435r;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_RENAULT_0435R_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_renault_0435r_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderRenault0435R* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_renault_0435r_stop(void* context) {
    TPMSProtocolEncoderRenault0435R* instance = context;
    instance->encoder.is_running = false;
}

// Rebuild the full 9-byte frame. b[1..8] come from generic.data; b[0] (top ID
// byte) is recovered from generic.id; b[7] (XOR checksum) is recomputed so that
// XOR(b[0..8]) == 0.
static void
    tpms_protocol_encoder_renault_0435r_build_frame(TPMSBlockGeneric* generic, uint8_t b[9]) {
    for(int i = 0; i < 8; i++) {
        b[i + 1] = (generic->data >> (8 * (7 - i))) & 0xFF;
    }
    b[0] = (uint8_t)((generic->id >> 16) & 0xFF);
    b[7] = 0x00;
    uint8_t x = 0;
    for(int i = 0; i < 9; i++)
        if(i != 7) x ^= b[i];
    b[7] = x; // makes XOR(b[0..8]) == 0
}

static bool
    tpms_protocol_encoder_renault_0435r_get_upload(TPMSProtocolEncoderRenault0435R* instance) {
    furi_assert(instance);
    const uint32_t te = tpms_protocol_renault_0435r_const.te_short;

    uint8_t frame[9];
    tpms_protocol_encoder_renault_0435r_build_frame(&instance->generic, frame);

    size_t index = 0;
    const size_t max = instance->encoder.size_upload;
    bool last_level = false;
    uint32_t last_duration = 0;

#define EMIT(lvl, dur)                                                                \
    do {                                                                              \
        if(last_duration != 0 && last_level == (lvl)) {                               \
            last_duration += (dur);                                                   \
        } else {                                                                      \
            if(last_duration != 0) {                                                  \
                if(index >= max) return false;                                        \
                instance->encoder.upload[index++] =                                   \
                    level_duration_make(last_level, last_duration);                   \
            }                                                                         \
            last_level = (lvl);                                                       \
            last_duration = (dur);                                                    \
        }                                                                             \
    } while(0)

// Manchester data bit: 1st half = the data bit, 2nd half = its complement (the
// 0x5556 convention the decoder uses: data = 1st half of each transitioning pair).
#define MAN_BIT(bit)                 \
    do {                             \
        if(bit) {                    \
            EMIT(true, te);          \
            EMIT(false, te);         \
        } else {                     \
            EMIT(false, te);         \
            EMIT(true, te);          \
        }                            \
    } while(0)

    EMIT(false, te * 50);

    // Raw sync preamble — NOT Manchester. Each preamble bit is ONE raw
    // half-symbol; the decoder hunts the raw 0x5556 (or inverted 0xAAA9) pattern.
    static const uint8_t preamble[4] = {0x55, 0x55, 0x55, 0x56};
    for(int i = 0; i < 4; i++) {
        for(int b = 7; b >= 0; b--) {
            EMIT(((preamble[i] >> b) & 0x1) ? true : false, te);
        }
    }

    // 72 data bits, MSB first (frame[0]..frame[8]).
    for(int i = 0; i < RENAULT_0435R_DATA_BITS; i++) {
        int byte = i / 8;
        int bit = 7 - (i % 8);
        MAN_BIT((frame[byte] >> bit) & 0x1);
    }

    if(last_duration != 0) {
        if(index >= max) return false;
        instance->encoder.upload[index++] = level_duration_make(last_level, last_duration);
    }

#undef MAN_BIT
#undef EMIT

    instance->encoder.size_upload = index;
    return true;
}

SubGhzProtocolStatus tpms_protocol_encoder_renault_0435r_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderRenault0435R* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_renault_0435r_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_RENAULT_0435R_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_renault_0435r_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_renault_0435r_yield(void* context) {
    TPMSProtocolEncoderRenault0435R* instance = context;

    if(instance->encoder.repeat == 0 || !instance->encoder.is_running) {
        instance->encoder.is_running = false;
        return level_duration_reset();
    }

    LevelDuration ret = instance->encoder.upload[instance->encoder.front];

    if(++instance->encoder.front == instance->encoder.size_upload) {
        if(!subghz_block_generic_global.endless_tx) instance->encoder.repeat--;
        instance->encoder.front = 0;
    }

    return ret;
}
