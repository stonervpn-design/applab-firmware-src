#include "tpms_airpuxem.h"
#include "../blocks/generic.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSAirpuxem"

// Port of rtl_433/src/devices/tpms_airpuxem.c
// (Alexander Grau, Bruno Octau, Christian W. Zuckschwerdt 2019, GPL-2+).
//
// Airpuxem TPMS model TYH11_EU6_ZQ (SENASIC SNP739D IC; likely a white-labeled
// Jansite). FSK 433.92 MHz, FSK_PULSE_PCM at 52 us/bit — i.e. the demodulator
// yields raw NRZ half-symbols, and the frame is Manchester-encoded on top of
// them. This is exactly the tpms_citroen wire structure (fixed raw sync in the
// half-symbol stream, then Manchester data), so this port follows that template.
//
// Wire (nibbles), after Manchester-decoding, 84 bits total:
//   F  IIIIIIII  M N  PP  TT  BB  CC  CC
//   F : 4 bit constant header/sync (0x5)
//   I : 32 bit ID
//   M : bit7 = pressure MSB_ONE (bit8), bits6..4 = 3 flags
//   N : bit3 = pressure MSB_TWO (bit9), bits2..0 = 3-bit sensor position
//   P : 8 bit pressure LSB
//   T : 8 bit temperature (signed deg C)
//   B : 8 bit battery level
//   C : 8 bit CRC, then an 8 bit spare/second CRC byte
//
// rtl_433 strips the 4-bit header, then id_bytes[0..9] are the 80 bits after it:
//   id_bytes[0..3] = ID, id_bytes[4] = "M N", id_bytes[5] = pressure LSB,
//   id_bytes[6] = temperature, id_bytes[7] = battery, id_bytes[8] = CRC,
//   id_bytes[9] = spare CRC byte.
//   CRC = crc8(id_bytes[0..7], 8, poly 0x2f, init 0xaa) == id_bytes[8].
//   pressure = (id_bytes[5] | MSB_ONE<<8 | MSB_TWO<<9) - 100  [kPa]
//   temperature = (int8)id_bytes[6]  [deg C]
//   battery volts = id_bytes[7] * 0.02
//
// Preamble on air is 0x55..556 (raw). rtl_433 inverts and searches 0xaa 0xaa 0xa9
// (24 bits); the final 16 bits of that are 0xAAA9 (== 0x5556 non-inverted), so a
// 16-bit rolling window on 0x5556 / 0xAAA9 lands at the same post-preamble bit,
// identically to tpms_citroen.
//
// Storage: the 8 CRC'd payload bytes (id_bytes[0..7]) fit exactly in the 64-bit
// generic.data (data_count_bit = 64). The constant header, CRC and spare byte are
// derivable and are rebuilt by the encoder — no head_64_bit needed.

#define AIRPUXEM_PREAMBLE_A 0x5556U
#define AIRPUXEM_PREAMBLE_B 0xAAA9U
#define AIRPUXEM_FRAME_BITS 84 // 4 header + 80 (id_bytes[0..9])
#define AIRPUXEM_STORE_BITS 64 // id_bytes[0..7] kept in generic.data
#define AIRPUXEM_CRC_POLY   0x2f
#define AIRPUXEM_CRC_INIT   0xaa

static const SubGhzBlockConst tpms_protocol_tpms_airpuxem_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = AIRPUXEM_STORE_BITS,
};

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    uint16_t raw_shift; // rolling window of raw half-symbol bits for preamble hunt
    bool in_data; // true once the preamble matched, collecting manchester data
    bool data_second; // pair convention: false=data is 1st half (0x5556), true=2nd (0xAAA9)
    bool have_half; // holding the first half of a manchester pair
    uint8_t first_half; // the first half-symbol of the current pair
    uint8_t raw_bytes[11]; // 84 Manchester-decoded bits, MSB-first
    uint16_t bit_count; // Manchester bits collected so far
} TPMSProtocolDecoderAirpuxem;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderAirpuxem;

const SubGhzProtocolDecoder tpms_protocol_tpms_airpuxem_decoder = {
    .alloc = tpms_protocol_decoder_tpms_airpuxem_alloc,
    .free = tpms_protocol_decoder_tpms_airpuxem_free,
    .feed = tpms_protocol_decoder_tpms_airpuxem_feed,
    .reset = tpms_protocol_decoder_tpms_airpuxem_reset,
    .get_hash_data = tpms_protocol_decoder_tpms_airpuxem_get_hash_data,
    .serialize = tpms_protocol_decoder_tpms_airpuxem_serialize,
    .deserialize = tpms_protocol_decoder_tpms_airpuxem_deserialize,
    .get_string = tpms_protocol_decoder_tpms_airpuxem_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_tpms_airpuxem_encoder = {
    .alloc = tpms_protocol_encoder_tpms_airpuxem_alloc,
    .free = tpms_protocol_encoder_tpms_airpuxem_free,
    .deserialize = tpms_protocol_encoder_tpms_airpuxem_deserialize,
    .stop = tpms_protocol_encoder_tpms_airpuxem_stop,
    .yield = tpms_protocol_encoder_tpms_airpuxem_yield,
};

const SubGhzProtocol tpms_protocol_tpms_airpuxem = {
    .name = TPMS_PROTOCOL_AIRPUXEM_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_tpms_airpuxem_decoder,
    .encoder = &tpms_protocol_tpms_airpuxem_encoder,
};

// rtl_433 crc8(): MSB-first bitwise CRC-8, no reflection, no final XOR.
static uint8_t airpuxem_crc8(const uint8_t* msg, unsigned n, uint8_t poly, uint8_t init) {
    uint8_t r = init;
    for(unsigned i = 0; i < n; i++) {
        r ^= msg[i];
        for(int b = 0; b < 8; b++) {
            if(r & 0x80)
                r = (uint8_t)((r << 1) ^ poly);
            else
                r = (uint8_t)(r << 1);
        }
    }
    return r;
}

void* tpms_protocol_decoder_tpms_airpuxem_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderAirpuxem* instance = calloc(1, sizeof(TPMSProtocolDecoderAirpuxem));
    instance->base.protocol = &tpms_protocol_tpms_airpuxem;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_tpms_airpuxem_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_tpms_airpuxem_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderAirpuxem* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->raw_shift = 0;
    instance->in_data = false;
    instance->data_second = false;
    instance->have_half = false;
    instance->first_half = 0;
    instance->bit_count = 0;
    memset(instance->raw_bytes, 0, sizeof(instance->raw_bytes));
}

// Extract id_bytes[k] (k=0..9): the 8 bits at bit offset 4 + 8*k in raw_bytes
// (MSB-first). rtl_433 does bitbuffer_extract_bytes(&dec, 0, 4, ...), i.e. drops
// the 4-bit header, so every id_byte straddles a nibble boundary.
static inline uint8_t airpuxem_id_byte(const uint8_t* raw, int k) {
    return (uint8_t)((raw[k] << 4) | (raw[k + 1] >> 4));
}

static bool airpuxem_check_crc(TPMSProtocolDecoderAirpuxem* instance) {
    // Header nibble must be 0x5.
    if((instance->raw_bytes[0] >> 4) != 0x5) return false;
    uint8_t id_bytes[9];
    for(int k = 0; k < 9; k++) id_bytes[k] = airpuxem_id_byte(instance->raw_bytes, k);
    uint8_t crc = airpuxem_crc8(id_bytes, 8, AIRPUXEM_CRC_POLY, AIRPUXEM_CRC_INIT);
    return crc == id_bytes[8]; // id_bytes[8] is the CRC byte
}

static void airpuxem_analyze(TPMSProtocolDecoderAirpuxem* instance) {
    uint8_t b[8];
    for(int k = 0; k < 8; k++) b[k] = airpuxem_id_byte(instance->raw_bytes, k);
    instance->generic.id =
        ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    // pressure (kPa) per rtl_433: 10-bit raw minus 100 offset. Stored as bar.
    int msb_one = (b[4] >> 7) & 1;
    int msb_two = (b[4] >> 3) & 1;
    int pressure_kpa = (b[5] | (msb_one << 8) | (msb_two << 9)) - 100;
    instance->generic.pressure = (float)pressure_kpa * 0.01f; // kPa -> bar
    instance->generic.temperature = (float)((int8_t)b[6]);
    instance->generic.battery_low = b[7]; // raw battery level byte
    // Keep the 8 CRC'd payload bytes in generic.data (MSB-first), 64 bits.
    uint64_t data = 0;
    for(int k = 0; k < 8; k++) data = (data << 8) | b[k];
    instance->generic.data = data;
    instance->generic.data_count_bit = AIRPUXEM_STORE_BITS;
}

void tpms_protocol_tpms_airpuxem_pack(TPMSBlockGeneric* g) {
    furi_assert(g);
    // pressure: bar -> kPa -> 10-bit raw (raw = kPa + 100).
    int pressure_kpa = (int)(g->pressure * 100.0f + (g->pressure >= 0 ? 0.5f : -0.5f));
    int p_raw = pressure_kpa + 100;
    if(p_raw < 0) p_raw = 0;
    if(p_raw > 0x3FF) p_raw = 0x3FF;
    int t = (int)(g->temperature + (g->temperature >= 0 ? 0.5f : -0.5f)); // signed deg C

    uint8_t b[8];
    b[0] = (uint8_t)((g->id >> 24) & 0xFF);
    b[1] = (uint8_t)((g->id >> 16) & 0xFF);
    b[2] = (uint8_t)((g->id >> 8) & 0xFF);
    b[3] = (uint8_t)(g->id & 0xFF);
    // "M N": pressure MSB_ONE at bit7, 3 flags bits6..4 (0), MSB_TWO at bit3,
    // 3-bit position bits2..0 (0).
    b[4] = (uint8_t)((((p_raw >> 8) & 1) << 7) | (((p_raw >> 9) & 1) << 3));
    b[5] = (uint8_t)(p_raw & 0xFF); // pressure LSB
    b[6] = (uint8_t)(t & 0xFF); // temperature (signed)
    b[7] = g->battery_low; // battery level byte
    uint64_t data = 0;
    for(int k = 0; k < 8; k++) data = (data << 8) | b[k];
    g->data = data;
    g->data_count_bit = AIRPUXEM_STORE_BITS;
}

// Consume one raw half-symbol bit: hunt the raw preamble (0x5556, or the
// inverted-polarity 0xAAA9), then Manchester-decode the 84-bit frame into
// raw_bytes (MSB-first). Matches rtl_433 tpms_airpuxem (preamble searched in raw
// bits; only the frame is Manchester, via bitbuffer_manchester_decode).
static void airpuxem_emit_raw_bit(TPMSProtocolDecoderAirpuxem* instance, uint8_t bit) {
    bit &= 1;
    if(!instance->in_data) {
        instance->raw_shift = (uint16_t)((instance->raw_shift << 1) | bit);
        if(instance->raw_shift == AIRPUXEM_PREAMBLE_A ||
           instance->raw_shift == AIRPUXEM_PREAMBLE_B) {
            instance->in_data = true;
            instance->data_second = (instance->raw_shift == AIRPUXEM_PREAMBLE_B);
            instance->have_half = false;
            instance->bit_count = 0;
            memset(instance->raw_bytes, 0, sizeof(instance->raw_bytes));
        }
        return;
    }
    // Inside the Manchester frame: pair up half-symbols. A valid pair is a
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
    // Append MSB-first into raw_bytes.
    if(data_bit)
        instance->raw_bytes[instance->bit_count >> 3] |=
            (uint8_t)(0x80 >> (instance->bit_count & 7));
    instance->bit_count++;
    if(instance->bit_count == AIRPUXEM_FRAME_BITS) {
        if(airpuxem_check_crc(instance)) {
            airpuxem_analyze(instance);
            // reflect stored payload into decoder for hash
            instance->decoder.decode_data = instance->generic.data;
            instance->decoder.decode_count_bit = instance->generic.data_count_bit;
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        instance->in_data = false;
        instance->raw_shift = 0;
    }
}

void tpms_protocol_decoder_tpms_airpuxem_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderAirpuxem* instance = context;

    const uint32_t te = tpms_protocol_tpms_airpuxem_const.te_short; // 52us half-symbol
    // A gap (or any pulse longer than two half-symbols) ends the burst.
    if(duration >= te * 3) {
        tpms_protocol_decoder_tpms_airpuxem_reset(instance);
        return;
    }
    // Expand this pulse into 1 or 2 raw half-symbol bits of the current level.
    uint32_t n = (duration + te / 2) / te;
    if(n < 1) n = 1;
    if(n > 2) n = 2;
    for(uint32_t k = 0; k < n; k++) {
        airpuxem_emit_raw_bit(instance, level ? 1 : 0);
    }
}

uint8_t tpms_protocol_decoder_tpms_airpuxem_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderAirpuxem* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_tpms_airpuxem_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderAirpuxem* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_tpms_airpuxem_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderAirpuxem* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_tpms_airpuxem_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_tpms_airpuxem_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderAirpuxem* instance = context;
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
// Encoder
// -----------------------------------------------------------------------------
//
// FSK Manchester, matching the decoder's 0x5556 convention (data bit = 1st half
// of each transitioning pair). Frame: gap (reset) + raw 0x55.. 0x56 preamble +
// 84 Manchester data bits (4-bit header 0x5 + id_bytes[0..9]).
//
// generic.data holds id_bytes[0..7]; the CRC byte id_bytes[8] is recomputed in
// build_frame; id_bytes[9] (unvalidated spare CRC byte) is emitted as 0x00.
//
// Worst-case entries: 1 gap + 32 preamble half-symbols + 2*84 data + 1 = 202.
#define TPMS_AIRPUXEM_ENCODER_UPLOAD_SIZE 260

void* tpms_protocol_encoder_tpms_airpuxem_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderAirpuxem* instance = malloc(sizeof(TPMSProtocolEncoderAirpuxem));
    instance->base.protocol = &tpms_protocol_tpms_airpuxem;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_AIRPUXEM_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_tpms_airpuxem_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderAirpuxem* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_tpms_airpuxem_stop(void* context) {
    TPMSProtocolEncoderAirpuxem* instance = context;
    instance->encoder.is_running = false;
}

// Rebuild the full 84-bit frame into an 11-byte MSB-first buffer:
//   bits 0..3   = header 0x5
//   bits 4..83  = id_bytes[0..9]
// id_bytes[0..7] come from generic.data, id_bytes[8] = recomputed CRC,
// id_bytes[9] = 0x00 (spare byte the decoder does not validate).
static void
    tpms_protocol_encoder_tpms_airpuxem_build_frame(TPMSBlockGeneric* generic, uint8_t frame[11]) {
    uint8_t id_bytes[10];
    for(int k = 0; k < 8; k++) {
        id_bytes[k] = (uint8_t)((generic->data >> (8 * (7 - k))) & 0xFF);
    }
    id_bytes[8] = airpuxem_crc8(id_bytes, 8, AIRPUXEM_CRC_POLY, AIRPUXEM_CRC_INIT);
    id_bytes[9] = 0x00;

    memset(frame, 0, 11);
    frame[0] = (uint8_t)((0x5 << 4) | (id_bytes[0] >> 4));
    for(int k = 1; k <= 10; k++) {
        uint8_t lo = (uint8_t)(id_bytes[k - 1] << 4);
        uint8_t hi = (k < 10) ? (uint8_t)(id_bytes[k] >> 4) : 0;
        frame[k] = (uint8_t)(lo | hi);
    }
}

static bool tpms_protocol_encoder_tpms_airpuxem_get_upload(TPMSProtocolEncoderAirpuxem* instance) {
    furi_assert(instance);
    const uint32_t te = tpms_protocol_tpms_airpuxem_const.te_short;

    uint8_t frame[11];
    tpms_protocol_encoder_tpms_airpuxem_build_frame(&instance->generic, frame);

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

// Manchester data bit: 1st half = the data bit, 2nd half = its complement (the
// 0x5556 convention the decoder uses: data = 1st half of each transitioning pair).
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

    // Raw sync preamble — NOT Manchester. The decoder hunts the raw 0x5556 (or
    // inverted 0xAAA9) pattern in the half-symbol stream, so each preamble bit is
    // ONE raw half-symbol; only the data below is Manchester-encoded.
    static const uint8_t preamble[4] = {0x55, 0x55, 0x55, 0x56};
    for(int i = 0; i < 4; i++) {
        for(int b = 7; b >= 0; b--) {
            EMIT(((preamble[i] >> b) & 0x1) ? true : false, te);
        }
    }

    // 84 data bits, MSB first (frame[0]..frame[10] top nibble).
    for(int i = 0; i < AIRPUXEM_FRAME_BITS; i++) {
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
    tpms_protocol_encoder_tpms_airpuxem_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderAirpuxem* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_tpms_airpuxem_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_AIRPUXEM_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_tpms_airpuxem_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_tpms_airpuxem_yield(void* context) {
    TPMSProtocolEncoderAirpuxem* instance = context;

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
