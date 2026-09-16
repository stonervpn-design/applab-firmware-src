#include "tpms_hyundai_vdo.h"
#include "../blocks/generic.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSHyundaiVdo"

// Hyundai / Kia (Continental/VDO-family) TPMS sensor.
// Ported from at0m-b0mb/Odograph-FlipperZero parse_hyundai() (traces to rtl_433).
//
// FSK 433 MHz, Manchester-encoded, 10-byte payload — the SAME VDO wire format as
// Citroen/Peugeot/Fiat (preamble 0x55 0x55 0x55 0x56, 52us half-bit). It differs
// from Citroen only in the integrity check and the pressure scale, so it reuses the
// Citroen front-end and is a distinct decode-only protocol.
//
// Wire format (after Manchester-decoding):
//   b[0]     = state (covered by CRC)
//   b[1..4]  = 32-bit sensor ID
//   b[5]     = flags
//   b[6]     = pressure raw, 1.375 kPa per step
//   b[7]     = temperature in C + 50
//   b[8]     = battery / status
//   b[9]     = CRC-8 (poly 0x07, init 0xAA) over b[0..8]
//
// Transmittable: the encoder (bottom of file) reuses the raw-preamble + Manchester
// machinery with the Hyundai/VDO byte layout, 1.375 kPa pressure scaling and CRC-8
// (poly 0x07, init 0xAA over b[0..8], with b[0]=0 covered), so the TPMS 0-PSI
// flat-tire spoof can rebuild and re-send a picked-up Hyundai/Kia sensor. b[1] (the
// ID top byte) is preserved from generic.id, not from generic.data.

#define HYUNDAI_VDO_PREAMBLE 0x5556U
#define HYUNDAI_VDO_DATA_BITS 80

static const SubGhzBlockConst tpms_protocol_hyundai_vdo_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = HYUNDAI_VDO_DATA_BITS,
};

typedef enum {
    HyundaiVdoDecoderStepReset = 0,
    HyundaiVdoDecoderStepFindPreamble,
    HyundaiVdoDecoderStepData,
} HyundaiVdoDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    // Same VDO wire format as Ford: a fixed sync preamble lives in the RAW
    // half-symbol bits, then the 80-bit payload is Manchester-encoded (matches
    // rtl_433 tpms_hyundai_vdo: search 0xaaaaaaa9/0x55555556 in raw bits, then
    // manchester-decode only the data). The old port ran manchester over
    // EVERYTHING and hunted the preamble in the manchester-decoded stream, which
    // never syncs. Payload is 80 bits, so it accumulates into head_64_bit too.
    uint16_t raw_shift; // rolling window of raw half-symbol bits for preamble hunt
    bool in_data; // true once the preamble matched, collecting manchester data
    bool data_second; // pair convention: false=data is 1st half (0x5556), true=2nd (0xAAA9)
    bool have_half; // holding the first half of a manchester pair
    uint8_t first_half; // the first half-symbol of the current pair
    uint64_t head_64_bit; // upper payload bits (>64) collected via add_to_128_bit
} TPMSProtocolDecoderHyundaiVdo;

const SubGhzProtocolDecoder tpms_protocol_hyundai_vdo_decoder = {
    .alloc = tpms_protocol_decoder_hyundai_vdo_alloc,
    .free = tpms_protocol_decoder_hyundai_vdo_free,
    .feed = tpms_protocol_decoder_hyundai_vdo_feed,
    .reset = tpms_protocol_decoder_hyundai_vdo_reset,
    .get_hash_data = tpms_protocol_decoder_hyundai_vdo_get_hash_data,
    .serialize = tpms_protocol_decoder_hyundai_vdo_serialize,
    .deserialize = tpms_protocol_decoder_hyundai_vdo_deserialize,
    .get_string = tpms_protocol_decoder_hyundai_vdo_get_string,
};

// -- encoder forward declarations (Hyundai/VDO is transmittable) --------------
void* tpms_protocol_encoder_hyundai_vdo_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_hyundai_vdo_free(void* context);
void tpms_protocol_encoder_hyundai_vdo_stop(void* context);
SubGhzProtocolStatus
    tpms_protocol_encoder_hyundai_vdo_deserialize(void* context, FlipperFormat* flipper_format);
LevelDuration tpms_protocol_encoder_hyundai_vdo_yield(void* context);

const SubGhzProtocolEncoder tpms_protocol_hyundai_vdo_encoder = {
    .alloc = tpms_protocol_encoder_hyundai_vdo_alloc,
    .free = tpms_protocol_encoder_hyundai_vdo_free,
    .deserialize = tpms_protocol_encoder_hyundai_vdo_deserialize,
    .stop = tpms_protocol_encoder_hyundai_vdo_stop,
    .yield = tpms_protocol_encoder_hyundai_vdo_yield,
};

const SubGhzProtocol tpms_protocol_hyundai_vdo = {
    .name = TPMS_PROTOCOL_HYUNDAI_VDO_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_hyundai_vdo_decoder,
    .encoder = &tpms_protocol_hyundai_vdo_encoder,
};

void* tpms_protocol_decoder_hyundai_vdo_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderHyundaiVdo* instance = calloc(1, sizeof(TPMSProtocolDecoderHyundaiVdo));
    instance->base.protocol = &tpms_protocol_hyundai_vdo;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_hyundai_vdo_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_hyundai_vdo_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderHyundaiVdo* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->head_64_bit = 0;
    instance->raw_shift = 0;
    instance->in_data = false;
    instance->data_second = false;
    instance->have_half = false;
    instance->first_half = 0;
}

static void hyundai_vdo_get_bytes(TPMSProtocolDecoderHyundaiVdo* instance, uint8_t b[10]) {
    // 80 bits: head_64_bit lower 16 bits = b[0..1] (MSB-first), decode_data 64 = b[2..9].
    b[0] = (uint8_t)((instance->head_64_bit >> 8) & 0xFF);
    b[1] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 2] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

// CRC-8, polynomial 0x07, init 0xAA (rtl_433 tpms_hyundai_vdo).
static uint8_t hyundai_vdo_crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0xAA;
    for(size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for(int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static bool hyundai_vdo_check_crc(TPMSProtocolDecoderHyundaiVdo* instance) {
    uint8_t b[10];
    hyundai_vdo_get_bytes(instance, b);
    return hyundai_vdo_crc8(b, 9) == b[9];
}

static void hyundai_vdo_analyze(TPMSProtocolDecoderHyundaiVdo* instance) {
    uint8_t b[10];
    hyundai_vdo_get_bytes(instance, b);
    instance->generic.id =
        ((uint32_t)b[1] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 8) | b[4];
    // Pressure raw -> kPa (1.375 per step) -> bar for display.
    instance->generic.pressure = ((float)b[6] * 1.375f) * 0.01f;
    instance->generic.temperature = (float)((int)b[7] - 50);
    instance->generic.battery_low = b[8];
}

// Consume one raw half-symbol bit: hunt the raw preamble (0x5556, or the
// inverted-polarity 0xAAA9), then Manchester-decode the 80-bit payload. Matches
// rtl_433 tpms_hyundai_vdo (preamble searched in raw bits; only the data is
// Manchester). Payload is >64 bits, so it accumulates through head_64_bit.
static void hyundai_vdo_emit_raw_bit(TPMSProtocolDecoderHyundaiVdo* instance, uint8_t bit) {
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
    if(instance->decoder.decode_count_bit == HYUNDAI_VDO_DATA_BITS) {
        if(hyundai_vdo_check_crc(instance)) {
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            hyundai_vdo_analyze(instance);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        instance->in_data = false;
        instance->raw_shift = 0;
    }
}

void tpms_protocol_decoder_hyundai_vdo_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderHyundaiVdo* instance = context;

    const uint32_t te = tpms_protocol_hyundai_vdo_const.te_short; // 52us half-symbol
    // A gap (or any pulse longer than two half-symbols) ends the burst.
    if(duration >= te * 3) {
        tpms_protocol_decoder_hyundai_vdo_reset(instance);
        return;
    }
    // Expand this pulse into 1 or 2 raw half-symbol bits of the current level.
    uint32_t n = (duration + te / 2) / te;
    if(n < 1) n = 1;
    if(n > 2) n = 2;
    for(uint32_t k = 0; k < n; k++) {
        hyundai_vdo_emit_raw_bit(instance, level ? 1 : 0);
    }
}

uint8_t tpms_protocol_decoder_hyundai_vdo_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderHyundaiVdo* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_hyundai_vdo_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderHyundaiVdo* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_hyundai_vdo_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderHyundaiVdo* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_hyundai_vdo_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_hyundai_vdo_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderHyundaiVdo* instance = context;
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
// Encoder — same VDO wire format as Citroen/Jeep (FSK Manchester-II, raw
// 0x55 0x55 0x55 0x56 preamble, 80 data bits). Byte layout:
//   b[0]     = state (COVERED by CRC, TX as 0x00)   b[1..4] = 32-bit ID
//   b[5]     = flags                                b[6]    = pressure (1.375 kPa/step)
//   b[7]     = temperature (degC + 50)              b[8]    = battery
//   b[9]     = CRC-8 (poly 0x07, init 0xAA) over b[0..8]
// generic.data holds b[2..9]; b[1] is the ID top byte. build_frame rebuilds all
// 10 bytes with b[0]=0 and recomputes b[9] so every frame carries a valid CRC
// (essential for the 0-PSI spoof, which zeroes b[6] then must re-checksum).
// -----------------------------------------------------------------------------
#define TPMS_HYUNDAI_VDO_ENCODER_UPLOAD_SIZE 240

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderHyundaiVdo;

// Rebuild the 10-byte frame: b[0] = 0 (covered, TX zero), b[1] = ID top byte,
// b[2..9] from generic.data, then b[9] = CRC-8 over b[0..8].
static void
    tpms_protocol_encoder_hyundai_vdo_build_frame(TPMSBlockGeneric* generic, uint8_t b[10]) {
    b[1] = (uint8_t)((generic->id >> 24) & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 2] = (generic->data >> (8 * (7 - i))) & 0xFF;
    }
    b[0] = 0x00;
    b[9] = hyundai_vdo_crc8(b, 9); // CRC over b[0..8]
}

// Re-pack pressure/temperature/battery into generic.data (b[2..9]) and recompute
// the CRC. b[2..5] (low 3 ID bytes + flags) are preserved from generic.data.
void tpms_protocol_hyundai_vdo_pack(TPMSBlockGeneric* g) {
    furi_assert(g);
    // Pressure: bar = b[6] * 1.375 * 0.01 -> raw = bar / 0.01375.
    int p_raw = (int)(g->pressure / 0.01375f + 0.5f);
    if(p_raw < 0) p_raw = 0;
    if(p_raw > 0xFF) p_raw = 0xFF;
    int t = (int)(g->temperature + (g->temperature >= 0 ? 0.5f : -0.5f)) + 50; // b[7] = C + 50
    if(t < 0) t = 0;
    if(t > 0xFF) t = 0xFF;

    uint8_t b[10];
    b[0] = 0x00; // state, covered by CRC
    b[1] = (uint8_t)((g->id >> 24) & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 2] = (g->data >> (8 * (7 - i))) & 0xFF; // b[2..9] preserved
    }
    b[6] = (uint8_t)p_raw; // pressure
    b[7] = (uint8_t)t; // temperature
    b[8] = g->battery_low; // battery
    b[9] = hyundai_vdo_crc8(b, 9); // CRC-8 over b[0..8]

    uint64_t data = 0;
    for(int i = 0; i < 8; i++) data = (data << 8) | b[i + 2]; // store b[2..9]
    g->data = data;
    g->data_count_bit = HYUNDAI_VDO_DATA_BITS;
}

void* tpms_protocol_encoder_hyundai_vdo_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderHyundaiVdo* instance = malloc(sizeof(TPMSProtocolEncoderHyundaiVdo));
    instance->base.protocol = &tpms_protocol_hyundai_vdo;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_HYUNDAI_VDO_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_hyundai_vdo_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderHyundaiVdo* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_hyundai_vdo_stop(void* context) {
    TPMSProtocolEncoderHyundaiVdo* instance = context;
    instance->encoder.is_running = false;
}

static bool
    tpms_protocol_encoder_hyundai_vdo_get_upload(TPMSProtocolEncoderHyundaiVdo* instance) {
    furi_assert(instance);
    const uint32_t te = tpms_protocol_hyundai_vdo_const.te_short;

    uint8_t frame[10];
    tpms_protocol_encoder_hyundai_vdo_build_frame(&instance->generic, frame);

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
    for(int i = 0; i < HYUNDAI_VDO_DATA_BITS; i++) {
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
    tpms_protocol_encoder_hyundai_vdo_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderHyundaiVdo* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_hyundai_vdo_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_HYUNDAI_VDO_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_hyundai_vdo_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_hyundai_vdo_yield(void* context) {
    TPMSProtocolEncoderHyundaiVdo* instance = context;

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
