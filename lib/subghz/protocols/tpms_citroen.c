#include "tpms_citroen.h"
#include "../blocks/generic.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSCitroen"

// Port of rtl_433/src/devices/tpms_citroen.c (Christian W. Zuckschwerdt 2017, GPL-2+)
//
// FSK 433 MHz, Manchester-encoded, 10 byte payload.
// Also Peugeot and likely Fiat, Mitsubishi, VDO-types.
//
// Wire format (after Manchester-decoding):
//   UU IIIIIIII FR PP TT BB CC          (10 bytes total)
//   b[0]     = state (not covered by CRC)
//   b[1..4]  = 32-bit ID
//   b[5]     = flags (high nibble) + repeat (low nibble)
//   b[6]     = pressure raw, 1.364 kPa per step
//   b[7]     = temperature in °C + 50
//   b[8]     = battery (or other)
//   b[9]     = XOR(b[1..9]) == 0
//
// Preamble raw 0x55 0x55 0x55 0x56 → trailing 16-bit pattern 0x5556.

#define CITROEN_PREAMBLE 0x5556U
#define CITROEN_DATA_BITS 80

static const SubGhzBlockConst tpms_protocol_citroen_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = CITROEN_DATA_BITS,
};

typedef enum {
    CitroenDecoderStepReset = 0,
    CitroenDecoderStepFindPreamble,
    CitroenDecoderStepData,
} CitroenDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    // Citroen/PSA is FSK where the frame is RAW half-symbols: a fixed sync
    // preamble in the raw bits, then the 80-bit payload is Manchester-encoded
    // (matches rtl_433 tpms_citroen: search 0xaaa9 in raw bits, then
    // manchester-decode only the data). The old port ran manchester over
    // EVERYTHING and hunted the preamble in the manchester-decoded stream, which
    // never syncs. The payload is 80 bits, so its top 16 bits spill into
    // head_64_bit via subghz_protocol_blocks_add_to_128_bit.
    uint16_t raw_shift; // rolling window of raw half-symbol bits for preamble hunt
    bool in_data; // true once the preamble matched, collecting manchester data
    bool data_second; // pair convention: false=data is 1st half (0x5556), true=2nd (0xAAA9)
    bool have_half; // holding the first half of a manchester pair
    uint8_t first_half; // the first half-symbol of the current pair
    uint64_t head_64_bit; // payload bits above 64 (80-bit frame)
} TPMSProtocolDecoderCitroen;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderCitroen;

const SubGhzProtocolDecoder tpms_protocol_citroen_decoder = {
    .alloc = tpms_protocol_decoder_citroen_alloc,
    .free = tpms_protocol_decoder_citroen_free,
    .feed = tpms_protocol_decoder_citroen_feed,
    .reset = tpms_protocol_decoder_citroen_reset,
    .get_hash_data = tpms_protocol_decoder_citroen_get_hash_data,
    .serialize = tpms_protocol_decoder_citroen_serialize,
    .deserialize = tpms_protocol_decoder_citroen_deserialize,
    .get_string = tpms_protocol_decoder_citroen_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_citroen_encoder = {
    .alloc = tpms_protocol_encoder_citroen_alloc,
    .free = tpms_protocol_encoder_citroen_free,
    .deserialize = tpms_protocol_encoder_citroen_deserialize,
    .stop = tpms_protocol_encoder_citroen_stop,
    .yield = tpms_protocol_encoder_citroen_yield,
};

const SubGhzProtocol tpms_protocol_citroen = {
    .name = TPMS_PROTOCOL_CITROEN_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_citroen_decoder,
    .encoder = &tpms_protocol_citroen_encoder,
};

void* tpms_protocol_decoder_citroen_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderCitroen* instance = calloc(1, sizeof(TPMSProtocolDecoderCitroen));
    instance->base.protocol = &tpms_protocol_citroen;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_citroen_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_citroen_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderCitroen* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->raw_shift = 0;
    instance->in_data = false;
    instance->data_second = false;
    instance->have_half = false;
    instance->first_half = 0;
    instance->head_64_bit = 0;
}

static void citroen_get_bytes(TPMSProtocolDecoderCitroen* instance, uint8_t b[10]) {
    // 80 bits total: head_64_bit lower 16 bits = b[0..1] (MSB-first),
    // decode_data 64 bits = b[2..9].
    b[0] = (uint8_t)((instance->head_64_bit >> 8) & 0xFF);
    b[1] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 2] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

static bool citroen_check_crc(TPMSProtocolDecoderCitroen* instance) {
    uint8_t b[10];
    citroen_get_bytes(instance, b);
    uint8_t x = 0;
    for(int i = 1; i <= 9; i++) x ^= b[i];
    return x == 0;
}

static void citroen_analyze(TPMSProtocolDecoderCitroen* instance) {
    uint8_t b[10];
    citroen_get_bytes(instance, b);
    instance->generic.id =
        ((uint32_t)b[1] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 8) | b[4];
    // Pressure raw → kPa → bar
    instance->generic.pressure = ((float)b[6] * 1.364f) * 0.01f;
    instance->generic.temperature = (float)((int)b[7] - 50);
    // b[8] is suspected battery byte — without a confirmed bit mapping just keep
    // the raw byte in battery_low for now; the view can show it.
    instance->generic.battery_low = b[8];
}

void tpms_protocol_citroen_pack(TPMSBlockGeneric* g) {
    furi_assert(g);
    // pressure: bar = b[6]*1.364*0.01 -> raw = bar/0.01364.
    int p_raw = (int)(g->pressure / 0.01364f + 0.5f);
    if(p_raw < 0) p_raw = 0;
    if(p_raw > 0xFF) p_raw = 0xFF;
    int t = (int)(g->temperature + (g->temperature >= 0 ? 0.5f : -0.5f)) + 50; // b[7] = C + 50
    if(t < 0) t = 0;
    if(t > 0xFF) t = 0xFF;
    // generic.data holds b[2..9]; the encoder computes b[1] (XOR checksum) and
    // b[0]=0. The decoder folds b[1] into the ID's top byte, so only 24 ID bits
    // (b[2..4]) are freely set from scratch.
    uint8_t b[8]; // = frame b[2..9]
    b[0] = (uint8_t)((g->id >> 16) & 0xFF); // b[2]
    b[1] = (uint8_t)((g->id >> 8) & 0xFF); // b[3]
    b[2] = (uint8_t)(g->id & 0xFF); // b[4]
    b[3] = 0; // b[5]
    b[4] = (uint8_t)p_raw; // b[6]
    b[5] = (uint8_t)t; // b[7]
    b[6] = 0; // b[8] (battery)
    b[7] = 0; // b[9]
    uint64_t data = 0;
    for(int i = 0; i < 8; i++) data = (data << 8) | b[i];
    g->data = data;
    g->data_count_bit = 80;
}

// Consume one raw half-symbol bit: hunt the raw preamble (0x5556, or the
// inverted-polarity 0xAAA9), then Manchester-decode the 80-bit payload. Matches
// rtl_433 tpms_citroen (preamble 0xaaa9 searched in raw bits; only the data is
// Manchester). The 80-bit payload spills its top 16 bits into head_64_bit.
static void citroen_emit_raw_bit(TPMSProtocolDecoderCitroen* instance, uint8_t bit) {
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
    if(instance->decoder.decode_count_bit == CITROEN_DATA_BITS) {
        if(citroen_check_crc(instance)) {
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            citroen_analyze(instance);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        instance->in_data = false;
        instance->raw_shift = 0;
    }
}

void tpms_protocol_decoder_citroen_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderCitroen* instance = context;

    const uint32_t te = tpms_protocol_citroen_const.te_short; // 52us half-symbol
    // A gap (or any pulse longer than two half-symbols) ends the burst.
    if(duration >= te * 3) {
        tpms_protocol_decoder_citroen_reset(instance);
        return;
    }
    // Expand this pulse into 1 or 2 raw half-symbol bits of the current level.
    uint32_t n = (duration + te / 2) / te;
    if(n < 1) n = 1;
    if(n > 2) n = 2;
    for(uint32_t k = 0; k < n; k++) {
        citroen_emit_raw_bit(instance, level ? 1 : 0);
    }
}

uint8_t tpms_protocol_decoder_citroen_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderCitroen* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_citroen_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderCitroen* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_citroen_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderCitroen* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, tpms_protocol_citroen_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_citroen_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderCitroen* instance = context;
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
// FSK Manchester-II, non-inverting (see round-trip verification):
//   bit 1 -> first half LOW,  second half HIGH
//   bit 0 -> first half HIGH, second half LOW
// Frame: gap (reset) + 0x55 0x55 0x55 0x56 preamble + 80 data bits.
//
// The 80-bit frame is b[0..9]; the serialized generic.data holds only the low 64
// bits (b[2..9]). b[1] is recovered from the XOR integrity field (XOR(b[1..9])==0
// => b[1] = XOR(b[2..9])), which restores it exactly (and makes the decoder's
// XOR check pass). b[0] is a state byte the decoder neither validates nor
// displays, so it is emitted as 0x00.
//
// Worst-case entries: 1 gap + 2*(32 preamble + 80 data) + 1 = 226.
#define TPMS_CITROEN_ENCODER_UPLOAD_SIZE 240

void* tpms_protocol_encoder_citroen_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderCitroen* instance = malloc(sizeof(TPMSProtocolEncoderCitroen));
    instance->base.protocol = &tpms_protocol_citroen;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_CITROEN_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_citroen_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderCitroen* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_citroen_stop(void* context) {
    TPMSProtocolEncoderCitroen* instance = context;
    instance->encoder.is_running = false;
}

// Rebuild the full 10-byte frame. b[2..9] come from generic.data; b[1] is
// recovered from the XOR checksum; b[0] (uncovered state byte) is set to 0.
static void tpms_protocol_encoder_citroen_build_frame(TPMSBlockGeneric* generic, uint8_t b[10]) {
    for(int i = 0; i < 8; i++) {
        b[i + 2] = (generic->data >> (8 * (7 - i))) & 0xFF;
    }
    uint8_t x = 0;
    for(int i = 2; i <= 9; i++) x ^= b[i];
    b[1] = x; // makes XOR(b[1..9]) == 0
    b[0] = 0x00;
}

static bool tpms_protocol_encoder_citroen_get_upload(TPMSProtocolEncoderCitroen* instance) {
    furi_assert(instance);
    const uint32_t te = tpms_protocol_citroen_const.te_short;

    uint8_t frame[10];
    tpms_protocol_encoder_citroen_build_frame(&instance->generic, frame);

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

    // Raw sync preamble — NOT Manchester. The decoder hunts the raw 0x5556 (or
    // inverted 0xAAA9) pattern in the half-symbol stream, so each preamble bit is
    // ONE raw half-symbol; only the data below is Manchester-encoded. (Passing the
    // preamble through MAN_BIT was the bug: it never produced a raw 0x5556.)
    static const uint8_t preamble[4] = {0x55, 0x55, 0x55, 0x56};
    for(int i = 0; i < 4; i++) {
        for(int b = 7; b >= 0; b--) {
            EMIT(((preamble[i] >> b) & 0x1) ? true : false, te);
        }
    }

    // 80 data bits, MSB first (frame[0]..frame[9])
    for(int i = 0; i < CITROEN_DATA_BITS; i++) {
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
    tpms_protocol_encoder_citroen_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderCitroen* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_citroen_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_CITROEN_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_citroen_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_citroen_yield(void* context) {
    TPMSProtocolEncoderCitroen* instance = context;

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
