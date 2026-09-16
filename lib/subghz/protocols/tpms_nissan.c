#include "tpms_nissan.h"
#include "../blocks/generic.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSNissan"

// Port of rtl_433/src/devices/tpms_nissan.c (Benjamin Larsson / rtl_433, GPL-2+).
//
// FSK 315 MHz (US) / 433.92 MHz (EU), Manchester-encoded (G.E. Thomas), sensor
// found on modern Nissan / Infiniti vehicles. Decode-only.
//
// Wire format (after Manchester-decoding), 37 useful data bits packed MSB-first
// into 5 bytes b[0..4] (the low 3 bits of b[4] are not transmitted):
//   MODE:3  ID:24  PRESSURE:8  UNKNOWN:2
//   mode         = b[0] >> 5
//   id (24-bit)  = (b[0]&0x1F)<<19 | b[1]<<11 | b[2]<<3 | b[3]>>5
//   pressure_raw = (b[3]&0x1F)<<3  | b[4]>>5        → PSI = raw/4.0 - 3.0
//   unknown      = (b[4]&0x1F) >> 3
// Temperature is NOT transmitted by this sensor.
//
// Integrity: rtl_433's 2-bit "sum2N" style checksum
//   chk = Σ_{i=0..3}( b[i]>>7 + b[i]>>5 + b[i]>>3 + b[i]>>1 + (uint8_t)(b[i]<<1) )
//         + b[4]>>7 + b[4]>>5 + b[4]>>3
//   frame is valid when (~chk & 0x03) == 0   (i.e. chk & 0x03 == 0x03)
//
// Demod / preamble: rtl_433 hunts the raw preamble 0xf5 0x55 0x55 0x55 0xe0 (top
// 36 bits = 0xF5555555E) in the on-air (pre-Manchester) half-symbol stream, then
// Manchester-decodes the payload and inverts it (G.E. Thomas). This port mirrors
// tpms_ford: expand each pulse into raw half-symbol bits, hunt the 36-bit raw
// preamble (or its inverted-polarity twin 0x0AAAAAAA1), then pair half-symbols and
// Manchester-decode the 37 data bits. Because a valid pair is a transition, the
// "take 2nd half, then invert the whole buffer" of rtl_433 reduces to taking the
// 1st half (normal preamble) or the 2nd half (inverted preamble). The old port ran
// Manchester over the WHOLE signal and hunted the preamble in the DECODED stream,
// which never syncs.

#define NISSAN_DATA_BITS 37
#define NISSAN_PREAMBLE 0xF5555555EULL // raw 36-bit sync 0xf5 0x55 0x55 0x55 0xe(0)
#define NISSAN_PREAMBLE_INV 0x0AAAAAAA1ULL // inverted-polarity variant
#define NISSAN_PREAMBLE_MASK 0xFFFFFFFFFULL // low 36 bits

static const SubGhzBlockConst tpms_protocol_nissan_const = {
    .te_short = 120,
    .te_long = 240,
    .te_delta = 40,
    .min_count_bit_for_found = NISSAN_DATA_BITS,
};

typedef enum {
    NissanDecoderStepReset = 0,
    NissanDecoderStepFindPreamble,
    NissanDecoderStepData,
} NissanDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    // rtl_433 tpms_nissan puts a fixed sync preamble in the RAW half-symbols and
    // Manchester-encodes only the payload. Hunt the raw preamble, then pair
    // half-symbols to Manchester-decode the data (see tpms_ford for the shape).
    uint64_t raw_shift; // rolling window of raw half-symbol bits for preamble hunt
    bool in_data; // true once the preamble matched, collecting manchester data
    bool data_second; // pair convention: false=data is 1st half, true=2nd half
    bool have_half; // holding the first half of a manchester pair
    uint8_t first_half; // the first half-symbol of the current pair
} TPMSProtocolDecoderNissan;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderNissan;

const SubGhzProtocolDecoder tpms_protocol_nissan_decoder = {
    .alloc = tpms_protocol_decoder_nissan_alloc,
    .free = tpms_protocol_decoder_nissan_free,
    .feed = tpms_protocol_decoder_nissan_feed,
    .reset = tpms_protocol_decoder_nissan_reset,
    .get_hash_data = tpms_protocol_decoder_nissan_get_hash_data,
    .serialize = tpms_protocol_decoder_nissan_serialize,
    .deserialize = tpms_protocol_decoder_nissan_deserialize,
    .get_string = tpms_protocol_decoder_nissan_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_nissan_encoder = {
    .alloc = tpms_protocol_encoder_nissan_alloc,
    .free = tpms_protocol_encoder_nissan_free,
    .deserialize = tpms_protocol_encoder_nissan_deserialize,
    .stop = tpms_protocol_encoder_nissan_stop,
    .yield = tpms_protocol_encoder_nissan_yield,
};

const SubGhzProtocol tpms_protocol_nissan = {
    .name = TPMS_PROTOCOL_NISSAN_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_nissan_decoder,
    .encoder = &tpms_protocol_nissan_encoder,
};

void* tpms_protocol_decoder_nissan_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderNissan* instance = calloc(1, sizeof(TPMSProtocolDecoderNissan));
    instance->base.protocol = &tpms_protocol_nissan;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_nissan_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_nissan_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderNissan* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->raw_shift = 0;
    instance->in_data = false;
    instance->data_second = false;
    instance->have_half = false;
    instance->first_half = 0;
}

// Reconstruct the 5 payload bytes from the 37-bit (right-aligned) decode_data.
// Left-shifting by 3 pads the frame to 40 bits so the first received bit lands
// in b[0] bit7 (MSB-first), matching rtl_433's byte slicing.
static void nissan_get_bytes(TPMSProtocolDecoderNissan* instance, uint8_t b[5]) {
    uint64_t v = (instance->decoder.decode_data & 0x1FFFFFFFFFULL) << 3; // 37 bits → 40
    for(int i = 0; i < 5; i++) {
        b[i] = (uint8_t)((v >> (8 * (4 - i))) & 0xFF);
    }
}

static bool nissan_check_crc(TPMSProtocolDecoderNissan* instance) {
    uint8_t b[5];
    nissan_get_bytes(instance, b);
    uint8_t chk = 0;
    for(int i = 0; i < 4; i++) {
        chk += (uint8_t)(b[i] >> 7);
        chk += (uint8_t)(b[i] >> 5);
        chk += (uint8_t)(b[i] >> 3);
        chk += (uint8_t)(b[i] >> 1);
        chk += (uint8_t)(b[i] << 1);
    }
    chk += (uint8_t)(b[4] >> 7);
    chk += (uint8_t)(b[4] >> 5);
    chk += (uint8_t)(b[4] >> 3);
    return ((uint8_t)(~chk) & 0x03) == 0;
}

static void nissan_analyze(TPMSProtocolDecoderNissan* instance) {
    uint8_t b[5];
    nissan_get_bytes(instance, b);
    instance->generic.id = (uint32_t)(
        ((uint32_t)(b[0] & 0x1F) << 19) | ((uint32_t)b[1] << 11) | ((uint32_t)b[2] << 3) |
        ((uint32_t)b[3] >> 5));
    int pressure_raw = (int)(((b[3] & 0x1F) << 3) | (b[4] >> 5));
    float psi = (float)pressure_raw / 4.0f - 3.0f;
    instance->generic.pressure = psi * 0.0689476f; // PSI → bar (view stores bar)
    instance->generic.temperature = 0.0f; // not transmitted
    instance->generic.battery_low = TPMS_NO_BATT;
}

// Consume one raw half-symbol bit: hunt the raw preamble (0xF5555555E, or the
// inverted-polarity 0x0AAAAAAA1), then Manchester-decode the 37-bit payload.
// Matches rtl_433 tpms_nissan (preamble searched in raw bits; only the data is
// Manchester, then inverted — which, for a valid transition pair, means taking
// the 1st half for the normal preamble / 2nd half for the inverted one).
static void nissan_emit_raw_bit(TPMSProtocolDecoderNissan* instance, uint8_t bit) {
    bit &= 1;
    if(!instance->in_data) {
        instance->raw_shift = ((instance->raw_shift << 1) | bit) & NISSAN_PREAMBLE_MASK;
        if(instance->raw_shift == NISSAN_PREAMBLE || instance->raw_shift == NISSAN_PREAMBLE_INV) {
            instance->in_data = true;
            instance->data_second = (instance->raw_shift == NISSAN_PREAMBLE_INV);
            instance->have_half = false;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
        }
        return;
    }
    // Inside the Manchester payload: pair up half-symbols. A valid pair is a
    // transition (halves differ); the data bit is the 1st half (normal preamble)
    // or the 2nd half (inverted preamble). No transition = end/garbage -> resync.
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
    subghz_protocol_blocks_add_bit(&instance->decoder, data_bit);
    if(instance->decoder.decode_count_bit == NISSAN_DATA_BITS) {
        if(nissan_check_crc(instance)) {
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            nissan_analyze(instance);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        instance->in_data = false;
        instance->raw_shift = 0;
    }
}

void tpms_protocol_decoder_nissan_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderNissan* instance = context;

    const uint32_t te = tpms_protocol_nissan_const.te_short; // 120us half-symbol
    // A gap (or any pulse past the preamble's 4-symbol sync run) ends the burst.
    // Unlike Ford's fully alternating preamble, the Nissan sync carries runs of
    // four identical half-symbols (the 0xf.. / ..e nibbles), so expand up to 4.
    if(duration >= te * 6) {
        tpms_protocol_decoder_nissan_reset(instance);
        return;
    }
    // Expand this pulse into raw half-symbol bits of the current level.
    uint32_t n = (duration + te / 2) / te;
    if(n < 1) n = 1;
    if(n > 4) n = 4;
    for(uint32_t k = 0; k < n; k++) {
        nissan_emit_raw_bit(instance, level ? 1 : 0);
    }
}

uint8_t tpms_protocol_decoder_nissan_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderNissan* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_nissan_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderNissan* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_nissan_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderNissan* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, tpms_protocol_nissan_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_nissan_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderNissan* instance = context;

    // Recompute display fields from the stored payload.
    uint8_t b[5];
    uint64_t v = (instance->generic.data & 0x1FFFFFFFFFULL) << 3;
    for(int i = 0; i < 5; i++) {
        b[i] = (uint8_t)((v >> (8 * (4 - i))) & 0xFF);
    }
    uint8_t mode = (uint8_t)(b[0] >> 5);
    uint32_t id = (uint32_t)(
        ((uint32_t)(b[0] & 0x1F) << 19) | ((uint32_t)b[1] << 11) | ((uint32_t)b[2] << 3) |
        ((uint32_t)b[3] >> 5));
    int pressure_raw = (int)(((b[3] & 0x1F) << 3) | (b[4] >> 5));
    float psi = (float)pressure_raw / 4.0f - 3.0f;
    float bar = psi * 0.0689476f;

    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%06lX Mode:%u\r\n"
        "Bar:%3.2f PSI:%2.0f",
        instance->generic.protocol_name,
        (unsigned long)id,
        (unsigned)mode,
        (double)bar,
        (double)psi);
}

// -----------------------------------------------------------------------------
// Encoder
// -----------------------------------------------------------------------------
//
// FSK, G.E. Thomas Manchester (data = 1st half of each transitioning pair, the
// normal-preamble convention the decoder uses). te = 120us half-symbol.
// Frame: inter-frame gap + RAW 36-bit sync preamble 0xF5555555E (one raw
// half-symbol per bit -- NOT Manchester) + 37 Manchester data bits (MSB-first,
// b[0] bit7 .. b[4] bit3). The low 3 bits of b[4] are never transmitted.
//
// The serialized generic.data holds the 37-bit right-aligned payload. The 2-bit
// "unknown" field (b[4] bits 4:3) is the sum2N checksum: it is recomputed so the
// decoder low-2-bit sum comes out to 0b11 ((~chk & 3)==0), which makes any
// modified/0-PSI frame valid. Temperature is not part of this wire format.
//
// Worst-case entries: 1 gap + 36 preamble + 2*37 data + 1 flush = 112.
#define TPMS_NISSAN_ENCODER_UPLOAD_SIZE 200

// Rebuild the 5 payload bytes (b[0..4], top 37 bits significant) from the 37-bit
// right-aligned decode value, matching nissan_get_bytes().
static void nissan_frame_from_data(uint64_t data, uint8_t b[5]) {
    uint64_t v = (data & 0x1FFFFFFFFFULL) << 3; // 37 bits -> 40
    for(int i = 0; i < 5; i++) {
        b[i] = (uint8_t)((v >> (8 * (4 - i))) & 0xFF);
    }
}

// rtl_433 sum2N-style 2-bit checksum over b[0..4] (same terms as nissan_check_crc).
static uint8_t nissan_calc_chk(const uint8_t b[5]) {
    uint8_t chk = 0;
    for(int i = 0; i < 4; i++) {
        chk += (uint8_t)(b[i] >> 7);
        chk += (uint8_t)(b[i] >> 5);
        chk += (uint8_t)(b[i] >> 3);
        chk += (uint8_t)(b[i] >> 1);
        chk += (uint8_t)(b[i] << 1);
    }
    chk += (uint8_t)(b[4] >> 7);
    chk += (uint8_t)(b[4] >> 5);
    chk += (uint8_t)(b[4] >> 3);
    return chk;
}

// Choose the 2-bit "unknown" field (b[4] bits 4:3) so the sum2N low 2 bits == 0b11.
// The unknown bits enter chk only via (b[4] >> 3), with unit weight, so
// chk(u) = chk(0) + u, and u = (3 - chk(0)) & 3 satisfies (~chk & 3)==0.
static void nissan_apply_checksum(uint8_t b[5]) {
    b[4] &= (uint8_t)~0x18; // clear bits 4:3
    uint8_t chk0 = nissan_calc_chk(b);
    uint8_t u = (uint8_t)((3 - chk0) & 0x03);
    b[4] |= (uint8_t)(u << 3);
}

void tpms_protocol_nissan_pack(TPMSBlockGeneric* g) {
    furi_assert(g);
    // Invert analyze(): bar = (raw/4 - 3) * 0.0689476 (PSI->bar).
    //   raw = round((bar / 0.0689476 + 3) * 4)
    float psi = g->pressure / 0.0689476f;
    int p_raw = (int)((psi + 3.0f) * 4.0f + 0.5f);
    if(p_raw < 0) p_raw = 0;
    if(p_raw > 0xFF) p_raw = 0xFF;

    // Preserve a captured MODE nibble (b[0] bits 7:5) when re-packing an existing
    // frame; defaults to 0 for a synthesized one.
    uint8_t prev[5];
    nissan_frame_from_data(g->data, prev);
    uint8_t mode = (uint8_t)(prev[0] >> 5);

    uint32_t id = g->id & 0xFFFFFFUL; // 24-bit ID
    uint8_t b[5];
    b[0] = (uint8_t)((mode << 5) | ((id >> 19) & 0x1F));
    b[1] = (uint8_t)((id >> 11) & 0xFF);
    b[2] = (uint8_t)((id >> 3) & 0xFF);
    b[3] = (uint8_t)((((id & 0x7) << 5)) | (((uint32_t)p_raw >> 3) & 0x1F));
    b[4] = (uint8_t)(((uint32_t)p_raw & 0x7) << 5); // low 3 pressure bits; unknown/chk below
    nissan_apply_checksum(b);

    uint64_t v = ((uint64_t)b[0] << 32) | ((uint64_t)b[1] << 24) | ((uint64_t)b[2] << 16) |
                 ((uint64_t)b[3] << 8) | (uint64_t)b[4];
    g->data = v >> 3; // 40 -> 37-bit right-aligned
    g->data_count_bit = NISSAN_DATA_BITS;
}

void* tpms_protocol_encoder_nissan_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderNissan* instance = malloc(sizeof(TPMSProtocolEncoderNissan));
    instance->base.protocol = &tpms_protocol_nissan;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_NISSAN_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_nissan_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderNissan* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_nissan_stop(void* context) {
    TPMSProtocolEncoderNissan* instance = context;
    instance->encoder.is_running = false;
}

// Rebuild b[0..4] from generic.data and recompute the sum2N checksum so a
// modified/0-PSI frame is always valid.
static void tpms_protocol_encoder_nissan_build_frame(TPMSBlockGeneric* generic, uint8_t b[5]) {
    nissan_frame_from_data(generic->data, b);
    nissan_apply_checksum(b);
}

static bool tpms_protocol_encoder_nissan_get_upload(TPMSProtocolEncoderNissan* instance) {
    furi_assert(instance);
    const uint32_t te = tpms_protocol_nissan_const.te_short;

    uint8_t frame[5];
    tpms_protocol_encoder_nissan_build_frame(&instance->generic, frame);

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

// Manchester data bit (G.E. Thomas, data = 1st half): the decoder takes the 1st
// half of each transitioning pair (normal preamble 0xF5555555E), so bit=1 ->
// HIGH then LOW, bit=0 -> LOW then HIGH.
#define MAN_BIT(bit)         \
    do {                     \
        if(bit) {            \
            EMIT(true, te);  \
            EMIT(false, te); \
        } else {             \
            EMIT(false, te); \
            EMIT(true, te);  \
        }                    \
    } while(0)

    EMIT(false, te * 50); // inter-frame gap (>= 6*te resets the decoder)

    // RAW 36-bit sync preamble 0xF5555555E, MSB-first -- one raw half-symbol per
    // bit (NOT Manchester). The decoder hunts this exact pattern (or its inverted
    // twin) in the raw half-symbol stream before switching to Manchester payload.
    for(int i = 35; i >= 0; i--) {
        EMIT(((NISSAN_PREAMBLE >> i) & 0x1ULL) ? true : false, te);
    }

    // 37 Manchester data bits, MSB-first: b[0] bit7 .. b[4] bit3 (low 3 bits of
    // b[4] are not transmitted).
    for(int i = 0; i < NISSAN_DATA_BITS; i++) {
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
    tpms_protocol_encoder_nissan_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderNissan* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_nissan_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_NISSAN_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_nissan_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_nissan_yield(void* context) {
    TPMSProtocolEncoderNissan* instance = context;

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
