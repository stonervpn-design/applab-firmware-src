#include "tpms_porsche.h"
#include "../blocks/generic.h"

#define TAG "TPMSPorsche"

// Port of rtl_433/src/devices/tpms_porsche.c
//   (Porsche Boxster/Cayman TPMS, Christian W. Zuckschwerdt 2021, GPL-2+).
//
// RF: FSK 433.92 MHz, Differential-Manchester Coded (DMC), 10-byte (80-bit)
// payload. Seen on Porsche 2nd-gen (Typ 987) Boxster/Cayman.
//
// On air: ~30-bit alternating preamble then 80 Differential-Manchester encoded
// data bits. rtl_433 slices the FSK demod into 52 us half-bit samples, searches
// the 20-bit preamble pattern {0x33,0x33,0x20} = 0x33332, and then runs
// bitbuffer_differential_manchester_decode() from bitpos+20 for up to 80 bits.
//
// This firmware receives (level,duration) edges rather than a PCM bitstream, so
// feed() reconstructs the 52 us half-bit stream by expanding each pulse into
// n = round(duration / te_short) same-level raw half-symbol bits (a short = 1
// half-bit, a full-bit long = 2). On a gap the accumulated raw buffer is
// searched for the 20-bit preamble (either receiver polarity) and the payload is
// differential-Manchester decoded with a faithful port of rtl_433's
// bitbuffer_differential_manchester_decode (the old pulse-width DM + rolling
// framing anchor never matched real frames).
//
// Wire format (after DMC decode, 10 bytes b[0..9]):
//   II II II II PP TT SS SS CC CC
//   b[0..3] = 32-bit ID, big-endian
//   b[4]    = pressure raw     -> kPa = raw * 5 / 2 - 100
//   b[5]    = temperature raw  -> C   = raw - 40
//   b[6..7] = status flags
//   b[8..9] = CRC-16 CCITT-FALSE (poly 0x1021, init 0xFFFF) over b[0..9] == 0
//
// The CRC-16 covers its own two bytes, so a correct frame yields crc16()==0.
// This strong 16-bit MIC (enforced before the callback) is the false-positive
// gate; no additional plausibility bound is required.

#define PORSCHE_DATA_BITS 80

#define PORSCHE_PREAMBLE20      0x33332UL
#define PORSCHE_PREAMBLE20_INV  0xCCCCDUL // 20-bit inverse (opposite receiver polarity)
#define PORSCHE_PREAMBLE20_MASK 0xFFFFFUL

static const SubGhzBlockConst tpms_protocol_porsche_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = PORSCHE_DATA_BITS,
};

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;

    // Raw half-symbol bits for the current burst. On a gap the buffer is searched
    // for the 20-bit preamble and the 80-bit payload is differential-Manchester
    // decoded, faithfully mirroring rtl_433 bitbuffer_differential_manchester_decode
    // (the old pulse-width DM + rolling framing anchor never matched real frames).
    uint8_t rawbuf[56]; // up to 448 raw half-symbol bits
    uint16_t rawbits;
    uint64_t head_64_bit; // upper 16 bits of the 80-bit frame (add_to_128_bit)
} TPMSProtocolDecoderPorsche;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderPorsche;

// Encoder ops (definitions at the bottom of this file).
void* tpms_protocol_encoder_porsche_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_porsche_free(void* context);
void tpms_protocol_encoder_porsche_stop(void* context);
SubGhzProtocolStatus
    tpms_protocol_encoder_porsche_deserialize(void* context, FlipperFormat* flipper_format);
LevelDuration tpms_protocol_encoder_porsche_yield(void* context);

const SubGhzProtocolDecoder tpms_protocol_porsche_decoder = {
    .alloc = tpms_protocol_decoder_porsche_alloc,
    .free = tpms_protocol_decoder_porsche_free,
    .feed = tpms_protocol_decoder_porsche_feed,
    .reset = tpms_protocol_decoder_porsche_reset,
    .get_hash_data = tpms_protocol_decoder_porsche_get_hash_data,
    .serialize = tpms_protocol_decoder_porsche_serialize,
    .deserialize = tpms_protocol_decoder_porsche_deserialize,
    .get_string = tpms_protocol_decoder_porsche_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_porsche_encoder = {
    .alloc = tpms_protocol_encoder_porsche_alloc,
    .free = tpms_protocol_encoder_porsche_free,
    .deserialize = tpms_protocol_encoder_porsche_deserialize,
    .stop = tpms_protocol_encoder_porsche_stop,
    .yield = tpms_protocol_encoder_porsche_yield,
};

const SubGhzProtocol tpms_protocol_porsche = {
    .name = TPMS_PROTOCOL_PORSCHE_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_porsche_decoder,
    .encoder = &tpms_protocol_porsche_encoder,
};

void* tpms_protocol_decoder_porsche_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderPorsche* instance = calloc(1, sizeof(TPMSProtocolDecoderPorsche));
    instance->base.protocol = &tpms_protocol_porsche;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_porsche_free(void* context) {
    furi_assert(context);
    free(context);
}

static void porsche_reset_state(TPMSProtocolDecoderPorsche* instance) {
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->head_64_bit = 0;
    instance->rawbits = 0;
}

void tpms_protocol_decoder_porsche_reset(void* context) {
    furi_assert(context);
    porsche_reset_state(context);
}

// Raw half-symbol bit access (MSB-first per byte), matching rtl_433 bit_at.
static inline uint8_t porsche_bit_at(const uint8_t* buf, unsigned i) {
    return (uint8_t)((buf[i >> 3] >> (7 - (i & 7))) & 1);
}

// Faithful port of rtl_433 bitbuffer_differential_manchester_decode: decode up to
// `maxbits` data bits starting at raw-bit `start` into out[] (one byte per bit),
// returning the count. bit1 is the clock half (must differ from the previous data
// half); the data bit is (bit1==bit2)?1:0.
static int porsche_dm_decode(
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
        uint8_t b1 = porsche_bit_at(buf, ipos++);
        uint8_t b2 = porsche_bit_at(buf, ipos++);
        uint8_t b3 = (ipos < nbits) ? porsche_bit_at(buf, ipos) : 0;
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
        uint8_t b1 = porsche_bit_at(buf, ipos++);
        if(b1 == bit2) break; // clock missing
        uint8_t b2 = porsche_bit_at(buf, ipos++);
        out[n++] = (b1 == b2) ? 1 : 0;
        bit2 = b2;
    }
    return n;
}

static void porsche_get_bytes(TPMSProtocolDecoderPorsche* instance, uint8_t b[10]) {
    // 80 bits: add_to_128_bit keeps the upper 16 bits in head_64_bit (= b[0..1])
    // and the low 64 bits in decode_data (= b[2..9]).
    b[0] = (uint8_t)((instance->head_64_bit >> 8) & 0xFF);
    b[1] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 2] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

static bool porsche_check(TPMSProtocolDecoderPorsche* instance) {
    uint8_t b[10];
    porsche_get_bytes(instance, b);
    // CRC-16 CCITT-FALSE over all 10 bytes (incl. the 2 CRC bytes) must be 0.
    return subghz_protocol_blocks_crc16(b, 10, 0x1021, 0xFFFF) == 0;
}

static void porsche_analyze(TPMSProtocolDecoderPorsche* instance) {
    uint8_t b[10];
    porsche_get_bytes(instance, b);
    instance->generic.id =
        ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    int pressure_kpa = (int)b[4] * 5 / 2 - 100;
    instance->generic.pressure = (float)pressure_kpa; // kPa
    instance->generic.temperature = (float)((int)b[5] - 40);
    instance->generic.battery_low = TPMS_NO_BATT;
}

// Search the accumulated raw buffer for the 20-bit preamble 0x33332 (either
// receiver polarity) and, from each hit, differential-Manchester-decode the
// 80-bit payload (from bitpos+20) and accept on CRC-16 == 0. Mirrors rtl_433
// tpms_porsche_callback.
static void porsche_process(TPMSProtocolDecoderPorsche* instance) {
    unsigned nbits = instance->rawbits;
    if(nbits < 20u + PORSCHE_DATA_BITS * 2u) return;
    for(unsigned pos = 0; pos + 20u <= nbits; pos++) {
        uint32_t v = 0;
        for(int k = 0; k < 20; k++)
            v = (uint32_t)((v << 1) | porsche_bit_at(instance->rawbuf, pos + k));
        v &= PORSCHE_PREAMBLE20_MASK;
        if(v != PORSCHE_PREAMBLE20 && v != PORSCHE_PREAMBLE20_INV) continue;

        // Decode up to 80 bits (the sync loop can emit leading data bits before
        // the clock locks, per rtl_433), then require the full 80-bit payload.
        uint8_t bits[PORSCHE_DATA_BITS];
        int n = porsche_dm_decode(
            instance->rawbuf, nbits, pos + 20, bits, PORSCHE_DATA_BITS);
        if(n < (int)PORSCHE_DATA_BITS) continue;

        instance->decoder.decode_data = 0;
        instance->head_64_bit = 0;
        instance->decoder.decode_count_bit = 0;
        for(int i = 0; i < (int)PORSCHE_DATA_BITS; i++) {
            subghz_protocol_blocks_add_to_128_bit(
                &instance->decoder, bits[i], &instance->head_64_bit);
        }
        if(porsche_check(instance)) {
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            porsche_analyze(instance);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
            return; // one good frame per burst is enough
        }
    }
}

void tpms_protocol_decoder_porsche_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderPorsche* instance = context;

    const uint32_t te = tpms_protocol_porsche_const.te_short; // 52us half-symbol
    // A gap (or an over-long pulse) ends the burst: decode what we have, reset.
    if(duration >= te * 5) {
        porsche_process(instance);
        porsche_reset_state(instance);
        return;
    }
    // Expand this pulse into 1..4 raw half-symbol bits of the current level (a
    // full-bit DM interval is two identical half-symbols).
    uint32_t nb = (duration + te / 2) / te;
    if(nb < 1) nb = 1;
    if(nb > 4) nb = 4;
    for(uint32_t k = 0; k < nb; k++) {
        if(instance->rawbits >= (uint16_t)(sizeof(instance->rawbuf) * 8)) {
            porsche_process(instance);
            porsche_reset_state(instance);
        }
        unsigned idx = instance->rawbits;
        if(level)
            instance->rawbuf[idx >> 3] |= (uint8_t)(0x80u >> (idx & 7u));
        else
            instance->rawbuf[idx >> 3] &= (uint8_t)~(0x80u >> (idx & 7u));
        instance->rawbits++;
    }
}

uint8_t tpms_protocol_decoder_porsche_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderPorsche* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_porsche_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderPorsche* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_porsche_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderPorsche* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, tpms_protocol_porsche_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_porsche_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderPorsche* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX\r\n"
        "Temp:%2.0f C  %3.0f kPa",
        instance->generic.protocol_name,
        instance->generic.id,
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}

// -----------------------------------------------------------------------------
// 0-PSI packer + Encoder
// -----------------------------------------------------------------------------
//
// Wire frame (10 bytes, see the decoder header):
//   II II II II PP TT SS SS CC CC
//   b[0..3] = 32-bit ID (big-endian)   -> generic.id
//   b[4]    = pressure raw  (kPa = raw*5/2 - 100)
//   b[5]    = temperature raw (C = raw - 40)
//   b[6..7] = status flags (preserved from the captured frame)
//   b[8..9] = CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over b[0..7],
//             stored big-endian so crc16(b[0..9]) == 0.
//
// generic.data holds the low 64 bits b[2..9]; generic.id holds b[0..3]. The
// decoder puts pressure into generic.pressure in kPa, but the whole TPMS edit
// UI (and this harness) express pressure in bar - so pack() reads g->pressure
// as bar and maps it to the raw byte: raw = round(bar*40 + 40)
// (= ((bar*100 kPa) + 100) * 2 / 5). Decode then reports bar*100 kPa, i.e. the
// same bar reading back. 0 bar -> raw 0x28 -> 0 kPa (true 0-PSI).

// Rebuild all 10 wire bytes from generic.id + generic.data and RECOMPUTE the CRC
// so any edited/0-PSI frame is always valid. b[0..3] come from the ID, b[4..9]
// from generic.data (the decoder's b[2..9] window); the CRC bytes b[8..9] are
// overwritten with the freshly computed value.
static void tpms_protocol_encoder_porsche_build_frame(TPMSBlockGeneric* generic, uint8_t b[10]) {
    uint32_t id = generic->id;
    b[0] = (uint8_t)((id >> 24) & 0xFF);
    b[1] = (uint8_t)((id >> 16) & 0xFF);
    b[2] = (uint8_t)((id >> 8) & 0xFF);
    b[3] = (uint8_t)(id & 0xFF);
    // generic.data = b[2..9] (b[2] is the MSB). Take b[4..9] from its low 48 bits.
    uint64_t d = generic->data;
    b[4] = (uint8_t)((d >> 40) & 0xFF);
    b[5] = (uint8_t)((d >> 32) & 0xFF);
    b[6] = (uint8_t)((d >> 24) & 0xFF);
    b[7] = (uint8_t)((d >> 16) & 0xFF);
    // Recompute CRC-16/CCITT-FALSE over b[0..7], appended big-endian.
    uint16_t crc = subghz_protocol_blocks_crc16(b, 8, 0x1021, 0xFFFF);
    b[8] = (uint8_t)((crc >> 8) & 0xFF);
    b[9] = (uint8_t)(crc & 0xFF);
}

void tpms_protocol_porsche_pack(TPMSBlockGeneric* g) {
    furi_assert(g);
    // Preserve captured status bytes b[6..7] from the loaded frame (generic.data
    // = b[2..9], so b[6]=bits 24..31, b[7]=bits 16..23).
    uint64_t d0 = g->data;
    uint8_t st6 = (uint8_t)((d0 >> 24) & 0xFF);
    uint8_t st7 = (uint8_t)((d0 >> 16) & 0xFF);

    // Pressure: g->pressure is bar. raw = round(bar*40 + 40) inverts the decoder's
    // kPa = raw*5/2 - 100 with a bar->kPa (x100) scale. 0 bar -> 0x28 -> 0 kPa.
    int p_raw = (int)(g->pressure * 40.0f + 40.0f + 0.5f);
    if(p_raw < 0) p_raw = 0;
    if(p_raw > 0xFF) p_raw = 0xFF;
    // Temperature: b[5] = C + 40.
    int t = (int)(g->temperature + (g->temperature >= 0 ? 0.5f : -0.5f)) + 40;
    if(t < 0) t = 0;
    if(t > 0xFF) t = 0xFF;

    uint8_t b[10];
    uint32_t id = g->id;
    b[0] = (uint8_t)((id >> 24) & 0xFF);
    b[1] = (uint8_t)((id >> 16) & 0xFF);
    b[2] = (uint8_t)((id >> 8) & 0xFF);
    b[3] = (uint8_t)(id & 0xFF);
    b[4] = (uint8_t)p_raw;
    b[5] = (uint8_t)t;
    b[6] = st6;
    b[7] = st7;
    uint16_t crc = subghz_protocol_blocks_crc16(b, 8, 0x1021, 0xFFFF);
    b[8] = (uint8_t)((crc >> 8) & 0xFF);
    b[9] = (uint8_t)(crc & 0xFF);

    // Store b[2..9] back into generic.data (64 bits) and set the bit count.
    uint64_t data = 0;
    for(int i = 2; i < 10; i++) data = (data << 8) | b[i];
    g->data = data;
    g->data_count_bit = PORSCHE_DATA_BITS;
}

// Worst-case upload: 1 gap + preamble/payload emitted one raw half-symbol per
// entry (no run merge crossing the cap). 20 preamble + 80*2 payload = 180 raw
// half-symbols; +1 gap, with head-room.
#define TPMS_PORSCHE_ENCODER_UPLOAD_SIZE 256

void* tpms_protocol_encoder_porsche_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderPorsche* instance = malloc(sizeof(TPMSProtocolEncoderPorsche));
    instance->base.protocol = &tpms_protocol_porsche;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_PORSCHE_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_porsche_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderPorsche* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_porsche_stop(void* context) {
    TPMSProtocolEncoderPorsche* instance = context;
    instance->encoder.is_running = false;
}

static bool tpms_protocol_encoder_porsche_get_upload(TPMSProtocolEncoderPorsche* instance) {
    furi_assert(instance);
    const uint32_t te_short = tpms_protocol_porsche_const.te_short;

    uint8_t frame[10];
    tpms_protocol_encoder_porsche_build_frame(&instance->generic, frame);

    // 80 payload data bits: b[0..9] MSB-first (matches the decoder's
    // add_to_128_bit fill: first bit = MSB of b[0]).
    uint8_t bits[PORSCHE_DATA_BITS];
    int n = 0;
    for(int i = 0; i < 10; i++) {
        for(int j = 7; j >= 0; j--) bits[n++] = (frame[i] >> j) & 0x1;
    }

    size_t index = 0;
    const size_t max = instance->encoder.size_upload;
    bool last_level = false;
    uint32_t last_duration = 0;

    // Run-length-merged emit: each RAW() is ONE raw half-symbol of te_short. The
    // decoder rebuilds the half-symbol count via round(dur/te_short) (cap 4), and
    // no run here exceeds 2 half-symbols (DM: a '1' is the only 2-run; preamble
    // runs are <=2), so a merged run is at most 2*te and never truncates.
#define EMIT(lvl, dur)                                                            \
    do {                                                                          \
        if(last_duration != 0 && last_level == (lvl)) {                           \
            last_duration += (dur);                                               \
        } else {                                                                  \
            if(last_duration != 0) {                                              \
                if(index >= max) return false;                                    \
                instance->encoder.upload[index++] =                               \
                    level_duration_make(last_level, last_duration);               \
            }                                                                     \
            last_level = (lvl);                                                   \
            last_duration = (dur);                                                \
        }                                                                         \
    } while(0)
#define RAW(bit) EMIT((bit) ? true : false, te_short)

    // Inter-frame gap (>= te_short*5 reset threshold). Level = high so it does NOT
    // merge with the first preamble half-symbol (which is low), keeping the 20-bit
    // preamble intact for the decoder's raw search.
    EMIT(true, te_short * 40);

    // Raw 20-bit preamble 0x33332 (MSB-first), emitted as literal half-symbols
    // (NOT differential-Manchester); the decoder hunts this pattern in the raw
    // half-symbol stream.
    for(int k = 19; k >= 0; k--) {
        RAW((PORSCHE_PREAMBLE20 >> k) & 0x1);
    }

    // 80 data bits, differential-Manchester encoded to match porsche_dm_decode:
    // clock half b1 flips the previous data half; second half b2 == b1 for a '1',
    // != b1 for a '0'. prev starts at 0 (the last preamble half-symbol is 0).
    int prev = 0;
    for(int i = 0; i < PORSCHE_DATA_BITS; i++) {
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
    tpms_protocol_encoder_porsche_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderPorsche* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_porsche_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_PORSCHE_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_porsche_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_porsche_yield(void* context) {
    TPMSProtocolEncoderPorsche* instance = context;

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
