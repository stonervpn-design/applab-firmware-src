#include "tpms_schraeder.h"
#include <lib/toolbox/manchester_decoder.h>
#include "../blocks/generic.h"

#define TAG "Schraeder"

// Port of the rtl_433 "schraeder" decoder (src/devices/schraeder.c), the
// decoder registered under r_device `schraeder` (spelled with the extra 'e';
// NOT schrader_EG53MA4 / SMD3MA4 / NIS315G3 / MRXBC5A4, which are separate
// r_devices in that same file).
//
// FCC-Id: MRXGG4
// Frequency: 433.92 MHz
// Modulation: OOK / ASK, OOK_PULSE_MANCHESTER_ZEROBIT (short_width 120 us,
//             reset_limit 480 us).
//
// Packet: 1 sync nibble + 8 data bytes = 68 bits on the wire (rtl_433 rejects
// any row whose length != 68). rtl_433 drops the 4 sync bits
// (bitbuffer_extract_bytes(bitbuffer, 0, 4, b, 64)) and works on the 64-bit
// payload b[0..7]:
//
//     0 12 34 56 78 9A BC DE F0   <- nibble index
//     7 f6 70 3a 38 b2 00 49 49   <- example
//     S PF FI II II II PP TT CC
//
//   S : sync nibble (0x7 in the corpus, discarded)
//   P : preamble nibble, always 0xf (high nibble of b[0])
//   F : 8-bit flags = (b[0] & 0x0F) << 4 | b[1] >> 4
//   I : 28-bit id  = (b[1] & 0x0F) << 24 | b[2] << 16 | b[3] << 8 | b[4]
//   P : pressure   = b[5] * 25 mbar  (25 mbar/bit, 0..6.375 bar) => kPa = b5*2.5
//   T : temperature= b[6] - 50 C     (1 C/bit, -50..205 C)
//   C : CRC8 over b[0..6], poly 0x07, init 0xf0, compared against b[7]
//
// This firmware stores the 64-bit payload b[0..7] (MSB=b[0]) in generic.data.

#define PREAMBLE          0b0111 // the 0x7 sync nibble
#define PREAMBLE_BITS_LEN 4

static const SubGhzBlockConst tpms_protocol_tpms_schraeder_const = {
    .te_short = 120,
    .te_long = 240,
    .te_delta = 55, // ~50% of te_short due to poor sensitivity
    .min_count_bit_for_found = 64,
};

struct TPMSProtocolDecoderTpmsSchraeder {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;

    ManchesterState manchester_saved_state;
    uint16_t header_count;
    uint8_t sync_bits;
};

struct TPMSProtocolEncoderTpmsSchraeder {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
};

typedef enum {
    TpmsSchraederDecoderStepReset = 0,
    TpmsSchraederDecoderStepCheckPreamble,
    TpmsSchraederDecoderStepDecoderData,
} TpmsSchraederDecoderStep;

const SubGhzProtocolDecoder tpms_protocol_tpms_schraeder_decoder = {
    .alloc = tpms_protocol_decoder_tpms_schraeder_alloc,
    .free = tpms_protocol_decoder_tpms_schraeder_free,

    .feed = tpms_protocol_decoder_tpms_schraeder_feed,
    .reset = tpms_protocol_decoder_tpms_schraeder_reset,

    .get_hash_data = tpms_protocol_decoder_tpms_schraeder_get_hash_data,
    .serialize = tpms_protocol_decoder_tpms_schraeder_serialize,
    .deserialize = tpms_protocol_decoder_tpms_schraeder_deserialize,
    .get_string = tpms_protocol_decoder_tpms_schraeder_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_tpms_schraeder_encoder = {
    .alloc = tpms_protocol_encoder_tpms_schraeder_alloc,
    .free = tpms_protocol_encoder_tpms_schraeder_free,

    .deserialize = tpms_protocol_encoder_tpms_schraeder_deserialize,
    .stop = tpms_protocol_encoder_tpms_schraeder_stop,
    .yield = tpms_protocol_encoder_tpms_schraeder_yield,
};

const SubGhzProtocol tpms_protocol_tpms_schraeder = {
    .name = TPMS_PROTOCOL_SCHRAEDER_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Send,

    .decoder = &tpms_protocol_tpms_schraeder_decoder,
    .encoder = &tpms_protocol_tpms_schraeder_encoder,
};

void* tpms_protocol_decoder_tpms_schraeder_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderTpmsSchraeder* instance = malloc(sizeof(TPMSProtocolDecoderTpmsSchraeder));
    instance->base.protocol = &tpms_protocol_tpms_schraeder;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_tpms_schraeder_free(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderTpmsSchraeder* instance = context;
    free(instance);
}

void tpms_protocol_decoder_tpms_schraeder_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderTpmsSchraeder* instance = context;
    instance->decoder.parser_step = TpmsSchraederDecoderStepReset;
}

// CRC8 over the six payload bytes b[0..5]... note rtl_433 covers b[0..6] (7
// bytes) and compares byte b[7]. Our 64-bit word is b[0](MSB)..b[7](LSB).
static bool tpms_protocol_tpms_schraeder_check_crc(TPMSProtocolDecoderTpmsSchraeder* instance) {
    uint64_t data = instance->decoder.decode_data;
    uint8_t msg[7] = {
        (uint8_t)(data >> 56),
        (uint8_t)(data >> 48),
        (uint8_t)(data >> 40),
        (uint8_t)(data >> 32),
        (uint8_t)(data >> 24),
        (uint8_t)(data >> 16),
        (uint8_t)(data >> 8),
    };
    uint8_t crc = subghz_protocol_blocks_crc8(msg, 7, 0x07, 0xf0);
    return (crc == (uint8_t)(data & 0xFF));
}

static void tpms_protocol_tpms_schraeder_analyze(TPMSBlockGeneric* instance) {
    uint64_t data = instance->data;
    // 28-bit id: (b[1] & 0x0F) << 24 | b[2] << 16 | b[3] << 8 | b[4]
    instance->id = (uint32_t)((data >> 24) & 0x0FFFFFFFUL);
    // schraeder carries no documented battery-low bit.
    instance->battery_low = 0;
    // temperature = b[6] - 50  (deg C)
    instance->temperature = (float)((int)((data >> 8) & 0xFF) - 50);
    // pressure raw b[5] = 25 mbar/bit -> kPa = raw * 2.5 -> bar = raw * 0.025
    instance->pressure = (float)((data >> 16) & 0xFF) * 0.025f;
}

static ManchesterEvent level_and_duration_to_event(bool level, uint32_t duration) {
    bool is_long = false;

    if(DURATION_DIFF(duration, tpms_protocol_tpms_schraeder_const.te_long) <
       tpms_protocol_tpms_schraeder_const.te_delta) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_tpms_schraeder_const.te_short) <
        tpms_protocol_tpms_schraeder_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }

    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

void tpms_protocol_decoder_tpms_schraeder_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    bool bit = false;
    bool have_bit = false;
    TPMSProtocolDecoderTpmsSchraeder* instance = context;

    // low-level bit sequence decoding
    if(instance->decoder.parser_step != TpmsSchraederDecoderStepReset) {
        ManchesterEvent event = level_and_duration_to_event(level, duration);

        if(event == ManchesterEventReset) {
            instance->decoder.parser_step = TpmsSchraederDecoderStepReset;
        } else {
            have_bit = manchester_advance(
                instance->manchester_saved_state, event, &instance->manchester_saved_state, &bit);
            if(!have_bit) return;

            // Invert value, due to signal is Manchester II and decoder is Manchester I
            bit = !bit;
        }
    }

    switch(instance->decoder.parser_step) {
    case TpmsSchraederDecoderStepReset:
        // wait for the ~480 us start pulse (te_long * 2) that frames each burst
        if((level) && (DURATION_DIFF(duration, tpms_protocol_tpms_schraeder_const.te_long * 2) <
                       tpms_protocol_tpms_schraeder_const.te_delta)) {
            instance->decoder.parser_step = TpmsSchraederDecoderStepCheckPreamble;
            instance->header_count = 0;
            instance->sync_bits = 0;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;

            // First will be short space -> initial Manchester state
            instance->manchester_saved_state = ManchesterStateStart1;
        }
        break;
    case TpmsSchraederDecoderStepCheckPreamble:
        // collect the 4-bit sync nibble and verify it matches 0x7
        instance->sync_bits = (instance->sync_bits << 1) | (bit ? 1 : 0);
        instance->header_count++;
        if(instance->header_count == PREAMBLE_BITS_LEN) {
            if((instance->sync_bits & 0x0F) != PREAMBLE) {
                instance->decoder.parser_step = TpmsSchraederDecoderStepReset;
            } else {
                instance->decoder.parser_step = TpmsSchraederDecoderStepDecoderData;
            }
        }
        break;

    case TpmsSchraederDecoderStepDecoderData:
        subghz_protocol_blocks_add_bit(&instance->decoder, bit);
        if(instance->decoder.decode_count_bit ==
           tpms_protocol_tpms_schraeder_const.min_count_bit_for_found) {
            if(!tpms_protocol_tpms_schraeder_check_crc(instance)) {
                FURI_LOG_D(TAG, "CRC mismatch drop");
            } else {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                tpms_protocol_tpms_schraeder_analyze(&instance->generic);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = TpmsSchraederDecoderStepReset;
        }
        break;
    }
}

uint8_t tpms_protocol_decoder_tpms_schraeder_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderTpmsSchraeder* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_tpms_schraeder_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderTpmsSchraeder* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_tpms_schraeder_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderTpmsSchraeder* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_tpms_schraeder_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_tpms_schraeder_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderTpmsSchraeder* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX\r\n"
        "Bat:%d\r\n"
        "Temp:%2.0f C Bar:%2.2f",
        instance->generic.protocol_name,
        instance->generic.id,
        instance->generic.battery_low,
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}

// -----------------------------------------------------------------------------
// Encoder
// -----------------------------------------------------------------------------
//
// Worst case: 4 sync bits + 64 data bits = 68 bits, each producing two
// te_short half-symbols -> 136 half-symbols. With the start pulse and the
// inter-frame gap, 200 LevelDuration entries is plenty even before run-length
// merging.
#define TPMS_SCHRAEDER_ENCODER_UPLOAD_SIZE 200

void* tpms_protocol_encoder_tpms_schraeder_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderTpmsSchraeder* instance = malloc(sizeof(TPMSProtocolEncoderTpmsSchraeder));
    instance->base.protocol = &tpms_protocol_tpms_schraeder;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_SCHRAEDER_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_tpms_schraeder_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderTpmsSchraeder* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_tpms_schraeder_stop(void* context) {
    TPMSProtocolEncoderTpmsSchraeder* instance = context;
    instance->encoder.is_running = false;
}

// Manchester-II convention as decoded by the feed() above (which reads
// Manchester I and inverts the output bit):
//
//   bit 0 -> first half LOW, second half HIGH
//   bit 1 -> first half HIGH, second half LOW
//
// Adjacent identical half-symbols are run-length merged so back-to-back equal
// bits produce a single te_long entry on the wire.
static bool
    tpms_protocol_encoder_tpms_schraeder_get_upload(TPMSProtocolEncoderTpmsSchraeder* instance) {
    furi_assert(instance);

    const uint32_t te_short = tpms_protocol_tpms_schraeder_const.te_short;
    const uint32_t te_long = tpms_protocol_tpms_schraeder_const.te_long;

    size_t index = 0;
    const size_t max = instance->encoder.size_upload;
    bool last_level = false;
    uint32_t last_duration = 0;

#define EMIT(lvl, dur)                                          \
    do {                                                        \
        if(last_duration != 0 && last_level == (lvl)) {         \
            last_duration += (dur);                             \
        } else {                                                \
            if(last_duration != 0) {                            \
                if(index >= max) return false;                  \
                instance->encoder.upload[index++] =             \
                    level_duration_make(last_level, last_duration); \
            }                                                   \
            last_level = (lvl);                                 \
            last_duration = (dur);                              \
        }                                                       \
    } while(0)

#define EMIT_BIT(b)             \
    do {                        \
        if(b) {                 \
            EMIT(true, te_short);  \
            EMIT(false, te_short); \
        } else {                \
            EMIT(false, te_short); \
            EMIT(true, te_short);  \
        }                       \
    } while(0)

    // Inter-frame gap before the start pulse so consecutive repeats are framed.
    EMIT(false, te_short * 50);

    // ~480 us start pulse
    EMIT(true, te_long * 2);

    // 4-bit sync nibble (0x7), MSB first
    for(int i = PREAMBLE_BITS_LEN - 1; i >= 0; i--) {
        EMIT_BIT((PREAMBLE >> i) & 0x1);
    }

    // 64 data bits, MSB first
    for(int bit = instance->generic.data_count_bit - 1; bit >= 0; bit--) {
        EMIT_BIT((instance->generic.data >> bit) & 0x1);
    }

    // Flush trailing entry
    if(last_duration != 0) {
        if(index >= max) return false;
        instance->encoder.upload[index++] = level_duration_make(last_level, last_duration);
    }

#undef EMIT_BIT
#undef EMIT

    instance->encoder.size_upload = index;
    return true;
}

SubGhzProtocolStatus
    tpms_protocol_encoder_tpms_schraeder_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderTpmsSchraeder* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_tpms_schraeder_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }

        // Optional repeat override
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        // Restore allocator-side upload capacity before regenerating the upload
        instance->encoder.size_upload = TPMS_SCHRAEDER_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_tpms_schraeder_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

void tpms_protocol_tpms_schraeder_pack(TPMSBlockGeneric* g) {
    furi_assert(g);

    // Inverse of tpms_protocol_tpms_schraeder_analyze().
    //   pressure raw = round(bar / 0.025) = round(bar * 40)   (25 mbar/bit)
    //   temperature raw = round(C + 50)
    int32_t pressure_raw = (int32_t)(g->pressure / 0.025f + 0.5f);
    if(pressure_raw < 0) pressure_raw = 0;
    if(pressure_raw > 0xFF) pressure_raw = 0xFF;

    int32_t temperature_raw = (int32_t)(g->temperature + 50.0f + 0.5f);
    if(temperature_raw < 0) temperature_raw = 0;
    if(temperature_raw > 0xFF) temperature_raw = 0xFF;

    uint8_t b[8];
    // b[0]: high nibble = 0xf preamble, low nibble = flags high (flags = 0)
    b[0] = 0xF0;
    // b[1]: high nibble = flags low (0), low nibble = id[27:24]
    b[1] = (uint8_t)((g->id >> 24) & 0x0F);
    b[2] = (uint8_t)((g->id >> 16) & 0xFF);
    b[3] = (uint8_t)((g->id >> 8) & 0xFF);
    b[4] = (uint8_t)(g->id & 0xFF);
    b[5] = (uint8_t)pressure_raw;
    b[6] = (uint8_t)temperature_raw;
    // CRC8 over b[0..6] (7 bytes), poly 0x07, init 0xf0 — matches the decoder.
    b[7] = subghz_protocol_blocks_crc8(b, 7, 0x07, 0xf0);

    uint64_t data = 0;
    for(int i = 0; i < 8; i++) {
        data = (data << 8) | b[i];
    }
    g->data = data;
    g->data_count_bit = 64;
}

LevelDuration tpms_protocol_encoder_tpms_schraeder_yield(void* context) {
    TPMSProtocolEncoderTpmsSchraeder* instance = context;

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
