#include "tpms_jeep.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include "tpms_generic.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSJeep"

// Port of rtl_433/src/devices/tpms_jeep.c
// (Copyright (C) 2026 Christian W. Zuckschwerdt <zany@triq.net>, GPL-2+).
//
// Jeep (Continental?) FSK 433 MHz, 10 byte Manchester-encoded TPMS data.
// Seen on a 2021 Jeep Grand Cherokee (WK2). The wire format, framing and
// checksum are IDENTICAL to tpms_citroen.c (same 0x55 0x55 0x55 0x56 preamble,
// Manchester coding, byte layout, XOR checksum). The ONLY functional difference
// is pressure scaling: Jeep reports exactly DOUBLE Citroen's `pressure * 1.364`,
// i.e. 2.728 kPa per step (confirmed against a captured session with tires set
// to 33-36 PSI). Temperature and id match without any change.
//
// Transmittable: the encoder (bottom of file) reuses Citroen's raw-preamble +
// Manchester machinery with Jeep's byte layout + 2.728 kPa pressure scaling, so
// the TPMS 0-PSI flat-tire spoof can rebuild and re-send a picked-up Jeep sensor.
//
// Wire format (after Manchester-decoding, 10 bytes):
//   UU IIIIIIII FR PP TT BB CC
//   b[0]     = state (not covered by checksum)
//   b[1..4]  = 32-bit ID
//   b[5]     = flags (high nibble) + repeat (low nibble)
//   b[6]     = pressure raw, 2.728 kPa per step
//   b[7]     = temperature in degC + 50
//   b[8]     = battery? (raw)
//   b[9]     = checksum: XOR(b[1..9]) == 0
//
// Preamble raw 0x55 0x55 0x55 0x56 -> trailing 16-bit pattern 0x5556.

#define JEEP_PREAMBLE   0x5556U
#define JEEP_DATA_BITS  80

static const SubGhzBlockConst tpms_protocol_jeep_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = JEEP_DATA_BITS,
};

typedef enum {
    JeepDecoderStepReset = 0,
    JeepDecoderStepFindPreamble,
    JeepDecoderStepData,
} JeepDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    // Jeep/PSA is FSK where the frame is RAW half-symbols: a fixed sync preamble
    // in the raw bits, then the 80-bit payload is Manchester-encoded (matches
    // rtl_433 tpms_citroen, which decodes this recording: search 0xaaa9 in raw
    // bits, then manchester-decode only the data). The old port ran manchester
    // over EVERYTHING and hunted the preamble in the manchester-decoded stream,
    // which never syncs. The payload is 80 bits, so its top 16 bits spill into
    // head_64_bit via subghz_protocol_blocks_add_to_128_bit.
    uint16_t raw_shift; // rolling window of raw half-symbol bits for preamble hunt
    bool in_data; // true once the preamble matched, collecting manchester data
    bool data_second; // pair convention: false=data is 1st half (0x5556), true=2nd (0xAAA9)
    bool have_half; // holding the first half of a manchester pair
    uint8_t first_half; // the first half-symbol of the current pair
    uint64_t head_64_bit; // payload bits above 64 (80-bit frame)
} TPMSProtocolDecoderJeep;

// -- forward declarations (decoder functions are file-static) ----------------
static void* tpms_protocol_decoder_jeep_alloc(SubGhzEnvironment* environment);
static void tpms_protocol_decoder_jeep_free(void* context);
static void tpms_protocol_decoder_jeep_reset(void* context);
static void tpms_protocol_decoder_jeep_feed(void* context, bool level, uint32_t duration);
static uint8_t tpms_protocol_decoder_jeep_get_hash_data(void* context);
static SubGhzProtocolStatus tpms_protocol_decoder_jeep_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
static SubGhzProtocolStatus
    tpms_protocol_decoder_jeep_deserialize(void* context, FlipperFormat* flipper_format);
static void tpms_protocol_decoder_jeep_get_string(void* context, FuriString* output);

const SubGhzProtocolDecoder tpms_protocol_jeep_decoder = {
    .alloc = tpms_protocol_decoder_jeep_alloc,
    .free = tpms_protocol_decoder_jeep_free,
    .feed = tpms_protocol_decoder_jeep_feed,
    .reset = tpms_protocol_decoder_jeep_reset,
    .get_hash_data = tpms_protocol_decoder_jeep_get_hash_data,
    .serialize = tpms_protocol_decoder_jeep_serialize,
    .deserialize = tpms_protocol_decoder_jeep_deserialize,
    .get_string = tpms_protocol_decoder_jeep_get_string,
};

// -- encoder forward declarations (Jeep is transmittable: same wire format as
//    Citroen — raw 0x55555556 preamble + 80 Manchester bits — so the flat-tire
//    0-PSI spoof can rebuild and re-send a picked-up Jeep sensor) -------------
void* tpms_protocol_encoder_jeep_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_jeep_free(void* context);
void tpms_protocol_encoder_jeep_stop(void* context);
SubGhzProtocolStatus
    tpms_protocol_encoder_jeep_deserialize(void* context, FlipperFormat* flipper_format);
LevelDuration tpms_protocol_encoder_jeep_yield(void* context);

const SubGhzProtocolEncoder tpms_protocol_jeep_encoder = {
    .alloc = tpms_protocol_encoder_jeep_alloc,
    .free = tpms_protocol_encoder_jeep_free,
    .deserialize = tpms_protocol_encoder_jeep_deserialize,
    .stop = tpms_protocol_encoder_jeep_stop,
    .yield = tpms_protocol_encoder_jeep_yield,
};

const SubGhzProtocol tpms_protocol_jeep = {
    .name = TPMS_PROTOCOL_JEEP_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_jeep_decoder,
    .encoder = &tpms_protocol_jeep_encoder,
};

static void* tpms_protocol_decoder_jeep_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderJeep* instance = calloc(1, sizeof(TPMSProtocolDecoderJeep));
    instance->base.protocol = &tpms_protocol_jeep;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

static void tpms_protocol_decoder_jeep_free(void* context) {
    furi_assert(context);
    free(context);
}

static void tpms_protocol_decoder_jeep_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderJeep* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->raw_shift = 0;
    instance->in_data = false;
    instance->data_second = false;
    instance->have_half = false;
    instance->first_half = 0;
    instance->head_64_bit = 0;
}

static void jeep_get_bytes(TPMSProtocolDecoderJeep* instance, uint8_t b[10]) {
    // 80 bits total: head_64_bit low 16 bits = b[0..1] (MSB-first),
    // decode_data 64 bits = b[2..9].
    b[0] = (uint8_t)((instance->head_64_bit >> 8) & 0xFF);
    b[1] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 2] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

static bool jeep_check_crc(TPMSProtocolDecoderJeep* instance) {
    uint8_t b[10];
    jeep_get_bytes(instance, b);
    // rtl_433 sanity: pressure and temperature bytes must be non-zero.
    if(b[6] == 0 || b[7] == 0) return false;
    uint8_t x = 0;
    for(int i = 1; i <= 9; i++) x ^= b[i];
    return x == 0;
}

static void jeep_analyze(TPMSProtocolDecoderJeep* instance) {
    uint8_t b[10];
    jeep_get_bytes(instance, b);
    instance->generic.id =
        ((uint32_t)b[1] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 8) | b[4];
    // Pressure: 2.728 kPa per step -> bar (kPa * 0.01). This is the only
    // difference from Citroen (which uses 1.364 kPa per step).
    instance->generic.pressure = ((float)b[6] * 2.728f) * 0.01f;
    instance->generic.temperature = (float)((int)b[7] - 50);
    instance->generic.battery_low = b[8];
}

// Consume one raw half-symbol bit: hunt the raw preamble (0x5556, or the
// inverted-polarity 0xAAA9), then Manchester-decode the 80-bit payload. Matches
// rtl_433 tpms_citroen (preamble 0xaaa9 searched in raw bits; only the data is
// Manchester). The 80-bit payload spills its top 16 bits into head_64_bit.
static void jeep_emit_raw_bit(TPMSProtocolDecoderJeep* instance, uint8_t bit) {
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
    // Inside the Manchester payload: pair up half-symbols. A valid pair is a
    // transition (halves differ); the data bit is the 1st half (0x5556) or 2nd
    // half (0xAAA9). No transition = end/garbage -> resync on the preamble.
    if(!instance->have_half) {
        instance->first_half = bit;
        instance->have_half = true;
        return;
    }
    instance->have_half = false;
    if(instance->first_half == bit) {
        instance->in_data = false;
        instance->raw_shift = 0;
        return;
    }
    uint8_t data_bit = instance->data_second ? bit : instance->first_half;
    // 80-bit accumulation: add_to_128_bit shifts the top bits into head_64_bit.
    subghz_protocol_blocks_add_to_128_bit(&instance->decoder, data_bit, &instance->head_64_bit);
    if(instance->decoder.decode_count_bit == tpms_protocol_jeep_const.min_count_bit_for_found) {
        if(jeep_check_crc(instance)) {
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            jeep_analyze(instance);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        instance->in_data = false;
        instance->raw_shift = 0;
    }
}

static void tpms_protocol_decoder_jeep_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderJeep* instance = context;

    const uint32_t te = tpms_protocol_jeep_const.te_short; // 52us half-symbol
    // A gap (or any pulse longer than two half-symbols) ends the burst.
    if(duration >= te * 3) {
        tpms_protocol_decoder_jeep_reset(instance);
        return;
    }
    // Expand this pulse into 1 or 2 raw half-symbol bits of the current level.
    uint32_t n = (duration + te / 2) / te;
    if(n < 1) n = 1;
    if(n > 2) n = 2;
    for(uint32_t k = 0; k < n; k++) {
        jeep_emit_raw_bit(instance, level ? 1 : 0);
    }
}

static uint8_t tpms_protocol_decoder_jeep_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderJeep* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

static SubGhzProtocolStatus tpms_protocol_decoder_jeep_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderJeep* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

static SubGhzProtocolStatus
    tpms_protocol_decoder_jeep_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderJeep* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, tpms_protocol_jeep_const.min_count_bit_for_found);
}

static void tpms_protocol_decoder_jeep_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderJeep* instance = context;
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
// Encoder — Jeep is wire-identical to Citroen (FSK Manchester-II, raw
// 0x55 0x55 0x55 0x56 preamble, 80 data bits). Only the byte layout differs:
//   b[0]     = state (uncovered)          b[1..4] = 32-bit ID
//   b[5]     = flags/repeat               b[6]    = pressure (2.728 kPa/step)
//   b[7]     = temperature (degC + 50)    b[8]    = battery
//   b[9]     = XOR checksum: XOR(b[1..9]) == 0
// generic.data holds b[2..9]; b[1] is the ID top byte. build_frame rebuilds all
// 10 bytes and recomputes b[9] so every frame carries a valid checksum
// (essential for the 0-PSI spoof, which zeroes b[6] then must re-checksum).
// -----------------------------------------------------------------------------
#define TPMS_JEEP_ENCODER_UPLOAD_SIZE 240

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderJeep;

// Rebuild the 10-byte frame: b[1] = ID top byte, b[2..9] from generic.data,
// b[0] = 0, and b[9] = XOR(b[1..8]) so XOR(b[1..9]) == 0.
static void tpms_protocol_encoder_jeep_build_frame(TPMSBlockGeneric* generic, uint8_t b[10]) {
    b[1] = (uint8_t)((generic->id >> 24) & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 2] = (generic->data >> (8 * (7 - i))) & 0xFF;
    }
    b[0] = 0x00;
    uint8_t x = 0;
    for(int i = 1; i <= 8; i++) x ^= b[i];
    b[9] = x; // makes XOR(b[1..9]) == 0
}

// Re-pack pressure/temperature/battery into generic.data (b[2..9]) and recompute
// the checksum. b[2..5] (low 3 ID bytes + flags) are preserved from generic.data.
void tpms_protocol_jeep_pack(TPMSBlockGeneric* g) {
    furi_assert(g);
    // Pressure: bar = b[6] * 2.728 * 0.01 -> raw = bar / 0.02728.
    int p_raw = (int)(g->pressure / 0.02728f + 0.5f);
    if(p_raw < 0) p_raw = 0;
    if(p_raw > 0xFF) p_raw = 0xFF;
    int t = (int)(g->temperature + (g->temperature >= 0 ? 0.5f : -0.5f)) + 50; // b[7] = C + 50
    if(t < 0) t = 0;
    if(t > 0xFF) t = 0xFF;

    uint8_t b[10];
    b[1] = (uint8_t)((g->id >> 24) & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 2] = (g->data >> (8 * (7 - i))) & 0xFF; // b[2..9] preserved
    }
    b[6] = (uint8_t)p_raw; // pressure
    b[7] = (uint8_t)t; // temperature
    b[8] = g->battery_low; // battery
    uint8_t x = 0;
    for(int i = 1; i <= 8; i++) x ^= b[i];
    b[9] = x; // checksum

    uint64_t data = 0;
    for(int i = 0; i < 8; i++) data = (data << 8) | b[i + 2]; // store b[2..9]
    g->data = data;
    g->data_count_bit = JEEP_DATA_BITS;
}

void* tpms_protocol_encoder_jeep_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderJeep* instance = malloc(sizeof(TPMSProtocolEncoderJeep));
    instance->base.protocol = &tpms_protocol_jeep;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_JEEP_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_jeep_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderJeep* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_jeep_stop(void* context) {
    TPMSProtocolEncoderJeep* instance = context;
    instance->encoder.is_running = false;
}

static bool tpms_protocol_encoder_jeep_get_upload(TPMSProtocolEncoderJeep* instance) {
    furi_assert(instance);
    const uint32_t te = tpms_protocol_jeep_const.te_short;

    uint8_t frame[10];
    tpms_protocol_encoder_jeep_build_frame(&instance->generic, frame);

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

    // Raw sync preamble (one raw half-symbol per bit; NOT Manchester).
    static const uint8_t preamble[4] = {0x55, 0x55, 0x55, 0x56};
    for(int i = 0; i < 4; i++) {
        for(int b = 7; b >= 0; b--) {
            EMIT(((preamble[i] >> b) & 0x1) ? true : false, te);
        }
    }

    // 80 data bits, MSB first (frame[0]..frame[9]), Manchester-encoded.
    for(int i = 0; i < JEEP_DATA_BITS; i++) {
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

SubGhzProtocolStatus
    tpms_protocol_encoder_jeep_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderJeep* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_jeep_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_JEEP_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_jeep_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_jeep_yield(void* context) {
    TPMSProtocolEncoderJeep* instance = context;

    if(instance->encoder.repeat == 0 || !instance->encoder.is_running) {
        instance->encoder.is_running = false;
        return level_duration_reset();
    }

    LevelDuration ret = instance->encoder.upload[instance->encoder.front];

    if(++instance->encoder.front == instance->encoder.size_upload) {
        instance->encoder.front = 0;
        if(!subghz_block_generic_global.endless_tx) instance->encoder.repeat--;
    }

    return ret;
}
