#include "tpms_jansite.h"
#include "../blocks/generic.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSJansite"

// Port of rtl_433/src/devices/tpms_jansite_solar.c
//   (Andreas Spiess and Christian W. Zuckschwerdt 2019, GPL-2+)
//
// NOTE ON SOURCE SELECTION:
//   The task named tpms_jansite.c, but that rtl_433 device is shipped
//   .disabled = 1 with the comment "Unknown checksum" (its checksum byte is
//   never validated). A decode-only Manchester receiver with no verified MIC
//   false-positives constantly on noise, which violates the CRC-enforcement
//   requirement. rtl_433 later added tpms_jansite_solar.c for the same Jansite
//   Solar TY02S family, and THAT variant carries a validated CRC-16/BUYPASS.
//   This port implements the CRC-validated Jansite (solar) protocol so the
//   callback only fires on a checksum-valid frame.
//
// FSK 433.92 MHz, Manchester-coded, ~51 us half-bit.
//
// Wire format after Manchester decode (88 bits total = 16-bit sync + 72 data):
//   SS SS II II II 00 TT PP 00 CC CC
//   b[0..1] = 0xDD33 sync word
//   b[2..4] = 24-bit ID (big-endian)
//   b[5]    = status/unknown-1
//   b[6]    = temperature, deg C offset by 55  (C = b[6] - 55)
//   b[7]    = pressure, kPa = b[7] * 1.6
//   b[8]    = unknown-2
//   b[9..10]= CRC-16/BUYPASS (poly 0x8005, init 0x0000) over b[2..8]
//
// The frame is FSK PCM: a fixed sync/preamble lives in the RAW half-symbol bits
// and ONLY the payload is Manchester-encoded (rtl_433 searches the raw preamble
// 0xa6 0xa6 0x5a, then manchester-decodes 88 bits whose leading 16 form the
// 0xdd33 sync). The old port ran manchester over EVERYTHING and hunted the sync
// in the manchester-decoded stream, which never phase-locks.
//
// We hunt the full 16-bit 0xdd33 sync directly in the RAW bits: with the
// Manchester convention "data bit d -> raw halves (d, !d)", 0xdd33 encodes to
// the 32 raw bits 0xA6A65A5A (its leading 24 bits are exactly rtl_433's
// 0xa6a65a preamble). The opposite FSK polarity is the bit-complement
// 0x5959A5A5, where the data bit is the 2nd half of each pair instead of the
// 1st. After the sync locks we Manchester-decode the remaining 72 data bits
// (b[2..10]) and CRC-check them.

#define JANSITE_SYNC_RAW     0xA6A65A5AUL // raw manchester encoding of 0xdd33 sync
#define JANSITE_SYNC_RAW_INV 0x5959A5A5UL // opposite-polarity (bit-complement)
#define JANSITE_DATA_BITS    72 // b[2..10], collected after the 16-bit sync lock

static const SubGhzBlockConst tpms_protocol_jansite_const = {
    .te_short = 51,
    .te_long = 102,
    .te_delta = 20,
    .min_count_bit_for_found = JANSITE_DATA_BITS,
};

typedef enum {
    JansiteDecoderStepReset = 0,
    JansiteDecoderStepFindSync,
    JansiteDecoderStepData,
} JansiteDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    // Jansite is FSK PCM: a fixed sync lives in the RAW half-symbols, then the
    // payload is Manchester-encoded. We hunt the raw sync (0xA6A65A5A, or the
    // opposite-polarity complement 0x5959A5A5) then manchester-decode the data.
    // The old port ran manchester over EVERYTHING and hunted the sync in the
    // manchester-decoded stream, which never syncs.
    uint32_t raw_shift; // rolling window of raw half-symbol bits for the sync hunt
    bool in_data; // true once the sync matched, collecting manchester data
    bool data_second; // pair convention: false=data is 1st half, true=2nd (inverted)
    bool have_half; // holding the first half of a manchester pair
    uint8_t first_half; // the first half-symbol of the current pair
    uint64_t head_64_bit; // upper bits when frame > 64 bit (add_to_128_bit)
} TPMSProtocolDecoderJansite;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderJansite;

static void* tpms_protocol_decoder_jansite_alloc(SubGhzEnvironment* environment);
static void tpms_protocol_decoder_jansite_free(void* context);
static void tpms_protocol_decoder_jansite_reset(void* context);
static void tpms_protocol_decoder_jansite_feed(void* context, bool level, uint32_t duration);
static uint8_t tpms_protocol_decoder_jansite_get_hash_data(void* context);
static SubGhzProtocolStatus tpms_protocol_decoder_jansite_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
static SubGhzProtocolStatus
    tpms_protocol_decoder_jansite_deserialize(void* context, FlipperFormat* flipper_format);
static void tpms_protocol_decoder_jansite_get_string(void* context, FuriString* output);

static void* tpms_protocol_encoder_jansite_alloc(SubGhzEnvironment* environment);
static void tpms_protocol_encoder_jansite_free(void* context);
static void tpms_protocol_encoder_jansite_stop(void* context);
static SubGhzProtocolStatus
    tpms_protocol_encoder_jansite_deserialize(void* context, FlipperFormat* flipper_format);
static LevelDuration tpms_protocol_encoder_jansite_yield(void* context);

const SubGhzProtocolDecoder tpms_protocol_jansite_decoder = {
    .alloc = tpms_protocol_decoder_jansite_alloc,
    .free = tpms_protocol_decoder_jansite_free,
    .feed = tpms_protocol_decoder_jansite_feed,
    .reset = tpms_protocol_decoder_jansite_reset,
    .get_hash_data = tpms_protocol_decoder_jansite_get_hash_data,
    .serialize = tpms_protocol_decoder_jansite_serialize,
    .deserialize = tpms_protocol_decoder_jansite_deserialize,
    .get_string = tpms_protocol_decoder_jansite_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_jansite_encoder = {
    .alloc = tpms_protocol_encoder_jansite_alloc,
    .free = tpms_protocol_encoder_jansite_free,
    .deserialize = tpms_protocol_encoder_jansite_deserialize,
    .stop = tpms_protocol_encoder_jansite_stop,
    .yield = tpms_protocol_encoder_jansite_yield,
};

const SubGhzProtocol tpms_protocol_jansite = {
    .name = TPMS_PROTOCOL_JANSITE_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_jansite_decoder,
    .encoder = &tpms_protocol_jansite_encoder,
};

static void* tpms_protocol_decoder_jansite_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderJansite* instance = calloc(1, sizeof(TPMSProtocolDecoderJansite));
    instance->base.protocol = &tpms_protocol_jansite;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

static void tpms_protocol_decoder_jansite_free(void* context) {
    furi_assert(context);
    free(context);
}

static void tpms_protocol_decoder_jansite_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderJansite* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->raw_shift = 0;
    instance->in_data = false;
    instance->data_second = false;
    instance->have_half = false;
    instance->first_half = 0;
    instance->head_64_bit = 0;
}

// Assemble the 9 payload bytes b[2..10] from the 72-bit accumulator. Polarity is
// resolved at the half-symbol level (see jansite_emit_raw_bit), so the collected
// bits are already canonical and need no post-complement here.
static void jansite_get_bytes(TPMSProtocolDecoderJansite* instance, uint8_t raw[9]) {
    raw[0] = (uint8_t)(instance->head_64_bit & 0xFF); // b[2]
    for(int i = 0; i < 8; i++) {
        raw[i + 1] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF; // b[3..10]
    }
}

static bool jansite_check_crc(TPMSProtocolDecoderJansite* instance, uint8_t raw[9]) {
    jansite_get_bytes(instance, raw);
    // CRC-16/BUYPASS over b[2..8] (raw[0..6]) == b[9..10] (raw[7..8]).
    uint16_t crc_calc = subghz_protocol_blocks_crc16(raw, 7, 0x8005, 0x0000);
    uint16_t crc_recv = ((uint16_t)raw[7] << 8) | raw[8];
    return crc_calc == crc_recv;
}

static void jansite_analyze(TPMSProtocolDecoderJansite* instance, const uint8_t raw[9]) {
    instance->generic.id = ((uint32_t)raw[0] << 16) | ((uint32_t)raw[1] << 8) | raw[2]; // b[2..4]
    instance->generic.temperature = (float)((int)raw[4] - 55); // b[6] offset 55
    instance->generic.pressure = (float)raw[5] * 1.6f * 0.01f; // b[7] kPa -> bar
    instance->generic.battery_low = TPMS_NO_BATT;
    // Store canonical low-64 payload (b[3..10]) for save/load.
    uint64_t data = 0;
    for(int i = 1; i <= 8; i++) data = (data << 8) | raw[i];
    instance->generic.data = data;
    instance->generic.data_count_bit = JANSITE_DATA_BITS;
}

// Consume one raw half-symbol bit: hunt the raw sync (0xA6A65A5A, or the
// opposite-polarity complement 0x5959A5A5), then Manchester-decode the 72-bit
// payload. Only the data is Manchester; the sync is matched in the raw bits. The
// frame is >64 bits, so the top byte spills into head_64_bit via
// subghz_protocol_blocks_add_to_128_bit (low byte lands in head_64_bit = b[2]).
static void jansite_emit_raw_bit(TPMSProtocolDecoderJansite* instance, uint8_t bit) {
    bit &= 1;
    if(!instance->in_data) {
        instance->raw_shift = (instance->raw_shift << 1) | bit;
        if(instance->raw_shift == JANSITE_SYNC_RAW ||
           instance->raw_shift == JANSITE_SYNC_RAW_INV) {
            instance->in_data = true;
            instance->data_second = (instance->raw_shift == JANSITE_SYNC_RAW_INV);
            instance->have_half = false;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->head_64_bit = 0;
        }
        return;
    }
    // Inside the Manchester payload: pair up half-symbols. A valid pair is a
    // transition (halves differ); the data bit is the 1st half (normal) or 2nd
    // half (inverted). No transition = end/garbage -> resync on the sync word.
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
    if(instance->decoder.decode_count_bit == JANSITE_DATA_BITS) {
        uint8_t raw[9];
        if(jansite_check_crc(instance, raw)) {
            jansite_analyze(instance, raw);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        instance->in_data = false;
        instance->raw_shift = 0;
    }
}

static void tpms_protocol_decoder_jansite_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderJansite* instance = context;

    const uint32_t te = tpms_protocol_jansite_const.te_short; // 51us half-symbol
    // A gap (or any pulse longer than two half-symbols) ends the burst.
    if(duration >= te * 3) {
        tpms_protocol_decoder_jansite_reset(instance);
        return;
    }
    // Expand this pulse into 1 or 2 raw half-symbol bits of the current level.
    uint32_t n = (duration + te / 2) / te;
    if(n < 1) n = 1;
    if(n > 2) n = 2;
    for(uint32_t k = 0; k < n; k++) {
        jansite_emit_raw_bit(instance, level ? 1 : 0);
    }
}

static uint8_t tpms_protocol_decoder_jansite_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderJansite* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

static SubGhzProtocolStatus tpms_protocol_decoder_jansite_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderJansite* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

static SubGhzProtocolStatus
    tpms_protocol_decoder_jansite_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderJansite* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, tpms_protocol_jansite_const.min_count_bit_for_found);
}

static void tpms_protocol_decoder_jansite_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderJansite* instance = context;
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

// Invert jansite_analyze(): write pressure/temperature/id into the wire bytes,
// recompute the CRC, and store the canonical payload (b[3..10]) back into
// g->data. The frame is b[2..10] (9 bytes); b[2] is the ID top byte (kept in
// g->id), b[3..10] land in g->data exactly as the decoder's analyze stores them.
void tpms_protocol_jansite_pack(TPMSBlockGeneric* g) {
    furi_assert(g);
    // pressure: bar = b[7] * 1.6 * 0.01 = b[7] * 0.016  ->  b[7] = bar / 0.016
    int p_raw = (int)(g->pressure / 0.016f + 0.5f);
    if(p_raw < 0) p_raw = 0;
    if(p_raw > 0xFF) p_raw = 0xFF;
    // temperature: C = b[6] - 55  ->  b[6] = C + 55
    int t = (int)(g->temperature + (g->temperature >= 0 ? 0.5f : -0.5f)) + 55;
    if(t < 0) t = 0;
    if(t > 0xFF) t = 0xFF;

    uint8_t f[9]; // f[0]=b[2] .. f[8]=b[10]
    f[0] = (uint8_t)((g->id >> 16) & 0xFF); // b[2] ID hi
    f[1] = (uint8_t)((g->id >> 8) & 0xFF); // b[3] ID mid
    f[2] = (uint8_t)(g->id & 0xFF); // b[4] ID lo
    f[3] = 0; // b[5] status/unknown-1
    f[4] = (uint8_t)t; // b[6] temperature
    f[5] = (uint8_t)p_raw; // b[7] pressure
    f[6] = 0; // b[8] unknown-2
    // CRC-16/BUYPASS (poly 0x8005, init 0x0000) over b[2..8] -> b[9..10]
    uint16_t crc = subghz_protocol_blocks_crc16(f, 7, 0x8005, 0x0000);
    f[7] = (uint8_t)(crc >> 8); // b[9]
    f[8] = (uint8_t)(crc & 0xFF); // b[10]

    // g->data holds the canonical low-64 payload b[3..10] (matches jansite_analyze).
    uint64_t data = 0;
    for(int i = 1; i <= 8; i++) data = (data << 8) | f[i];
    g->data = data;
    g->data_count_bit = JANSITE_DATA_BITS;
}

// -----------------------------------------------------------------------------
// Encoder
// -----------------------------------------------------------------------------
//
// FSK: a fixed sync lives in the RAW half-symbols and only the payload is
// Manchester-encoded. The decoder hunts the raw 32-bit sync 0xA6A65A5A (the
// Manchester encoding of the 0xdd33 sync word), so each of those 32 bits is ONE
// raw half-symbol (NOT Manchester). Then the 72 data bits (b[2..10]) are
// Manchester with the "data = 1st half of each transitioning pair" convention:
//   bit 1 -> first half HIGH, second half LOW
//   bit 0 -> first half LOW,  second half HIGH
//
// The 9 wire bytes b[2..10] are rebuilt from g->id (b[2]) + g->data (b[3..10]);
// the CRC (b[9..10]) is always recomputed so a modified/0-PSI frame stays valid.
//
// Worst-case entries: 1 gap + 32 raw sync + 2*72 data + 1 = 178.
#define TPMS_JANSITE_ENCODER_UPLOAD_SIZE 192

static void* tpms_protocol_encoder_jansite_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderJansite* instance = malloc(sizeof(TPMSProtocolEncoderJansite));
    instance->base.protocol = &tpms_protocol_jansite;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_JANSITE_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

static void tpms_protocol_encoder_jansite_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderJansite* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

static void tpms_protocol_encoder_jansite_stop(void* context) {
    TPMSProtocolEncoderJansite* instance = context;
    instance->encoder.is_running = false;
}

// Rebuild the 9 wire bytes b[2..10]. b[2] = ID top byte; b[3..10] from
// generic.data; b[9..10] = recomputed CRC-16/BUYPASS over b[2..8].
static void tpms_protocol_encoder_jansite_build_frame(TPMSBlockGeneric* generic, uint8_t f[9]) {
    f[0] = (uint8_t)((generic->id >> 16) & 0xFF); // b[2]
    for(int i = 0; i < 8; i++) {
        f[i + 1] = (generic->data >> (8 * (7 - i))) & 0xFF; // b[3..10]
    }
    uint16_t crc = subghz_protocol_blocks_crc16(f, 7, 0x8005, 0x0000); // over b[2..8]
    f[7] = (uint8_t)(crc >> 8); // b[9]
    f[8] = (uint8_t)(crc & 0xFF); // b[10]
}

static bool tpms_protocol_encoder_jansite_get_upload(TPMSProtocolEncoderJansite* instance) {
    furi_assert(instance);
    const uint32_t te = tpms_protocol_jansite_const.te_short;

    uint8_t frame[9];
    tpms_protocol_encoder_jansite_build_frame(&instance->generic, frame);

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

// Manchester data bit (decoder convention: data = 1st half of each transitioning
// pair, i.e. JANSITE_SYNC_RAW / data_second == false).
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

    EMIT(false, te * 50); // inter-frame gap (resets decoder: >= 3*te)

    // Raw sync 0xA6A65A5A — NOT Manchester. The decoder hunts this exact 32-bit
    // pattern in the raw half-symbol stream, so emit one raw half-symbol per bit.
    static const uint8_t sync[4] = {0xA6, 0xA6, 0x5A, 0x5A};
    for(int i = 0; i < 4; i++) {
        for(int b = 7; b >= 0; b--) {
            EMIT(((sync[i] >> b) & 0x1) ? true : false, te);
        }
    }

    // 72 data bits (b[2..10]), MSB first, Manchester-encoded.
    for(int i = 0; i < JANSITE_DATA_BITS; i++) {
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

static SubGhzProtocolStatus
    tpms_protocol_encoder_jansite_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderJansite* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_jansite_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_JANSITE_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_jansite_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

static LevelDuration tpms_protocol_encoder_jansite_yield(void* context) {
    TPMSProtocolEncoderJansite* instance = context;

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
