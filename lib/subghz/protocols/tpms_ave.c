#include "tpms_ave.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#define TAG "TPMSAve"

// Port of rtl_433/src/devices/tpms_ave.c (Christian W. Zuckschwerdt, GPL-2+).
//
// FSK 433.92 MHz, AVE TPMS. rtl_433 captures with FSK_PULSE_PCM (short=long=100us)
// so the demodulator emits one raw bit per 100us half-symbol; the payload is then
// software differential-Manchester decoded (bitbuffer_differential_manchester_decode).
//
// Wire format:
//   Raw preamble (NOT Manchester): 0xcc 0xcc 0xcc 0xcd = 32 raw bits
//     (which differential-Manchester decodes to 0xff 0xfe).
//   160 raw half-symbols follow -> up to 80 DM data bits; a full frame is 8 bytes:
//
//     II II II II PP TT MF CC
//
//     I  = 32-bit sensor ID           (b[0..3])
//     P  = pressure raw               (b[4])
//     T  = temperature raw, degC + 50 (b[5])
//     MF = mode(2) | battery(3) | flags(3)  (b[6])
//     C  = CRC-8, poly 0x31 init 0xff over bytes 0..7 == 0  (b[7])
//
//   Pressure (kPa) = (P - offset) * ratio, mode-dependent:
//     mode 0: ratio 2.352 offset 47 ; mode 1: ratio 2.352 offset 0
//     mode 2: ratio 5.491 offset 18.2; mode 3: ratio 5.491 offset 0
//   Temperature (degC) = T - 50. Battery raw 7 = low, 6 = not full, else full.
//
// This mirrors rtl_433 exactly: feed() expands each pulse into round(dur/te) raw
// half-symbol bits; on a gap the raw buffer is searched for the 32-bit preamble
// and the payload is differential-Manchester decoded from bitpos+32.

#define AVE_PREAMBLE_RAW  0xCCCCCCCDUL // raw (pre-Manchester) 32-bit sync
#define AVE_PREAMBLE_BITS 32
#define AVE_DATA_BITS     64 // 8 bytes

static const SubGhzBlockConst tpms_protocol_tpms_ave_const = {
    .te_short = 100,
    .te_long = 200,
    .te_delta = 35,
    .min_count_bit_for_found = AVE_DATA_BITS,
};

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;

    // Raw half-symbol bits for the current burst. On a gap the buffer is searched
    // for the 32-bit 0xCCCCCCCD preamble and the payload is differential-Manchester
    // decoded, faithfully mirroring rtl_433 bitbuffer_differential_manchester_decode.
    uint8_t rawbuf[32]; // up to 256 raw half-symbol bits (frame = 160)
    uint16_t rawbits;
} TPMSProtocolDecoderTpmsAve;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderTpmsAve;

const SubGhzProtocolDecoder tpms_protocol_tpms_ave_decoder = {
    .alloc = tpms_protocol_decoder_tpms_ave_alloc,
    .free = tpms_protocol_decoder_tpms_ave_free,
    .feed = tpms_protocol_decoder_tpms_ave_feed,
    .reset = tpms_protocol_decoder_tpms_ave_reset,
    .get_hash_data = tpms_protocol_decoder_tpms_ave_get_hash_data,
    .serialize = tpms_protocol_decoder_tpms_ave_serialize,
    .deserialize = tpms_protocol_decoder_tpms_ave_deserialize,
    .get_string = tpms_protocol_decoder_tpms_ave_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_tpms_ave_encoder = {
    .alloc = tpms_protocol_encoder_tpms_ave_alloc,
    .free = tpms_protocol_encoder_tpms_ave_free,
    .deserialize = tpms_protocol_encoder_tpms_ave_deserialize,
    .stop = tpms_protocol_encoder_tpms_ave_stop,
    .yield = tpms_protocol_encoder_tpms_ave_yield,
};

const SubGhzProtocol tpms_protocol_tpms_ave = {
    .name = TPMS_PROTOCOL_AVE_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_tpms_ave_decoder,
    .encoder = &tpms_protocol_tpms_ave_encoder,
};

void* tpms_protocol_decoder_tpms_ave_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderTpmsAve* instance = calloc(1, sizeof(TPMSProtocolDecoderTpmsAve));
    instance->base.protocol = &tpms_protocol_tpms_ave;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_tpms_ave_free(void* context) {
    furi_assert(context);
    free(context);
}

static void ave_reset_state(TPMSProtocolDecoderTpmsAve* instance) {
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->rawbits = 0;
}

void tpms_protocol_decoder_tpms_ave_reset(void* context) {
    furi_assert(context);
    ave_reset_state(context);
}

// Raw half-symbol bit access (MSB-first per byte), matching rtl_433 bit_at.
static inline uint8_t ave_bit_at(const uint8_t* buf, unsigned i) {
    return (uint8_t)((buf[i >> 3] >> (7 - (i & 7))) & 1);
}

// Faithful port of rtl_433 bitbuffer_differential_manchester_decode: decode up to
// `maxbits` data bits starting at raw-bit `start` into out[] (one byte per bit),
// returning the count. bit1 is the clock half (must differ from the previous data
// half); the data bit is (bit1==bit2)?1:0.
static int
    ave_dm_decode(const uint8_t* buf, unsigned nbits, unsigned start, uint8_t* out, int maxbits) {
    unsigned ipos = start, len = nbits;
    if(maxbits && len > start + (unsigned)maxbits * 2) len = start + (unsigned)maxbits * 2;
    uint8_t bit2 = 0;
    int n = 0;
    // sync: establish the clock (may emit leading data bits, per rtl_433)
    while(ipos + 1 < len) {
        uint8_t b1 = ave_bit_at(buf, ipos++);
        uint8_t b2 = ave_bit_at(buf, ipos++);
        uint8_t b3 = (ipos < nbits) ? ave_bit_at(buf, ipos) : 0;
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
        uint8_t b1 = ave_bit_at(buf, ipos++);
        if(b1 == bit2) break; // clock missing
        uint8_t b2 = ave_bit_at(buf, ipos++);
        out[n++] = (b1 == b2) ? 1 : 0;
        bit2 = b2;
    }
    return n;
}

static void ave_get_bytes(TPMSProtocolDecoderTpmsAve* instance, uint8_t b[8]) {
    for(int i = 0; i < 8; i++) {
        b[i] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

static bool ave_check(TPMSProtocolDecoderTpmsAve* instance) {
    uint8_t b[8];
    ave_get_bytes(instance, b);
    // CRC-8 poly 0x31 init 0xff over all 8 bytes must be 0.
    return subghz_protocol_blocks_crc8(b, 8, 0x31, 0xff) == 0;
}

static void ave_analyze(TPMSProtocolDecoderTpmsAve* instance) {
    uint8_t b[8];
    ave_get_bytes(instance, b);

    instance->generic.id =
        ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];

    int pressure_raw = b[4];
    int mode = (b[6] >> 6) & 0x3;
    int battery_raw = (b[6] >> 3) & 0x7;

    float ratio, offset;
    switch(mode) {
    case 0:
        ratio = 2.352f;
        offset = 47.0f;
        break;
    case 2:
        ratio = 5.491f;
        offset = 18.2f;
        break;
    case 3:
        ratio = 5.491f;
        offset = 0.0f;
        break;
    case 1:
    default:
        ratio = 2.352f;
        offset = 0.0f;
        break;
    }
    float pressure_kpa = ((float)pressure_raw - offset) * ratio; // rtl_433 kPa
    instance->generic.pressure = pressure_kpa * 0.01f; // store as bar
    instance->generic.temperature = (float)((int)b[5] - 50); // degC
    instance->generic.battery_low = (battery_raw == 7) ? 1 : 0; // 7 = low
}

// Inverse of ave_analyze: write the 8 wire bytes from id/pressure/temp/battery and
// recompute the CRC. Uses mode 1 (ratio 2.352, offset 0) scaling — pressure kPa =
// bar*100 => raw = bar*100/2.352.
void tpms_protocol_tpms_ave_pack(TPMSBlockGeneric* g) {
    furi_assert(g);
    uint32_t id = g->id;

    int p_raw = (int)(g->pressure * 100.0f / 2.352f + 0.5f); // bar -> mode-1 raw
    if(p_raw < 0) p_raw = 0;
    if(p_raw > 0xFF) p_raw = 0xFF;

    int t_raw = (int)(g->temperature + (g->temperature >= 0 ? 0.5f : -0.5f)) + 50; // degC + 50
    if(t_raw < 0) t_raw = 0;
    if(t_raw > 0xFF) t_raw = 0xFF;

    int mode = 1; // ratio 2.352, offset 0
    int battery_raw = g->battery_low ? 7 : 0; // 7 = low
    int flags = 0;

    uint8_t b[8];
    b[0] = (uint8_t)((id >> 24) & 0xFF);
    b[1] = (uint8_t)((id >> 16) & 0xFF);
    b[2] = (uint8_t)((id >> 8) & 0xFF);
    b[3] = (uint8_t)(id & 0xFF);
    b[4] = (uint8_t)p_raw;
    b[5] = (uint8_t)t_raw;
    b[6] = (uint8_t)(((mode & 0x3) << 6) | ((battery_raw & 0x7) << 3) | (flags & 0x7));
    // b[7] = running CRC over the first 7 bytes so crc8(b, 8) == 0.
    b[7] = subghz_protocol_blocks_crc8(b, 7, 0x31, 0xff);

    uint64_t data = 0;
    for(int i = 0; i < 8; i++) data = (data << 8) | b[i];
    g->data = data;
    g->data_count_bit = AVE_DATA_BITS;
}

// Search the accumulated raw buffer for the 32-bit preamble 0xCCCCCCCD and, from
// each hit, differential-Manchester-decode the payload (from bitpos+32), accepting
// on CRC. Mirrors rtl_433 tpms_ave_callback (bitbuffer_search 32, decode bitpos+32).
static void ave_process(TPMSProtocolDecoderTpmsAve* instance) {
    unsigned nbits = instance->rawbits;
    if(nbits < AVE_PREAMBLE_BITS + AVE_DATA_BITS * 2u) return;
    for(unsigned pos = 0; pos + AVE_PREAMBLE_BITS <= nbits; pos++) {
        uint32_t v = 0;
        for(int k = 0; k < AVE_PREAMBLE_BITS; k++)
            v = (v << 1) | ave_bit_at(instance->rawbuf, pos + k);
        if(v != AVE_PREAMBLE_RAW) continue;

        // Decode up to 80 bits (the sync loop can emit leading data bits before
        // the clock locks, per rtl_433), then require the full 64-bit payload.
        uint8_t bits[80];
        int n = ave_dm_decode(instance->rawbuf, nbits, pos + AVE_PREAMBLE_BITS, bits, 80);
        if(n < (int)AVE_DATA_BITS) continue;

        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;
        for(int i = 0; i < (int)AVE_DATA_BITS; i++) {
            instance->decoder.decode_data = (instance->decoder.decode_data << 1) | (bits[i] & 1);
            instance->decoder.decode_count_bit++;
        }
        if(ave_check(instance)) {
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            ave_analyze(instance);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
            return; // one good frame per burst is enough
        }
    }
}

void tpms_protocol_decoder_tpms_ave_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderTpmsAve* instance = context;

    const uint32_t te = tpms_protocol_tpms_ave_const.te_short; // 100us half-symbol
    // A gap ends the burst. The longest legit raw run (preamble and DM data alike)
    // is two half-symbols (~200us), so the reset threshold sits above it at 4*te
    // (rtl_433 reset_limit = 400us).
    if(duration >= te * 4) {
        ave_process(instance);
        ave_reset_state(instance);
        return;
    }
    // Expand this pulse into 1..4 raw half-symbol bits of the current level.
    uint32_t nb = (duration + te / 2) / te;
    if(nb < 1) nb = 1;
    if(nb > 4) nb = 4;
    for(uint32_t k = 0; k < nb; k++) {
        if(instance->rawbits >= (uint16_t)(sizeof(instance->rawbuf) * 8)) {
            ave_process(instance);
            ave_reset_state(instance);
        }
        unsigned idx = instance->rawbits;
        if(level)
            instance->rawbuf[idx >> 3] |= (uint8_t)(0x80u >> (idx & 7u));
        else
            instance->rawbuf[idx >> 3] &= (uint8_t)~(0x80u >> (idx & 7u));
        instance->rawbits++;
    }
}

uint8_t tpms_protocol_decoder_tpms_ave_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderTpmsAve* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_tpms_ave_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderTpmsAve* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_tpms_ave_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderTpmsAve* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, tpms_protocol_tpms_ave_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_tpms_ave_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderTpmsAve* instance = context;
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
// Reproduce the raw half-symbol stream the decoder consumes (it ignores level and
// looks only at pulse durations, expanding each via round(dur/te_short)):
//   inter-frame gap (reset) + 32-bit raw preamble 0xCCCCCCCD (NOT DM-encoded) +
//   64 data bits differential-Manchester encoded to match ave_dm_decode.
//
// Worst case: 1 gap + 32 preamble halves + 64*2 data halves = 161 entries.
#define TPMS_AVE_ENCODER_UPLOAD_SIZE 200

void* tpms_protocol_encoder_tpms_ave_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderTpmsAve* instance = malloc(sizeof(TPMSProtocolEncoderTpmsAve));
    instance->base.protocol = &tpms_protocol_tpms_ave;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_AVE_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_tpms_ave_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderTpmsAve* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_tpms_ave_stop(void* context) {
    TPMSProtocolEncoderTpmsAve* instance = context;
    instance->encoder.is_running = false;
}

// Rebuild b[0..7] from generic.data and recompute the CRC so the wire frame is
// always valid even if data was hand-edited.
static void tpms_protocol_encoder_tpms_ave_build_frame(TPMSBlockGeneric* generic, uint8_t b[8]) {
    for(int i = 0; i < 8; i++) {
        b[i] = (generic->data >> (8 * (7 - i))) & 0xFF;
    }
    b[7] = subghz_protocol_blocks_crc8(b, 7, 0x31, 0xff);
}

static bool tpms_protocol_encoder_tpms_ave_get_upload(TPMSProtocolEncoderTpmsAve* instance) {
    furi_assert(instance);
    const uint32_t te_short = tpms_protocol_tpms_ave_const.te_short;

    uint8_t frame[8];
    tpms_protocol_encoder_tpms_ave_build_frame(&instance->generic, frame);

    // 64-bit data stream, MSB-first per byte.
    uint8_t bits[AVE_DATA_BITS];
    int n = 0;
    for(int i = 0; i < 8; i++) {
        for(int j = 7; j >= 0; j--) bits[n++] = (frame[i] >> j) & 0x1;
    }

    size_t index = 0;
    const size_t max = instance->encoder.size_upload;
    bool last_level = false;
    uint32_t last_duration = 0;

    // Run-length-merged emit: each RAW() is ONE raw half-symbol of te_short; equal
    // consecutive levels merge into a single pulse the decoder re-expands.
#define EMIT(lvl, dur)                                            \
    do {                                                          \
        if(last_duration != 0 && last_level == (lvl)) {           \
            last_duration += (dur);                               \
        } else {                                                  \
            if(last_duration != 0) {                              \
                if(index >= max) return false;                    \
                instance->encoder.upload[index++] =               \
                    level_duration_make(last_level, last_duration); \
            }                                                     \
            last_level = (lvl);                                   \
            last_duration = (dur);                                \
        }                                                         \
    } while(0)
#define RAW(bit) EMIT((bit) ? true : false, te_short)

    // Inter-frame gap (>= te_short*4 reset threshold) so each frame starts clean.
    EMIT(false, te_short * 40);

    // 32-bit raw preamble 0xCCCCCCCD, MSB-first, emitted literally (NOT DM-encoded).
    for(int j = AVE_PREAMBLE_BITS - 1; j >= 0; j--) {
        RAW((AVE_PREAMBLE_RAW >> j) & 0x1);
    }

    // 64 data bits, differential-Manchester encoded to match ave_dm_decode: clock
    // half b1 flips the previous data half; second half b2 == b1 for a '1', != b1
    // for a '0'. prev starts at the last preamble raw half (LSB of 0xCD = 1).
    int prev = (int)(AVE_PREAMBLE_RAW & 0x1);
    for(int i = 0; i < AVE_DATA_BITS; i++) {
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
    tpms_protocol_encoder_tpms_ave_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderTpmsAve* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_tpms_ave_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_AVE_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_tpms_ave_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_tpms_ave_yield(void* context) {
    TPMSProtocolEncoderTpmsAve* instance = context;

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
