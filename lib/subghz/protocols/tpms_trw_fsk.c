#include "tpms_trw_fsk.h"
#include "../blocks/generic.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSTrwFsk"

// Port of rtl_433/src/devices/tpms_trw.c, decoder "tpms_trw_fsk"
// (Bruno OCTAU @ProfBoc75 2025, GPL-2+).
//
// TRW TPMS Sensor (FCC-ID GQ4-70T), Chrysler 2014-2022, OEM + Clone models.
// rtl_433 r_device: FSK_PULSE_MANCHESTER_ZEROBIT, short=long=52 us, reset=150 us.
//
// The whole frame is IEEE-802.3 Manchester coded (like tpms_honda). rtl_433
// Manchester-decodes first, then searches the DECODED bitstream for the 16-bit
// FSK preamble 0x7FFF, skips it (pos += 16) and extracts the payload bytes.
//
// Wire format (after Manchester-decoding, MSB-first), 10 CRC-covered bytes:
//   b[0]     = mode / model
//   b[1..4]  = 32-bit sensor ID (b[1] MSB)
//   b[5]     = flags (high nibble) + sequence number (low nibble)
//   b[6]     = pressure raw          -> PSI = raw * 0.4   (i.e. raw = PSI * 2.5)
//   b[7]     = temperature raw       -> C   = raw - 50
//   b[8]     = motion flags          (0x0e = Parked, else Moving)
//   b[9]     = CRC-8/SMBUS (poly 0x07, init 0x00) over b[0..8]
// rtl_433 also reads a trailing b[10] high nibble (oem_model) that is NOT
// CRC-covered; we decode only the 80 CRC-covered bits (b[0..9]).
//
// Preamble 0x7FFF is a leading 0 followed by fifteen 1 bits in the Manchester-
// decoded stream (matches rtl_433 preamble_fsk = {0x7f,0xff}).

#define TRW_FSK_PREAMBLE  0x7FFFU
#define TRW_FSK_DATA_BITS 80

static const SubGhzBlockConst tpms_protocol_tpms_trw_fsk_const = {
    .te_short = 52,
    .te_long = 104,
    .te_delta = 25,
    .min_count_bit_for_found = TRW_FSK_DATA_BITS,
};

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    // The entire frame is Manchester; pair raw half-symbols continuously (phase is
    // established by the inter-frame gap, which resets the decoder). Each valid pair
    // is a transition and its data bit is the 2nd half (rtl_433 zerobit / 802.3
    // convention, same as tpms_honda). Preamble 0x7FFF is hunted in the DECODED
    // bit stream; the 80-bit payload then spills its top 16 bits into head_64_bit.
    bool have_half; // holding the first half of a manchester pair
    uint8_t first_half; // the first half-symbol of the current pair
    bool preamble_found; // true once 0x7FFF matched in the decoded stream
    uint16_t pre_shift; // rolling 16-bit window of decoded bits for the preamble hunt
    uint64_t head_64_bit; // payload bits above 64 (80-bit frame)
} TPMSProtocolDecoderTrwFsk;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderTrwFsk;

const SubGhzProtocolDecoder tpms_protocol_tpms_trw_fsk_decoder = {
    .alloc = tpms_protocol_decoder_tpms_trw_fsk_alloc,
    .free = tpms_protocol_decoder_tpms_trw_fsk_free,
    .feed = tpms_protocol_decoder_tpms_trw_fsk_feed,
    .reset = tpms_protocol_decoder_tpms_trw_fsk_reset,
    .get_hash_data = tpms_protocol_decoder_tpms_trw_fsk_get_hash_data,
    .serialize = tpms_protocol_decoder_tpms_trw_fsk_serialize,
    .deserialize = tpms_protocol_decoder_tpms_trw_fsk_deserialize,
    .get_string = tpms_protocol_decoder_tpms_trw_fsk_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_tpms_trw_fsk_encoder = {
    .alloc = tpms_protocol_encoder_tpms_trw_fsk_alloc,
    .free = tpms_protocol_encoder_tpms_trw_fsk_free,
    .deserialize = tpms_protocol_encoder_tpms_trw_fsk_deserialize,
    .stop = tpms_protocol_encoder_tpms_trw_fsk_stop,
    .yield = tpms_protocol_encoder_tpms_trw_fsk_yield,
};

const SubGhzProtocol tpms_protocol_tpms_trw_fsk = {
    .name = TPMS_PROTOCOL_TRW_FSK_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_tpms_trw_fsk_decoder,
    .encoder = &tpms_protocol_tpms_trw_fsk_encoder,
};

void* tpms_protocol_decoder_tpms_trw_fsk_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderTrwFsk* instance = calloc(1, sizeof(TPMSProtocolDecoderTrwFsk));
    instance->base.protocol = &tpms_protocol_tpms_trw_fsk;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_tpms_trw_fsk_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_tpms_trw_fsk_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderTrwFsk* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->have_half = false;
    instance->first_half = 0;
    instance->preamble_found = false;
    instance->pre_shift = 0;
    instance->head_64_bit = 0;
}

static void trw_fsk_get_bytes(TPMSProtocolDecoderTrwFsk* instance, uint8_t b[10]) {
    // 80 bits total: head_64_bit lower 16 bits = b[0..1] (MSB-first),
    // decode_data 64 bits = b[2..9].
    b[0] = (uint8_t)((instance->head_64_bit >> 8) & 0xFF);
    b[1] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 2] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

static bool trw_fsk_check_crc(TPMSProtocolDecoderTrwFsk* instance) {
    uint8_t b[10];
    trw_fsk_get_bytes(instance, b);
    // rtl_433: crc8(b, 10, 0x07, 0x00) == 0  <=>  b[9] == crc8(b[0..8]).
    return subghz_protocol_blocks_crc8(b, 9, 0x07, 0x00) == b[9];
}

static void trw_fsk_analyze(TPMSProtocolDecoderTrwFsk* instance) {
    uint8_t b[10];
    trw_fsk_get_bytes(instance, b);
    instance->generic.id =
        ((uint32_t)b[1] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 8) | (uint32_t)b[4];
    // Pressure: PSI = raw * 0.4; store as bar (PSI -> bar = *0.0689476).
    instance->generic.pressure = ((float)b[6] * 0.4f) * 0.0689476f;
    instance->generic.temperature = (float)((int)b[7] - 50);
    // No battery field in the TRW frame.
    instance->generic.battery_low = TPMS_NO_BATT;
}

// Feed one decoded Manchester bit: hunt the 16-bit preamble 0x7FFF, then collect
// the 80-bit CRC-covered payload (top 16 bits spill into head_64_bit).
static void trw_fsk_emit_data_bit(TPMSProtocolDecoderTrwFsk* instance, uint8_t bit) {
    bit &= 1;
    if(!instance->preamble_found) {
        instance->pre_shift = (uint16_t)((instance->pre_shift << 1) | bit);
        if(instance->pre_shift == TRW_FSK_PREAMBLE) {
            instance->preamble_found = true;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->head_64_bit = 0;
        }
        return;
    }
    subghz_protocol_blocks_add_to_128_bit(&instance->decoder, bit, &instance->head_64_bit);
    if(instance->decoder.decode_count_bit == TRW_FSK_DATA_BITS) {
        if(trw_fsk_check_crc(instance)) {
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            trw_fsk_analyze(instance);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        // Ready to hunt the next frame within the same burst.
        instance->preamble_found = false;
        instance->pre_shift = 0;
    }
}

// Consume one raw half-symbol bit: pair half-symbols into Manchester data bits
// (data bit = 2nd half of each transitioning pair, rtl_433 zerobit convention),
// feeding each decoded bit to the preamble hunt / payload collector.
static void trw_fsk_emit_raw_half(TPMSProtocolDecoderTrwFsk* instance, uint8_t bit) {
    bit &= 1;
    if(!instance->have_half) {
        instance->first_half = bit;
        instance->have_half = true;
        return;
    }
    instance->have_half = false;
    if(instance->first_half == bit) {
        // No transition: Manchester phase error. Slip one half-symbol to re-align
        // and drop any in-progress preamble hunt.
        instance->first_half = bit;
        instance->have_half = true;
        instance->preamble_found = false;
        instance->pre_shift = 0;
        return;
    }
    trw_fsk_emit_data_bit(instance, bit); // data bit = 2nd half
}

void tpms_protocol_decoder_tpms_trw_fsk_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderTrwFsk* instance = context;

    const uint32_t te = tpms_protocol_tpms_trw_fsk_const.te_short; // 52us half-symbol
    // Manchester runs are at most two half-symbols long; a longer pulse is the
    // inter-frame gap (rtl_433 reset_limit 150us ~= 3*te) -> end the burst.
    if(duration >= te * 3) {
        tpms_protocol_decoder_tpms_trw_fsk_reset(instance);
        return;
    }
    // Expand this pulse into 1 or 2 raw half-symbol bits of the current level.
    uint32_t n = (duration + te / 2) / te;
    if(n < 1) n = 1;
    if(n > 2) n = 2;
    for(uint32_t k = 0; k < n; k++) {
        trw_fsk_emit_raw_half(instance, level ? 1 : 0);
    }
}

uint8_t tpms_protocol_decoder_tpms_trw_fsk_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderTrwFsk* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_tpms_trw_fsk_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderTrwFsk* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_tpms_trw_fsk_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderTrwFsk* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_tpms_trw_fsk_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_tpms_trw_fsk_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderTrwFsk* instance = context;
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
// Packer
// -----------------------------------------------------------------------------
//
// Invert trw_fsk_analyze(). generic.data holds the low 64 bits (b[2..9]); the
// full 32-bit ID is persisted separately (generic.id), so the encoder recovers
// b[1] (ID MSB) from it. b[0] (mode) is not persisted and is emitted as 0 (the
// decoder does not surface it, and the CRC is recomputed so any b[0] validates).
//   b[6] = pressure raw, PSI = raw*0.4, bar = PSI*0.0689476 -> raw = bar/(0.4*0.0689476)
//   b[7] = temperature raw, C = raw-50 -> raw = C+50
//   b[9] = CRC-8/SMBUS (poly 0x07, init 0x00) over b[0..8]
void tpms_protocol_tpms_trw_fsk_pack(TPMSBlockGeneric* g) {
    furi_assert(g);
    // Preserve any captured b[2..9] (mode/flags/motion), then overwrite fields.
    uint8_t b[10];
    b[0] = 0; // mode (not persisted)
    b[1] = (uint8_t)((g->id >> 24) & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 2] = (uint8_t)((g->data >> (8 * (7 - i))) & 0xFF);
    }
    // ID bytes b[2..4] come from generic.id (authoritative).
    b[2] = (uint8_t)((g->id >> 16) & 0xFF);
    b[3] = (uint8_t)((g->id >> 8) & 0xFF);
    b[4] = (uint8_t)(g->id & 0xFF);

    // pressure: bar = raw*0.4*0.0689476 -> raw = bar/(0.4*0.0689476)
    int p_raw = (int)(g->pressure / (0.4f * 0.0689476f) + 0.5f);
    if(p_raw < 0) p_raw = 0;
    if(p_raw > 0xFF) p_raw = 0xFF;
    b[6] = (uint8_t)p_raw;

    int t = (int)(g->temperature + (g->temperature >= 0 ? 0.5f : -0.5f)) + 50; // b[7] = C + 50
    if(t < 0) t = 0;
    if(t > 0xFF) t = 0xFF;
    b[7] = (uint8_t)t;

    b[9] = subghz_protocol_blocks_crc8(b, 9, 0x07, 0x00);

    // generic.data holds b[2..9] (low 64 bits of the 80-bit frame).
    uint64_t data = 0;
    for(int i = 2; i < 10; i++) data = (data << 8) | b[i];
    g->data = data;
    g->data_count_bit = TRW_FSK_DATA_BITS;
}

// -----------------------------------------------------------------------------
// Encoder
// -----------------------------------------------------------------------------
//
// FSK, whole frame Manchester (802.3): inter-frame gap + 16-bit preamble 0x7FFF
// (Manchester-coded) + 80 Manchester-coded data bits. The decoder takes the 2nd
// half of each transitioning pair as the data bit, so:
//   bit 1 -> 1st half LOW,  2nd half HIGH   (LOW-then-HIGH)
//   bit 0 -> 1st half HIGH, 2nd half LOW    (HIGH-then-LOW)
// build_frame recomputes the CRC so a modified / 0-PSI frame always validates.
//
// The 80-bit frame is b[0..9]; generic.data holds b[2..9] and b[1] is recovered
// from generic.id. b[0] (mode) is emitted as 0.
//
// Worst-case entries: 1 gap + 2*(16 preamble + 80 data) + 1 = 194.
#define TPMS_TRW_FSK_ENCODER_UPLOAD_SIZE 220

void* tpms_protocol_encoder_tpms_trw_fsk_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderTrwFsk* instance = malloc(sizeof(TPMSProtocolEncoderTrwFsk));
    instance->base.protocol = &tpms_protocol_tpms_trw_fsk;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_TRW_FSK_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_tpms_trw_fsk_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderTrwFsk* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_tpms_trw_fsk_stop(void* context) {
    TPMSProtocolEncoderTrwFsk* instance = context;
    instance->encoder.is_running = false;
}

// Rebuild the full 10-byte frame. b[2..9] come from generic.data; b[1] is the ID
// MSB (from generic.id); b[0] (mode) is 0. RECOMPUTE the CRC so an edited / 0-PSI
// frame is always valid (the decoder's CRC check passes).
static void
    tpms_protocol_encoder_tpms_trw_fsk_build_frame(TPMSBlockGeneric* generic, uint8_t b[10]) {
    b[0] = 0x00;
    b[1] = (uint8_t)((generic->id >> 24) & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 2] = (uint8_t)((generic->data >> (8 * (7 - i))) & 0xFF);
    }
    b[9] = subghz_protocol_blocks_crc8(b, 9, 0x07, 0x00);
}

static bool tpms_protocol_encoder_tpms_trw_fsk_get_upload(TPMSProtocolEncoderTrwFsk* instance) {
    furi_assert(instance);
    const uint32_t te = tpms_protocol_tpms_trw_fsk_const.te_short;

    uint8_t frame[10];
    tpms_protocol_encoder_tpms_trw_fsk_build_frame(&instance->generic, frame);

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

// Manchester data bit (rtl_433 zerobit / 802.3: the decoder takes the 2nd half of
// each transitioning pair): 1st half = complement, 2nd half = the data bit.
#define MAN_BIT(bit)                 \
    do {                             \
        if(bit) {                    \
            EMIT(false, te);         \
            EMIT(true, te);          \
        } else {                     \
            EMIT(true, te);          \
            EMIT(false, te);         \
        }                            \
    } while(0)

    EMIT(false, te * 50);

    // 16-bit preamble 0x7FFF, Manchester-coded (MSB first) -- the decoder hunts
    // this pattern in the DECODED bit stream, so it is NOT a raw pattern.
    for(int i = 15; i >= 0; i--) {
        MAN_BIT((TRW_FSK_PREAMBLE >> i) & 0x1);
    }

    // 80 data bits, MSB first (frame[0]..frame[9])
    for(int i = 0; i < TRW_FSK_DATA_BITS; i++) {
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
    tpms_protocol_encoder_tpms_trw_fsk_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderTrwFsk* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_tpms_trw_fsk_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_TRW_FSK_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_tpms_trw_fsk_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_tpms_trw_fsk_yield(void* context) {
    TPMSProtocolEncoderTrwFsk* instance = context;

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
