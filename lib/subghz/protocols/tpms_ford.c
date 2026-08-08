#include "tpms_ford.h"
#include "../blocks/generic.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSFord"

// Port of rtl_433/src/devices/tpms_ford.c (Christian W. Zuckschwerdt 2017, GPL-2+)
//
// FSK, Manchester encoded, 8 byte payload.
// Seen on Ford Fiesta/Focus/Kuga/Transit (Continental S180084730Z).
// 315 MHz (US) / 433.92 MHz (EU).
//
// Wire format (after Manchester-decoding, before checksum):
//   II II II II PP TT FF CC
//   I = ID (32 bit)
//   P = Pressure-LSB (combined with bit5 of F → 9-bit raw, * 0.25 PSI)
//   T = Temperature: when high bit clear → (byte & 0x7F) - 56 °C; else invalid
//   F = Flags (bit5 = 9th pressure bit, bit3 = learn, bit2 = moving)
//   C = Checksum = SUM(byte[0..6]) & 0xFF
//
// Preamble (raw bits): 0x55 0x55 0x55 0x56  (we hunt for the trailing 16 bits 0x5556)

#define FORD_PREAMBLE 0x5556U
#define FORD_PREAMBLE_BITS 16
#define FORD_DATA_BITS 64

static const SubGhzBlockConst tpms_protocol_ford_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = FORD_DATA_BITS,
};

typedef enum {
    FordDecoderStepReset = 0,
    FordDecoderStepFindPreamble,
    FordDecoderStepData,
} FordDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    ManchesterState manchester_saved_state;
    uint16_t header_shift;
} TPMSProtocolDecoderFord;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderFord;

const SubGhzProtocolDecoder tpms_protocol_ford_decoder = {
    .alloc = tpms_protocol_decoder_ford_alloc,
    .free = tpms_protocol_decoder_ford_free,
    .feed = tpms_protocol_decoder_ford_feed,
    .reset = tpms_protocol_decoder_ford_reset,
    .get_hash_data = tpms_protocol_decoder_ford_get_hash_data,
    .serialize = tpms_protocol_decoder_ford_serialize,
    .deserialize = tpms_protocol_decoder_ford_deserialize,
    .get_string = tpms_protocol_decoder_ford_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_ford_encoder = {
    .alloc = tpms_protocol_encoder_ford_alloc,
    .free = tpms_protocol_encoder_ford_free,
    .deserialize = tpms_protocol_encoder_ford_deserialize,
    .stop = tpms_protocol_encoder_ford_stop,
    .yield = tpms_protocol_encoder_ford_yield,
};

const SubGhzProtocol tpms_protocol_ford = {
    .name = TPMS_PROTOCOL_FORD_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Send,

    .decoder = &tpms_protocol_ford_decoder,
    .encoder = &tpms_protocol_ford_encoder,
};

void* tpms_protocol_decoder_ford_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderFord* instance = calloc(1, sizeof(TPMSProtocolDecoderFord));
    instance->base.protocol = &tpms_protocol_ford;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->manchester_saved_state = ManchesterStateStart1;
    return instance;
}

void tpms_protocol_decoder_ford_free(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderFord* instance = context;
    free(instance);
}

void tpms_protocol_decoder_ford_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderFord* instance = context;
    instance->decoder.parser_step = FordDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->header_shift = 0;
    instance->manchester_saved_state = ManchesterStateStart1;
}

static ManchesterEvent ford_level_and_duration_to_event(bool level, uint32_t duration) {
    bool is_long = false;
    if(DURATION_DIFF(duration, tpms_protocol_ford_const.te_long) <
       tpms_protocol_ford_const.te_delta * 2) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_ford_const.te_short) <
        tpms_protocol_ford_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }
    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

static bool tpms_protocol_ford_check_checksum(TPMSProtocolDecoderFord* instance) {
    uint8_t b[8];
    for(int i = 0; i < 8; i++) {
        b[i] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
    uint16_t sum = 0;
    for(int i = 0; i < 7; i++) sum += b[i];
    return ((uint8_t)(sum & 0xFF)) == b[7];
}

static void tpms_protocol_ford_analyze(TPMSBlockGeneric* g) {
    uint8_t b[8];
    for(int i = 0; i < 8; i++) {
        b[i] = (g->data >> (8 * (7 - i))) & 0xFF;
    }
    g->id = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];

    // 9-bit pressure: bit5 of flags (b[6]) is the MSB of pressure.
    uint16_t psi_bits = ((uint16_t)(b[6] & 0x20) << 3) | b[4];
    float psi = (float)psi_bits * 0.25f;
    g->pressure = psi * 0.06895f; // PSI → bar

    if((b[5] & 0x80) == 0) {
        g->temperature = (float)((int)(b[5] & 0x7F) - 56);
    } else {
        // Sensor signals invalid temperature; rtl_433 simply omits it.
        g->temperature = -100.0f;
    }
    g->battery_low = TPMS_NO_BATT;
}

void tpms_protocol_ford_pack(TPMSBlockGeneric* g) {
    furi_assert(g);
    // pressure: bar -> psi -> 9-bit quarter-psi. b[4] = low 8 bits, b[6] bit5 = MSB.
    int psi_q = (int)(g->pressure / 0.06895f / 0.25f + 0.5f);
    if(psi_q < 0) psi_q = 0;
    if(psi_q > 0x1FF) psi_q = 0x1FF;
    // temperature: stored as (C + 56) in 7 bits with bit7 (invalid flag) clear.
    int t = (int)(g->temperature + (g->temperature >= 0 ? 0.5f : -0.5f)) + 56;
    if(t < 0) t = 0;
    if(t > 0x7F) t = 0x7F;

    uint8_t b[8];
    b[0] = (g->id >> 24) & 0xFF;
    b[1] = (g->id >> 16) & 0xFF;
    b[2] = (g->id >> 8) & 0xFF;
    b[3] = (g->id >> 0) & 0xFF;
    b[4] = (uint8_t)(psi_q & 0xFF);
    b[5] = (uint8_t)(t & 0x7F); // bit7 clear = valid temperature
    b[6] = (psi_q & 0x100) ? 0x20 : 0x00; // bit5 = pressure MSB
    uint16_t sum = 0;
    for(int i = 0; i < 7; i++) sum += b[i];
    b[7] = (uint8_t)(sum & 0xFF); // checksum = sum(b[0..6]) & 0xFF

    uint64_t data = 0;
    for(int i = 0; i < 8; i++) data = (data << 8) | b[i];
    g->data = data;
    g->data_count_bit = 64;
}

void tpms_protocol_decoder_ford_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderFord* instance = context;

    ManchesterEvent event = ford_level_and_duration_to_event(level, duration);
    if(event == ManchesterEventReset) {
        instance->decoder.parser_step = FordDecoderStepReset;
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
    case FordDecoderStepReset:
        instance->header_shift = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        instance->decoder.parser_step = FordDecoderStepFindPreamble;
        // fall through to consume this bit as preamble candidate
        /* fallthrough */
    case FordDecoderStepFindPreamble:
        instance->header_shift = (uint16_t)((instance->header_shift << 1) | (bit ? 1 : 0));
        if(instance->header_shift == FORD_PREAMBLE) {
            instance->decoder.parser_step = FordDecoderStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
        }
        break;

    case FordDecoderStepData:
        subghz_protocol_blocks_add_bit(&instance->decoder, bit ? 1 : 0);
        if(instance->decoder.decode_count_bit ==
           tpms_protocol_ford_const.min_count_bit_for_found) {
            if(tpms_protocol_ford_check_checksum(instance)) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                tpms_protocol_ford_analyze(&instance->generic);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            instance->decoder.parser_step = FordDecoderStepReset;
        }
        break;
    }
}

uint8_t tpms_protocol_decoder_ford_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderFord* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_ford_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderFord* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_ford_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderFord* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_ford_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_ford_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderFord* instance = context;
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
// FSK Manchester, reproduces exactly the waveform accepted by the decoder:
//   inter-frame gap (reset), 32-bit preamble 0x55 0x55 0x55 0x56, then 64 data
//   bits Manchester-encoded. The decoder is Manchester-II non-inverting, so:
//     bit 1 -> first half LOW,  second half HIGH
//     bit 0 -> first half HIGH, second half LOW
//   Adjacent equal half-symbols are run-length merged (te_short -> te_long).
//
// All 64 payload bits (including the trailing checksum) live in generic.data, so
// this is a bit-exact reproduction of the captured frame.
//
// Worst-case entries: 1 gap + 2*(32 preamble + 64 data) + 1 flush = 194.
#define TPMS_FORD_ENCODER_UPLOAD_SIZE 208

void* tpms_protocol_encoder_ford_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderFord* instance = malloc(sizeof(TPMSProtocolEncoderFord));
    instance->base.protocol = &tpms_protocol_ford;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_FORD_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_ford_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderFord* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_ford_stop(void* context) {
    TPMSProtocolEncoderFord* instance = context;
    instance->encoder.is_running = false;
}

static bool tpms_protocol_encoder_ford_get_upload(TPMSProtocolEncoderFord* instance) {
    furi_assert(instance);
    const uint32_t te = tpms_protocol_ford_const.te_short;

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

    // Inter-frame gap -> decoder ManchesterEventReset before the preamble
    EMIT(false, te * 50);

    // 32-bit preamble 0x55 0x55 0x55 0x56 (decoder hunts trailing 16 bits 0x5556)
    static const uint8_t preamble[4] = {0x55, 0x55, 0x55, 0x56};
    for(int i = 0; i < 4; i++) {
        for(int b = 7; b >= 0; b--) {
            MAN_BIT((preamble[i] >> b) & 0x1);
        }
    }

    // 64 data bits, MSB first
    for(int bit = instance->generic.data_count_bit - 1; bit >= 0; bit--) {
        MAN_BIT((instance->generic.data >> bit) & 0x1);
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
    tpms_protocol_encoder_ford_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderFord* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic, flipper_format, tpms_protocol_ford_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_FORD_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_ford_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_ford_yield(void* context) {
    TPMSProtocolEncoderFord* instance = context;

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
