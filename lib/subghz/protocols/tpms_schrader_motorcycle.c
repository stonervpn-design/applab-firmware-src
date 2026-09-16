#include "tpms_schrader_motorcycle.h"
#include <lib/toolbox/manchester_decoder.h>
#include "../blocks/generic.h"

#define TAG "TPMSSchraderMoto"

// Port of rtl_433/src/devices/tpms_schrader_motorcycle.c
//   (Bruno OCTAU / ProfBoc75, 2026, GPL-2+; PR #3569, issue #3512).
//
// Schrader Motorcycle TPMS sensor (e.g. FCC ID MRXRDC3, PN 3142M/29074).
//
// Frequency:  433.92 MHz
// Modulation: OOK, Manchester code with a hardcoded leading zero bit
//             (rtl_433 OOK_PULSE_MANCHESTER_ZEROBIT), s = l = 122 us,
//             reset_limit 375 us.
//
// Wire framing (all Manchester-coded):
//   13-bit preamble 0b0111111111111 (0x0FFF, = first 13 bits of 0x7FF8),
//   then 56 data bits = 7 bytes b[0..6], MSB first:
//
//     Byte      0        1        2        3        4        5        6
//     Layout  FFFFFFII IIIIIIII IIIIIIII IIIIIIPP PPPPPPPP TTTTTTTT CCCCCCCC
//
//   F : 6 bits, fixed 0b111111.
//   I : 24-bit sensor ID (decimal on the sensor label).
//         id = ((b[0]&0x03)<<22) | (b[1]<<14) | (b[2]<<6) | (b[3]>>2)
//   P : 10-bit pressure, kPa = raw * 0.5.
//         raw = ((b[3]&0x03)<<8) | b[4]
//   T : 8-bit temperature, degC = b[5] - 50.
//   C : CRC-8, poly 0x07, init 0xE0, no final xor, over b[0..5].
//         (rtl_433: crc8(b, 7, 0x07, 0xE0) == 0)

#define SCHRADER_MOTO_PREAMBLE     0x0FFFU // 13-bit preamble (leading zerobit + twelve 1s)
#define SCHRADER_MOTO_PREAMBLE_LEN 13
#define SCHRADER_MOTO_DATA_BITS    56

static const SubGhzBlockConst tpms_protocol_tpms_schrader_motorcycle_const = {
    .te_short = 122,
    .te_long = 244,
    .te_delta = 55,
    .min_count_bit_for_found = SCHRADER_MOTO_DATA_BITS,
};

struct TPMSProtocolDecoderSchraderMotorcycle {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;

    ManchesterState manchester_saved_state;
    uint16_t preamble_shift; // rolling window of decoded bits for the preamble hunt
    bool in_data; // true once the preamble matched and we are collecting data bits
};

struct TPMSProtocolEncoderSchraderMotorcycle {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
};

const SubGhzProtocolDecoder tpms_protocol_tpms_schrader_motorcycle_decoder = {
    .alloc = tpms_protocol_decoder_tpms_schrader_motorcycle_alloc,
    .free = tpms_protocol_decoder_tpms_schrader_motorcycle_free,

    .feed = tpms_protocol_decoder_tpms_schrader_motorcycle_feed,
    .reset = tpms_protocol_decoder_tpms_schrader_motorcycle_reset,

    .get_hash_data = tpms_protocol_decoder_tpms_schrader_motorcycle_get_hash_data,
    .serialize = tpms_protocol_decoder_tpms_schrader_motorcycle_serialize,
    .deserialize = tpms_protocol_decoder_tpms_schrader_motorcycle_deserialize,
    .get_string = tpms_protocol_decoder_tpms_schrader_motorcycle_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_tpms_schrader_motorcycle_encoder = {
    .alloc = tpms_protocol_encoder_tpms_schrader_motorcycle_alloc,
    .free = tpms_protocol_encoder_tpms_schrader_motorcycle_free,

    .deserialize = tpms_protocol_encoder_tpms_schrader_motorcycle_deserialize,
    .stop = tpms_protocol_encoder_tpms_schrader_motorcycle_stop,
    .yield = tpms_protocol_encoder_tpms_schrader_motorcycle_yield,
};

const SubGhzProtocol tpms_protocol_tpms_schrader_motorcycle = {
    .name = TPMS_PROTOCOL_SCHRADER_MOTO_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,

    .decoder = &tpms_protocol_tpms_schrader_motorcycle_decoder,
    .encoder = &tpms_protocol_tpms_schrader_motorcycle_encoder,
};

void* tpms_protocol_decoder_tpms_schrader_motorcycle_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderSchraderMotorcycle* instance =
        calloc(1, sizeof(TPMSProtocolDecoderSchraderMotorcycle));
    instance->base.protocol = &tpms_protocol_tpms_schrader_motorcycle;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_tpms_schrader_motorcycle_free(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderSchraderMotorcycle* instance = context;
    free(instance);
}

void tpms_protocol_decoder_tpms_schrader_motorcycle_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderSchraderMotorcycle* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->manchester_saved_state = ManchesterStateMid1;
    instance->preamble_shift = 0;
    instance->in_data = false;
}

static void schrader_motorcycle_get_bytes(
    TPMSProtocolDecoderSchraderMotorcycle* instance,
    uint8_t b[7]) {
    // decode_data holds the 56-bit payload, MSB first: b[0] = bits 55..48.
    for(int i = 0; i < 7; i++) {
        b[i] = (uint8_t)((instance->decoder.decode_data >> (8 * (6 - i))) & 0xFF);
    }
}

static bool schrader_motorcycle_check_crc(TPMSProtocolDecoderSchraderMotorcycle* instance) {
    uint8_t b[7];
    schrader_motorcycle_get_bytes(instance, b);
    // rtl_433: crc8(b, 7, 0x07, 0xE0) == 0  <=>  crc8(b[0..5], 6, 0x07, 0xE0) == b[6].
    uint8_t crc = subghz_protocol_blocks_crc8(b, 6, 0x07, 0xE0);
    return crc == b[6];
}

static void schrader_motorcycle_analyze(TPMSProtocolDecoderSchraderMotorcycle* instance) {
    uint8_t b[7];
    schrader_motorcycle_get_bytes(instance, b);

    instance->generic.id = ((uint32_t)(b[0] & 0x03) << 22) | ((uint32_t)b[1] << 14) |
                           ((uint32_t)b[2] << 6) | (b[3] >> 2);

    uint16_t pressure_raw = (uint16_t)(((b[3] & 0x03) << 8) | b[4]);
    // rtl_433: pressure_kPa = raw * 0.5. Framework stores bar (kPa / 100).
    instance->generic.pressure = (float)pressure_raw * 0.5f * 0.01f;

    instance->generic.temperature = (float)((int)b[5] - 50);

    // No documented battery bit; the F field is fixed 0b111111, not a battery flag.
    instance->generic.battery_low = 0;
}

static ManchesterEvent schrader_motorcycle_level_to_event(bool level, uint32_t duration) {
    bool is_long = false;

    if(DURATION_DIFF(duration, tpms_protocol_tpms_schrader_motorcycle_const.te_long) <
       tpms_protocol_tpms_schrader_motorcycle_const.te_delta) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_tpms_schrader_motorcycle_const.te_short) <
        tpms_protocol_tpms_schrader_motorcycle_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }

    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

void tpms_protocol_decoder_tpms_schrader_motorcycle_feed(
    void* context,
    bool level,
    uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderSchraderMotorcycle* instance = context;

    ManchesterEvent event = schrader_motorcycle_level_to_event(level, duration);

    // A gap (or any out-of-range pulse) resets the Manchester machine and the
    // preamble hunt. The inter-frame silence is what re-syncs between bursts.
    if(event == ManchesterEventReset) {
        instance->manchester_saved_state = ManchesterStateMid1;
        instance->preamble_shift = 0;
        instance->in_data = false;
        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;
        return;
    }

    bool bit = false;
    bool have_bit = manchester_advance(
        instance->manchester_saved_state, event, &instance->manchester_saved_state, &bit);
    if(!have_bit) return;

    // Signal is Manchester II, the decoder machine is Manchester I -> invert
    // (same convention as the working schrader_gg4 OOK_PULSE_MANCHESTER_ZEROBIT
    // port). This reproduces rtl_433's decoded bit values.
    bit = !bit;

    if(!instance->in_data) {
        // Hunt the 13-bit preamble. The leading zero bit is the "zerobit" that
        // rtl_433 hardcodes (its first Manchester half-symbol is silence), so the
        // window starts at 0 and the twelve decoded 1s complete 0x0FFF.
        instance->preamble_shift = (uint16_t)(((instance->preamble_shift << 1) | bit) &
                                              0x1FFF);
        if(instance->preamble_shift == SCHRADER_MOTO_PREAMBLE) {
            instance->in_data = true;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
        }
        return;
    }

    subghz_protocol_blocks_add_bit(&instance->decoder, bit);
    if(instance->decoder.decode_count_bit == SCHRADER_MOTO_DATA_BITS) {
        if(schrader_motorcycle_check_crc(instance)) {
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            schrader_motorcycle_analyze(instance);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        // Re-arm for the next frame in the burst.
        instance->in_data = false;
        instance->preamble_shift = 0;
        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;
    }
}

uint8_t tpms_protocol_decoder_tpms_schrader_motorcycle_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderSchraderMotorcycle* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_tpms_schrader_motorcycle_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderSchraderMotorcycle* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus tpms_protocol_decoder_tpms_schrader_motorcycle_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderSchraderMotorcycle* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_tpms_schrader_motorcycle_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_tpms_schrader_motorcycle_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderSchraderMotorcycle* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX\r\n"
        "Bat:%d\r\n"
        "Temp:%2.0f C Bar:%2.2f (%2.0f kPa)",
        instance->generic.protocol_name,
        instance->generic.id,
        instance->generic.battery_low,
        (double)instance->generic.temperature,
        (double)instance->generic.pressure,
        (double)(instance->generic.pressure * 100.0f));
}

// -----------------------------------------------------------------------------
// Encoder
// -----------------------------------------------------------------------------
//
// Reproduces the exact wire format: an inter-frame gap, the 13-bit preamble, and
// the 56 data bits, all Manchester-encoded (Manchester II, matching the decoder
// which reads Manchester I and inverts):
//   bit 1 -> first half HIGH, second half LOW
//   bit 0 -> first half LOW,  second half HIGH
// Adjacent identical half-symbols are run-length merged, so back-to-back equal
// halves collapse into a single te_long entry.
//
// Worst case: 1 gap + 2*(13 preamble + 56 data) + 1 flush = 140 entries.
#define TPMS_SCHRADER_MOTO_ENCODER_UPLOAD_SIZE 200

void* tpms_protocol_encoder_tpms_schrader_motorcycle_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderSchraderMotorcycle* instance =
        malloc(sizeof(TPMSProtocolEncoderSchraderMotorcycle));
    instance->base.protocol = &tpms_protocol_tpms_schrader_motorcycle;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_SCHRADER_MOTO_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_tpms_schrader_motorcycle_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderSchraderMotorcycle* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_tpms_schrader_motorcycle_stop(void* context) {
    TPMSProtocolEncoderSchraderMotorcycle* instance = context;
    instance->encoder.is_running = false;
}

// Rebuild the 7 payload bytes from generic.data (the low 56 bits).
static void schrader_motorcycle_build_frame(TPMSBlockGeneric* generic, uint8_t b[7]) {
    for(int i = 0; i < 7; i++) {
        b[i] = (uint8_t)((generic->data >> (8 * (6 - i))) & 0xFF);
    }
    // Recompute the CRC so an edited payload is always internally consistent.
    b[6] = subghz_protocol_blocks_crc8(b, 6, 0x07, 0xE0);
}

static bool tpms_protocol_encoder_tpms_schrader_motorcycle_get_upload(
    TPMSProtocolEncoderSchraderMotorcycle* instance) {
    furi_assert(instance);

    const uint32_t te = tpms_protocol_tpms_schrader_motorcycle_const.te_short;

    uint8_t frame[7];
    schrader_motorcycle_build_frame(&instance->generic, frame);

    size_t index = 0;
    const size_t max = instance->encoder.size_upload;
    bool last_level = false;
    uint32_t last_duration = 0;

#define EMIT(lvl, dur)                                                    \
    do {                                                                  \
        if(last_duration != 0 && last_level == (lvl)) {                   \
            last_duration += (dur);                                       \
        } else {                                                          \
            if(last_duration != 0) {                                      \
                if(index >= max) return false;                           \
                instance->encoder.upload[index++] =                       \
                    level_duration_make(last_level, last_duration);       \
            }                                                             \
            last_level = (lvl);                                           \
            last_duration = (dur);                                        \
        }                                                                 \
    } while(0)

#define MAN_BIT(bit)             \
    do {                         \
        if(bit) {                \
            EMIT(true, te);      \
            EMIT(false, te);     \
        } else {                 \
            EMIT(false, te);     \
            EMIT(true, te);      \
        }                        \
    } while(0)

    // Inter-frame gap so consecutive repeats re-sync (long low -> Reset).
    EMIT(false, te * 50);

    // 13-bit preamble 0b0111111111111 (0x0FFF), MSB first.
    for(int i = SCHRADER_MOTO_PREAMBLE_LEN - 1; i >= 0; i--) {
        MAN_BIT((SCHRADER_MOTO_PREAMBLE >> i) & 0x1);
    }

    // 56 data bits, MSB first (frame[0]..frame[6]).
    for(int i = 0; i < SCHRADER_MOTO_DATA_BITS; i++) {
        int byte = i / 8;
        int bit = 7 - (i % 8);
        MAN_BIT((frame[byte] >> bit) & 0x1);
    }

    // Flush the trailing pending entry.
    if(last_duration != 0) {
        if(index >= max) return false;
        instance->encoder.upload[index++] = level_duration_make(last_level, last_duration);
    }

#undef MAN_BIT
#undef EMIT

    instance->encoder.size_upload = index;
    return true;
}

SubGhzProtocolStatus tpms_protocol_encoder_tpms_schrader_motorcycle_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderSchraderMotorcycle* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_tpms_schrader_motorcycle_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }

        // Optional repeat override.
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_SCHRADER_MOTO_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_tpms_schrader_motorcycle_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

void tpms_protocol_tpms_schrader_motorcycle_pack(TPMSBlockGeneric* g) {
    furi_assert(g);

    uint32_t id = g->id & 0x00FFFFFFUL; // 24-bit ID

    // pressure: bar -> kPa (*100) -> raw (/0.5) == bar * 200. 10-bit field.
    int32_t pressure_raw = (int32_t)(g->pressure * 200.0f + 0.5f);
    if(pressure_raw < 0) pressure_raw = 0;
    if(pressure_raw > 0x3FF) pressure_raw = 0x3FF;

    // temperature: degC + 50, 8-bit field. (temp + 50) is >= 0 for any real
    // temperature (>= -50 C), so a straight +0.5 rounds to nearest correctly.
    int32_t temp_raw = (int32_t)(g->temperature + 50.0f + 0.5f);
    if(temp_raw < 0) temp_raw = 0;
    if(temp_raw > 0xFF) temp_raw = 0xFF;

    uint8_t b[7];
    b[0] = (uint8_t)(0xFC | ((id >> 22) & 0x03)); // F=0b111111 + ID[23:22]
    b[1] = (uint8_t)((id >> 14) & 0xFF); // ID[21:14]
    b[2] = (uint8_t)((id >> 6) & 0xFF); // ID[13:6]
    b[3] = (uint8_t)(((id & 0x3F) << 2) | ((pressure_raw >> 8) & 0x03)); // ID[5:0] + P[9:8]
    b[4] = (uint8_t)(pressure_raw & 0xFF); // P[7:0]
    b[5] = (uint8_t)temp_raw; // temperature
    b[6] = subghz_protocol_blocks_crc8(b, 6, 0x07, 0xE0); // CRC-8

    uint64_t data = 0;
    for(int i = 0; i < 7; i++) {
        data = (data << 8) | b[i];
    }
    g->data = data;
    g->data_count_bit = SCHRADER_MOTO_DATA_BITS;
}

LevelDuration tpms_protocol_encoder_tpms_schrader_motorcycle_yield(void* context) {
    TPMSProtocolEncoderSchraderMotorcycle* instance = context;

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
