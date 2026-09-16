#include "tpms_truck.h"
#include "../blocks/generic.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSTruck"

// Port of rtl_433/src/devices/tpms_truck.c
//   "Unbranded SolarTPMS for trucks" (GPL-2+).
//
// FSK 433 MHz, Manchester-coded, ~52 us half-bit, set of 6 wheels.
//
// Wire format after Manchester decode (76 bits = 4-bit sync-state + 72 data):
//   U II II II II WW F PPP TT CC ?
//   U       = 4-bit state (sync, not part of checksum) -> dropped
//   b[0..3] = 32-bit ID (big-endian)
//   b[4]    = wheel position
//   b[5]>>4 = 4 status flags (0x4 = pressure alert; 0x3 = battery ok, 0x0 = low)
//   ((b[5]&0x0F)<<8)|b[6] = 12-bit pressure (kPa)
//   b[7]    = temperature, deg C, signed
//   b[8]    = XOR checksum: xor_bytes(b[0..8]) == 0
//
// rtl_433 inverts the bitbuffer, searches the raw preamble 0xAAAAA9 (== 0x555556
// before invert), then Manchester-decodes 76 bits and drops the leading 4-bit U
// nibble. So the sync preamble lives in the RAW half-symbol bits and only the
// payload is Manchester-encoded. This port hunts the raw 16-bit tail 0x5556 (or
// the inverted-polarity 0xAAA9) in the raw bits, then Manchester-decodes 76 data
// bits and drops the top 4 (U) to recover b[0..8]. Frames are accepted only when
// the XOR checksum is zero and the ID is non-zero.

#define TRUCK_PREAMBLE   0x5556U
#define TRUCK_DATA_BITS  76 // U(4) + b[0..8](72); the U nibble is discarded

static const SubGhzBlockConst tpms_protocol_truck_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = TRUCK_DATA_BITS,
};

typedef enum {
    TruckDecoderStepReset = 0,
    TruckDecoderStepFindPreamble,
    TruckDecoderStepData,
} TruckDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    // Truck TPMS is FSK where the frame is RAW half-symbols: a fixed sync preamble
    // in the raw bits, then the 76-bit payload is Manchester-encoded (matches
    // rtl_433 tpms_truck: it inverts then searches 0xAAAAA9 == raw 0x555556, and
    // manchester-decodes only the data). The old port ran manchester over
    // EVERYTHING and hunted the preamble in the manchester-decoded stream, which
    // never syncs.
    uint16_t raw_shift; // rolling window of raw half-symbol bits for preamble hunt
    bool in_data; // true once the preamble matched, collecting manchester data
    bool data_second; // pair convention: false=data is 1st half (0x5556), true=2nd (0xAAA9)
    bool have_half; // holding the first half of a manchester pair
    uint8_t first_half; // the first half-symbol of the current pair
    uint64_t head_64_bit; // upper 12 bits of the 76-bit accumulator (see add_to_128_bit)
} TPMSProtocolDecoderTruck;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderTruck;

static void* tpms_protocol_decoder_truck_alloc(SubGhzEnvironment* environment);
static void tpms_protocol_decoder_truck_free(void* context);

static void* tpms_protocol_encoder_truck_alloc(SubGhzEnvironment* environment);
static void tpms_protocol_encoder_truck_free(void* context);
static void tpms_protocol_encoder_truck_stop(void* context);
static SubGhzProtocolStatus
    tpms_protocol_encoder_truck_deserialize(void* context, FlipperFormat* flipper_format);
static LevelDuration tpms_protocol_encoder_truck_yield(void* context);
static void tpms_protocol_decoder_truck_reset(void* context);
static void tpms_protocol_decoder_truck_feed(void* context, bool level, uint32_t duration);
static uint8_t tpms_protocol_decoder_truck_get_hash_data(void* context);
static SubGhzProtocolStatus tpms_protocol_decoder_truck_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
static SubGhzProtocolStatus
    tpms_protocol_decoder_truck_deserialize(void* context, FlipperFormat* flipper_format);
static void tpms_protocol_decoder_truck_get_string(void* context, FuriString* output);

const SubGhzProtocolDecoder tpms_protocol_truck_decoder = {
    .alloc = tpms_protocol_decoder_truck_alloc,
    .free = tpms_protocol_decoder_truck_free,
    .feed = tpms_protocol_decoder_truck_feed,
    .reset = tpms_protocol_decoder_truck_reset,
    .get_hash_data = tpms_protocol_decoder_truck_get_hash_data,
    .serialize = tpms_protocol_decoder_truck_serialize,
    .deserialize = tpms_protocol_decoder_truck_deserialize,
    .get_string = tpms_protocol_decoder_truck_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_truck_encoder = {
    .alloc = tpms_protocol_encoder_truck_alloc,
    .free = tpms_protocol_encoder_truck_free,
    .deserialize = tpms_protocol_encoder_truck_deserialize,
    .stop = tpms_protocol_encoder_truck_stop,
    .yield = tpms_protocol_encoder_truck_yield,
};

const SubGhzProtocol tpms_protocol_truck = {
    .name = TPMS_PROTOCOL_TRUCK_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_truck_decoder,
    .encoder = &tpms_protocol_truck_encoder,
};

static void* tpms_protocol_decoder_truck_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderTruck* instance = calloc(1, sizeof(TPMSProtocolDecoderTruck));
    instance->base.protocol = &tpms_protocol_truck;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

static void tpms_protocol_decoder_truck_free(void* context) {
    furi_assert(context);
    free(context);
}

static void tpms_protocol_decoder_truck_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderTruck* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->raw_shift = 0;
    instance->in_data = false;
    instance->data_second = false;
    instance->have_half = false;
    instance->first_half = 0;
    instance->head_64_bit = 0;
}

// Recover the 9 payload bytes b[0..8] from the 76-bit accumulator, dropping the
// leading 4-bit U nibble. Layout of the accumulator after 76 bits:
//   head_64_bit[11..8] = U (dropped)
//   head_64_bit[7..0]  = b[0]
//   decode_data[63..0] = b[1..8]
static void truck_get_bytes(TPMSProtocolDecoderTruck* instance, uint8_t b[9]) {
    b[0] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 1] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

static bool truck_check(TPMSProtocolDecoderTruck* instance, uint8_t b[9]) {
    truck_get_bytes(instance, b);
    // Reject all-zero ID (XOR checksum cannot catch it).
    if(!b[0] && !b[1] && !b[2] && !b[3]) return false;
    // XOR of all 9 bytes must be zero.
    return subghz_protocol_blocks_xor_bytes(b, 9) == 0;
}

static void truck_analyze(TPMSProtocolDecoderTruck* instance, const uint8_t b[9]) {
    instance->generic.id =
        ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    uint16_t pressure_kpa = (uint16_t)((b[5] & 0x0F) << 8) | b[6]; // 12-bit kPa
    instance->generic.pressure = (float)pressure_kpa * 0.01f; // kPa -> bar
    instance->generic.temperature = (float)((int8_t)b[7]); // signed deg C
    uint8_t flags = b[5] >> 4;
    instance->generic.battery_low = ((flags & 0x3) == 0x3) ? 0 : 1; // 0x3 = ok
    instance->generic.data = instance->decoder.decode_data; // b[1..8]
    instance->generic.data_count_bit = 72;
}

// Consume one raw half-symbol bit: hunt the raw preamble (0x5556, or the
// inverted-polarity 0xAAA9), then Manchester-decode the 76-bit payload. Matches
// rtl_433 tpms_truck (preamble searched in raw bits; only the data is
// Manchester). The frame is >64 bits, so the top bits spill into head_64_bit via
// subghz_protocol_blocks_add_to_128_bit; the leading 4-bit U nibble ends up in
// head_64_bit[11..8] and is dropped by truck_get_bytes.
static void truck_emit_raw_bit(TPMSProtocolDecoderTruck* instance, uint8_t bit) {
    bit &= 1;
    if(!instance->in_data) {
        instance->raw_shift = (uint16_t)((instance->raw_shift << 1) | bit);
        if(instance->raw_shift == 0x5556 || instance->raw_shift == 0xAAA9) {
            instance->in_data = true;
            instance->data_second = (instance->raw_shift == 0xAAA9);
            instance->have_half = false;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->head_64_bit = 0;
        }
        return;
    }
    // Inside the Manchester payload: pair up half-symbols. A valid pair is a
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
    subghz_protocol_blocks_add_to_128_bit(&instance->decoder, data_bit, &instance->head_64_bit);
    if(instance->decoder.decode_count_bit == TRUCK_DATA_BITS) {
        uint8_t b[9];
        if(truck_check(instance, b)) {
            truck_analyze(instance, b);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        instance->in_data = false;
        instance->raw_shift = 0;
    }
}

static void tpms_protocol_decoder_truck_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderTruck* instance = context;

    const uint32_t te = tpms_protocol_truck_const.te_short; // 52us half-symbol
    // A gap (or any pulse longer than two half-symbols) ends the burst.
    if(duration >= te * 3) {
        tpms_protocol_decoder_truck_reset(instance);
        return;
    }
    // Expand this pulse into 1 or 2 raw half-symbol bits of the current level.
    uint32_t n = (duration + te / 2) / te;
    if(n < 1) n = 1;
    if(n > 2) n = 2;
    for(uint32_t k = 0; k < n; k++) {
        truck_emit_raw_bit(instance, level ? 1 : 0);
    }
}

static uint8_t tpms_protocol_decoder_truck_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderTruck* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

static SubGhzProtocolStatus tpms_protocol_decoder_truck_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderTruck* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

static SubGhzProtocolStatus
    tpms_protocol_decoder_truck_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderTruck* instance = context;
    // Stored payload is the 72-bit frame (b[0..8]); check against that count.
    return tpms_block_generic_deserialize_check_count_bit(&instance->generic, flipper_format, 72);
}

static void tpms_protocol_decoder_truck_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderTruck* instance = context;
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
// 0-PSI packer / frame rebuild (invert truck_analyze)
// -----------------------------------------------------------------------------
//
// Wire frame is 76 bits: U(4) + b[0..8](72). The serialized generic.data holds
// only the low 64 bits (b[1..8]); b[0] is the top ID byte, recovered from
// generic.id. b[8] is the XOR integrity byte (XOR(b[0..8])==0). The 4-bit U sync
// state is dropped on decode, so the encoder emits U=0.
//
//   b[0..3] = 32-bit ID (big-endian)          <- generic.id
//   b[4]    = wheel position
//   b[5]>>4 = 4 status flags (0x3 low bit-pair = battery ok)
//   ((b[5]&0x0F)<<8)|b[6] = 12-bit pressure (kPa); bar = kPa*0.01
//   b[7]    = temperature, deg C, signed
//   b[8]    = XOR checksum over b[0..8] == 0

// Rebuild the full 9-byte frame b[0..8]. b[1..8] come from generic.data; b[0] is
// the top ID byte from generic.id; b[8] is recomputed so XOR(b[0..8]) == 0 (a
// modified / 0-PSI payload stays valid).
static void tpms_protocol_encoder_truck_build_frame(TPMSBlockGeneric* generic, uint8_t b[9]) {
    b[0] = (uint8_t)((generic->id >> 24) & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 1] = (generic->data >> (8 * (7 - i))) & 0xFF;
    }
    uint8_t x = 0;
    for(int i = 0; i < 8; i++) x ^= b[i];
    b[8] = x; // makes XOR(b[0..8]) == 0
}

void tpms_protocol_truck_pack(TPMSBlockGeneric* g) {
    furi_assert(g);
    // pressure: bar = kPa*0.01 -> kPa = bar*100 (12-bit raw across b[5]lo|b[6]).
    int p_raw = (int)(g->pressure * 100.0f + 0.5f);
    if(p_raw < 0) p_raw = 0;
    if(p_raw > 0x0FFF) p_raw = 0x0FFF;
    // temperature: signed deg C in b[7].
    int t = (int)(g->temperature + (g->temperature >= 0 ? 0.5f : -0.5f));
    if(t < -128) t = -128;
    if(t > 127) t = 127;
    // status flags: low bit-pair == 0x3 means battery ok (battery_low==0).
    uint8_t flags = g->battery_low ? 0x0 : 0x3;

    uint8_t b[9];
    b[0] = (uint8_t)((g->id >> 24) & 0xFF);
    b[1] = (uint8_t)((g->id >> 16) & 0xFF);
    b[2] = (uint8_t)((g->id >> 8) & 0xFF);
    b[3] = (uint8_t)(g->id & 0xFF);
    b[4] = 0; // wheel position
    b[5] = (uint8_t)((flags << 4) | ((p_raw >> 8) & 0x0F));
    b[6] = (uint8_t)(p_raw & 0xFF);
    b[7] = (uint8_t)((int8_t)t);
    uint8_t x = 0;
    for(int i = 0; i < 8; i++) x ^= b[i];
    b[8] = x; // XOR(b[0..8]) == 0

    // generic.data holds b[1..8] (the decoder keeps b[0] folded into the ID).
    uint64_t data = 0;
    for(int i = 1; i <= 8; i++) data = (data << 8) | b[i];
    g->data = data;
    g->data_count_bit = 72;
}

// -----------------------------------------------------------------------------
// Encoder
// -----------------------------------------------------------------------------
//
// FSK Manchester-II, non-inverting (the 0x5556 convention: the data bit is the
// 1st half of each transitioning pair):
//   bit 1 -> first half HIGH, second half LOW
//   bit 0 -> first half LOW,  second half HIGH
// Frame: gap (reset) + raw 0x55 0x55 0x55 0x56 preamble + 76 Manchester bits
// (U(4)=0 + b[0..8](72)). The raw preamble is emitted one raw half-symbol per
// bit (NOT Manchester); only the 76-bit payload is Manchester-encoded, matching
// the decoder's raw-preamble hunt.
//
// Worst-case entries: 1 gap + 32 preamble + 2*76 data + 1 = 186.
#define TPMS_TRUCK_ENCODER_UPLOAD_SIZE 200

static void* tpms_protocol_encoder_truck_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderTruck* instance = malloc(sizeof(TPMSProtocolEncoderTruck));
    instance->base.protocol = &tpms_protocol_truck;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_TRUCK_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

static void tpms_protocol_encoder_truck_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderTruck* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

static void tpms_protocol_encoder_truck_stop(void* context) {
    TPMSProtocolEncoderTruck* instance = context;
    instance->encoder.is_running = false;
}

static bool tpms_protocol_encoder_truck_get_upload(TPMSProtocolEncoderTruck* instance) {
    furi_assert(instance);
    const uint32_t te = tpms_protocol_truck_const.te_short;

    uint8_t frame[9];
    tpms_protocol_encoder_truck_build_frame(&instance->generic, frame);

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
    // ONE raw half-symbol; only the payload below is Manchester-encoded.
    static const uint8_t preamble[4] = {0x55, 0x55, 0x55, 0x56};
    for(int i = 0; i < 4; i++) {
        for(int b = 7; b >= 0; b--) {
            EMIT(((preamble[i] >> b) & 0x1) ? true : false, te);
        }
    }

    // 76 Manchester payload bits, MSB first: U(4)=0 then b[0..8](72).
    for(int i = 0; i < 4; i++) MAN_BIT(0); // U sync nibble (dropped on decode)
    for(int i = 0; i < 72; i++) {
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
    tpms_protocol_encoder_truck_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderTruck* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        // Stored payload is the 72-bit frame (b[0..8]); check against that count.
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic, flipper_format, 72);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_TRUCK_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_truck_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

static LevelDuration tpms_protocol_encoder_truck_yield(void* context) {
    TPMSProtocolEncoderTruck* instance = context;

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
