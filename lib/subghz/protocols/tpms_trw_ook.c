#include "tpms_trw_ook.h"
#include "../blocks/generic.h"

#define TAG "TPMSTRWook"

// Port of rtl_433/src/devices/tpms_trw.c decoder "tpms_trw_ook"
//   (C) 2025 Bruno OCTAU (@ProfBoc75), GPL-2+.  FCC-ID: GQ4-70T.
//   Used in Chrysler cars 2014..2022 (OEM and Clone-OEM models).
//
// Modulation: OOK, OOK_PULSE_MANCHESTER_ZEROBIT, short=long=52 us, reset 150 us.
//
// rtl_433 Manchester-decodes the whole burst and searches the 16-bit preamble
// 0x0001 in the decoded stream, then extracts 11 bytes (88 bits). Manchester of
// the decoded preamble 0x0001 (fifteen 0s + one 1; 0->raw"01", 1->raw"10") is the
// raw half-symbol stream 0x55 0x55 0x55 0x56 - so, exactly like tpms_citroen, we
// hunt the trailing raw word 0x5556 (or its RF-inverted twin 0xAAA9) in the raw
// half-symbol stream, which locks the Manchester pair phase, then decode the data
// as transitioning pairs whose value is the FIRST half-symbol.
//
// Wire format after Manchester-decode (rtl_433 layout, MSB-first bytes b[0..10]):
//   MM II II II II FN PP TT SS CC X.
//     b[0]      MM  Mode/Model: 0x5c, 0x5d or 0x5e (0x5c for Clone-OEM)
//     b[1..4]   II  32-bit sensor ID
//     b[5] hi   F   flag status (0x6 drop, 0x9 increase, 0xb/0xc in motion)
//     b[5] lo   N   sequence number
//     b[6]      PP  pressure, PSI = PP * 0.4  (raw scale 2.5)
//     b[7]      TT  temperature, C = TT - 50
//     b[8]      SS  motion status (0x0e parked, other = moving)
//     b[9]      CC  CRC-8/SMBUS, poly 0x07, init 0x00, over b[0..8]
//     b[10] hi  X   trailing nibble: 0x4 OEM, 0x0 Clone-OEM (not covered by CRC)
//
// rtl_433 validates with crc8(b,10,0x07,0x00)==0, i.e. b[9]==crc8(b[0..8]).

#define TRW_OOK_PREAMBLE     0x5556U // raw half-symbols of decoded 0x0001 tail
#define TRW_OOK_PREAMBLE_INV 0xAAA9U // RF-inverted polarity
#define TRW_OOK_FULL_BITS    88 // 11 bytes
#define TRW_OOK_FULL_BYTES   11

// PSI <-> bar. rtl_433 works in PSI; TPMSBlockGeneric stores bar.
#define TRW_PSI_PER_BAR 14.5037738f

static const SubGhzBlockConst tpms_protocol_tpms_trw_ook_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 20,
    .min_count_bit_for_found = TRW_OOK_FULL_BITS,
};

typedef enum {
    TrwOokStepReset = 0, // hunting the raw preamble
    TrwOokStepData, // collecting Manchester-decoded data bits
} TrwOokStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;

    uint16_t header_shift; // rolling raw half-symbol register for preamble hunt
    bool data_second; // false: data = 1st half (0x5556); true: 2nd half (0xAAA9)
    bool half_have; // holding the first half of the current Manchester pair
    uint8_t half_bit; // stored first half-symbol
    uint64_t head_64_bit; // payload bits above 64 (88-bit frame -> top 24 bits)
} TPMSProtocolDecoderTrwOok;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderTrwOok;

const SubGhzProtocolDecoder tpms_protocol_tpms_trw_ook_decoder = {
    .alloc = tpms_protocol_decoder_tpms_trw_ook_alloc,
    .free = tpms_protocol_decoder_tpms_trw_ook_free,
    .feed = tpms_protocol_decoder_tpms_trw_ook_feed,
    .reset = tpms_protocol_decoder_tpms_trw_ook_reset,
    .get_hash_data = tpms_protocol_decoder_tpms_trw_ook_get_hash_data,
    .serialize = tpms_protocol_decoder_tpms_trw_ook_serialize,
    .deserialize = tpms_protocol_decoder_tpms_trw_ook_deserialize,
    .get_string = tpms_protocol_decoder_tpms_trw_ook_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_tpms_trw_ook_encoder = {
    .alloc = tpms_protocol_encoder_tpms_trw_ook_alloc,
    .free = tpms_protocol_encoder_tpms_trw_ook_free,
    .deserialize = tpms_protocol_encoder_tpms_trw_ook_deserialize,
    .stop = tpms_protocol_encoder_tpms_trw_ook_stop,
    .yield = tpms_protocol_encoder_tpms_trw_ook_yield,
};

const SubGhzProtocol tpms_protocol_tpms_trw_ook = {
    .name = TPMS_PROTOCOL_TRW_OOK_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_tpms_trw_ook_decoder,
    .encoder = &tpms_protocol_tpms_trw_ook_encoder,
};

// -----------------------------------------------------------------------------
// Decoder
// -----------------------------------------------------------------------------

void* tpms_protocol_decoder_tpms_trw_ook_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderTrwOok* instance = calloc(1, sizeof(TPMSProtocolDecoderTrwOok));
    instance->base.protocol = &tpms_protocol_tpms_trw_ook;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_tpms_trw_ook_free(void* context) {
    furi_assert(context);
    free(context);
}

static void trw_ook_reset_state(TPMSProtocolDecoderTrwOok* instance) {
    instance->decoder.parser_step = TrwOokStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->header_shift = 0;
    instance->data_second = false;
    instance->half_have = false;
    instance->half_bit = 0;
    instance->head_64_bit = 0;
}

void tpms_protocol_decoder_tpms_trw_ook_reset(void* context) {
    furi_assert(context);
    trw_ook_reset_state(context);
}

// Extract the 88-bit frame into b[0..10], MSB-first. The top 24 bits (b[0..2])
// live in head_64_bit, the low 64 bits (b[3..10]) in decode_data.
static void trw_ook_get_bytes(TPMSProtocolDecoderTrwOok* instance, uint8_t b[TRW_OOK_FULL_BYTES]) {
    const int head_bytes = (TRW_OOK_FULL_BITS - 64) / 8; // 3
    for(int i = 0; i < head_bytes; i++) {
        b[i] = (uint8_t)((instance->head_64_bit >> (8 * (head_bytes - 1 - i))) & 0xFF);
    }
    for(int i = 0; i < 8; i++) {
        b[head_bytes + i] = (uint8_t)((instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF);
    }
}

// rtl_433: crc8(b,10,0x07,0x00)==0  <=>  b[9] == crc8(b[0..8]).
static bool trw_ook_check_crc(const uint8_t* b) {
    return subghz_protocol_blocks_crc8(b, 9, 0x07, 0x00) == b[9];
}

static void trw_ook_analyze(TPMSProtocolDecoderTrwOok* instance, const uint8_t* b) {
    instance->generic.id =
        ((uint32_t)b[1] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 8) | b[4];
    // Pressure: PSI = raw * 0.4, stored as bar.
    instance->generic.pressure = ((float)b[6] * 0.4f) / TRW_PSI_PER_BAR;
    // Temperature: C = raw - 50.
    instance->generic.temperature = (float)((int)b[7] - 50);
    // No battery status is decoded upstream (line commented out in rtl_433).
    instance->generic.battery_low = TPMS_NO_BATT;
}

static void trw_ook_process_raw_bit(TPMSProtocolDecoderTrwOok* instance, uint8_t raw) {
    if(instance->decoder.parser_step == TrwOokStepReset) {
        instance->header_shift = (uint16_t)((instance->header_shift << 1) | (raw & 1));
        if(instance->header_shift == TRW_OOK_PREAMBLE ||
           instance->header_shift == TRW_OOK_PREAMBLE_INV) {
            instance->decoder.parser_step = TrwOokStepData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->head_64_bit = 0;
            instance->half_have = false;
            instance->half_bit = 0;
            instance->data_second = (instance->header_shift == TRW_OOK_PREAMBLE_INV);
        }
        return;
    }

    // TrwOokStepData: pair up half-symbols. A valid pair is a transition (halves
    // differ); the data bit is the 1st half (0x5556) or the 2nd half (0xAAA9).
    if(!instance->half_have) {
        instance->half_bit = raw & 1;
        instance->half_have = true;
        return;
    }
    instance->half_have = false;
    uint8_t second = raw & 1;
    if(instance->half_bit == second) {
        // Manchester coding violation -> resync on the preamble.
        trw_ook_reset_state(instance);
        return;
    }
    uint8_t data_bit = instance->data_second ? second : instance->half_bit;
    subghz_protocol_blocks_add_to_128_bit(&instance->decoder, data_bit, &instance->head_64_bit);

    if(instance->decoder.decode_count_bit == TRW_OOK_FULL_BITS) {
        uint8_t b[TRW_OOK_FULL_BYTES];
        trw_ook_get_bytes(instance, b);
        if(trw_ook_check_crc(b)) {
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            trw_ook_analyze(instance, b);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        trw_ook_reset_state(instance);
    }
}

void tpms_protocol_decoder_tpms_trw_ook_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderTrwOok* instance = context;

    const uint32_t te = tpms_protocol_tpms_trw_ook_const.te_short; // 52 us half-symbol
    // A gap (or any run longer than two half-symbols) ends the burst.
    if(duration >= te * 3) {
        trw_ook_reset_state(instance);
        return;
    }
    // Expand this edge into 1 or 2 raw half-symbol bits of the current level.
    uint32_t n = (duration + te / 2) / te;
    if(n < 1) n = 1;
    if(n > 2) n = 2;
    uint8_t raw = level ? 1 : 0;
    for(uint32_t k = 0; k < n; k++) {
        trw_ook_process_raw_bit(instance, raw);
    }
}

uint8_t tpms_protocol_decoder_tpms_trw_ook_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderTrwOok* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_tpms_trw_ook_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderTrwOok* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_tpms_trw_ook_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderTrwOok* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_tpms_trw_ook_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_tpms_trw_ook_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderTrwOok* instance = context;
    float psi = instance->generic.pressure * TRW_PSI_PER_BAR;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX\r\n"
        "Temp:%2.0f C %.1f PSI %.1f Bar",
        instance->generic.protocol_name,
        instance->generic.id,
        (double)instance->generic.temperature,
        (double)psi,
        (double)instance->generic.pressure);
}

// -----------------------------------------------------------------------------
// Packer + Encoder (TX)
// -----------------------------------------------------------------------------
//
// We transmit the full 88-bit (11-byte) frame; the decoder fires on 88 collected
// bits and validates the CRC-8/SMBUS over b[0..8]. generic.data carries the low
// 64 bits of the frame = b[3..10] (b[3],b[4] = low 16 id bits; b[5]=FN; b[6]=PP;
// b[7]=TT; b[8]=SS; b[9]=CC; b[10]=X). generic.id carries the full 32-bit id, and
// build_frame recovers b[0]=MM, b[1],b[2] (high id) and recomputes b[9]=CRC.
//
// Manchester: data bit = FIRST half-symbol of each transitioning pair (the 0x5556
// convention), so bit 1 => raw high-then-low, bit 0 => raw low-then-high. The
// preamble is emitted as RAW half-symbols (0x55 0x55 0x55 0x56) - NOT Manchester.
//
// Worst-case entries: 1 gap + 32 raw preamble + 2*88 data + 1 tail = 210.
#define TPMS_TRW_OOK_ENCODER_UPLOAD_SIZE 240

void* tpms_protocol_encoder_tpms_trw_ook_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderTrwOok* instance = malloc(sizeof(TPMSProtocolEncoderTrwOok));
    instance->base.protocol = &tpms_protocol_tpms_trw_ook;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_TRW_OOK_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_tpms_trw_ook_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderTrwOok* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_tpms_trw_ook_stop(void* context) {
    TPMSProtocolEncoderTrwOok* instance = context;
    instance->encoder.is_running = false;
}

void tpms_protocol_tpms_trw_ook_pack(TPMSBlockGeneric* g) {
    furi_assert(g);
    // Pressure: PSI = bar * PSI_PER_BAR; raw = PSI * 2.5.
    int p_raw = (int)(g->pressure * TRW_PSI_PER_BAR * 2.5f + 0.5f);
    if(p_raw < 0) p_raw = 0;
    if(p_raw > 0xFF) p_raw = 0xFF;
    // Temperature: raw = C + 50.
    int t = (int)(g->temperature + (g->temperature >= 0 ? 0.5f : -0.5f)) + 50;
    if(t < 0) t = 0;
    if(t > 0xFF) t = 0xFF;

    // generic.data holds the low 64 bits of the frame = b[3..10]. b[9]=CRC and
    // b[10]=X are recomputed/fixed in build_frame; b[3],b[4] (low id) are also
    // rebuilt there from generic.id, so they are placeholders here.
    uint8_t b[8]; // = frame b[3..10]
    b[0] = (uint8_t)((g->id >> 8) & 0xFF); // b[3]
    b[1] = (uint8_t)(g->id & 0xFF); // b[4]
    b[2] = 0x00; // b[5] flags/seq
    b[3] = (uint8_t)p_raw; // b[6] pressure
    b[4] = (uint8_t)t; // b[7] temperature
    b[5] = 0x0e; // b[8] motion status (0x0e = parked)
    b[6] = 0x00; // b[9] CRC (recomputed in build_frame)
    b[7] = 0x40; // b[10] X nibble = 0x4 (OEM)
    uint64_t data = 0;
    for(int i = 0; i < 8; i++) data = (data << 8) | b[i];
    g->data = data;
    g->data_count_bit = TRW_OOK_FULL_BITS;
}

// Rebuild the full 11-byte frame from generic.id + generic.data and recompute the
// CRC so a modified / 0-PSI frame is always valid.
static void tpms_protocol_encoder_tpms_trw_ook_build_frame(
    TPMSBlockGeneric* generic,
    uint8_t b[TRW_OOK_FULL_BYTES]) {
    b[0] = 0x5c; // MM mode/model (Clone-OEM baseline); only CRC-covered
    b[1] = (uint8_t)((generic->id >> 24) & 0xFF);
    b[2] = (uint8_t)((generic->id >> 16) & 0xFF);
    // b[3..10] = low 64 bits of the frame, held in generic.data (MSB-first)
    for(int i = 0; i < 8; i++) {
        b[i + 3] = (generic->data >> (8 * (7 - i))) & 0xFF;
    }
    // CRC-8/SMBUS poly 0x07 init 0x00 over b[0..8], stored in b[9].
    b[9] = subghz_protocol_blocks_crc8(b, 9, 0x07, 0x00);
}

static bool tpms_protocol_encoder_tpms_trw_ook_get_upload(TPMSProtocolEncoderTrwOok* instance) {
    furi_assert(instance);
    const uint32_t te = tpms_protocol_tpms_trw_ook_const.te_short;

    uint8_t frame[TRW_OOK_FULL_BYTES];
    tpms_protocol_encoder_tpms_trw_ook_build_frame(&instance->generic, frame);

    size_t index = 0;
    const size_t max = instance->encoder.size_upload;
    bool last_level = false;
    uint32_t last_duration = 0;

#define EMIT(lvl, dur)                                                  \
    do {                                                                \
        if(last_duration != 0 && last_level == (lvl)) {                 \
            last_duration += (dur);                                     \
        } else {                                                        \
            if(last_duration != 0) {                                    \
                if(index >= max) return false;                          \
                instance->encoder.upload[index++] =                     \
                    level_duration_make(last_level, last_duration);     \
            }                                                           \
            last_level = (lvl);                                         \
            last_duration = (dur);                                      \
        }                                                               \
    } while(0)

// Manchester data bit: 1st half = the data bit, 2nd half = its complement (the
// 0x5556 convention the decoder uses: data = 1st half of each transitioning pair).
#define MAN_BIT(bit)         \
    do {                     \
        if(bit) {            \
            EMIT(true, te);  \
            EMIT(false, te); \
        } else {             \
            EMIT(false, te); \
            EMIT(true, te);  \
        }                    \
    } while(0)

    // Inter-frame gap so consecutive repeats are framed and the decoder resyncs.
    EMIT(false, te * 50);

    // Raw sync preamble 0x55 0x55 0x55 0x56 - NOT Manchester. Each preamble bit is
    // ONE raw half-symbol; the decoder hunts the trailing raw 0x5556 word, which
    // locks the Manchester pair phase for the data that follows. (This is the raw
    // half-symbol stream of rtl_433's decoded 0x0001 OOK preamble.)
    static const uint8_t preamble[4] = {0x55, 0x55, 0x55, 0x56};
    for(int i = 0; i < 4; i++) {
        for(int b = 7; b >= 0; b--) {
            EMIT(((preamble[i] >> b) & 0x1) ? true : false, te);
        }
    }

    // 88 data bits, MSB first (frame[0]..frame[10]).
    for(int i = 0; i < TRW_OOK_FULL_BITS; i++) {
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
    tpms_protocol_encoder_tpms_trw_ook_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderTrwOok* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_tpms_trw_ook_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_TRW_OOK_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_tpms_trw_ook_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_tpms_trw_ook_yield(void* context) {
    TPMSProtocolEncoderTrwOok* instance = context;

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
