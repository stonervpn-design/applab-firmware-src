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
    ManchesterState manchester_saved_state;
    uint16_t header_shift;
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
    instance->manchester_saved_state = ManchesterStateStart1;
    return instance;
}

void tpms_protocol_decoder_renault_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_renault_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderRenault* instance = context;
    instance->decoder.parser_step = RenaultDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->head_64_bit = 0;
    instance->header_shift = 0;
    instance->manchester_saved_state = ManchesterStateStart1;
}

static ManchesterEvent renault_evt(bool level, uint32_t duration) {
    bool is_long = false;
    if(DURATION_DIFF(duration, tpms_protocol_renault_const.te_long) <
       tpms_protocol_renault_const.te_delta * 2) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_renault_const.te_short) <
        tpms_protocol_renault_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }
    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
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

void tpms_protocol_decoder_renault_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderRenault* instance = context;

    ManchesterEvent event = renault_evt(level, duration);
    if(event == ManchesterEventReset) {
        instance->decoder.parser_step = RenaultDecoderStepReset;
        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;
        instance->head_64_bit = 0;
        instance->header_shift = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        return;
    }

    bool bit = false;
    bool have_bit = manchester_advance(
        instance->manchester_saved_state, event, &instance->manchester_saved_state, &bit);
    if(!have_bit) return;

    switch(instance->decoder.parser_step) {
    case RenaultDecoderStepReset:
        instance->header_shift = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        instance->decoder.parser_step = RenaultDecoderStepFindPreamble;
        /* fallthrough */
    case RenaultDecoderStepFindPreamble:
        instance->header_shift = (uint16_t)((instance->header_shift << 1) | (bit ? 1 : 0));
        if(instance->header_shift == RENAULT_PREAMBLE) {
            instance->decoder.parser_step = RenaultDecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->head_64_bit = 0;
        }
        break;

    case RenaultDecoderStepData:
        subghz_protocol_blocks_add_to_128_bit(
            &instance->decoder, bit ? 1 : 0, &instance->head_64_bit);
        if(instance->decoder.decode_count_bit ==
           tpms_protocol_renault_const.min_count_bit_for_found) {
            if(renault_check_crc(instance)) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                renault_analyze(instance);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = RenaultDecoderStepReset;
        }
        break;
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

#define MAN_BIT(bit)                 \
    do {                             \
        if(bit) {                    \
            EMIT(false, te);         \
            EMIT(true, te);          \
        } else {                     \
            EMIT(true, te);          \
            EMIT(false, te);         \
        }                            \
    } while(0)

    EMIT(false, te * 50);

    static const uint8_t preamble[4] = {0x55, 0x55, 0x55, 0x56};
    for(int i = 0; i < 4; i++) {
        for(int b = 7; b >= 0; b--) {
            MAN_BIT((preamble[i] >> b) & 0x1);
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
