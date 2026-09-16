#include "tpms_renault.h"
#include "../blocks/generic.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSRenault"

// Port of rtl_433/src/devices/tpms_renault.c (Christian W. Zuckschwerdt 2017, GPL-2+)
//
// FSK 433 MHz, Manchester-encoded, 9 byte payload, CRC-8 poly 0x07 init 0x00.
// Seen on Renault Clio/Captur/Zoe and Dacia Sandero.
//
// Wire format (after Manchester-decoding):
//   F F/PP PP TT II II II ?? ?? CC   (9 bytes total)
//   b[0]&0xFC = flags
//   pressure_raw 10-bit = (b[0]&0x03) << 8 | b[1]   → kPa = raw * 0.75
//   b[2]      = temperature in °C + 30
//   b[3..5]   = 24-bit ID, little-endian (b[5] is MSB)
//   b[6..7]   = unknown (usually 0xFFFF)
//   b[8]      = CRC-8 (poly 0x07, init 0x00) over b[0..7]
//
// Preamble raw 0x55 0x55 0x55 0x56 → we hunt for the trailing 16-bit pattern 0x5556.

#define RENAULT_PREAMBLE 0x5556U
#define RENAULT_DATA_BITS 72

static const SubGhzBlockConst tpms_protocol_renault_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = RENAULT_DATA_BITS,
};

typedef enum {
    RenaultDecoderStepReset = 0,
    RenaultDecoderStepFindPreamble,
    RenaultDecoderStepData,
} RenaultDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    // Renault is FSK where the frame is RAW half-symbols: a fixed sync preamble in
    // the raw bits, then the 72-bit payload is Manchester-encoded (matches
    // rtl_433 tpms_renault: it inverts then searches 0xAAA9 == raw 0x5556, and
    // manchester-decodes only the data). The old port ran manchester over
    // EVERYTHING and hunted the preamble in the manchester-decoded stream, which
    // never syncs.
    uint16_t raw_shift; // rolling window of raw half-symbol bits for preamble hunt
    bool in_data; // true once the preamble matched, collecting manchester data
    bool data_second; // pair convention: false=data is 1st half (0x5556), true=2nd (0xAAA9)
    bool have_half; // holding the first half of a manchester pair
    uint8_t first_half; // the first half-symbol of the current pair
    uint64_t head_64_bit; // upper bits when frame > 64 bit (see subghz_protocol_blocks_add_to_128_bit)
} TPMSProtocolDecoderRenault;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderRenault;

const SubGhzProtocolDecoder tpms_protocol_renault_decoder = {
    .alloc = tpms_protocol_decoder_renault_alloc,
    .free = tpms_protocol_decoder_renault_free,
    .feed = tpms_protocol_decoder_renault_feed,
    .reset = tpms_protocol_decoder_renault_reset,
    .get_hash_data = tpms_protocol_decoder_renault_get_hash_data,
    .serialize = tpms_protocol_decoder_renault_serialize,
    .deserialize = tpms_protocol_decoder_renault_deserialize,
    .get_string = tpms_protocol_decoder_renault_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_renault_encoder = {
    .alloc = tpms_protocol_encoder_renault_alloc,
    .free = tpms_protocol_encoder_renault_free,
    .deserialize = tpms_protocol_encoder_renault_deserialize,
    .stop = tpms_protocol_encoder_renault_stop,
    .yield = tpms_protocol_encoder_renault_yield,
};

const SubGhzProtocol tpms_protocol_renault = {
    .name = TPMS_PROTOCOL_RENAULT_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_renault_decoder,
    .encoder = &tpms_protocol_renault_encoder,
};

void* tpms_protocol_decoder_renault_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderRenault* instance = calloc(1, sizeof(TPMSProtocolDecoderRenault));
    instance->base.protocol = &tpms_protocol_renault;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_renault_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_renault_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderRenault* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->raw_shift = 0;
    instance->in_data = false;
    instance->data_second = false;
    instance->have_half = false;
    instance->first_half = 0;
    instance->head_64_bit = 0;
}

static void renault_get_bytes(TPMSProtocolDecoderRenault* instance, uint8_t b[9]) {
    // 72 bits total. add_to_128_bit shifts the upper 8 bits into head_64_bit
    // (low byte = b[0]); the remaining 64 bits are in decode_data (= b[1..8]).
    b[0] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 1] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

static bool renault_check_crc(TPMSProtocolDecoderRenault* instance) {
    uint8_t b[9];
    renault_get_bytes(instance, b);
    return subghz_protocol_blocks_crc8(b, 8, 0x07, 0x00) == b[8];
}

static void renault_analyze(TPMSProtocolDecoderRenault* instance) {
    uint8_t b[9];
    renault_get_bytes(instance, b);
    uint16_t p_raw = ((uint16_t)(b[0] & 0x03) << 8) | b[1];
    instance->generic.pressure = (float)p_raw * 0.75f * 0.01f; // kPa → bar
    instance->generic.temperature = (float)((int)b[2] - 30);
    instance->generic.id = ((uint32_t)b[5] << 16) | ((uint32_t)b[4] << 8) | b[3];
    instance->generic.battery_low = TPMS_NO_BATT;
}

void tpms_protocol_renault_pack(TPMSBlockGeneric* g) {
    furi_assert(g);
    // pressure (10-bit): b[0] bits1:0 = MSB, b[1] = LSB. bar = p_raw*0.75*0.01.
    int p_raw = (int)(g->pressure / 0.0075f + 0.5f);
    if(p_raw < 0) p_raw = 0;
    if(p_raw > 0x3FF) p_raw = 0x3FF;
    int t = (int)(g->temperature + (g->temperature >= 0 ? 0.5f : -0.5f)) + 30; // b[2] = C + 30
    if(t < 0) t = 0;
    if(t > 0xFF) t = 0xFF;
    uint8_t b[9];
    b[0] = (uint8_t)((p_raw >> 8) & 0x03);
    b[1] = (uint8_t)(p_raw & 0xFF);
    b[2] = (uint8_t)t;
    b[3] = (uint8_t)(g->id & 0xFF);
    b[4] = (uint8_t)((g->id >> 8) & 0xFF);
    b[5] = (uint8_t)((g->id >> 16) & 0xFF);
    b[6] = 0;
    b[7] = 0;
    b[8] = subghz_protocol_blocks_crc8(b, 8, 0x07, 0x00);
    // generic.data holds b[1..8]; the encoder recovers b[0] from the CRC.
    uint64_t data = 0;
    for(int i = 1; i <= 8; i++) data = (data << 8) | b[i];
    g->data = data;
    g->data_count_bit = 72;
}

// Consume one raw half-symbol bit: hunt the raw preamble (0x5556, or the
// inverted-polarity 0xAAA9), then Manchester-decode the 72-bit payload. Matches
// rtl_433 tpms_renault (preamble searched in raw bits; only the data is
// Manchester). The frame is >64 bits, so the top bits spill into head_64_bit via
// subghz_protocol_blocks_add_to_128_bit (low byte lands in head_64_bit = b[0]).
static void renault_emit_raw_bit(TPMSProtocolDecoderRenault* instance, uint8_t bit) {
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
    subghz_protocol_blocks_add_to_128_bit(&instance->decoder, data_bit, &instance->head_64_bit);
    if(instance->decoder.decode_count_bit == RENAULT_DATA_BITS) {
        if(renault_check_crc(instance)) {
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            renault_analyze(instance);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        instance->in_data = false;
        instance->raw_shift = 0;
    }
}

void tpms_protocol_decoder_renault_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderRenault* instance = context;

    const uint32_t te = tpms_protocol_renault_const.te_short; // 52us half-symbol
    // A gap (or any pulse longer than two half-symbols) ends the burst.
    if(duration >= te * 3) {
        tpms_protocol_decoder_renault_reset(instance);
        return;
    }
    // Expand this pulse into 1 or 2 raw half-symbol bits of the current level.
    uint32_t n = (duration + te / 2) / te;
    if(n < 1) n = 1;
    if(n > 2) n = 2;
    for(uint32_t k = 0; k < n; k++) {
        renault_emit_raw_bit(instance, level ? 1 : 0);
    }
}

uint8_t tpms_protocol_decoder_renault_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderRenault* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_renault_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderRenault* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_renault_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderRenault* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, tpms_protocol_renault_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_renault_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderRenault* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%06lX\r\n"
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
// Frame: gap (reset) + 0x55 0x55 0x55 0x56 preamble + 72 data bits.
//
// The 72-bit frame is b[0..8]; the serialized generic.data only holds the low 64
// bits (b[1..8]). The top byte b[0] (flags + 2 high pressure bits) is recovered
// by brute force from the trailing CRC-8 (poly 0x07, init 0x00 over b[0..7] ==
// b[8]) — a leading input byte is uniquely determined by the CRC given the rest,
// so this restores the original b[0] bit-exactly.
//
// Worst-case entries: 1 gap + 2*(32 preamble + 72 data) + 1 = 210.
#define TPMS_RENAULT_ENCODER_UPLOAD_SIZE 224

void* tpms_protocol_encoder_renault_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderRenault* instance = malloc(sizeof(TPMSProtocolEncoderRenault));
    instance->base.protocol = &tpms_protocol_renault;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_RENAULT_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_renault_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderRenault* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_renault_stop(void* context) {
    TPMSProtocolEncoderRenault* instance = context;
    instance->encoder.is_running = false;
}

// Rebuild the full 9-byte frame. b[1..8] come from generic.data; b[0] is
// recovered from the CRC so the decoder's CRC check passes on replay.
static void tpms_protocol_encoder_renault_build_frame(TPMSBlockGeneric* generic, uint8_t b[9]) {
    for(int i = 0; i < 8; i++) {
        b[i + 1] = (generic->data >> (8 * (7 - i))) & 0xFF;
    }
    b[0] = 0x00;
    for(int cand = 0; cand < 256; cand++) {
        uint8_t msg[8];
        msg[0] = (uint8_t)cand;
        for(int i = 1; i < 8; i++) msg[i] = b[i];
        if(subghz_protocol_blocks_crc8(msg, 8, 0x07, 0x00) == b[8]) {
            b[0] = (uint8_t)cand;
            break;
        }
    }
}

static bool tpms_protocol_encoder_renault_get_upload(TPMSProtocolEncoderRenault* instance) {
    furi_assert(instance);
    const uint32_t te = tpms_protocol_renault_const.te_short;

    uint8_t frame[9];
    tpms_protocol_encoder_renault_build_frame(&instance->generic, frame);

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

// Manchester data bit: 1st half = the data bit (decoder reads data = 1st half of
// each transitioning pair for the 0x5556 preamble).
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

    // Raw sync preamble (NOT Manchester): decoder hunts the raw 0x5556 pattern.
    static const uint8_t preamble[4] = {0x55, 0x55, 0x55, 0x56};
    for(int i = 0; i < 4; i++) {
        for(int b = 7; b >= 0; b--) {
            EMIT(((preamble[i] >> b) & 0x1) ? true : false, te);
        }
    }

    // 72 data bits, MSB first (frame[0]..frame[8])
    for(int i = 0; i < RENAULT_DATA_BITS; i++) {
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
    tpms_protocol_encoder_renault_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderRenault* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_renault_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_RENAULT_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_renault_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_renault_yield(void* context) {
    TPMSProtocolEncoderRenault* instance = context;

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
