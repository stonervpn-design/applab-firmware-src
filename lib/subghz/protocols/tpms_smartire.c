#include "tpms_smartire.h"
#include "../blocks/generic.h"

#define TAG "TPMSSmarTire"

// Port of rtl_433/src/devices/tpms_smartire.c
//   (SmarTire Vantage / Aston Martin DB9 protocol, 1/2005 - 12/2011,
//    Bruno OCTAU (ProfBoc75) 2024, GPL-2+).
//
// RF: OOK, 433.92 MHz. rtl_433 flex string:
//   m=OOK_PCM,s=167,l=167,r=600,preamble=32b4,decode_dm
// i.e. the OOK demod is sliced into 167 us PCM (NRZ) raw bits, the 16-bit raw
// sync word 0x32b4 is searched, and the payload is Differential-Manchester
// decoded from bitpos+16.
//
// The sensor sends 10 messages at a time: a Pressure message (msg_type 0)
// followed by a Temperature message (msg_type 1), each repeated 5 times.
//
// Wire format (after DMC decode, 6 bytes b[0..5], 48 bits - the very last raw
// bit is always missing, so 47 payload bits are common; the missing bit is the
// unused LSB of the CRC byte):
//   Byte    0   1   2   3   4   5
//   Sample 28  3f  ff  ff  00  fa
//          VV  MMII II  II  FF  CC
//   b[0] VV : {8} value. Pressure msg: kPa = (b[0]-40) * 2.5.
//                        Temperature msg: C   = (b[0]-40).
//   b[1] MM : {2} message type (0=pressure, 1=temperature) = b[1]>>6
//        II : high 6 bits of the 22-bit sensor ID
//   b[2..3] : low 16 bits of the ID -> id = ((b[1]&0x3f)<<16)|(b[2]<<8)|b[3]
//   b[4] F  : {8} flags; bit7 = quick-inflate detected, bits0..6 unknown
//   b[5] C  : {7} CRC-7, poly 0x45, init 0x6f, final XOR 0x00, stored in bits
//             7..1 (bit0 unused). crc7(b, 6, 0x45, 0x6f) == 0 on a valid frame.
//
// This firmware receives (level,duration) edges rather than a PCM bitstream, so
// feed() reconstructs the 167 us raw-bit stream by expanding each pulse into
// n = round(duration / te_short) same-level raw bits. On a gap the accumulated
// raw buffer is searched for the 16-bit preamble (either receiver polarity) and
// the payload is Differential-Manchester decoded with a faithful port of
// rtl_433's bitbuffer_differential_manchester_decode.

#define SMARTIRE_DATA_BITS   48
#define SMARTIRE_MIN_BITS    47 // rtl_433 tolerates the always-missing last bit

#define SMARTIRE_PREAMBLE16      0x32B4UL
#define SMARTIRE_PREAMBLE16_INV  0xCD4BUL // 16-bit inverse (opposite RX polarity)
#define SMARTIRE_PREAMBLE16_MASK 0xFFFFUL

static const SubGhzBlockConst tpms_protocol_tpms_smartire_const = {
    .te_short = 167,
    .te_long = 334,
    .te_delta = 60,
    .min_count_bit_for_found = SMARTIRE_DATA_BITS,
};

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;

    // Raw 167 us bits for the current burst. On a gap the buffer is searched for
    // the 16-bit preamble and the 48-bit payload is Differential-Manchester
    // decoded, faithfully mirroring rtl_433 bitbuffer_differential_manchester_decode.
    uint8_t rawbuf[40]; // up to 320 raw bits (one frame: 16 preamble + 96 DMC)
    uint16_t rawbits;
} TPMSProtocolDecoderSmarTire;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderSmarTire;

// Encoder ops (definitions at the bottom of this file).
void* tpms_protocol_encoder_tpms_smartire_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_tpms_smartire_free(void* context);
void tpms_protocol_encoder_tpms_smartire_stop(void* context);
SubGhzProtocolStatus
    tpms_protocol_encoder_tpms_smartire_deserialize(void* context, FlipperFormat* flipper_format);
LevelDuration tpms_protocol_encoder_tpms_smartire_yield(void* context);

const SubGhzProtocolDecoder tpms_protocol_tpms_smartire_decoder = {
    .alloc = tpms_protocol_decoder_tpms_smartire_alloc,
    .free = tpms_protocol_decoder_tpms_smartire_free,
    .feed = tpms_protocol_decoder_tpms_smartire_feed,
    .reset = tpms_protocol_decoder_tpms_smartire_reset,
    .get_hash_data = tpms_protocol_decoder_tpms_smartire_get_hash_data,
    .serialize = tpms_protocol_decoder_tpms_smartire_serialize,
    .deserialize = tpms_protocol_decoder_tpms_smartire_deserialize,
    .get_string = tpms_protocol_decoder_tpms_smartire_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_tpms_smartire_encoder = {
    .alloc = tpms_protocol_encoder_tpms_smartire_alloc,
    .free = tpms_protocol_encoder_tpms_smartire_free,
    .deserialize = tpms_protocol_encoder_tpms_smartire_deserialize,
    .stop = tpms_protocol_encoder_tpms_smartire_stop,
    .yield = tpms_protocol_encoder_tpms_smartire_yield,
};

const SubGhzProtocol tpms_protocol_tpms_smartire = {
    .name = TPMS_PROTOCOL_SMARTIRE_NAME,
    .type = SubGhzProtocolTypeTpms,
    // OOK -> AM. 433 band, sensor protocol, decodable + save/load/send.
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_tpms_smartire_decoder,
    .encoder = &tpms_protocol_tpms_smartire_encoder,
};

void* tpms_protocol_decoder_tpms_smartire_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderSmarTire* instance = calloc(1, sizeof(TPMSProtocolDecoderSmarTire));
    instance->base.protocol = &tpms_protocol_tpms_smartire;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_tpms_smartire_free(void* context) {
    furi_assert(context);
    free(context);
}

static void smartire_reset_state(TPMSProtocolDecoderSmarTire* instance) {
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->rawbits = 0;
}

void tpms_protocol_decoder_tpms_smartire_reset(void* context) {
    furi_assert(context);
    smartire_reset_state(context);
}

// Raw bit access (MSB-first per byte), matching rtl_433 bit_at.
static inline uint8_t smartire_bit_at(const uint8_t* buf, unsigned i) {
    return (uint8_t)((buf[i >> 3] >> (7 - (i & 7))) & 1);
}

// Faithful port of rtl_433 bitbuffer_differential_manchester_decode: decode up to
// `maxbits` data bits starting at raw-bit `start` into out[] (one byte per bit),
// returning the count. bit1 is the clock half (must differ from the previous data
// half); the data bit is (bit1==bit2)?1:0.
static int smartire_dm_decode(
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
        uint8_t b1 = smartire_bit_at(buf, ipos++);
        uint8_t b2 = smartire_bit_at(buf, ipos++);
        uint8_t b3 = (ipos < nbits) ? smartire_bit_at(buf, ipos) : 0;
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
        uint8_t b1 = smartire_bit_at(buf, ipos++);
        if(b1 == bit2) break; // clock missing
        uint8_t b2 = smartire_bit_at(buf, ipos++);
        out[n++] = (b1 == b2) ? 1 : 0;
        bit2 = b2;
    }
    return n;
}

static void smartire_get_bytes(TPMSProtocolDecoderSmarTire* instance, uint8_t b[6]) {
    // 48 bits held MSB-first in decode_data: b[0] is the top byte.
    for(int i = 0; i < 6; i++) {
        b[i] = (uint8_t)((instance->decoder.decode_data >> (8 * (5 - i))) & 0xFF);
    }
}

static bool smartire_check(TPMSProtocolDecoderSmarTire* instance) {
    uint8_t b[6];
    smartire_get_bytes(instance, b);
    // CRC-7 over all 6 bytes (incl. the CRC byte) must be 0.
    return subghz_protocol_blocks_crc7(b, 6, 0x45, 0x6f) == 0;
}

// Decode the ID (always) and, depending on message type, only the pressure OR
// the temperature field - leaving the complementary field untouched, exactly as
// the real sensor alternates the two message types. Returns false for an unknown
// message type (rtl_433 aborts on those).
static bool smartire_analyze(TPMSProtocolDecoderSmarTire* instance) {
    uint8_t b[6];
    smartire_get_bytes(instance, b);

    uint8_t msg_type = (uint8_t)(b[1] >> 6);
    if(msg_type != 0 && msg_type != 1) return false;

    instance->generic.id = ((uint32_t)(b[1] & 0x3f) << 16) | ((uint32_t)b[2] << 8) | b[3];
    instance->generic.battery_low = 0; // protocol carries no battery indicator

    int value = (int)b[0] - 40;
    if(msg_type == 0) {
        // Pressure: rtl_433 kPa = value * 2.5; store as bar (/100).
        instance->generic.pressure = (float)value * 2.5f * 0.01f;
    } else {
        // Temperature: rtl_433 C = value.
        instance->generic.temperature = (float)value;
    }
    return true;
}

// Search the accumulated raw buffer for the 16-bit preamble 0x32b4 (either
// receiver polarity) and, from each hit, Differential-Manchester-decode the
// 48-bit payload (from bitpos+16) and accept on CRC-7 == 0. Mirrors rtl_433
// tpms_smartire_decode.
static void smartire_process(TPMSProtocolDecoderSmarTire* instance) {
    unsigned nbits = instance->rawbits;
    if(nbits < 16u + SMARTIRE_MIN_BITS * 2u) return;
    for(unsigned pos = 0; pos + 16u <= nbits; pos++) {
        uint32_t v = 0;
        for(int k = 0; k < 16; k++)
            v = (uint32_t)((v << 1) | smartire_bit_at(instance->rawbuf, pos + k));
        v &= SMARTIRE_PREAMBLE16_MASK;
        if(v != SMARTIRE_PREAMBLE16 && v != SMARTIRE_PREAMBLE16_INV) continue;

        uint8_t bits[SMARTIRE_DATA_BITS];
        int n = smartire_dm_decode(
            instance->rawbuf, nbits, pos + 16, bits, SMARTIRE_DATA_BITS);
        if(n < SMARTIRE_MIN_BITS) continue;

        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;
        for(int i = 0; i < SMARTIRE_DATA_BITS; i++) {
            // Zero-pad the always-missing last bit (unused CRC LSB).
            subghz_protocol_blocks_add_bit(&instance->decoder, (i < n) ? bits[i] : 0);
        }
        if(!smartire_check(instance)) continue;

        instance->generic.data = instance->decoder.decode_data;
        instance->generic.data_count_bit = instance->decoder.decode_count_bit;
        if(!smartire_analyze(instance)) continue;
        if(instance->base.callback) instance->base.callback(&instance->base, instance->base.context);
        return; // one good frame per burst is enough
    }
}

void tpms_protocol_decoder_tpms_smartire_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderSmarTire* instance = context;

    const uint32_t te = tpms_protocol_tpms_smartire_const.te_short; // 167 us raw bit
    // A gap (or an over-long pulse) ends the burst: decode what we have, reset.
    if(duration >= te * 5) {
        smartire_process(instance);
        smartire_reset_state(instance);
        return;
    }
    // Expand this pulse into 1..4 raw bits of the current level.
    uint32_t nb = (duration + te / 2) / te;
    if(nb < 1) nb = 1;
    if(nb > 4) nb = 4;
    for(uint32_t k = 0; k < nb; k++) {
        if(instance->rawbits >= (uint16_t)(sizeof(instance->rawbuf) * 8)) {
            smartire_process(instance);
            smartire_reset_state(instance);
        }
        unsigned idx = instance->rawbits;
        if(level)
            instance->rawbuf[idx >> 3] |= (uint8_t)(0x80u >> (idx & 7u));
        else
            instance->rawbuf[idx >> 3] &= (uint8_t)~(0x80u >> (idx & 7u));
        instance->rawbits++;
    }
}

uint8_t tpms_protocol_decoder_tpms_smartire_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderSmarTire* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_tpms_smartire_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderSmarTire* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_tpms_smartire_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderSmarTire* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_tpms_smartire_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_tpms_smartire_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderSmarTire* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%06lX\r\n"
        "Bat:%d\r\n"
        "Temp:%2.0f C Bar:%2.2f",
        instance->generic.protocol_name,
        (unsigned long)instance->generic.id,
        instance->generic.battery_low,
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}

// -----------------------------------------------------------------------------
// 0-PSI packer + Encoder
// -----------------------------------------------------------------------------
//
// The sensor's 22-bit ID and single-value-per-message layout mean one wire frame
// carries EITHER pressure OR temperature. To reproduce a full sensor cycle the
// encoder builds and transmits both a pressure message and a temperature message
// (as the real sensor does), rebuilt from g->id + g->pressure + g->temperature and
// with a freshly recomputed CRC-7 for each. build_frame layout (6 bytes):
//   b[0] = value byte (pressure raw or temperature raw)
//   b[1] = (msg_type<<6) | ((id>>16)&0x3f)
//   b[2] = (id>>8)&0xff
//   b[3] = id&0xff
//   b[4] = flags (0)
//   b[5] = CRC-7 (bits7..1), chosen so crc7(b,6,0x45,0x6f) == 0
//
// Scaling (inverse of smartire_analyze):
//   pressure msg:  value = round(bar * 40)   [bar*100 kPa / 2.5]; b[0] = value+40
//   temperature:   value = round(C);          b[0] = value+40
// 0 bar -> value 0 -> b[0] = 0x28 (true 0-PSI). 0x28 matches the rtl_433 sample.

#define SMARTIRE_MSG_PRESSURE 0
#define SMARTIRE_MSG_TEMP     1

// Fill b[0..4] then brute-force the 7-bit CRC byte (exactly one value makes the
// whole-message crc7 zero; the CRC's LSB is unused so we search value<<1).
static void smartire_build_frame(uint8_t b[6], uint8_t msg_type, int value, uint32_t id) {
    if(value < 0) value = 0;
    if(value > 0xFF) value = 0xFF;
    id &= 0x3FFFFFUL; // 22-bit ID
    b[0] = (uint8_t)value;
    b[1] = (uint8_t)(((msg_type & 0x3) << 6) | ((id >> 16) & 0x3F));
    b[2] = (uint8_t)((id >> 8) & 0xFF);
    b[3] = (uint8_t)(id & 0xFF);
    b[4] = 0x00; // flags
    b[5] = 0x00;
    for(int c = 0; c < 128; c++) {
        b[5] = (uint8_t)(c << 1);
        if(subghz_protocol_blocks_crc7(b, 6, 0x45, 0x6f) == 0) break;
    }
}

static int smartire_pressure_value(float bar) {
    return (int)(bar * 40.0f + (bar >= 0 ? 0.5f : -0.5f)) + 40;
}
static int smartire_temp_value(float c) {
    return (int)(c + (c >= 0 ? 0.5f : -0.5f)) + 40;
}

void tpms_protocol_tpms_smartire_pack(TPMSBlockGeneric* g) {
    furi_assert(g);
    g->id &= 0x3FFFFFUL; // 22-bit protocol ID
    g->battery_low = 0;

    // Store the pressure message bytes into g->data (inverse of analyze). The
    // encoder rebuilds both messages from g->pressure / g->temperature, so the
    // exact frame stashed here only needs to be a valid, self-consistent one.
    uint8_t b[6];
    smartire_build_frame(b, SMARTIRE_MSG_PRESSURE, smartire_pressure_value(g->pressure), g->id);
    uint64_t data = 0;
    for(int i = 0; i < 6; i++) data = (data << 8) | b[i];
    g->data = data;
    g->data_count_bit = SMARTIRE_DATA_BITS;
}

// Worst-case upload: 2 frames, each = 1 gap + 16 raw preamble + 96 DMC raw
// half-symbols, emitted one raw half-symbol per entry (no run merge crossing the
// cap). 2 * (1 + 16 + 96) = 226; round up generously.
#define TPMS_SMARTIRE_ENCODER_UPLOAD_SIZE 320

void* tpms_protocol_encoder_tpms_smartire_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderSmarTire* instance = malloc(sizeof(TPMSProtocolEncoderSmarTire));
    instance->base.protocol = &tpms_protocol_tpms_smartire;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 8;
    instance->encoder.size_upload = TPMS_SMARTIRE_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_tpms_smartire_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderSmarTire* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_tpms_smartire_stop(void* context) {
    TPMSProtocolEncoderSmarTire* instance = context;
    instance->encoder.is_running = false;
}

static bool tpms_protocol_encoder_tpms_smartire_get_upload(TPMSProtocolEncoderSmarTire* instance) {
    furi_assert(instance);
    const uint32_t te_short = tpms_protocol_tpms_smartire_const.te_short;

    size_t index = 0;
    const size_t max = instance->encoder.size_upload;
    bool last_level = false;
    uint32_t last_duration = 0;

    // Run-length-merged emit: each RAW() is ONE raw 167 us half-symbol. The
    // decoder rebuilds the count via round(dur/te_short) (cap 4); no run here
    // exceeds 2 half-symbols so a merged run is at most 2*te and never truncates.
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

    // Build the two messages (pressure then temperature) and emit each as:
    //   inter-frame gap (HIGH so it never merges with the LOW first preamble
    //   half-symbol) + raw 16-bit preamble 0x32b4 + 48 DMC data bits.
    uint8_t frames[2][6];
    smartire_build_frame(
        frames[0], SMARTIRE_MSG_PRESSURE, smartire_pressure_value(instance->generic.pressure),
        instance->generic.id);
    smartire_build_frame(
        frames[1], SMARTIRE_MSG_TEMP, smartire_temp_value(instance->generic.temperature),
        instance->generic.id);

    for(int f = 0; f < 2; f++) {
        // Inter-frame gap (>= te_short*5 reset threshold), HIGH level.
        EMIT(true, te_short * 40);

        // Raw 16-bit preamble 0x32b4 (MSB-first), literal half-symbols.
        for(int k = 15; k >= 0; k--) {
            RAW((SMARTIRE_PREAMBLE16 >> k) & 0x1);
        }

        // 48 data bits MSB-first, Differential-Manchester encoded to match
        // smartire_dm_decode: clock half b1 flips the previous data half; second
        // half b2 == b1 for a '1', != b1 for a '0'. prev = last preamble half
        // (bit0 of 0x32b4 = 0).
        int prev = 0;
        for(int i = 0; i < SMARTIRE_DATA_BITS; i++) {
            int bit = (frames[f][i >> 3] >> (7 - (i & 7))) & 0x1;
            int b1 = 1 - prev;
            int b2 = bit ? b1 : (1 - b1);
            RAW(b1);
            RAW(b2);
            prev = b2;
        }
    }

    // Final trailing gap so the last (temperature) frame is framed by a gap even
    // on the very last repeat.
    EMIT(true, te_short * 40);

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
    tpms_protocol_encoder_tpms_smartire_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderSmarTire* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_tpms_smartire_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_SMARTIRE_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_tpms_smartire_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_tpms_smartire_yield(void* context) {
    TPMSProtocolEncoderSmarTire* instance = context;

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
