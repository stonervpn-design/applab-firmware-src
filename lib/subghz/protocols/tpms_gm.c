#include "tpms_gm.h"
#include "../blocks/generic.h"

#define TAG "TPMSGm"

// Port of rtl_433/src/devices/tpms_gm.c
//   "GM-Aftermarket TPMS"  (Eric Blevins, 2025, GPL-2+)
//   https://github.com/merbanan/rtl_433/blob/master/src/devices/tpms_gm.c
//
// Modulation : OOK, Manchester (OOK_PULSE_MANCHESTER_ZEROBIT), short_width 120us,
//              so one Manchester bit = two 120us half-symbols (240us/bit).
// Frame      : 130 raw bits. rtl_433 Manchester-decodes the whole row then checks
//              a fixed preamble and a modulo-256 checksum.
//
// Byte layout (rtl_433 b[], MSB-first, one byte per 8 decoded bits):
//   b[0..5]   preamble, six 0x00 bytes  (48 zero bits)
//   b[6..7]   flags   (16 bits)
//   b[8..12]  id      (40 bits; rtl_433 folds device-type + unique id together)
//   b[13]     pressure raw
//   b[14]     temperature raw
//   b[15]     checksum = (sum(b[6..14])) & 0xFF
//   (b[16] holds 2 trailing bits -> 130 total; not needed to validate)
//
// Scaling (rtl_433):
//   pressure_kPa  = pressure_raw * 2.75
//   temperature_C = temperature_raw - 60
//   battery_ok    = !((flags >> 5) & 1)      -> battery_low = (flags >> 5) & 1
//   learn_mode when flags bits 0,1,8 are all 0 (operational sets them to 1)
//
// This firmware stores pressure in BAR: bar = kPa / 100 = raw * 0.0275.
//
// generic.data holds the low 64 bits of the 80-bit payload (frame bytes
// b[8..15]); data_count_bit = 80. The upper 16 bits (flags, b[6..7]) are not
// stored: on (re)encode they are synthesized as an operational-mode frame whose
// battery bit comes from generic.battery_low, and the checksum is recomputed, so
// the encoder reproduces a self-consistent wire frame the decoder accepts.

#define GM_PREAMBLE_BYTES 6 // b[0..5]
#define GM_TOTAL_BYTES 16 // b[0..15]
#define GM_TOTAL_BITS 128 // 48 preamble + 80 payload
#define GM_PAYLOAD_BITS 80

static const SubGhzBlockConst tpms_protocol_tpms_gm_const = {
    .te_short = 120,
    .te_long = 240,
    .te_delta = 60,
    .min_count_bit_for_found = GM_PAYLOAD_BITS,
};

struct TPMSProtocolDecoderGm {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;

    uint8_t bytes[GM_TOTAL_BYTES]; // accumulated Manchester bits, MSB-first
    uint16_t bit_count; // number of decoded payload+preamble bits so far
    bool synced; // true once a gap has established Manchester phase
    bool have_half; // holding the first half-symbol of a Manchester pair
    uint8_t first_half; // level of the held first half-symbol
};

struct TPMSProtocolEncoderGm {
    SubGhzProtocolEncoderBase base;

    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
};

typedef enum {
    GmDecoderStepReset = 0,
} GmDecoderStep;

const SubGhzProtocolDecoder tpms_protocol_tpms_gm_decoder = {
    .alloc = tpms_protocol_decoder_tpms_gm_alloc,
    .free = tpms_protocol_decoder_tpms_gm_free,

    .feed = tpms_protocol_decoder_tpms_gm_feed,
    .reset = tpms_protocol_decoder_tpms_gm_reset,

    .get_hash_data = tpms_protocol_decoder_tpms_gm_get_hash_data,
    .serialize = tpms_protocol_decoder_tpms_gm_serialize,
    .deserialize = tpms_protocol_decoder_tpms_gm_deserialize,
    .get_string = tpms_protocol_decoder_tpms_gm_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_tpms_gm_encoder = {
    .alloc = tpms_protocol_encoder_tpms_gm_alloc,
    .free = tpms_protocol_encoder_tpms_gm_free,

    .deserialize = tpms_protocol_encoder_tpms_gm_deserialize,
    .stop = tpms_protocol_encoder_tpms_gm_stop,
    .yield = tpms_protocol_encoder_tpms_gm_yield,
};

const SubGhzProtocol tpms_protocol_tpms_gm = {
    .name = TPMS_PROTOCOL_GM_NAME,
    .type = SubGhzProtocolTypeTpms,
    // OOK -> AM. 433 MHz band. Full sensor feature set.
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,

    .decoder = &tpms_protocol_tpms_gm_decoder,
    .encoder = &tpms_protocol_tpms_gm_encoder,
};

// -----------------------------------------------------------------------------
// Shared field helpers
// -----------------------------------------------------------------------------

// Build the 16-bit flags word from generic->battery_low, as an operational-mode
// frame: bits 0,1,8 = 1 (operational), bit 5 = battery_low, rest 0.
static uint16_t gm_flags_from_generic(const TPMSBlockGeneric* g) {
    uint16_t flags = 0x0103; // bit8 (=0x0100) + bit1 + bit0
    if(g->battery_low) flags |= (1u << 5);
    return flags;
}

// Fill frame b[0..15] from a serialized generic (b[8..15] come from generic->data,
// flags b[6..7] are synthesized, preamble is zero, checksum recomputed).
static void gm_build_frame(const TPMSBlockGeneric* g, uint8_t b[GM_TOTAL_BYTES]) {
    for(int i = 0; i < GM_TOTAL_BYTES; i++) b[i] = 0;
    uint16_t flags = gm_flags_from_generic(g);
    b[6] = (uint8_t)(flags >> 8);
    b[7] = (uint8_t)(flags & 0xFF);
    // b[8..15] = low 64 bits of generic->data, MSB-first.
    for(int i = 0; i < 8; i++) {
        b[8 + i] = (uint8_t)((g->data >> (8 * (7 - i))) & 0xFF);
    }
    uint8_t sum = 0;
    for(int i = 6; i < 15; i++) sum += b[i];
    b[15] = sum; // modulo-256 checksum
}

void* tpms_protocol_decoder_tpms_gm_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderGm* instance = malloc(sizeof(TPMSProtocolDecoderGm));
    instance->base.protocol = &tpms_protocol_tpms_gm;
    instance->generic.protocol_name = instance->base.protocol->name;
    tpms_protocol_decoder_tpms_gm_reset(instance);
    return instance;
}

void tpms_protocol_decoder_tpms_gm_free(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderGm* instance = context;
    free(instance);
}

void tpms_protocol_decoder_tpms_gm_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderGm* instance = context;
    instance->decoder.parser_step = GmDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->bit_count = 0;
    instance->synced = false;
    instance->have_half = false;
    instance->first_half = 0;
    for(int i = 0; i < GM_TOTAL_BYTES; i++) instance->bytes[i] = 0;
}

static void gm_analyze(TPMSProtocolDecoderGm* instance) {
    const uint8_t* b = instance->bytes;

    uint16_t flags = ((uint16_t)b[6] << 8) | b[7];

    // rtl_433 folds b[8..12] (40 bits) into one id; generic.id is 32-bit, so keep
    // the low 32 bits (b[9..12]). For frames synthesized here b[8] == 0.
    uint64_t full_id = ((uint64_t)b[8] << 32) | ((uint64_t)b[9] << 24) | ((uint32_t)b[10] << 16) |
                       ((uint16_t)b[11] << 8) | b[12];
    instance->generic.id = (uint32_t)(full_id & 0xFFFFFFFFUL);

    uint8_t pressure_raw = b[13];
    uint8_t temperature_raw = b[14];

    // rtl_433: kPa = raw * 2.75. This firmware stores BAR (kPa / 100).
    instance->generic.pressure = (float)pressure_raw * 2.75f * 0.01f;
    instance->generic.temperature = (float)((int)temperature_raw - 60);
    instance->generic.battery_low = (uint8_t)((flags >> 5) & 1);

    // generic.data = payload bytes b[8..15] (low 64 bits of the 80-bit payload).
    uint64_t data = 0;
    for(int i = 8; i < GM_TOTAL_BYTES; i++) data = (data << 8) | b[i];
    instance->generic.data = data;
    instance->generic.data_count_bit = GM_PAYLOAD_BITS;

    instance->decoder.decode_data = data;
    instance->decoder.decode_count_bit = GM_PAYLOAD_BITS;
}

// Push one decoded Manchester bit (MSB-first) into the byte buffer; validate the
// full frame once all 128 bits (48 preamble + 80 payload) are in.
static void gm_push_bit(TPMSProtocolDecoderGm* instance, uint8_t bit) {
    uint16_t idx = instance->bit_count;
    if(idx >= GM_TOTAL_BITS) return;
    if(bit) instance->bytes[idx / 8] |= (uint8_t)(0x80 >> (idx % 8));
    instance->bit_count++;

    if(instance->bit_count < GM_TOTAL_BITS) return;

    // Full frame collected -> validate exactly as rtl_433 does.
    const uint8_t* b = instance->bytes;
    bool ok = true;
    for(int i = 0; i < GM_PREAMBLE_BYTES; i++) {
        if(b[i] != 0) {
            ok = false;
            break;
        }
    }
    if(ok) {
        uint8_t sum = 0;
        int all_zero = 1;
        for(int i = 6; i < 15; i++) {
            sum += b[i];
            all_zero &= (b[i] == 0);
        }
        if((sum & 0xFF) != b[15]) ok = false;
        if(all_zero && b[15] == 0) ok = false; // reject trivial all-zero payload
    }
    if(ok) {
        gm_analyze(instance);
        if(instance->base.callback)
            instance->base.callback(&instance->base, instance->base.context);
    }

    // Re-arm: wait for the next inter-frame gap to re-establish phase.
    instance->synced = false;
    instance->have_half = false;
    instance->bit_count = 0;
    for(int i = 0; i < GM_TOTAL_BYTES; i++) instance->bytes[i] = 0;
}

// Consume one raw half-symbol of the given level, pairing half-symbols into
// Manchester bits: a transitioning pair (LOW,HIGH) = 0, (HIGH,LOW) = 1. This is
// the OOK_PULSE_MANCHESTER_ZEROBIT convention with an implicit leading 0 bit
// (the first LOW half of preamble bit 0 is swallowed by the inter-frame gap, so
// after a gap we seed the pair with an implicit LOW).
static void gm_push_half(TPMSProtocolDecoderGm* instance, uint8_t half) {
    half &= 1;
    if(!instance->have_half) {
        instance->first_half = half;
        instance->have_half = true;
        return;
    }
    instance->have_half = false;
    if(instance->first_half == half) {
        // No mid-symbol transition -> phase/noise error; drop to unsynced and
        // wait for the next gap.
        instance->synced = false;
        instance->bit_count = 0;
        for(int i = 0; i < GM_TOTAL_BYTES; i++) instance->bytes[i] = 0;
        return;
    }
    uint8_t bit = (instance->first_half == 0) ? 0 : 1; // (LOW,HIGH)=0, (HIGH,LOW)=1
    gm_push_bit(instance, bit);
}

void tpms_protocol_decoder_tpms_gm_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderGm* instance = context;

    const uint32_t te = tpms_protocol_tpms_gm_const.te_short; // 120us half-symbol

    // A gap (>= 3 half-symbols of one level) ends/frames a burst. Manchester never
    // holds one level for more than two half-symbols, so this re-establishes phase
    // and seeds the implicit leading LOW half (zerobit).
    if(duration >= te * 3) {
        instance->synced = true;
        instance->have_half = true;
        instance->first_half = 0; // implicit LOW leading half
        instance->bit_count = 0;
        for(int i = 0; i < GM_TOTAL_BYTES; i++) instance->bytes[i] = 0;
        return;
    }

    if(!instance->synced) return;

    // Expand this pulse into 1 or 2 half-symbols of the current level.
    uint32_t n = (duration + te / 2) / te;
    if(n < 1) n = 1;
    if(n > 2) n = 2;
    for(uint32_t k = 0; k < n; k++) {
        gm_push_half(instance, level ? 1 : 0);
    }
}

uint8_t tpms_protocol_decoder_tpms_gm_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderGm* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_tpms_gm_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderGm* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_tpms_gm_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderGm* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, tpms_protocol_tpms_gm_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_tpms_gm_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderGm* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX\r\n"
        "Bat:%d\r\n"
        "Temp:%2.0f C Bar:%2.1f",
        instance->generic.protocol_name,
        instance->generic.id,
        instance->generic.battery_low,
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}

// -----------------------------------------------------------------------------
// Pack (inverse of gm_analyze)
// -----------------------------------------------------------------------------

void tpms_protocol_tpms_gm_pack(TPMSBlockGeneric* generic) {
    furi_assert(generic);

    // Inverse scaling. pressure BAR -> raw = round(bar * 100 / 2.75); temp raw = C + 60.
    int32_t pressure_raw = (int32_t)(generic->pressure * 100.0f / 2.75f + 0.5f);
    if(pressure_raw < 0) pressure_raw = 0;
    if(pressure_raw > 0xFF) pressure_raw = 0xFF;

    // Round the (offset) raw value, which is non-negative for valid temps (>= -60 C).
    float temp_offset = generic->temperature + 60.0f;
    int32_t temperature_raw = (int32_t)(temp_offset >= 0 ? temp_offset + 0.5f : 0);
    if(temperature_raw < 0) temperature_raw = 0;
    if(temperature_raw > 0xFF) temperature_raw = 0xFF;

    uint8_t b[GM_TOTAL_BYTES] = {0};
    uint16_t flags = gm_flags_from_generic(generic);
    b[6] = (uint8_t)(flags >> 8);
    b[7] = (uint8_t)(flags & 0xFF);
    b[8] = 0; // device-type high byte (not represented in 32-bit generic.id)
    b[9] = (uint8_t)((generic->id >> 24) & 0xFF);
    b[10] = (uint8_t)((generic->id >> 16) & 0xFF);
    b[11] = (uint8_t)((generic->id >> 8) & 0xFF);
    b[12] = (uint8_t)(generic->id & 0xFF);
    b[13] = (uint8_t)pressure_raw;
    b[14] = (uint8_t)temperature_raw;
    uint8_t sum = 0;
    for(int i = 6; i < 15; i++) sum += b[i];
    b[15] = sum;

    // generic.data = payload bytes b[8..15] (low 64 bits of 80-bit payload).
    uint64_t data = 0;
    for(int i = 8; i < GM_TOTAL_BYTES; i++) data = (data << 8) | b[i];
    generic->data = data;
    generic->data_count_bit = GM_PAYLOAD_BITS;
}

// -----------------------------------------------------------------------------
// Encoder
// -----------------------------------------------------------------------------
//
// OOK Manchester, one bit = two 120us half-symbols:
//   bit 0 -> first half LOW,  second half HIGH
//   bit 1 -> first half HIGH, second half LOW
// Frame: inter-frame gap (LOW) + 128 bits (b[0..15]) where b[0..5]=0 form the
// 48-bit zero preamble. Adjacent equal half-symbols are run-length merged.
//
// 128 bits * 2 half-symbols = 256 entries worst case + gap + flush.
#define TPMS_GM_ENCODER_UPLOAD_SIZE 300

void* tpms_protocol_encoder_tpms_gm_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderGm* instance = malloc(sizeof(TPMSProtocolEncoderGm));
    instance->base.protocol = &tpms_protocol_tpms_gm;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_GM_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_tpms_gm_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderGm* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_tpms_gm_stop(void* context) {
    TPMSProtocolEncoderGm* instance = context;
    instance->encoder.is_running = false;
}

static bool tpms_protocol_encoder_tpms_gm_get_upload(TPMSProtocolEncoderGm* instance) {
    furi_assert(instance);
    const uint32_t te = tpms_protocol_tpms_gm_const.te_short;

    uint8_t frame[GM_TOTAL_BYTES];
    gm_build_frame(&instance->generic, frame);

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
                if(index >= max) return false;                            \
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

    // Inter-frame gap so repeats are framed (and so the decoder seeds its implicit
    // leading LOW half). The first data bit b[0]=0 begins with a LOW half that
    // merges into this gap.
    EMIT(false, te * 50);

    // 128 bits, MSB-first, b[0]..b[15] (b[0..5] = zero preamble).
    for(int i = 0; i < GM_TOTAL_BITS; i++) {
        int byte = i / 8;
        int bit = 7 - (i % 8);
        MAN_BIT((frame[byte] >> bit) & 0x1);
    }

    // Flush trailing entry.
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
    tpms_protocol_encoder_tpms_gm_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderGm* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_tpms_gm_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }

        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_GM_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_tpms_gm_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_tpms_gm_yield(void* context) {
    TPMSProtocolEncoderGm* instance = context;

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
