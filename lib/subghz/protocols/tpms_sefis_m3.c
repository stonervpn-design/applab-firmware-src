#include "tpms_sefis_m3.h"
#include "../blocks/generic.h"

#define TAG "TPMSSefisM3"

// Port of rtl_433/src/devices/tpms_sefis_m3.c (Benjamin Larsson 2026, GPL-2+):
//   "Sefis M3 / Careud / Sykik SRTP300 TPMS", Infineon SP400 (8051) transceiver.
//   Reverse engineered in rtl_433 issue #3342 (@ivantichy, @ProfBoc75,
//   @zuckschwerdt). Source studied via:
//     https://raw.githubusercontent.com/merbanan/rtl_433/master/src/devices/tpms_sefis_m3.c
//
// Physical layer: FSK PCM at 52 us/bit, Manchester (IEEE 802.3) coded, with a
// long 0xaa preamble followed by a 16-bit sync word 0x5a9d, then a 9-byte
// payload:  B0 B1 B2 B3 B4 B5 B6 CRC:16 .
//
// Because the payload is Manchester-*and*-inverted on the wire, rtl_433 searches
// the raw (pre-Manchester) sync bits {0x66,0x99,0x96,0xa6} — which are exactly
// Manchester(0x5a9d) with the convention data-bit 0 -> chips "01" (low,high) and
// data-bit 1 -> chips "10" (high,low) — then Manchester-decodes the payload and
// XORs every recovered byte with 0xff to obtain B0..B8.
//
//   B0..B6      : opaque application bytes. rtl_433 identifies no fixed per-sensor
//                 ID field; it reports B0..B6 as hex "code". This port folds the
//                 four bytes rtl_433 leaves undecoded (B0,B1,B3,B6) into the
//                 framework's 32-bit id so the Sensors UI / .sub round-trip has a
//                 stable handle. B2 (temperature) / B4,B5 (pressure) are decoded
//                 exactly as rtl_433 does and are NOT part of the id.
//   Pressure    : page = {7:0,4:1,5:2,2:3}[B4>>5];
//                 code = page<<13 | (B4&0x1f)<<8 | B5;
//                 pressure_kPa = (code - 0x0e00) / 102.4  (clamped >= 0).
//                 Stored in the generic block as BAR (kPa / 100).
//   Temperature : temp_code = (B2 + B5) & 0xff;  temperature_C = 14 + (temp_code & 0x0f).
//                 (rtl_433's field only spans 14..29 C.)
//   CRC:16      : poly 0x1021, init 0x0000, over B0..B6 (7 bytes); B7=hi, B8=lo.

#define SEFIS_M3_PAYLOAD_BYTES 9
#define SEFIS_M3_DATA_BITS     (SEFIS_M3_PAYLOAD_BYTES * 8) // 72
// Raw (pre-Manchester) 32-bit sync word == Manchester(0x5a9d).
#define SEFIS_M3_SYNC          0x669996A6UL

static const SubGhzBlockConst tpms_protocol_tpms_sefis_m3_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = SEFIS_M3_DATA_BITS,
};

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    // Sefis M3 is FSK where the frame is RAW half-symbols: a fixed 32-bit sync
    // word in the raw bits, then the 72-bit payload is Manchester-encoded (mirrors
    // rtl_433 tpms_sefis_m3: bitbuffer_search the raw sync, then
    // bitbuffer_manchester_decode only the data). The 72-bit payload spills its
    // top 8 bits into head_64_bit via subghz_protocol_blocks_add_to_128_bit.
    uint32_t raw_shift; // rolling window of raw half-symbol bits for sync hunt
    bool in_data; // true once the sync matched, collecting Manchester data
    bool have_half; // holding the first half of a Manchester pair
    uint8_t first_half; // the first half-symbol of the current pair
    uint64_t head_64_bit; // payload bits above 64 (72-bit frame)
} TPMSProtocolDecoderSefisM3;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderSefisM3;

const SubGhzProtocolDecoder tpms_protocol_tpms_sefis_m3_decoder = {
    .alloc = tpms_protocol_decoder_tpms_sefis_m3_alloc,
    .free = tpms_protocol_decoder_tpms_sefis_m3_free,
    .feed = tpms_protocol_decoder_tpms_sefis_m3_feed,
    .reset = tpms_protocol_decoder_tpms_sefis_m3_reset,
    .get_hash_data = tpms_protocol_decoder_tpms_sefis_m3_get_hash_data,
    .serialize = tpms_protocol_decoder_tpms_sefis_m3_serialize,
    .deserialize = tpms_protocol_decoder_tpms_sefis_m3_deserialize,
    .get_string = tpms_protocol_decoder_tpms_sefis_m3_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_tpms_sefis_m3_encoder = {
    .alloc = tpms_protocol_encoder_tpms_sefis_m3_alloc,
    .free = tpms_protocol_encoder_tpms_sefis_m3_free,
    .deserialize = tpms_protocol_encoder_tpms_sefis_m3_deserialize,
    .stop = tpms_protocol_encoder_tpms_sefis_m3_stop,
    .yield = tpms_protocol_encoder_tpms_sefis_m3_yield,
};

const SubGhzProtocol tpms_protocol_tpms_sefis_m3 = {
    .name = TPMS_PROTOCOL_SEFIS_M3_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_tpms_sefis_m3_decoder,
    .encoder = &tpms_protocol_tpms_sefis_m3_encoder,
};

// -----------------------------------------------------------------------------
// Shared byte helpers
// -----------------------------------------------------------------------------

// Pressure page -> B4 top-3-bit code, and its inverse, per rtl_433's table
// {7:0, 4:1, 5:2, 2:3}.
static int sefis_m3_page_from_b4(uint8_t b4) {
    switch(b4 >> 5) {
    case 7:
        return 0;
    case 4:
        return 1;
    case 5:
        return 2;
    case 2:
        return 3;
    default:
        return -1;
    }
}

static uint8_t sefis_m3_b4top_from_page(int page) {
    static const uint8_t map[4] = {7, 4, 5, 2};
    return (page >= 0 && page < 4) ? map[page] : 7;
}

static void sefis_m3_analyze_bytes(TPMSBlockGeneric* generic, const uint8_t b[9]) {
    // rtl_433 leaves B0,B1,B3,B6 undecoded ("code"); fold them into the 32-bit id.
    generic->id = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[3] << 8) |
                  (uint32_t)b[6];

    int page = sefis_m3_page_from_b4(b[4]);
    float pressure_kpa = 0.0f;
    if(page >= 0) {
        int code = (page << 13) | ((b[4] & 0x1f) << 8) | b[5];
        pressure_kpa = (float)(code - 0x0e00) / 102.4f;
        if(pressure_kpa < 0.0f) pressure_kpa = 0.0f;
    }
    generic->pressure = pressure_kpa / 100.0f; // store as BAR

    int temp_code = (b[2] + b[5]) & 0xff;
    generic->temperature = (float)(14 + (temp_code & 0x0f));

    generic->battery_low = TPMS_NO_BATT; // rtl_433 exposes no battery field
}

// -----------------------------------------------------------------------------
// Decoder
// -----------------------------------------------------------------------------

void* tpms_protocol_decoder_tpms_sefis_m3_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderSefisM3* instance = calloc(1, sizeof(TPMSProtocolDecoderSefisM3));
    instance->base.protocol = &tpms_protocol_tpms_sefis_m3;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_tpms_sefis_m3_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_tpms_sefis_m3_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderSefisM3* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->raw_shift = 0;
    instance->in_data = false;
    instance->have_half = false;
    instance->first_half = 0;
    instance->head_64_bit = 0;
}

// Extract the 9 payload bytes: head_64_bit low 8 bits = wire B0, decode_data 64
// bits = wire B1..B8; every wire byte is XORed with 0xff (rtl_433's inversion).
static void sefis_m3_get_bytes(TPMSProtocolDecoderSefisM3* instance, uint8_t b[9]) {
    b[0] = (uint8_t)((instance->head_64_bit & 0xFF) ^ 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 1] = (uint8_t)(((instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF) ^ 0xFF);
    }
}

static bool sefis_m3_check_crc(const uint8_t b[9]) {
    uint16_t crc = subghz_protocol_blocks_crc16(b, 7, 0x1021, 0x0000);
    uint16_t crc_recv = ((uint16_t)b[7] << 8) | b[8];
    return crc == crc_recv;
}

// Consume one raw half-symbol bit: hunt the raw 32-bit sync word, then
// Manchester-decode the 72-bit payload (mirrors rtl_433 tpms_sefis_m3).
static void sefis_m3_emit_raw_bit(TPMSProtocolDecoderSefisM3* instance, uint8_t bit) {
    bit &= 1;
    if(!instance->in_data) {
        instance->raw_shift = (instance->raw_shift << 1) | bit;
        if(instance->raw_shift == SEFIS_M3_SYNC) {
            instance->in_data = true;
            instance->have_half = false;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->head_64_bit = 0;
        }
        return;
    }
    // Inside the Manchester payload: pair up half-symbols. A valid pair is a
    // transition (halves differ); the data bit is the 1st half (chips "10"=1,
    // "01"=0). No transition = end/garbage -> resync on the sync word.
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
    uint8_t data_bit = instance->first_half;
    subghz_protocol_blocks_add_to_128_bit(&instance->decoder, data_bit, &instance->head_64_bit);
    if(instance->decoder.decode_count_bit == SEFIS_M3_DATA_BITS) {
        uint8_t b[9];
        sefis_m3_get_bytes(instance, b);
        if(sefis_m3_check_crc(b)) {
            // Store the de-inverted B1..B8 in generic.data (B0 is recovered from
            // the id by the encoder); id + fields via analyze().
            uint64_t data = 0;
            for(int i = 1; i < 9; i++) data = (data << 8) | b[i];
            instance->generic.data = data;
            instance->generic.data_count_bit = SEFIS_M3_DATA_BITS;
            sefis_m3_analyze_bytes(&instance->generic, b);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        instance->in_data = false;
        instance->raw_shift = 0;
    }
}

void tpms_protocol_decoder_tpms_sefis_m3_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderSefisM3* instance = context;

    const uint32_t te = tpms_protocol_tpms_sefis_m3_const.te_short; // 52us half-symbol
    // A gap (or any pulse longer than two half-symbols) ends the burst.
    if(duration >= te * 3) {
        tpms_protocol_decoder_tpms_sefis_m3_reset(instance);
        return;
    }
    uint32_t n = (duration + te / 2) / te;
    if(n < 1) n = 1;
    if(n > 2) n = 2;
    for(uint32_t k = 0; k < n; k++) {
        sefis_m3_emit_raw_bit(instance, level ? 1 : 0);
    }
}

uint8_t tpms_protocol_decoder_tpms_sefis_m3_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderSefisM3* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_tpms_sefis_m3_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderSefisM3* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_tpms_sefis_m3_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderSefisM3* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_tpms_sefis_m3_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_tpms_sefis_m3_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderSefisM3* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX\r\n"
        "Temp:%2.0f C Bar:%2.1f (%.0f kPa)",
        instance->generic.protocol_name,
        (unsigned long)instance->generic.id,
        (double)instance->generic.temperature,
        (double)instance->generic.pressure,
        (double)(instance->generic.pressure * 100.0f));
}

// -----------------------------------------------------------------------------
// Pack (inverse of analyze) — writes wire bytes into generic->data
// -----------------------------------------------------------------------------

void tpms_protocol_tpms_sefis_m3_pack(TPMSBlockGeneric* generic) {
    furi_assert(generic);

    uint8_t b[9] = {0};

    // Pressure: bar -> kPa -> rtl_433 raw code (0x0e00 + kPa*102.4), 15 bits.
    float kpa = generic->pressure * 100.0f;
    int code = (int)(kpa * 102.4f + 0.5f) + 0x0e00;
    if(code < 0) code = 0;
    if(code > 0x7FFF) code = 0x7FFF;
    int page = (code >> 13) & 0x03;
    b[4] = (uint8_t)((sefis_m3_b4top_from_page(page) << 5) | ((code >> 8) & 0x1f));
    b[5] = (uint8_t)(code & 0xff);

    // Temperature: rtl_433 field is 14..29 C -> nibble 0..15. temp_code low nibble
    // = (B2 + B5) & 0x0f, so pick B2 low nibble to hit the target.
    int nib = (int)(generic->temperature + (generic->temperature >= 0 ? 0.5f : -0.5f)) - 14;
    if(nib < 0) nib = 0;
    if(nib > 15) nib = 15;
    b[2] = (uint8_t)((nib - (b[5] & 0x0f)) & 0x0f);

    // Fold the 32-bit id into the four opaque bytes B0,B1,B3,B6.
    b[0] = (uint8_t)((generic->id >> 24) & 0xff);
    b[1] = (uint8_t)((generic->id >> 16) & 0xff);
    b[3] = (uint8_t)((generic->id >> 8) & 0xff);
    b[6] = (uint8_t)(generic->id & 0xff);

    // CRC-16/CCITT (poly 0x1021, init 0x0000) over B0..B6.
    uint16_t crc = subghz_protocol_blocks_crc16(b, 7, 0x1021, 0x0000);
    b[7] = (uint8_t)(crc >> 8);
    b[8] = (uint8_t)(crc & 0xff);

    uint64_t data = 0;
    for(int i = 1; i < 9; i++) data = (data << 8) | b[i]; // B1..B8 (B0 lives in id)
    generic->data = data;
    generic->data_count_bit = SEFIS_M3_DATA_BITS;
}

// -----------------------------------------------------------------------------
// Encoder
// -----------------------------------------------------------------------------
//
// FSK Manchester (IEEE 802.3 / rtl_433 convention): data bit 1 -> first half
// HIGH, second half LOW ("10"); data bit 0 -> first half LOW, second half HIGH
// ("01"). Frame: gap + 0xaa preamble + raw 32-bit sync {0x66,0x99,0x96,0xa6}
// (== Manchester(0x5a9d)) + 72 Manchester-coded, byte-inverted payload bits.
//
// The 72-bit frame is B0..B8; the serialized generic.data holds only B1..B8 (the
// low 64 bits). B0 is recovered from the id's top byte, then the CRC is
// recomputed so B7/B8 are always correct.
//
// Worst case: 1 gap + ~64 preamble + 32 sync + 144 data half-symbols + 1 tail.
#define TPMS_SEFIS_M3_ENCODER_UPLOAD_SIZE 400
#define TPMS_SEFIS_M3_PREAMBLE_BYTES      8

void* tpms_protocol_encoder_tpms_sefis_m3_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderSefisM3* instance = malloc(sizeof(TPMSProtocolEncoderSefisM3));
    instance->base.protocol = &tpms_protocol_tpms_sefis_m3;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_SEFIS_M3_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_tpms_sefis_m3_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderSefisM3* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_tpms_sefis_m3_stop(void* context) {
    TPMSProtocolEncoderSefisM3* instance = context;
    instance->encoder.is_running = false;
}

// Rebuild the full 9-byte frame: B1..B8 from generic.data, B0 from the id's top
// byte, then recompute the CRC-16 over B0..B6 into B7/B8.
static void
    tpms_protocol_encoder_tpms_sefis_m3_build_frame(TPMSBlockGeneric* generic, uint8_t b[9]) {
    for(int i = 0; i < 8; i++) {
        b[i + 1] = (uint8_t)((generic->data >> (8 * (7 - i))) & 0xFF);
    }
    b[0] = (uint8_t)((generic->id >> 24) & 0xFF);
    uint16_t crc = subghz_protocol_blocks_crc16(b, 7, 0x1021, 0x0000);
    b[7] = (uint8_t)(crc >> 8);
    b[8] = (uint8_t)(crc & 0xff);
}

static bool tpms_protocol_encoder_tpms_sefis_m3_get_upload(TPMSProtocolEncoderSefisM3* instance) {
    furi_assert(instance);
    const uint32_t te = tpms_protocol_tpms_sefis_m3_const.te_short;

    uint8_t frame[9];
    tpms_protocol_encoder_tpms_sefis_m3_build_frame(&instance->generic, frame);

    // Wire bytes are byte-inverted before Manchester coding (rtl_433 XORs 0xff).
    uint8_t wire[9];
    for(int i = 0; i < 9; i++) wire[i] = frame[i] ^ 0xFF;

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

// Manchester data bit: bit 1 -> HIGH then LOW ("10"), bit 0 -> LOW then HIGH
// ("01"); the decoder reads the data as the 1st half of each transitioning pair.
#define MAN_BIT(bit)        \
    do {                    \
        if(bit) {           \
            EMIT(true, te); \
            EMIT(false, te);\
        } else {            \
            EMIT(false, te);\
            EMIT(true, te); \
        }                   \
    } while(0)

    EMIT(false, te * 50);

    // 0xaa lead-in preamble — raw half-symbols (alternating), NOT Manchester.
    for(int i = 0; i < TPMS_SEFIS_M3_PREAMBLE_BYTES; i++) {
        for(int b = 7; b >= 0; b--) {
            EMIT(((0xAA >> b) & 0x1) ? true : false, te);
        }
    }

    // Raw 32-bit sync word {0x66,0x99,0x96,0xa6}, MSB first — raw half-symbols.
    static const uint8_t sync[4] = {0x66, 0x99, 0x96, 0xa6};
    for(int i = 0; i < 4; i++) {
        for(int b = 7; b >= 0; b--) {
            EMIT(((sync[i] >> b) & 0x1) ? true : false, te);
        }
    }

    // 72 Manchester payload bits, MSB first (wire[0]..wire[8]).
    for(int i = 0; i < SEFIS_M3_DATA_BITS; i++) {
        int byte = i / 8;
        int bit = 7 - (i % 8);
        MAN_BIT((wire[byte] >> bit) & 0x1);
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
    tpms_protocol_encoder_tpms_sefis_m3_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderSefisM3* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_tpms_sefis_m3_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_SEFIS_M3_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_tpms_sefis_m3_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_tpms_sefis_m3_yield(void* context) {
    TPMSProtocolEncoderSefisM3* instance = context;

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
