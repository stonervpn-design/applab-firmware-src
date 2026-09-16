#include "tpms_elantra2012.h"
#include "../blocks/generic.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSElantra2012"

// Port of rtl_433/src/devices/tpms_elantra2012.c (Kumar Vivek 2019, GPL-2+).
//
// TRW TPMS sensor FCC-id GQ4-44T, seen on Hyundai Elantra 2012 and Honda Civic.
// FSK, IEEE 802.3 Manchester coded, ~49 us half-bit (rtl_433 FSK_PULSE_PCM,
// short_width = long_width = 49, reset_limit 200 us). Frames start with a 16-bit
// raw preamble 0x7155 (= 111000101010101 with a leading pad 0); the 64 data bits
// (8 bytes, 128 raw half-symbols) that follow fit entirely in decode_data.
//
// Wire format (after Manchester-decoding), rtl_433 layout "PPTT IDID IDID FFCC":
//   PP TT II II II II FF CC   (8 bytes total)
//   b[0]      = pressure_raw           -> kPa = raw + 60
//   b[1]      = temperature_raw        -> C   = raw - 50
//   b[2..5]   = 32-bit ID, big-endian (b[2] is MSB)
//   b[6]      = flags ???? ?SBT: S=storage(0x04), B=battery low(0x02), T=triggered(0x01)
//   b[7]      = CRC-8/SMBUS (poly 0x07, init 0x00). rtl_433 checks crc8(b,8)==0,
//               which is equivalent to b[7] == crc8(b[0..6]).

// Raw (pre-Manchester) 16-bit preamble 0x7155, MSB-first, hunted in the RAW
// half-symbol bits (see struct comment). The rolling window is a uint16, so its
// initial-0 top bit supplies the leading pad 0 of 0x7155 for free (the first
// discrete raw bit the decoder sees after a gap is the '1' of the "111..." run).
// 0x8EAA is the same preamble seen on an inverted-polarity FSK demod.
#define ELANTRA2012_PREAMBLE     0x7155U
#define ELANTRA2012_PREAMBLE_INV 0x8EAAU
#define ELANTRA2012_DATA_BITS    64

static const SubGhzBlockConst tpms_protocol_elantra2012_const = {
    .te_short = 49,
    .te_long = 98,
    .te_delta = 25,
    .min_count_bit_for_found = ELANTRA2012_DATA_BITS,
};

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    // Elantra2012 is FSK_PULSE_PCM: the frame begins with a fixed 16-bit preamble
    // 0x7155 in the RAW half-symbol bits (NOT Manchester coded), then the 64-bit
    // payload is Manchester-encoded (128 raw bits). Matches rtl_433: search the
    // 16-bit preamble in raw bits (bitbuffer_search), then bitbuffer_manchester_decode
    // only the data starting at bitpos+16.
    uint16_t raw_shift; // rolling 16-bit window of raw half-symbol bits for the preamble hunt
    bool in_data; // true once the preamble matched, collecting manchester data
    bool data_second; // false=normal preamble (data bit=2nd half, per rtl_433); true=inverted polarity (1st half)
    bool have_half; // holding the first half of a manchester pair
    uint8_t first_half; // the first half-symbol of the current pair
} TPMSProtocolDecoderElantra2012;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderElantra2012;

const SubGhzProtocolDecoder tpms_protocol_tpms_elantra2012_decoder = {
    .alloc = tpms_protocol_decoder_elantra2012_alloc,
    .free = tpms_protocol_decoder_elantra2012_free,
    .feed = tpms_protocol_decoder_elantra2012_feed,
    .reset = tpms_protocol_decoder_elantra2012_reset,
    .get_hash_data = tpms_protocol_decoder_elantra2012_get_hash_data,
    .serialize = tpms_protocol_decoder_elantra2012_serialize,
    .deserialize = tpms_protocol_decoder_elantra2012_deserialize,
    .get_string = tpms_protocol_decoder_elantra2012_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_tpms_elantra2012_encoder = {
    .alloc = tpms_protocol_encoder_elantra2012_alloc,
    .free = tpms_protocol_encoder_elantra2012_free,
    .deserialize = tpms_protocol_encoder_elantra2012_deserialize,
    .stop = tpms_protocol_encoder_elantra2012_stop,
    .yield = tpms_protocol_encoder_elantra2012_yield,
};

const SubGhzProtocol tpms_protocol_tpms_elantra2012 = {
    .name = TPMS_PROTOCOL_ELANTRA2012_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_tpms_elantra2012_decoder,
    .encoder = &tpms_protocol_tpms_elantra2012_encoder,
};

void* tpms_protocol_decoder_elantra2012_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderElantra2012* instance = calloc(1, sizeof(TPMSProtocolDecoderElantra2012));
    instance->base.protocol = &tpms_protocol_tpms_elantra2012;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_elantra2012_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_elantra2012_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderElantra2012* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->raw_shift = 0;
    instance->in_data = false;
    instance->data_second = false;
    instance->have_half = false;
    instance->first_half = 0;
}

static void elantra2012_get_bytes(TPMSProtocolDecoderElantra2012* instance, uint8_t b[8]) {
    // 64-bit frame: b[0] = MSB .. b[7] = LSB of decode_data.
    for(int i = 0; i < 8; i++) {
        b[i] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

static bool elantra2012_check_crc(TPMSProtocolDecoderElantra2012* instance) {
    uint8_t b[8];
    elantra2012_get_bytes(instance, b);
    // rtl_433: crc8(b, 8, 0x07, 0x00) == 0  <=>  b[7] == crc8(b[0..6]).
    return subghz_protocol_blocks_crc8(b, 7, 0x07, 0x00) == b[7];
}

static void elantra2012_analyze(TPMSProtocolDecoderElantra2012* instance) {
    uint8_t b[8];
    elantra2012_get_bytes(instance, b);

    // Pressure: rtl_433 pressure_kPa = b[0] + 60. Store as bar (1 kPa = 0.01 bar).
    instance->generic.pressure = ((float)b[0] + 60.0f) * 0.01f;
    // Temperature: rtl_433 temperature_C = b[1] - 50.
    instance->generic.temperature = (float)((int)b[1] - 50);
    instance->generic.id = ((uint32_t)b[2] << 24) | ((uint32_t)b[3] << 16) | ((uint32_t)b[4] << 8) |
                           (uint32_t)b[5];
    // Flags byte b[6]: battery low is bit 1 (0x02).
    instance->generic.battery_low = (b[6] & 0x02) ? 1 : 0;
}

// Consume one raw half-symbol bit: hunt the 16-bit raw preamble 0x7155 (or the
// inverted-polarity 0x8EAA), then Manchester-decode the 64-bit payload. Matches
// rtl_433 tpms_elantra2012: the preamble is searched in the raw bits, and only the
// data is Manchester. rtl_433's bitbuffer_manchester_decode emits the 2nd half of
// each pair, so the data bit is the 2nd half for the normal preamble and the 1st
// half when the polarity is inverted.
static void elantra2012_emit_raw_bit(TPMSProtocolDecoderElantra2012* instance, uint8_t bit) {
    bit &= 1;
    if(!instance->in_data) {
        instance->raw_shift = (uint16_t)((instance->raw_shift << 1) | bit);
        if(instance->raw_shift == ELANTRA2012_PREAMBLE ||
           instance->raw_shift == ELANTRA2012_PREAMBLE_INV) {
            instance->in_data = true;
            instance->data_second = (instance->raw_shift == ELANTRA2012_PREAMBLE_INV);
            instance->have_half = false;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
        }
        return;
    }
    // Inside the Manchester payload: pair up half-symbols. A valid pair is a
    // transition (halves differ); the data bit is the 2nd half (normal preamble) or
    // the 1st half (inverted polarity). No transition = end/garbage -> resync.
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
    uint8_t data_bit = instance->data_second ? instance->first_half : bit;
    subghz_protocol_blocks_add_bit(&instance->decoder, data_bit);
    if(instance->decoder.decode_count_bit == ELANTRA2012_DATA_BITS) {
        if(elantra2012_check_crc(instance)) {
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            elantra2012_analyze(instance);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        instance->in_data = false;
        instance->raw_shift = 0;
    }
}

void tpms_protocol_decoder_elantra2012_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderElantra2012* instance = context;

    const uint32_t te = tpms_protocol_elantra2012_const.te_short; // 49us half-symbol
    // The raw preamble contains runs of up to three identical half-symbols (the
    // "111000" run of 0x7155); the Manchester payload never exceeds two. rtl_433's
    // reset_limit is 200us (~4*te); end the burst on a gap of >= 4*te.
    if(duration >= te * 4) {
        tpms_protocol_decoder_elantra2012_reset(instance);
        return;
    }
    // Expand this pulse into 1..3 raw half-symbol bits of the current level.
    uint32_t n = (duration + te / 2) / te;
    if(n < 1) n = 1;
    if(n > 3) n = 3;
    for(uint32_t k = 0; k < n; k++) {
        elantra2012_emit_raw_bit(instance, level ? 1 : 0);
    }
}

uint8_t tpms_protocol_decoder_elantra2012_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderElantra2012* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_elantra2012_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderElantra2012* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_elantra2012_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderElantra2012* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_elantra2012_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_elantra2012_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderElantra2012* instance = context;
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
// Invert elantra2012_analyze(). The 64-bit frame b[0..7] fits entirely in
// generic.data (decode_data): b[0]=MSB .. b[7]=LSB.
//   b[0] = pressure raw, kPa = raw+60, bar = kPa*0.01 -> raw = bar*100 - 60
//   b[1] = temperature raw, C = raw-50 -> raw = C+50
//   b[2..5] = 32-bit ID big-endian (from generic.id)
//   b[6] = flags: battery-low bit (0x02) reflects generic.battery_low; other flag
//          bits preserved from any captured frame.
//   b[7] = CRC-8/SMBUS (poly 0x07, init 0x00) over b[0..6]
// NOTE the rtl_433 pressure offset is +60 kPa, so the raw floor is 60 kPa
// (~0.6 bar / ~8.7 PSI): a requested pressure below that clamps to raw 0 and
// decodes back as 0.6 bar. 2.0 bar (200 kPa, raw 140) round-trips exactly.
void tpms_protocol_tpms_elantra2012_pack(TPMSBlockGeneric* g) {
    furi_assert(g);
    // Preserve any captured wire bytes, then overwrite the fields we synthesize.
    uint8_t b[8];
    for(int i = 0; i < 8; i++) {
        b[i] = (uint8_t)((g->data >> (8 * (7 - i))) & 0xFF);
    }

    // pressure: bar = (raw + 60) * 0.01 -> raw = bar*100 - 60
    int p_raw = (int)(g->pressure * 100.0f - 60.0f + (g->pressure * 100.0f >= 60.0f ? 0.5f : -0.5f));
    if(p_raw < 0) p_raw = 0;
    if(p_raw > 0xFF) p_raw = 0xFF;

    int t = (int)(g->temperature + (g->temperature >= 0 ? 0.5f : -0.5f)) + 50; // b[1] = C + 50
    if(t < 0) t = 0;
    if(t > 0xFF) t = 0xFF;

    b[0] = (uint8_t)p_raw;
    b[1] = (uint8_t)t;
    b[2] = (uint8_t)((g->id >> 24) & 0xFF);
    b[3] = (uint8_t)((g->id >> 16) & 0xFF);
    b[4] = (uint8_t)((g->id >> 8) & 0xFF);
    b[5] = (uint8_t)(g->id & 0xFF);
    // b[6] flags: set/clear battery-low bit, keep other captured flag bits.
    if(g->battery_low && g->battery_low != TPMS_NO_BATT) {
        b[6] |= 0x02;
    } else {
        b[6] &= (uint8_t)~0x02;
    }
    b[7] = subghz_protocol_blocks_crc8(b, 7, 0x07, 0x00);

    uint64_t data = 0;
    for(int i = 0; i < 8; i++) data = (data << 8) | b[i];
    g->data = data;
    g->data_count_bit = ELANTRA2012_DATA_BITS;
}

// -----------------------------------------------------------------------------
// Encoder
// -----------------------------------------------------------------------------
//
// FSK. Frame = inter-frame gap + RAW 16-bit preamble 0x7155 (one raw half-symbol
// per bit, MSB first -- NOT Manchester) + 64 Manchester-coded data bits. The
// decoder pairs the payload's raw half-symbols and takes the 2nd half of each
// transitioning pair as the data bit (data_second=false path for the normal
// preamble), matching rtl_433's bitbuffer_manchester_decode:
//   bit 1 -> 1st half LOW,  2nd half HIGH   (LOW-then-HIGH)
//   bit 0 -> 1st half HIGH, 2nd half LOW    (HIGH-then-LOW)
// build_frame recomputes the CRC so a modified/floored frame always validates.
//
// Worst-case entries: 1 gap + 16 raw preamble + 2*64 data + 1 = 146.
#define TPMS_ELANTRA2012_ENCODER_UPLOAD_SIZE 200

void* tpms_protocol_encoder_elantra2012_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderElantra2012* instance = malloc(sizeof(TPMSProtocolEncoderElantra2012));
    instance->base.protocol = &tpms_protocol_tpms_elantra2012;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_ELANTRA2012_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_elantra2012_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderElantra2012* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_elantra2012_stop(void* context) {
    TPMSProtocolEncoderElantra2012* instance = context;
    instance->encoder.is_running = false;
}

// Rebuild the full 8-byte frame from generic.data and RECOMPUTE the CRC so an
// edited/floored frame is always valid (decoder's CRC check passes).
static void
    tpms_protocol_encoder_elantra2012_build_frame(TPMSBlockGeneric* generic, uint8_t b[8]) {
    for(int i = 0; i < 8; i++) {
        b[i] = (uint8_t)((generic->data >> (8 * (7 - i))) & 0xFF);
    }
    b[7] = subghz_protocol_blocks_crc8(b, 7, 0x07, 0x00);
}

static bool tpms_protocol_encoder_elantra2012_get_upload(TPMSProtocolEncoderElantra2012* instance) {
    furi_assert(instance);
    const uint32_t te = tpms_protocol_elantra2012_const.te_short;

    uint8_t frame[8];
    tpms_protocol_encoder_elantra2012_build_frame(&instance->generic, frame);

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

// Manchester data bit (rtl_433 convention: the decoder takes the 2nd half of each
// transitioning pair): 1st half = complement, 2nd half = the data bit.
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

    // Raw 16-bit preamble 0x7155 -- NOT Manchester. The decoder hunts this pattern
    // one raw half-symbol at a time in a 16-bit window (its initial-0 top bit
    // supplies the leading pad 0), so each preamble bit is ONE raw half-symbol
    // (MSB first). Once matched, the decoder pairs the data half-symbols that follow.
    for(int b = 15; b >= 0; b--) {
        EMIT(((ELANTRA2012_PREAMBLE >> b) & 0x1U) ? true : false, te);
    }

    // 64 data bits, MSB first (frame[0]..frame[7])
    for(int i = 0; i < ELANTRA2012_DATA_BITS; i++) {
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
    tpms_protocol_encoder_elantra2012_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderElantra2012* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_elantra2012_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_ELANTRA2012_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_elantra2012_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_elantra2012_yield(void* context) {
    TPMSProtocolEncoderElantra2012* instance = context;

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
