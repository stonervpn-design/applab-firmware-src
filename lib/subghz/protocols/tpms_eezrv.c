#include "tpms_eezrv.h"
#include <lib/toolbox/manchester_decoder.h>
#include "../blocks/generic.h"

#define TAG "TPMSEezRV"

// Port of rtl_433/src/devices/tpms_eezrv.c
//   "EezTire E618, Carchet TPMS, TST-507 TPMS"
//   (C) 2023 Bruno OCTAU (ProfBoc75), Gliebig, Karen Suhm — GPL-2+.
//
// Modulation : OOK, Manchester (OOK_PULSE_MANCHESTER_ZEROBIT), 50 us half-symbol
//              (short_width == long_width == 50 us, reset_limit 120 us).
// Frequency  : 433.92 MHz (AM).
//
// rtl_433 inverts the whole bitstream, then searches a 16-bit preamble 0xFFFF
// (i.e. the raw demod carries the complement) and extracts, starting at pos 0:
//   [16 bit preamble 0xFFFF] [8 bit checksum CC] [7 data bytes b0..b6]
// = 80 bits total. This port works in that post-invert ("logical") domain: the
// 16 preamble channel bits are all 1, and the 64 payload channel bits are the
// logical CC + b0..b6, MSB first. (The residual global Manchester polarity — the
// one-bit invert that distinguishes Manchester-I/II — is handled the same way as
// the proven schrader_gg4 OOK port: manchester_advance then bit = !bit. It is the
// only quantity not pinned by the round-trip harness and would be confirmed
// against a real capture; see caveat.)
//
// Logical byte layout (CC + b0..b6):
//   CC     : additive checksum = (sum(b0..b6) & 0xFF), with bit7 forced set when
//            sum(b0..b6) > 0xFF.
//   b0..b2 : 24-bit sensor ID (b0 = MSB).
//   b3     : pressure low byte.
//   b4     : temperature, °C = b4 - 50.
//   b5     : flags1 — bit7 low battery, bit5 inflate detected, bit4 fast-leak.
//   b6     : flags2 — bit0 is pressure bit 8.
//   pressure_kPa = (((flags2 & 0x01) << 8) | b3) * 2.5   (9-bit raw * 2.5).
// Framework stores pressure in BAR: bar = kPa / 100 = raw9 * 0.025.

#define EEZRV_PREAMBLE_BITS 16
#define EEZRV_DATA_BITS     64
#define EEZRV_FRAME_BITS    (EEZRV_PREAMBLE_BITS + EEZRV_DATA_BITS)

static const SubGhzBlockConst tpms_protocol_eezrv_const = {
    .te_short = 50,
    .te_long = 100,
    .te_delta = 25, // 50% of te_short
    .min_count_bit_for_found = EEZRV_DATA_BITS,
};

typedef enum {
    EezRVDecoderStepReset = 0, // disarmed: wait for an inter-frame gap
    EezRVDecoderStepPreamble, // counting the 16 preamble (channel-1) bits
    EezRVDecoderStepData, // collecting the 64 payload bits
} EezRVDecoderStep;

typedef struct TPMSProtocolDecoderEezRV TPMSProtocolDecoderEezRV;
typedef struct TPMSProtocolEncoderEezRV TPMSProtocolEncoderEezRV;

struct TPMSProtocolDecoderEezRV {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;

    ManchesterState manchester_saved_state;
    uint16_t preamble_count;
};

struct TPMSProtocolEncoderEezRV {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
};

const SubGhzProtocolDecoder tpms_protocol_tpms_eezrv_decoder = {
    .alloc = tpms_protocol_decoder_tpms_eezrv_alloc,
    .free = tpms_protocol_decoder_tpms_eezrv_free,

    .feed = tpms_protocol_decoder_tpms_eezrv_feed,
    .reset = tpms_protocol_decoder_tpms_eezrv_reset,

    .get_hash_data = tpms_protocol_decoder_tpms_eezrv_get_hash_data,
    .serialize = tpms_protocol_decoder_tpms_eezrv_serialize,
    .deserialize = tpms_protocol_decoder_tpms_eezrv_deserialize,
    .get_string = tpms_protocol_decoder_tpms_eezrv_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_tpms_eezrv_encoder = {
    .alloc = tpms_protocol_encoder_tpms_eezrv_alloc,
    .free = tpms_protocol_encoder_tpms_eezrv_free,

    .deserialize = tpms_protocol_encoder_tpms_eezrv_deserialize,
    .stop = tpms_protocol_encoder_tpms_eezrv_stop,
    .yield = tpms_protocol_encoder_tpms_eezrv_yield,
};

const SubGhzProtocol tpms_protocol_tpms_eezrv = {
    .name = TPMS_PROTOCOL_EEZRV_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,

    .decoder = &tpms_protocol_tpms_eezrv_decoder,
    .encoder = &tpms_protocol_tpms_eezrv_encoder,
};

void* tpms_protocol_decoder_tpms_eezrv_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderEezRV* instance = calloc(1, sizeof(TPMSProtocolDecoderEezRV));
    instance->base.protocol = &tpms_protocol_tpms_eezrv;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_tpms_eezrv_free(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderEezRV* instance = context;
    free(instance);
}

void tpms_protocol_decoder_tpms_eezrv_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderEezRV* instance = context;
    instance->decoder.parser_step = EezRVDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->preamble_count = 0;
}

// Extract the 7 data bytes b0..b6 from the logical 64-bit payload word
// (bits 63..56 = checksum CC, 55..0 = b0..b6).
static void tpms_eezrv_get_bytes(uint64_t data, uint8_t b[7]) {
    for(int i = 0; i < 7; i++) {
        b[i] = (uint8_t)((data >> (8 * (6 - i))) & 0xFF);
    }
}

// rtl_433: computed = add_bytes(b, 7); if (computed > 0xFF) computed |= 0x80;
// stored CC == (computed & 0xFF).
static uint8_t tpms_eezrv_checksum(const uint8_t b[7]) {
    int computed = 0;
    for(int i = 0; i < 7; i++) computed += b[i];
    if(computed > 0xFF) computed |= 0x80;
    return (uint8_t)(computed & 0xFF);
}

static bool tpms_eezrv_check_checksum(uint64_t data) {
    uint8_t b[7];
    tpms_eezrv_get_bytes(data, b);
    uint8_t cc = (uint8_t)((data >> 56) & 0xFF);
    return tpms_eezrv_checksum(b) == cc;
}

static void tpms_eezrv_analyze(TPMSBlockGeneric* instance) {
    uint8_t b[7];
    tpms_eezrv_get_bytes(instance->data, b);

    // 24-bit ID (b0..b2).
    instance->id = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];

    int flags1 = b[5];
    int flags2 = b[6];

    // pressure raw is 9-bit: b[3] plus bit8 from flags2 bit0; * 2.5 -> kPa.
    int pressure_raw = (((flags2 & 0x01) << 8) | b[3]);
    instance->pressure = (float)pressure_raw * 2.5f * 0.01f; // kPa -> bar

    instance->temperature = (float)((int)b[4] - 50);

    instance->battery_low = (flags1 >> 7) & 0x01;
}

static ManchesterEvent tpms_eezrv_level_and_duration_to_event(bool level, uint32_t duration) {
    bool is_long = false;

    if(DURATION_DIFF(duration, tpms_protocol_eezrv_const.te_long) <
       tpms_protocol_eezrv_const.te_delta) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_eezrv_const.te_short) <
        tpms_protocol_eezrv_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }

    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

void tpms_protocol_decoder_tpms_eezrv_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderEezRV* instance = context;

    ManchesterEvent event = tpms_eezrv_level_and_duration_to_event(level, duration);

    // A gap (or any out-of-range duration) marks a burst boundary: (re)arm and
    // lock the Manchester phase for the fresh frame that follows.
    if(event == ManchesterEventReset) {
        instance->decoder.parser_step = EezRVDecoderStepPreamble;
        // Preamble symbols are HIGH-first (channel-1): the Manchester decoder
        // locks from Start0 (a leading ShortHigh yields a bit and returns to
        // Start0), whereas Start1 would drift.
        instance->manchester_saved_state = ManchesterStateStart0;
        instance->preamble_count = 0;
        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;
        return;
    }

    if(instance->decoder.parser_step == EezRVDecoderStepReset) {
        // Not armed yet (no leading gap seen) — ignore until the next gap.
        return;
    }

    bool bit = false;
    bool have_bit = manchester_advance(
        instance->manchester_saved_state, event, &instance->manchester_saved_state, &bit);
    if(!have_bit) return;

    // Signal is Manchester-II while the toolbox decoder is Manchester-I: invert.
    bit = !bit;
    uint8_t channel = bit ? 1 : 0;

    switch(instance->decoder.parser_step) {
    case EezRVDecoderStepPreamble:
        // 16 preamble channel bits, all 1 (rtl_433 post-invert 0xFFFF).
        if(channel != 1) {
            // Not our preamble — disarm and wait for the next gap.
            instance->decoder.parser_step = EezRVDecoderStepReset;
            break;
        }
        instance->preamble_count++;
        if(instance->preamble_count == EEZRV_PREAMBLE_BITS) {
            instance->decoder.parser_step = EezRVDecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
        }
        break;

    case EezRVDecoderStepData:
        subghz_protocol_blocks_add_bit(&instance->decoder, channel);
        if(instance->decoder.decode_count_bit == EEZRV_DATA_BITS) {
            if(tpms_eezrv_check_checksum(instance->decoder.decode_data)) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                tpms_eezrv_analyze(&instance->generic);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            // One frame consumed — disarm until the next inter-frame gap.
            instance->decoder.parser_step = EezRVDecoderStepReset;
        }
        break;

    default:
        break;
    }
}

uint8_t tpms_protocol_decoder_tpms_eezrv_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderEezRV* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_tpms_eezrv_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderEezRV* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_tpms_eezrv_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderEezRV* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, tpms_protocol_eezrv_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_tpms_eezrv_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderEezRV* instance = context;
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
// Reproduces the exact wire format the decoder consumes: an inter-frame gap,
// then 16 preamble channel-1 symbols, then the 64 logical payload bits
// (CC + b0..b6, MSB first), each as one Manchester symbol.
//   channel bit 1 -> first half HIGH, second half LOW
//   channel bit 0 -> first half LOW,  second half HIGH
// (matches the decoder: manchester_advance then bit = !bit). Adjacent identical
// half-symbols are run-length merged.
//
// Worst case: 1 gap + 2*(16 preamble + 64 data) + 1 flush = 162 entries.
#define TPMS_EEZRV_ENCODER_UPLOAD_SIZE 200

void* tpms_protocol_encoder_tpms_eezrv_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderEezRV* instance = malloc(sizeof(TPMSProtocolEncoderEezRV));
    instance->base.protocol = &tpms_protocol_tpms_eezrv;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_EEZRV_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_tpms_eezrv_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderEezRV* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_tpms_eezrv_stop(void* context) {
    TPMSProtocolEncoderEezRV* instance = context;
    instance->encoder.is_running = false;
}

static bool tpms_protocol_encoder_tpms_eezrv_get_upload(TPMSProtocolEncoderEezRV* instance) {
    furi_assert(instance);

    const uint32_t te = tpms_protocol_eezrv_const.te_short;

    // Recompute the checksum over the freshly (de)serialized payload so an edited
    // id/pressure/temp always ships a valid frame.
    uint64_t data = instance->generic.data;
    uint8_t b[7];
    tpms_eezrv_get_bytes(data, b);
    uint8_t cc = tpms_eezrv_checksum(b);
    data = (data & 0x00FFFFFFFFFFFFFFULL) | ((uint64_t)cc << 56);

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

    // Inter-frame gap so consecutive repeats resync on a Reset event.
    EMIT(false, te * 50);

    // 16 preamble channel-1 symbols (0xFFFF post-invert). The first half is HIGH,
    // so it does not merge with the preceding LOW gap.
    for(int i = 0; i < EEZRV_PREAMBLE_BITS; i++) {
        MAN_BIT(1);
    }

    // 64 payload bits, MSB first (CC first, then b0..b6).
    for(int bit = EEZRV_DATA_BITS - 1; bit >= 0; bit--) {
        MAN_BIT((data >> bit) & 0x1ULL);
    }

    // Flush the trailing entry.
    if(last_duration != 0) {
        if(index >= max) return false;
        instance->encoder.upload[index++] = level_duration_make(last_level, last_duration);
    }

#undef MAN_BIT
#undef EMIT

    instance->encoder.size_upload = index;
    return true;
}

void tpms_protocol_tpms_eezrv_pack(TPMSBlockGeneric* generic) {
    furi_assert(generic);

    // Inverse of eezrv_analyze.
    //   pressure_raw9 = round(bar / 0.025)  (bar = raw9 * 2.5 / 100)
    int32_t pressure_raw = (int32_t)(generic->pressure / (2.5f * 0.01f) + 0.5f);
    if(pressure_raw < 0) pressure_raw = 0;
    if(pressure_raw > 0x1FF) pressure_raw = 0x1FF; // 9-bit field

    int32_t temperature_raw =
        (int32_t)(generic->temperature + (generic->temperature >= 0 ? 0.5f : -0.5f)) + 50;
    if(temperature_raw < 0) temperature_raw = 0;
    if(temperature_raw > 0xFF) temperature_raw = 0xFF;

    uint8_t b[7];
    b[0] = (uint8_t)((generic->id >> 16) & 0xFF); // ID is 24-bit
    b[1] = (uint8_t)((generic->id >> 8) & 0xFF);
    b[2] = (uint8_t)(generic->id & 0xFF);
    b[3] = (uint8_t)(pressure_raw & 0xFF); // pressure low byte
    b[4] = (uint8_t)temperature_raw; // °C + 50
    b[5] = generic->battery_low ? 0x80 : 0x00; // flags1: bit7 low battery
    b[6] = (uint8_t)((pressure_raw >> 8) & 0x01); // flags2: bit0 = pressure bit8

    uint8_t cc = tpms_eezrv_checksum(b);

    uint64_t data = (uint64_t)cc << 56;
    for(int i = 0; i < 7; i++) {
        data |= (uint64_t)b[i] << (8 * (6 - i));
    }
    generic->data = data;
    generic->data_count_bit = EEZRV_DATA_BITS;
}

SubGhzProtocolStatus
    tpms_protocol_encoder_tpms_eezrv_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderEezRV* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_eezrv_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }

        // Optional repeat override.
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_EEZRV_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_tpms_eezrv_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_tpms_eezrv_yield(void* context) {
    TPMSProtocolEncoderEezRV* instance = context;

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
