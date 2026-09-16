#include "tpms_toyota.h"
#include "../blocks/generic.h"

#define TAG "TPMSToyota"

// Port of rtl_433/src/devices/tpms_toyota.c (Christian W. Zuckschwerdt 2017, GPL-2+).
//
// FSK 315 MHz (US) / 433.92 MHz (EU), 9-byte Differential-Manchester encoded
// TPMS data with CRC-8. Pacific Industries PMV-C210, seen on Toyota
// Auris/Corolla and other brands.
//
// On air: ~14 bits sync then 72 Differential-Manchester encoded data bits then
// a 3-bit trailer, e.g. raw "01010101001111" + [72 DM data] + trailer. The sync
// ends with a run of four identical half-bit symbols ("1111") which forms a
// ~2*te_long framing pulse — a duration that never occurs inside valid
// Differential Manchester data (max valid interval is one te_long, a "1" bit).
// We use that unique framing pulse as the sync anchor, then Differential-
// Manchester-decode 72 data bits directly from pulse durations (same coding as
// tpms_pmv107j.c: one te_long pulse = "1"; two te_short pulses = "0").
//
// Wire format (after Differential-Manchester decoding, 9 bytes):
//   II II II II  b4  b5  b6  NN  CC
//   b[0..3]  = 32-bit ID, big-endian
//   b[4]     = status bit (bit7) + pressure MSB bits (6:0)
//   b[5]     = pressure LSB (bit7) + temperature MSB bits (6:0)
//   b[6]     = temperature LSB (bit7) + 7-bit state (6:0)
//   b[7]     = inverted pressure (pressure1 ^ 0xFF) — integrity check
//   b[8]     = CRC-8 (poly 0x07, init 0x80) over b[0..7]
//
//   pressure1 (8-bit) = (b[4] & 0x7F) << 1 | b[5] >> 7   → PSI = raw*0.25 - 7.0
//   temp      (8-bit) = (b[5] & 0x7F) << 1 | b[6] >> 7   → °C  = raw - 40
//
// Decode-only port (encoder all-NULL, like audiovox.c).

#define TOYOTA_DATA_BITS 72

static const SubGhzBlockConst tpms_protocol_toyota_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = TOYOTA_DATA_BITS,
};

typedef enum {
    ToyotaDmAtBoundary = 0, // about to start a new bit
    ToyotaDmAtMid, // saw mid-bit transition, waiting for the next-bit boundary
} ToyotaDmState;

typedef enum {
    ToyotaStepReset = 0, // hunt for the framing sync pulse
    ToyotaStepData, // clocked, collecting 72 Differential-Manchester bits
} ToyotaStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;

    // Raw half-symbol bits for the current burst. On a gap the buffer is searched
    // for the 12-bit 0xA9E preamble and the payload is differential-Manchester
    // decoded, faithfully mirroring rtl_433 bitbuffer_differential_manchester_decode
    // (the old pulse-width DM + 2*te_long framing anchor never matched real frames).
    uint8_t rawbuf[48]; // up to 384 raw half-symbol bits
    uint16_t rawbits;
    uint64_t head_64_bit; // upper bits when frame > 64 bit (add_to_128_bit)
} TPMSProtocolDecoderToyota;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderToyota;

const SubGhzProtocolDecoder tpms_protocol_toyota_decoder = {
    .alloc = tpms_protocol_decoder_toyota_alloc,
    .free = tpms_protocol_decoder_toyota_free,
    .feed = tpms_protocol_decoder_toyota_feed,
    .reset = tpms_protocol_decoder_toyota_reset,
    .get_hash_data = tpms_protocol_decoder_toyota_get_hash_data,
    .serialize = tpms_protocol_decoder_toyota_serialize,
    .deserialize = tpms_protocol_decoder_toyota_deserialize,
    .get_string = tpms_protocol_decoder_toyota_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_toyota_encoder = {
    .alloc = tpms_protocol_encoder_toyota_alloc,
    .free = tpms_protocol_encoder_toyota_free,
    .deserialize = tpms_protocol_encoder_toyota_deserialize,
    .stop = tpms_protocol_encoder_toyota_stop,
    .yield = tpms_protocol_encoder_toyota_yield,
};

const SubGhzProtocol tpms_protocol_toyota = {
    .name = TPMS_PROTOCOL_TOYOTA_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_toyota_decoder,
    .encoder = &tpms_protocol_toyota_encoder,
};

void* tpms_protocol_decoder_toyota_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderToyota* instance = calloc(1, sizeof(TPMSProtocolDecoderToyota));
    instance->base.protocol = &tpms_protocol_toyota;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_toyota_free(void* context) {
    furi_assert(context);
    free(context);
}

static void toyota_reset_state(TPMSProtocolDecoderToyota* instance) {
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->head_64_bit = 0;
    instance->rawbits = 0;
}

void tpms_protocol_decoder_toyota_reset(void* context) {
    furi_assert(context);
    toyota_reset_state(context);
}

// Raw half-symbol bit access (MSB-first per byte), matching rtl_433 bit_at.
static inline uint8_t toyota_bit_at(const uint8_t* buf, unsigned i) {
    return (uint8_t)((buf[i >> 3] >> (7 - (i & 7))) & 1);
}

// Faithful port of rtl_433 bitbuffer_differential_manchester_decode: decode up to
// `maxbits` data bits starting at raw-bit `start` into out[] (one byte per bit),
// returning the count. bit1 is the clock half (must differ from the previous data
// half); the data bit is (bit1==bit2)?1:0.
static int toyota_dm_decode(
    const uint8_t* buf,
    unsigned nbits,
    unsigned start,
    uint8_t* out,
    int maxbits) {
    unsigned ipos = start, len = nbits;
    if(maxbits && len > start + (unsigned)maxbits * 2) len = start + (unsigned)maxbits * 2;
    uint8_t bit2 = 0;
    int n = 0;
    // sync: establish the clock (may emit leading data bits, per rtl_433)
    while(ipos + 1 < len) {
        uint8_t b1 = toyota_bit_at(buf, ipos++);
        uint8_t b2 = toyota_bit_at(buf, ipos++);
        uint8_t b3 = (ipos < nbits) ? toyota_bit_at(buf, ipos) : 0;
        if(b1 != b2) {
            if(b2 != b3) {
                if(n < maxbits) out[n++] = 0;
            } else {
                bit2 = b1;
                ipos -= 1;
                break;
            }
        } else {
            bit2 = (uint8_t)(1 - b1);
            ipos -= 2;
            break;
        }
    }
    // main: two raw halves per data bit
    while(ipos + 1 < len && n < maxbits) {
        uint8_t b1 = toyota_bit_at(buf, ipos++);
        if(b1 == bit2) break; // clock missing
        uint8_t b2 = toyota_bit_at(buf, ipos++);
        out[n++] = (b1 == b2) ? 1 : 0;
        bit2 = b2;
    }
    return n;
}

static void toyota_get_bytes(TPMSProtocolDecoderToyota* instance, uint8_t b[9]) {
    // 72 bits: add_to_128_bit keeps the upper 8 bits in head_64_bit (= b[0]) and
    // the low 64 bits in decode_data (= b[1..8]).
    b[0] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 1] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

static bool toyota_check(TPMSProtocolDecoderToyota* instance) {
    uint8_t b[9];
    toyota_get_bytes(instance, b);
    // CRC-8 poly 0x07 init 0x80 over b[0..7] must equal b[8].
    if(subghz_protocol_blocks_crc8(b, 8, 0x07, 0x80) != b[8]) return false;
    // Pressure integrity: b[7] is the inverted pressure byte.
    uint8_t pressure1 = (uint8_t)(((b[4] & 0x7F) << 1) | (b[5] >> 7));
    uint8_t pressure2 = (uint8_t)(b[7] ^ 0xFF);
    if(pressure1 != pressure2) return false;
    return true;
}

static void toyota_analyze(TPMSProtocolDecoderToyota* instance) {
    uint8_t b[9];
    toyota_get_bytes(instance, b);
    instance->generic.id = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                           ((uint32_t)b[2] << 8) | b[3];
    uint8_t pressure_raw = (uint8_t)(((b[4] & 0x7F) << 1) | (b[5] >> 7));
    uint8_t temp_raw = (uint8_t)(((b[5] & 0x7F) << 1) | (b[6] >> 7));
    // kPa = raw*0.25*6.895 - 48.26 → bar
    float kpa = (float)pressure_raw * 0.25f * 6.895f - 48.26f;
    instance->generic.pressure = kpa * 0.01f;
    instance->generic.temperature = (float)((int)temp_raw - 40);
    instance->generic.battery_low = TPMS_NO_BATT;
}

// Search the accumulated raw buffer for the 12-bit preamble 0xA9E and, from each
// hit, differential-Manchester-decode the 72-bit payload (from bitpos+11) and
// accept on CRC + the pressure/inverted-pressure integrity check. Mirrors
// rtl_433 tpms_toyota_callback.
static void toyota_process(TPMSProtocolDecoderToyota* instance) {
    unsigned nbits = instance->rawbits;
    if(nbits < 12u + TOYOTA_DATA_BITS * 2u) return;
    for(unsigned pos = 0; pos + 12u <= nbits; pos++) {
        uint16_t v = 0;
        for(int k = 0; k < 12; k++)
            v = (uint16_t)((v << 1) | toyota_bit_at(instance->rawbuf, pos + k));
        if(v != 0x0A9E) continue;

        // Decode up to 80 bits (the sync loop can emit leading data bits before
        // the clock locks, per rtl_433), then require the full 72-bit payload.
        uint8_t bits[80];
        int n = toyota_dm_decode(instance->rawbuf, nbits, pos + 11, bits, 80);
        if(n < (int)TOYOTA_DATA_BITS) continue;

        instance->decoder.decode_data = 0;
        instance->head_64_bit = 0;
        instance->decoder.decode_count_bit = 0;
        for(int i = 0; i < (int)TOYOTA_DATA_BITS; i++) {
            subghz_protocol_blocks_add_to_128_bit(
                &instance->decoder, bits[i], &instance->head_64_bit);
        }
        if(toyota_check(instance)) {
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            toyota_analyze(instance);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
            return; // one good frame per burst is enough
        }
    }
}

void tpms_protocol_decoder_toyota_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderToyota* instance = context;

    const uint32_t te = tpms_protocol_toyota_const.te_short; // 52us half-symbol
    // A gap (or an over-long pulse) ends the burst: decode what we have, reset.
    if(duration >= te * 5) {
        toyota_process(instance);
        toyota_reset_state(instance);
        return;
    }
    // Expand this pulse into 1..4 raw half-symbol bits of the current level (the
    // sync's "1111" run is four identical half-symbols).
    uint32_t nb = (duration + te / 2) / te;
    if(nb < 1) nb = 1;
    if(nb > 4) nb = 4;
    for(uint32_t k = 0; k < nb; k++) {
        if(instance->rawbits >= (uint16_t)(sizeof(instance->rawbuf) * 8)) {
            toyota_process(instance);
            toyota_reset_state(instance);
        }
        unsigned idx = instance->rawbits;
        if(level)
            instance->rawbuf[idx >> 3] |= (uint8_t)(0x80u >> (idx & 7u));
        else
            instance->rawbuf[idx >> 3] &= (uint8_t)~(0x80u >> (idx & 7u));
        instance->rawbits++;
    }
}

uint8_t tpms_protocol_decoder_toyota_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderToyota* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_toyota_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderToyota* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_toyota_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderToyota* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, tpms_protocol_toyota_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_toyota_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderToyota* instance = context;
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
// Packer (inverse of analyze())
// -----------------------------------------------------------------------------
//
// Rebuild the 9 wire bytes from generic.{id,pressure,temperature}, preserving the
// captured status bit (b[4] bit7) and 7-bit state (b[6] bits 6:0) from generic.data.
//   pressure1 (8-bit) = (b[4]&0x7F)<<1 | b[5]>>7 ; bar = (raw*1.72375 - 48.26)*0.01
//   temp      (8-bit) = (b[5]&0x7F)<<1 | b[6]>>7 ; C = raw - 40
// b[7] is the inverted pressure integrity byte, b[8] the CRC-8. generic.data holds
// b[1..8]; b[0] (the top ID byte) is carried by generic.id.
void tpms_protocol_toyota_pack(TPMSBlockGeneric* generic) {
    furi_assert(generic);
    // Recover the captured wire bytes b[1..8] so status/state bits survive.
    uint8_t b[9];
    b[0] = (uint8_t)((generic->id >> 24) & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 1] = (uint8_t)((generic->data >> (8 * (7 - i))) & 0xFF);
    }
    // 32-bit ID -> b[0..3] (b[0] recomputed above from generic.id).
    b[1] = (uint8_t)((generic->id >> 16) & 0xFF);
    b[2] = (uint8_t)((generic->id >> 8) & 0xFF);
    b[3] = (uint8_t)(generic->id & 0xFF);

    // bar = (raw*0.25*6.895 - 48.26)*0.01  ->  raw = (bar*100 + 48.26)/1.72375
    int p_raw = (int)((generic->pressure * 100.0f + 48.26f) / 1.72375f + 0.5f);
    if(p_raw < 0) p_raw = 0;
    if(p_raw > 0xFF) p_raw = 0xFF;
    // C = raw - 40  ->  raw = C + 40
    int t_raw =
        (int)(generic->temperature + (generic->temperature >= 0 ? 0.5f : -0.5f)) + 40;
    if(t_raw < 0) t_raw = 0;
    if(t_raw > 0xFF) t_raw = 0xFF;

    b[4] = (uint8_t)((b[4] & 0x80) | ((p_raw >> 1) & 0x7F)); // status bit7 preserved
    b[5] = (uint8_t)(((p_raw & 1) << 7) | ((t_raw >> 1) & 0x7F));
    b[6] = (uint8_t)(((t_raw & 1) << 7) | (b[6] & 0x7F)); // 7-bit state preserved
    b[7] = (uint8_t)(p_raw ^ 0xFF); // inverted-pressure integrity
    b[8] = subghz_protocol_blocks_crc8(b, 8, 0x07, 0x80);

    uint64_t data = 0;
    for(int i = 1; i <= 8; i++) data = (data << 8) | b[i];
    generic->data = data;
    generic->data_count_bit = TOYOTA_DATA_BITS;
}

// -----------------------------------------------------------------------------
// Encoder
// -----------------------------------------------------------------------------
//
// FSK Differential-Manchester as consumed by the decoder above (which ignores
// level and looks only at pulse durations, expanding each into round(dur/te_short)
// raw half-symbols):
//   bit 1 -> two identical half-symbols (mid-bit has NO transition)
//   bit 0 -> two differing half-symbols (mid-bit transition)
// with a boundary transition before every bit. Levels alternate half-to-half.
//
// Frame on the wire: inter-frame gap (reset) + the RAW 12-bit preamble 0xA9E
// (0b1010 1001 1110) + 72 differential-Manchester data bits. The decoder hunts the
// raw 12-bit 0xA9E and DM-decodes from bitpos+11 (one bit of overlap into the
// preamble's trailing 0, which the DM sync loop absorbs), so data DM-encoding
// begins right after the full preamble with prev = the preamble's last bit (0).
//
// The 72-bit frame is b[0](8 bits) + b[1..8]; generic.data holds b[1..8]
// (including the CRC b[8]) and b[0] (top ID byte) comes from generic.id. Worst
// case: 1 gap + 12 preamble + 72*2 data = 157 half-symbol entries.
#define TPMS_TOYOTA_ENCODER_UPLOAD_SIZE 200

void* tpms_protocol_encoder_toyota_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderToyota* instance = malloc(sizeof(TPMSProtocolEncoderToyota));
    instance->base.protocol = &tpms_protocol_toyota;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_TOYOTA_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_toyota_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderToyota* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_toyota_stop(void* context) {
    TPMSProtocolEncoderToyota* instance = context;
    instance->encoder.is_running = false;
}

// Rebuild b[0..8]: b[1..8] from generic.data, b[0] from generic.id, then recompute
// the CRC so a modified/0-PSI frame is always valid.
static void tpms_protocol_encoder_toyota_build_frame(TPMSBlockGeneric* generic, uint8_t b[9]) {
    b[0] = (uint8_t)((generic->id >> 24) & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 1] = (generic->data >> (8 * (7 - i))) & 0xFF;
    }
    b[8] = subghz_protocol_blocks_crc8(b, 8, 0x07, 0x80);
}

static bool tpms_protocol_encoder_toyota_get_upload(TPMSProtocolEncoderToyota* instance) {
    furi_assert(instance);
    const uint32_t te_short = tpms_protocol_toyota_const.te_short;

    uint8_t frame[9];
    tpms_protocol_encoder_toyota_build_frame(&instance->generic, frame);

    // Build the 72-bit stream MSB-first: b[0] then b[1..8].
    uint8_t bits[TOYOTA_DATA_BITS];
    int n = 0;
    for(int j = 7; j >= 0; j--) bits[n++] = (frame[0] >> j) & 0x1;
    for(int i = 1; i <= 8; i++) {
        for(int j = 7; j >= 0; j--) bits[n++] = (frame[i] >> j) & 0x1;
    }

    size_t index = 0;
    const size_t max = instance->encoder.size_upload;
    bool last_level = false;
    uint32_t last_duration = 0;

    // Run-length-merged emit (matches the raw-bit model the decoder expands via
    // round(dur/te_short)): each RAW() is ONE raw half-symbol of te_short.
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
#define RAW(bit) EMIT((bit) ? true : false, te_short)

    // Inter-frame gap (>= te_short*5 reset threshold) so each frame starts clean.
    EMIT(false, te_short * 40);

    // Raw 12-bit preamble 0xA9E = 0b1010 1001 1110. The decoder hunts this RAW
    // pattern; it must NOT be differential-Manchester encoded. The trailing "1111"
    // run stays within the feed's per-pulse cap (four half-symbols).
    static const uint8_t preamble[12] = {1, 0, 1, 0, 1, 0, 0, 1, 1, 1, 1, 0};
    for(int i = 0; i < 12; i++) RAW(preamble[i]);

    // 72 data bits, differential-Manchester encoded to match toyota_dm_decode:
    // clock half b1 flips the previous data half; second half b2 == b1 for a '1',
    // != b1 for a '0'. prev starts at 0 (the preamble's last bit).
    int prev = 0;
    for(int i = 0; i < TOYOTA_DATA_BITS; i++) {
        int b1 = 1 - prev;
        int b2 = bits[i] ? b1 : (1 - b1);
        RAW(b1);
        RAW(b2);
        prev = b2;
    }

    if(last_duration != 0) {
        if(index >= max) return false;
        instance->encoder.upload[index++] = level_duration_make(last_level, last_duration);
    }

#undef RAW
#undef EMIT

    instance->encoder.size_upload = index;
    return true;
}

SubGhzProtocolStatus
    tpms_protocol_encoder_toyota_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderToyota* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_toyota_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_TOYOTA_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_toyota_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_toyota_yield(void* context) {
    TPMSProtocolEncoderToyota* instance = context;

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
