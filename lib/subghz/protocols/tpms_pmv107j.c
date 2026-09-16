#include "tpms_pmv107j.h"
#include "../blocks/generic.h"

#define TAG "TPMSPmv107j"

// Port of rtl_433/src/devices/tpms_pmv107j.c (Christian W. Zuckschwerdt 2017,
// based on work by Werner Johansson, GPL-2+).
//
// FSK 315 MHz, 8-byte Differential Manchester encoded TPMS data with CRC-8.
// Pacific PMV-107J sensors used by Toyota.
//
// Wire format (66 bits effective, realigned with 6 bits of 0-padding to 9
// bytes including CRC):
//
//   II II II I F* PP NN TT CC
//
//   I  = 28-bit ID
//   F* = 6 bit flags (battery_low, repeat_counter[2], unknown, rapid_change, failed)
//   P  = pressure raw
//   N  = inverted pressure (b[6] == ~b[5]) — used as integrity check
//   T  = temperature, °C + 40
//   C  = CRC-8 (poly 0x13, init 0x00) over bytes 0..7
//
// Preamble (decoded): 6-bit `0b111110` = 0xF8.
//
// rtl_433 uses FSK_PULSE_PCM (short=long=100us): the raw bit stream is one bit
// per 100us half-symbol. It searches the 6-bit preamble 0xF8 (top 6 bits =
// 0b111110) in that RAW stream, then runs bitbuffer_differential_manchester_decode
// from bitpos+6 with maxbits=70, requiring at least 67*2 raw bits. We mirror that:
// feed() expands each pulse into round(dur/te_short) raw half-symbol bits, and on a
// gap we search the raw buffer for the 6-bit preamble and DM-decode the 66-bit
// payload. (The old pulse-width DM + preamble_shift state machine never matched
// real frames.)

// 6-bit preamble 0b111110 = top 6 bits of rtl_433's 0xF8 pattern.
#define PMV107J_PREAMBLE 0x3EU
#define PMV107J_PREAMBLE_BITS 6
#define PMV107J_DATA_BITS 66

static const SubGhzBlockConst tpms_protocol_pmv107j_const = {
    .te_short = 100,
    .te_long = 200,
    .te_delta = 35,
    .min_count_bit_for_found = PMV107J_DATA_BITS,
};

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;

    // Raw half-symbol bits for the current burst. On a gap the buffer is searched
    // for the 6-bit 0x3E preamble and the payload is differential-Manchester
    // decoded, faithfully mirroring rtl_433 bitbuffer_differential_manchester_decode
    // (the old pulse-width DM + preamble_shift state machine never matched frames).
    uint8_t rawbuf[48]; // up to 384 raw half-symbol bits
    uint16_t rawbits;
    uint64_t head_64_bit; // upper bits when frame > 64 bit (add_to_128_bit)
} TPMSProtocolDecoderPmv107j;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderPmv107j;

const SubGhzProtocolDecoder tpms_protocol_pmv107j_decoder = {
    .alloc = tpms_protocol_decoder_pmv107j_alloc,
    .free = tpms_protocol_decoder_pmv107j_free,
    .feed = tpms_protocol_decoder_pmv107j_feed,
    .reset = tpms_protocol_decoder_pmv107j_reset,
    .get_hash_data = tpms_protocol_decoder_pmv107j_get_hash_data,
    .serialize = tpms_protocol_decoder_pmv107j_serialize,
    .deserialize = tpms_protocol_decoder_pmv107j_deserialize,
    .get_string = tpms_protocol_decoder_pmv107j_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_pmv107j_encoder = {
    .alloc = tpms_protocol_encoder_pmv107j_alloc,
    .free = tpms_protocol_encoder_pmv107j_free,
    .deserialize = tpms_protocol_encoder_pmv107j_deserialize,
    .stop = tpms_protocol_encoder_pmv107j_stop,
    .yield = tpms_protocol_encoder_pmv107j_yield,
};

const SubGhzProtocol tpms_protocol_pmv107j = {
    .name = TPMS_PROTOCOL_PMV107J_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_pmv107j_decoder,
    .encoder = &tpms_protocol_pmv107j_encoder,
};

void* tpms_protocol_decoder_pmv107j_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderPmv107j* instance = calloc(1, sizeof(TPMSProtocolDecoderPmv107j));
    instance->base.protocol = &tpms_protocol_pmv107j;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_pmv107j_free(void* context) {
    furi_assert(context);
    free(context);
}

static void pmv107j_reset_state(TPMSProtocolDecoderPmv107j* instance) {
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->head_64_bit = 0;
    instance->rawbits = 0;
}

void tpms_protocol_decoder_pmv107j_reset(void* context) {
    furi_assert(context);
    pmv107j_reset_state(context);
}

// Raw half-symbol bit access (MSB-first per byte), matching rtl_433 bit_at.
static inline uint8_t pmv107j_bit_at(const uint8_t* buf, unsigned i) {
    return (uint8_t)((buf[i >> 3] >> (7 - (i & 7))) & 1);
}

// Faithful port of rtl_433 bitbuffer_differential_manchester_decode: decode up to
// `maxbits` data bits starting at raw-bit `start` into out[] (one byte per bit),
// returning the count. bit1 is the clock half (must differ from the previous data
// half); the data bit is (bit1==bit2)?1:0.
static int pmv107j_dm_decode(
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
        uint8_t b1 = pmv107j_bit_at(buf, ipos++);
        uint8_t b2 = pmv107j_bit_at(buf, ipos++);
        uint8_t b3 = (ipos < nbits) ? pmv107j_bit_at(buf, ipos) : 0;
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
        uint8_t b1 = pmv107j_bit_at(buf, ipos++);
        if(b1 == bit2) break; // clock missing
        uint8_t b2 = pmv107j_bit_at(buf, ipos++);
        out[n++] = (b1 == b2) ? 1 : 0;
        bit2 = b2;
    }
    return n;
}

static void pmv107j_get_bytes(TPMSProtocolDecoderPmv107j* instance, uint8_t b[9]) {
    // 66 bits collected: head lower 2 = stream bits 0..1 → b[0] lower 2 bits;
    // decode_data 64 bits = stream bits 2..65 → b[1..8].
    b[0] = (uint8_t)(instance->head_64_bit & 0x03);
    for(int i = 0; i < 8; i++) {
        b[i + 1] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

static bool pmv107j_check(TPMSProtocolDecoderPmv107j* instance) {
    uint8_t b[9];
    pmv107j_get_bytes(instance, b);
    if(subghz_protocol_blocks_crc8(b, 8, 0x13, 0x00) != b[8]) return false;
    // Pressure plausibility: b[6] should be inverted b[5].
    if((b[5] ^ 0xFF) != b[6]) return false;
    return true;
}

static void pmv107j_analyze(TPMSProtocolDecoderPmv107j* instance) {
    uint8_t b[9];
    pmv107j_get_bytes(instance, b);
    // 28-bit ID = b[0]<<26 | b[1]<<18 | b[2]<<10 | b[3]<<2 | b[4]>>6
    uint32_t id = ((uint32_t)b[0] << 26) | ((uint32_t)b[1] << 18) | ((uint32_t)b[2] << 10) |
                  ((uint32_t)b[3] << 2) | (b[4] >> 6);
    instance->generic.id = id;
    instance->generic.battery_low = (b[4] & 0x20) >> 5;
    // pressure_kpa = (pressure_raw - 40) * 2.48
    float kpa = ((float)b[5] - 40.0f) * 2.48f;
    instance->generic.pressure = kpa * 0.01f;
    instance->generic.temperature = (float)((int)b[7] - 40);
}

void tpms_protocol_pmv107j_pack(TPMSBlockGeneric* g) {
    furi_assert(g);
    uint32_t id = g->id & 0x0FFFFFFF; // 28-bit ID
    // bar = (raw-40)*2.48*0.01 -> raw = bar*100/2.48 + 40
    int p_raw = (int)(g->pressure * 100.0f / 2.48f + 40.0f + 0.5f);
    if(p_raw < 0) p_raw = 0;
    if(p_raw > 0xFF) p_raw = 0xFF;
    int t = (int)(g->temperature + (g->temperature >= 0 ? 0.5f : -0.5f)) + 40; // b[7] = C + 40
    if(t < 0) t = 0;
    if(t > 0xFF) t = 0xFF;
    uint8_t batt = g->battery_low ? 1 : 0;
    uint8_t b[9];
    b[0] = (uint8_t)((id >> 26) & 0x03);
    b[1] = (uint8_t)((id >> 18) & 0xFF);
    b[2] = (uint8_t)((id >> 10) & 0xFF);
    b[3] = (uint8_t)((id >> 2) & 0xFF);
    b[4] = (uint8_t)(((id & 0x03) << 6) | ((batt & 0x01) << 5));
    b[5] = (uint8_t)p_raw;
    b[6] = (uint8_t)(p_raw ^ 0xFF); // integrity: decoder requires b[6] == ~b[5]
    b[7] = (uint8_t)t;
    b[8] = subghz_protocol_blocks_crc8(b, 8, 0x13, 0x00);
    // generic.data holds b[1..8]; the encoder recovers the 2-bit b[0] from the CRC.
    uint64_t data = 0;
    for(int i = 1; i <= 8; i++) data = (data << 8) | b[i];
    g->data = data;
    g->data_count_bit = 66;
}

// Search the accumulated raw buffer for the 6-bit preamble 0x3E and, from each
// hit, differential-Manchester-decode the 66-bit payload (from bitpos+6) and
// accept on CRC + the pressure/inverted-pressure integrity check. Mirrors
// rtl_433 tpms_pmv107j_callback (bitbuffer_search 6, decode from bitpos+6).
static void pmv107j_process(TPMSProtocolDecoderPmv107j* instance) {
    unsigned nbits = instance->rawbits;
    if(nbits < PMV107J_PREAMBLE_BITS + PMV107J_DATA_BITS * 2u) return;
    for(unsigned pos = 0; pos + PMV107J_PREAMBLE_BITS <= nbits; pos++) {
        uint16_t v = 0;
        for(int k = 0; k < PMV107J_PREAMBLE_BITS; k++)
            v = (uint16_t)((v << 1) | pmv107j_bit_at(instance->rawbuf, pos + k));
        if(v != PMV107J_PREAMBLE) continue;

        // Decode up to 80 bits (the sync loop can emit leading data bits before
        // the clock locks, per rtl_433), then require the full 66-bit payload.
        uint8_t bits[80];
        int n = pmv107j_dm_decode(
            instance->rawbuf, nbits, pos + PMV107J_PREAMBLE_BITS, bits, 80);
        if(n < (int)PMV107J_DATA_BITS) continue;

        instance->decoder.decode_data = 0;
        instance->head_64_bit = 0;
        instance->decoder.decode_count_bit = 0;
        for(int i = 0; i < (int)PMV107J_DATA_BITS; i++) {
            subghz_protocol_blocks_add_to_128_bit(
                &instance->decoder, bits[i], &instance->head_64_bit);
        }
        if(pmv107j_check(instance)) {
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            pmv107j_analyze(instance);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
            return; // one good frame per burst is enough
        }
    }
}

void tpms_protocol_decoder_pmv107j_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderPmv107j* instance = context;

    const uint32_t te = tpms_protocol_pmv107j_const.te_short; // 100us half-symbol
    // A gap ends the burst. The PMV preamble 0b111110 is a raw 5-symbol run
    // (~500us), so the gap threshold must sit ABOVE it (7*te) and the per-pulse
    // expansion must allow up to 6 half-symbols (DM data never exceeds 2, but the
    // preamble run does — a 4-cap truncates it and a 5*te gap resets on it).
    if(duration >= te * 7) {
        pmv107j_process(instance);
        pmv107j_reset_state(instance);
        return;
    }
    // Expand this pulse into 1..6 raw half-symbol bits of the current level.
    uint32_t nb = (duration + te / 2) / te;
    if(nb < 1) nb = 1;
    if(nb > 6) nb = 6;
    for(uint32_t k = 0; k < nb; k++) {
        if(instance->rawbits >= (uint16_t)(sizeof(instance->rawbuf) * 8)) {
            pmv107j_process(instance);
            pmv107j_reset_state(instance);
        }
        unsigned idx = instance->rawbits;
        if(level)
            instance->rawbuf[idx >> 3] |= (uint8_t)(0x80u >> (idx & 7u));
        else
            instance->rawbuf[idx >> 3] &= (uint8_t)~(0x80u >> (idx & 7u));
        instance->rawbits++;
    }
}

uint8_t tpms_protocol_decoder_pmv107j_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderPmv107j* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_pmv107j_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderPmv107j* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_pmv107j_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderPmv107j* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, tpms_protocol_pmv107j_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_pmv107j_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderPmv107j* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX\r\n"
        "Bat:%s\r\n"
        "Temp:%2.0f C Bar:%2.1f",
        instance->generic.protocol_name,
        instance->generic.id,
        instance->generic.battery_low ? "low" : "ok",
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}

// -----------------------------------------------------------------------------
// Encoder
// -----------------------------------------------------------------------------
//
// FSK 315 MHz Differential-Manchester as consumed by the decoder above (which
// ignores level and looks only at pulse durations):
//   bit 1 -> one te_long pulse
//   bit 0 -> two te_short pulses (mid-bit transition)
// Levels strictly alternate pulse-to-pulse (no run-length merge).
//
// Frame on the wire: inter-frame gap (reset) + lead-in of '1' bits and a
// terminating '0' forming the 6-bit preamble 0b111110 (0x3E) + 66 data bits.
//
// The 66-bit frame is b[0](2 bits) + b[1..8]; generic.data holds b[1..8]
// (including the CRC b[8]). The 2-bit b[0] (top of the 28-bit ID) is recovered
// by brute force from the CRC-8 (poly 0x13, init 0x00 over b[0..7] == b[8]) so
// both the CRC check and the ID reproduce exactly.
//
// Worst-case entries: 1 gap + 8 lead-in + 2 ('0') + 66*2 (all-zero data) = 143.
#define TPMS_PMV107J_ENCODER_UPLOAD_SIZE 200
#define TPMS_PMV107J_PREAMBLE_ONES       8

void* tpms_protocol_encoder_pmv107j_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderPmv107j* instance = malloc(sizeof(TPMSProtocolEncoderPmv107j));
    instance->base.protocol = &tpms_protocol_pmv107j;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_PMV107J_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_pmv107j_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderPmv107j* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_pmv107j_stop(void* context) {
    TPMSProtocolEncoderPmv107j* instance = context;
    instance->encoder.is_running = false;
}

// Rebuild b[0..8]. b[1..8] from generic.data; b[0] (low 2 bits) recovered from CRC.
static void tpms_protocol_encoder_pmv107j_build_frame(TPMSBlockGeneric* generic, uint8_t b[9]) {
    for(int i = 0; i < 8; i++) {
        b[i + 1] = (generic->data >> (8 * (7 - i))) & 0xFF;
    }
    b[0] = 0x00;
    for(int cand = 0; cand < 4; cand++) {
        uint8_t msg[8];
        msg[0] = (uint8_t)cand;
        for(int i = 1; i < 8; i++) msg[i] = b[i];
        if(subghz_protocol_blocks_crc8(msg, 8, 0x13, 0x00) == b[8]) {
            b[0] = (uint8_t)cand;
            break;
        }
    }
}

static bool tpms_protocol_encoder_pmv107j_get_upload(TPMSProtocolEncoderPmv107j* instance) {
    furi_assert(instance);
    const uint32_t te_short = tpms_protocol_pmv107j_const.te_short;
    const uint32_t te_long = tpms_protocol_pmv107j_const.te_long;

    uint8_t frame[9];
    tpms_protocol_encoder_pmv107j_build_frame(&instance->generic, frame);

    // Build the 66-bit stream: b[0] bits 1,0 then b[1..8] MSB-first.
    uint8_t bits[PMV107J_DATA_BITS];
    int n = 0;
    bits[n++] = (frame[0] >> 1) & 0x1;
    bits[n++] = (frame[0] >> 0) & 0x1;
    for(int i = 1; i <= 8; i++) {
        for(int j = 7; j >= 0; j--) bits[n++] = (frame[i] >> j) & 0x1;
    }

    (void)te_long;
    size_t index = 0;
    const size_t max = instance->encoder.size_upload;
    bool last_level = false;
    uint32_t last_duration = 0;

    // Run-length-merged emit (matches the raw-bit model the fixed decoder expands
    // via round(dur/te_short)): each RAW() is ONE raw half-symbol of te_short.
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

    // Inter-frame gap (>= te_short*7 reset threshold) so each frame starts clean.
    EMIT(false, te_short * 40);

    // Raw preamble 0x3E = 0b111110 (five high half-symbols + one low). The decoder
    // hunts this RAW 6-bit pattern; it must NOT be differential-Manchester encoded,
    // and the 5-run must stay within the feed's per-pulse cap (so exactly five ones).
    RAW(1);
    RAW(1);
    RAW(1);
    RAW(1);
    RAW(1);
    RAW(0);

    // 66 data bits, differential-Manchester encoded to match pmv107j_dm_decode:
    // clock half b1 flips the previous data half; second half b2 == b1 for a '1',
    // != b1 for a '0'. prev starts at 0 (the last preamble bit).
    int prev = 0;
    for(int i = 0; i < PMV107J_DATA_BITS; i++) {
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
    tpms_protocol_encoder_pmv107j_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderPmv107j* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_pmv107j_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_PMV107J_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_pmv107j_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_pmv107j_yield(void* context) {
    TPMSProtocolEncoderPmv107j* instance = context;

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
