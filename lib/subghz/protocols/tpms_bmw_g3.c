#include "tpms_bmw_g3.h"
#include "../blocks/generic.h"

#define TAG "TPMSBmwG3"

// Port of rtl_433/src/devices/tpms_bmw_g3.c
//   (C) 2024 Bruno OCTAU (ProfBoc75), @Billymazze, GPL-2+.
//
// BMW Gen2/Gen3 TPMS sensor.
// FSK, PCM (NRZ) raw bits at 52 us, Differential-Manchester encoded.
//
// rtl_433 finds the 16-bit raw preamble 0xCCCD in the un-decoded PCM bit stream
// (bitbuffer_search), then runs bitbuffer_differential_manchester_decode from
// bitpos + 16 for up to 88 data bits. We reconstruct that raw half-symbol bit
// stream in feed(), search the 16-bit preamble, and differential-Manchester
// decode from pos+16 with the same faithful port used by tpms_toyota.c (the old
// streaming DM slicer never matched real frames). We also accept the RF-inverted
// preamble 0x3332 and invert every raw bit, so either FSK deviation sign decodes
// (differential Manchester is polarity-invariant, so the payload is identical).
//
// Frame length distinguishes generation (rtl_433 checks bits_per_row):
//   Gen3: DM stream runs the full 88 bits (11 bytes).
//   Gen2: DM stream breaks at 80 bits (10 bytes).
//
// Wire format after DM decoding:
//   Gen3 (11 bytes): II II II II PP TT F1 F2 F3 CC CC
//   Gen2 (10 bytes): II II II II PP TT F1 F2       CC CC
//     II II II II = 32-bit sensor id (big-endian)
//     PP          = pressure, kPa = (PP - 43) * 2.5
//     TT          = temperature, C = TT - 40
//     F1 F2 F3    = flags (F3 Gen3 only)
//     CC CC       = CRC-16 poly 0x1021 init 0x0000 over the preceding bytes
//                   (residue over the whole frame including CRC == 0).
//
// TX encoder (Gen2 80-bit) added below: rebuilds the 10-byte frame from
// generic.id + generic.data, recomputes the CRC-16, and emits the raw 0xCCCD
// preamble followed by the payload differential-Manchester encoded exactly as
// bmw_g3_dm_decode consumes it. Mirrors tpms_pmv107j.c (same DM family).

#define BMW_G3_PREAMBLE     0xCCCDU
#define BMW_G3_PREAMBLE_INV 0x3332U // (uint16_t)~0xCCCD
#define BMW_G3_GEN2_BITS    80 // 10 bytes
#define BMW_G3_GEN3_BITS    88 // 11 bytes
#define BMW_G3_GEN2_BYTES   10

static const SubGhzBlockConst tpms_protocol_bmw_g3_const = {
    .te_short = 52, // one raw PCM bit (DM half-symbol)
    .te_long = 104, // two merged raw bits (DM full symbol)
    .te_delta = 20,
    .min_count_bit_for_found = BMW_G3_GEN2_BITS,
};

typedef enum {
    BmwG3StepReset = 0, // hunting raw preamble
    BmwG3StepData, // collecting differential-Manchester data bits
} BmwG3Step;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;

    // Raw half-symbol bits for the current burst. On a gap the buffer is searched
    // for the 16-bit 0xCCCD preamble (or inverted 0x3332) and the payload is
    // differential-Manchester decoded, faithfully mirroring rtl_433
    // bitbuffer_differential_manchester_decode (the old streaming DM slicer never
    // matched real frames).
    uint8_t rawbuf[64]; // up to 512 raw half-symbol bits
    uint16_t rawbits;
    uint8_t len_bytes; // resolved frame length for the matched CRC (10 or 11)
    uint64_t head_64_bit; // upper bits when frame > 64 bit (add_to_128_bit)
} TPMSProtocolDecoderBmwG3;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderBmwG3;

const SubGhzProtocolDecoder tpms_protocol_bmw_g3_decoder = {
    .alloc = tpms_protocol_decoder_bmw_g3_alloc,
    .free = tpms_protocol_decoder_bmw_g3_free,
    .feed = tpms_protocol_decoder_bmw_g3_feed,
    .reset = tpms_protocol_decoder_bmw_g3_reset,
    .get_hash_data = tpms_protocol_decoder_bmw_g3_get_hash_data,
    .serialize = tpms_protocol_decoder_bmw_g3_serialize,
    .deserialize = tpms_protocol_decoder_bmw_g3_deserialize,
    .get_string = tpms_protocol_decoder_bmw_g3_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_bmw_g3_encoder = {
    .alloc = tpms_protocol_encoder_bmw_g3_alloc,
    .free = tpms_protocol_encoder_bmw_g3_free,
    .deserialize = tpms_protocol_encoder_bmw_g3_deserialize,
    .stop = tpms_protocol_encoder_bmw_g3_stop,
    .yield = tpms_protocol_encoder_bmw_g3_yield,
};

const SubGhzProtocol tpms_protocol_bmw_g3 = {
    .name = TPMS_PROTOCOL_BMW_G3_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_bmw_g3_decoder,
    .encoder = &tpms_protocol_bmw_g3_encoder,
};

void* tpms_protocol_decoder_bmw_g3_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderBmwG3* instance = calloc(1, sizeof(TPMSProtocolDecoderBmwG3));
    instance->base.protocol = &tpms_protocol_bmw_g3;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_bmw_g3_free(void* context) {
    furi_assert(context);
    free(context);
}

static void bmw_g3_reset_state(TPMSProtocolDecoderBmwG3* instance) {
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->head_64_bit = 0;
    instance->rawbits = 0;
}

void tpms_protocol_decoder_bmw_g3_reset(void* context) {
    furi_assert(context);
    bmw_g3_reset_state(context);
}

// Raw half-symbol bit access (MSB-first per byte), matching rtl_433 bit_at.
static inline uint8_t bmw_g3_bit_at(const uint8_t* buf, unsigned i) {
    return (uint8_t)((buf[i >> 3] >> (7 - (i & 7))) & 1);
}

// Faithful port of rtl_433 bitbuffer_differential_manchester_decode: decode up to
// `maxbits` data bits starting at raw-bit `start` into out[] (one byte per bit),
// returning the count. bit1 is the clock half (must differ from the previous data
// half); the data bit is (bit1==bit2)?1:0. `inv` XORs every raw half so the
// RF-inverted deviation sign decodes identically.
static int bmw_g3_dm_decode(
    const uint8_t* buf,
    unsigned nbits,
    unsigned start,
    uint8_t* out,
    int maxbits,
    uint8_t inv) {
    unsigned ipos = start, len = nbits;
    if(maxbits && len > start + (unsigned)maxbits * 2) len = start + (unsigned)maxbits * 2;
    uint8_t bit2 = 0;
    int n = 0;
    // sync: establish the clock (may emit leading data bits, per rtl_433)
    while(ipos + 1 < len) {
        uint8_t b1 = (uint8_t)(bmw_g3_bit_at(buf, ipos++) ^ inv);
        uint8_t b2 = (uint8_t)(bmw_g3_bit_at(buf, ipos++) ^ inv);
        uint8_t b3 = (ipos < nbits) ? (uint8_t)(bmw_g3_bit_at(buf, ipos) ^ inv) : inv;
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
        uint8_t b1 = (uint8_t)(bmw_g3_bit_at(buf, ipos++) ^ inv);
        if(b1 == bit2) break; // clock missing
        uint8_t b2 = (uint8_t)(bmw_g3_bit_at(buf, ipos++) ^ inv);
        out[n++] = (b1 == b2) ? 1 : 0;
        bit2 = b2;
    }
    return n;
}

// Extract a `count`-bit frame (count a multiple of 8) into b[], MSB-first. The
// top (count-64) bits live in head_64_bit, the low 64 bits in decode_data.
static void bmw_g3_get_bytes(TPMSProtocolDecoderBmwG3* instance, uint16_t count, uint8_t b[11]) {
    int head_bytes = (count > 64) ? (int)((count - 64) / 8) : 0;
    for(int i = 0; i < head_bytes; i++) {
        b[i] = (uint8_t)((instance->head_64_bit >> (8 * (head_bytes - 1 - i))) & 0xFF);
    }
    for(int i = 0; i < 8; i++) {
        b[head_bytes + i] = (uint8_t)((instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF);
    }
}

static void bmw_g3_analyze(TPMSProtocolDecoderBmwG3* instance, const uint8_t* b) {
    instance->generic.id =
        ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    instance->generic.pressure = ((float)b[4] - 43.0f) * 2.5f * 0.01f; // kPa -> bar
    instance->generic.temperature = (float)((int)b[5] - 40);
    instance->generic.battery_low = TPMS_NO_BATT;
}

// Search the accumulated raw buffer for the 16-bit preamble 0xCCCD (or the
// RF-inverted 0x3332) and, from each hit, differential-Manchester-decode up to 88
// bits from pos+16. Frame length picks the generation: 88 bits = Gen3 (11-byte
// CRC-16), 80..87 bits = Gen2 (10-byte CRC-16). Accept on CRC residue == 0.
// Mirrors rtl_433 tpms_bmw_g3 decode.
static void bmw_g3_process(TPMSProtocolDecoderBmwG3* instance) {
    unsigned nbits = instance->rawbits;
    if(nbits < 16u + BMW_G3_GEN2_BITS * 2u) return;
    for(unsigned pos = 0; pos + 16u <= nbits; pos++) {
        uint16_t v = 0;
        for(int k = 0; k < 16; k++)
            v = (uint16_t)((v << 1) | bmw_g3_bit_at(instance->rawbuf, pos + k));
        uint8_t inv;
        if(v == BMW_G3_PREAMBLE)
            inv = 0;
        else if(v == BMW_G3_PREAMBLE_INV)
            inv = 1;
        else
            continue;

        // Decode up to 88 bits (the sync loop can emit leading data bits before
        // the clock locks, per rtl_433), then select the generation by length.
        uint8_t bits[88];
        int n = bmw_g3_dm_decode(instance->rawbuf, nbits, pos + 16, bits, 88, inv);
        int used_bits, len_bytes;
        if(n >= (int)BMW_G3_GEN3_BITS) {
            used_bits = BMW_G3_GEN3_BITS; // 88
            len_bytes = 11;
        } else if(n >= (int)BMW_G3_GEN2_BITS) {
            used_bits = BMW_G3_GEN2_BITS; // 80
            len_bytes = 10;
        } else {
            continue;
        }

        instance->decoder.decode_data = 0;
        instance->head_64_bit = 0;
        instance->decoder.decode_count_bit = 0;
        for(int i = 0; i < used_bits; i++) {
            subghz_protocol_blocks_add_to_128_bit(
                &instance->decoder, bits[i], &instance->head_64_bit);
        }
        uint8_t b[11];
        bmw_g3_get_bytes(instance, (uint16_t)used_bits, b);
        // CRC-16 poly 0x1021 init 0x0000 over all len_bytes (data + CRC) == 0.
        if(subghz_protocol_blocks_crc16(b, len_bytes, 0x1021, 0x0000) == 0) {
            instance->len_bytes = (uint8_t)len_bytes;
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            bmw_g3_analyze(instance, b);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
            return; // one good frame per burst is enough
        }
    }
}

void tpms_protocol_decoder_bmw_g3_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderBmwG3* instance = context;

    const uint32_t te = tpms_protocol_bmw_g3_const.te_short; // 52us half-symbol
    // A gap (or an over-long pulse) ends the burst: decode what we have, reset.
    if(duration >= te * 5) {
        bmw_g3_process(instance);
        bmw_g3_reset_state(instance);
        return;
    }
    // Expand this pulse into 1..4 raw half-symbol bits of the current level.
    uint32_t nb = (duration + te / 2) / te;
    if(nb < 1) nb = 1;
    if(nb > 4) nb = 4;
    for(uint32_t k = 0; k < nb; k++) {
        if(instance->rawbits >= (uint16_t)(sizeof(instance->rawbuf) * 8)) {
            bmw_g3_process(instance);
            bmw_g3_reset_state(instance);
        }
        unsigned idx = instance->rawbits;
        if(level)
            instance->rawbuf[idx >> 3] |= (uint8_t)(0x80u >> (idx & 7u));
        else
            instance->rawbuf[idx >> 3] &= (uint8_t)~(0x80u >> (idx & 7u));
        instance->rawbits++;
    }
}

uint8_t tpms_protocol_decoder_bmw_g3_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderBmwG3* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_bmw_g3_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderBmwG3* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_bmw_g3_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderBmwG3* instance = context;
    // Accept either frame length (Gen2 80-bit or Gen3 88-bit); decode-only.
    return tpms_block_generic_deserialize(&instance->generic, flipper_format);
}

void tpms_protocol_decoder_bmw_g3_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderBmwG3* instance = context;
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
// Pack (inverse of bmw_g3_analyze) — build a valid Gen2 (80-bit / 10-byte) frame.
// -----------------------------------------------------------------------------
//
// Wire (Gen2, 10 bytes): II II II II PP TT F1 F2 CC CC
//   PP  = pressure raw, bar = (PP - 43) * 2.5 * 0.01  ->  PP = bar / 0.025 + 43
//   TT  = temperature raw, C = TT - 40                ->  TT = C + 40
//   CC CC = CRC-16 poly 0x1021 init 0x0000 over b[0..7], big-endian (residue 0).
//
// generic.data holds the low 64 bits of the frame = b[2..9] (the decoder keeps
// b[0..1], the top two ID bytes, in head_64_bit, which is not serialized). We
// therefore rebuild b[0..3] from generic.id and preserve the captured flag bytes
// F1/F2 from generic.data before recomputing the CRC.
void tpms_protocol_bmw_g3_pack(TPMSBlockGeneric* g) {
    furi_assert(g);
    uint8_t b[BMW_G3_GEN2_BYTES];
    b[0] = (uint8_t)(g->id >> 24);
    b[1] = (uint8_t)(g->id >> 16);
    b[2] = (uint8_t)(g->id >> 8);
    b[3] = (uint8_t)(g->id);
    int pp = (int)(g->pressure / 0.025f + 43.0f + 0.5f);
    if(pp < 0) pp = 0;
    if(pp > 0xFF) pp = 0xFF;
    b[4] = (uint8_t)pp;
    int tt = (int)(g->temperature + (g->temperature >= 0 ? 0.5f : -0.5f)) + 40;
    if(tt < 0) tt = 0;
    if(tt > 0xFF) tt = 0xFF;
    b[5] = (uint8_t)tt;
    // Preserve captured flags F1/F2 (b[6],b[7]) from generic.data (= b[2..9]).
    b[6] = (uint8_t)((g->data >> 24) & 0xFF);
    b[7] = (uint8_t)((g->data >> 16) & 0xFF);
    uint16_t crc = subghz_protocol_blocks_crc16(b, 8, 0x1021, 0x0000);
    b[8] = (uint8_t)(crc >> 8);
    b[9] = (uint8_t)(crc & 0xFF);
    uint64_t data = 0;
    for(int i = 2; i < BMW_G3_GEN2_BYTES; i++) data = (data << 8) | b[i];
    g->data = data;
    g->data_count_bit = BMW_G3_GEN2_BITS;
    g->battery_low = TPMS_NO_BATT;
}

// -----------------------------------------------------------------------------
// Encoder
// -----------------------------------------------------------------------------
//
// FSK PCM (NRZ) raw half-symbols at te_short = 52us, differential-Manchester as
// consumed by bmw_g3_dm_decode: each data bit is two raw halves; the clock half
// b1 flips the previous data half (b1 = 1 - prev), the second half b2 == b1 for a
// 1 bit and b2 != b1 for a 0 bit. The 16-bit raw preamble 0xCCCD is emitted RAW
// (one half-symbol per bit, NOT DM-encoded); prev starts at 1 (the LSB of 0xCCCD)
// so the first data half never merges with the last preamble half.
//
// Worst case entries: 1 gap + up to 16 preamble + 80*2 data halves = 177; merged
// runs make it fewer. 256 leaves ample headroom.
#define TPMS_BMW_G3_ENCODER_UPLOAD_SIZE 256

void* tpms_protocol_encoder_bmw_g3_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderBmwG3* instance = malloc(sizeof(TPMSProtocolEncoderBmwG3));
    instance->base.protocol = &tpms_protocol_bmw_g3;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_BMW_G3_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_bmw_g3_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderBmwG3* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_bmw_g3_stop(void* context) {
    TPMSProtocolEncoderBmwG3* instance = context;
    instance->encoder.is_running = false;
}

// Rebuild the 10-byte Gen2 frame from generic.id + generic.data and RECOMPUTE the
// CRC so a modified / 0-PSI frame is always valid. b[0..1] from id (the decoder
// keeps them in head_64_bit, not in generic.data); b[2..9] from generic.data.
static void tpms_protocol_encoder_bmw_g3_build_frame(
    TPMSBlockGeneric* generic,
    uint8_t b[BMW_G3_GEN2_BYTES]) {
    b[0] = (uint8_t)(generic->id >> 24);
    b[1] = (uint8_t)(generic->id >> 16);
    for(int i = 0; i < 8; i++) {
        b[2 + i] = (uint8_t)((generic->data >> (8 * (7 - i))) & 0xFF);
    }
    uint16_t crc = subghz_protocol_blocks_crc16(b, 8, 0x1021, 0x0000);
    b[8] = (uint8_t)(crc >> 8);
    b[9] = (uint8_t)(crc & 0xFF);
}

static bool tpms_protocol_encoder_bmw_g3_get_upload(TPMSProtocolEncoderBmwG3* instance) {
    furi_assert(instance);
    const uint32_t te_short = tpms_protocol_bmw_g3_const.te_short;

    uint8_t frame[BMW_G3_GEN2_BYTES];
    tpms_protocol_encoder_bmw_g3_build_frame(&instance->generic, frame);

    // 80 data bits, MSB-first over b[0..9].
    uint8_t bits[BMW_G3_GEN2_BITS];
    int nbits = 0;
    for(int i = 0; i < BMW_G3_GEN2_BYTES; i++) {
        for(int j = 7; j >= 0; j--) bits[nbits++] = (frame[i] >> j) & 0x1;
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

    // Raw 16-bit preamble 0xCCCD, one raw half-symbol per bit (NOT DM-encoded).
    for(int i = 15; i >= 0; i--) RAW((BMW_G3_PREAMBLE >> i) & 0x1);

    // 80 data bits, differential-Manchester (matches bmw_g3_dm_decode). prev starts
    // at the last raw preamble bit (0xCCCD LSB = 1).
    int prev = 1;
    for(int i = 0; i < BMW_G3_GEN2_BITS; i++) {
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
    tpms_protocol_encoder_bmw_g3_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderBmwG3* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_bmw_g3_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_BMW_G3_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_bmw_g3_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_bmw_g3_yield(void* context) {
    TPMSProtocolEncoderBmwG3* instance = context;

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
