#include "tpms_honda.h"
#include "../blocks/generic.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "TPMSHonda"

// Port of rtl_433/src/devices/tpms_honda.c (Benjamin Larsson 2026, GPL-2+)
//
// Honda (TRW PPA-GF33) TPMS sensor. FCC-ID GQ4-36T, PN 42753-SNA-A830-M1.
// Seen on a 2010 Honda Insight, likely shared with Honda models 2009-2016.
//
// FSK 315 MHz (US) / 433.92 MHz (EU), IEEE 802.3 Manchester coded, ~50 us
// half-bit. Frames start with a 24-bit sync marker 0xDAE354; the 64 data bits
// (8 bytes) that follow fit entirely in decode_data (no head_64_bit needed).
//
// Wire format (after Manchester-decoding):
//   PP TT II II II II FF CC   (8 bytes total)
//   b[0]      = pressure_raw           -> PSI = raw * 0.2
//   b[1]      = temperature_raw        -> C   = raw - 50
//   b[2..5]   = 32-bit ID, big-endian (b[2] is MSB)
//   b[6]      = flags (0xe1 stationary in every observed sample)
//   b[7]      = CRC-8/SMBUS (poly 0x07, init 0x00) over b[0..6]

// Raw (pre-Manchester) 23-bit desync marker {0xda,0xe3,0x54}, MSB-first, hunted
// in the RAW half-symbol bits (see struct comment). 0x128E55 is the same marker
// seen on an inverted-polarity FSK demod. The mask keeps the rolling hunt window
// at 23 bits.
#define HONDA_MARKER      0x6D71AAUL
#define HONDA_MARKER_INV  0x128E55UL
#define HONDA_MARKER_MASK 0x7FFFFFUL
#define HONDA_DATA_BITS   64

static const SubGhzBlockConst tpms_protocol_honda_const = {
    .te_short = 50,
    .te_long = 100,
    .te_delta = 25,
    .min_count_bit_for_found = HONDA_DATA_BITS,
};

typedef enum {
    HondaDecoderStepReset = 0,
    HondaDecoderStepFindPreamble,
    HondaDecoderStepData,
} HondaDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    // Honda is FSK_PULSE_PCM: the frame begins with a fixed 23-bit desync marker
    // {0xda,0xe3,0x54} in the RAW half-symbol bits (NOT Manchester coded -- it ends
    // mid-pair on a deliberately invalid bit1==bit2 transition), then the 64-bit
    // payload is Manchester-encoded (128 raw bits). Matches rtl_433 tpms_honda:
    // search the 23-bit marker in raw bits, then bitbuffer_manchester_decode only
    // the data. The old port ran manchester over EVERYTHING and hunted the marker
    // in the manchester-decoded stream, which never syncs.
    uint32_t raw_shift; // rolling 23-bit window of raw half-symbol bits for the marker hunt
    bool in_data; // true once the marker matched, collecting manchester data
    bool data_second; // false=normal marker (data bit=2nd half, per rtl_433); true=inverted polarity (data bit=1st half)
    bool have_half; // holding the first half of a manchester pair
    uint8_t first_half; // the first half-symbol of the current pair
} TPMSProtocolDecoderHonda;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderHonda;

const SubGhzProtocolDecoder tpms_protocol_honda_decoder = {
    .alloc = tpms_protocol_decoder_honda_alloc,
    .free = tpms_protocol_decoder_honda_free,
    .feed = tpms_protocol_decoder_honda_feed,
    .reset = tpms_protocol_decoder_honda_reset,
    .get_hash_data = tpms_protocol_decoder_honda_get_hash_data,
    .serialize = tpms_protocol_decoder_honda_serialize,
    .deserialize = tpms_protocol_decoder_honda_deserialize,
    .get_string = tpms_protocol_decoder_honda_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_honda_encoder = {
    .alloc = tpms_protocol_encoder_honda_alloc,
    .free = tpms_protocol_encoder_honda_free,
    .deserialize = tpms_protocol_encoder_honda_deserialize,
    .stop = tpms_protocol_encoder_honda_stop,
    .yield = tpms_protocol_encoder_honda_yield,
};

const SubGhzProtocol tpms_protocol_honda = {
    .name = TPMS_PROTOCOL_HONDA_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_honda_decoder,
    .encoder = &tpms_protocol_honda_encoder,
};

void* tpms_protocol_decoder_honda_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderHonda* instance = calloc(1, sizeof(TPMSProtocolDecoderHonda));
    instance->base.protocol = &tpms_protocol_honda;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_honda_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_honda_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderHonda* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->raw_shift = 0;
    instance->in_data = false;
    instance->data_second = false;
    instance->have_half = false;
    instance->first_half = 0;
}

static void honda_get_bytes(TPMSProtocolDecoderHonda* instance, uint8_t b[8]) {
    // 64-bit frame: b[0] = MSB .. b[7] = LSB of decode_data.
    for(int i = 0; i < 8; i++) {
        b[i] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

static bool honda_check_crc(TPMSProtocolDecoderHonda* instance) {
    uint8_t b[8];
    honda_get_bytes(instance, b);
    return subghz_protocol_blocks_crc8(b, 7, 0x07, 0x00) == b[7];
}

static bool honda_analyze(TPMSProtocolDecoderHonda* instance) {
    uint8_t b[8];
    honda_get_bytes(instance, b);

    int pressure_raw = b[0];
    // Reject TRW GQ4-44T / Elantra2012 overlap: those match this structure but
    // decode here as implausible nonzero sub-10 PSI pressure (rtl_433 guard).
    if(pressure_raw > 0 && pressure_raw < 50) {
        return false;
    }

    float pressure_psi = (float)pressure_raw * 0.2f;
    instance->generic.pressure = pressure_psi * 0.0689476f; // PSI -> bar
    instance->generic.temperature = (float)((int)b[1] - 50);
    instance->generic.id = ((uint32_t)b[2] << 24) | ((uint32_t)b[3] << 16) | ((uint32_t)b[4] << 8) |
                           (uint32_t)b[5];
    instance->generic.battery_low = TPMS_NO_BATT;
    return true;
}

// Consume one raw half-symbol bit: hunt the 23-bit raw marker (0x6D71AA, or the
// inverted-polarity 0x128E55), then Manchester-decode the 64-bit payload. Matches
// rtl_433 tpms_honda: the marker is searched in the raw bits, and only the data is
// Manchester. rtl_433's bitbuffer_manchester_decode emits the 2nd half of each
// pair, so the data bit is the 2nd half for the normal marker and the 1st half
// when the polarity is inverted.
static void honda_emit_raw_bit(TPMSProtocolDecoderHonda* instance, uint8_t bit) {
    bit &= 1;
    if(!instance->in_data) {
        instance->raw_shift = ((instance->raw_shift << 1) | bit) & HONDA_MARKER_MASK;
        if(instance->raw_shift == HONDA_MARKER || instance->raw_shift == HONDA_MARKER_INV) {
            instance->in_data = true;
            instance->data_second = (instance->raw_shift == HONDA_MARKER_INV);
            instance->have_half = false;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
        }
        return;
    }
    // Inside the Manchester payload: pair up half-symbols. A valid pair is a
    // transition (halves differ); the data bit is the 2nd half (normal marker) or
    // the 1st half (inverted polarity). No transition = end/garbage -> resync on
    // the marker.
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
    if(instance->decoder.decode_count_bit == HONDA_DATA_BITS) {
        if(honda_check_crc(instance) && honda_analyze(instance)) {
            instance->generic.data = instance->decoder.decode_data;
            instance->generic.data_count_bit = instance->decoder.decode_count_bit;
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        instance->in_data = false;
        instance->raw_shift = 0;
    }
}

void tpms_protocol_decoder_honda_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderHonda* instance = context;

    const uint32_t te = tpms_protocol_honda_const.te_short; // 50us half-symbol
    // The raw marker contains runs of up to three identical half-symbols (the
    // Manchester payload, by contrast, never exceeds two), so a valid pulse can be
    // as long as 3*te. rtl_433's reset_limit is 200us; end the burst only on a
    // gap of >= 4*te.
    if(duration >= te * 4) {
        tpms_protocol_decoder_honda_reset(instance);
        return;
    }
    // Expand this pulse into 1..3 raw half-symbol bits of the current level.
    uint32_t n = (duration + te / 2) / te;
    if(n < 1) n = 1;
    if(n > 3) n = 3;
    for(uint32_t k = 0; k < n; k++) {
        honda_emit_raw_bit(instance, level ? 1 : 0);
    }
}

uint8_t tpms_protocol_decoder_honda_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderHonda* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_honda_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderHonda* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_honda_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderHonda* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, tpms_protocol_honda_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_honda_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderHonda* instance = context;
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
// Invert honda_analyze(). The 64-bit frame b[0..7] fits entirely in generic.data
// (decode_data), so unlike Citroen there is no head-spill: b[0]=MSB .. b[7]=LSB.
//   b[0] = pressure raw, PSI = raw*0.2, bar = PSI*0.0689476 -> raw = bar/0.01378952
//   b[1] = temperature raw, C = raw-50 -> raw = C+50
//   b[2..5] = 32-bit ID big-endian (from generic.id)
//   b[6] = flags (preserved from any captured frame, else 0)
//   b[7] = CRC-8/SMBUS (poly 0x07, init 0x00) over b[0..6]
// The decoder's analyze() rejects 0 < pressure_raw < 50, so a nonzero pressure
// that rounds below raw 50 (~0.69 bar) will not round-trip; 0 bar (raw 0) is fine.
void tpms_protocol_honda_pack(TPMSBlockGeneric* g) {
    furi_assert(g);
    // Preserve any captured wire bytes, then overwrite the fields we synthesize.
    uint8_t b[8];
    for(int i = 0; i < 8; i++) {
        b[i] = (uint8_t)((g->data >> (8 * (7 - i))) & 0xFF);
    }

    // pressure: bar = raw*0.2*0.0689476 -> raw = bar/(0.2*0.0689476)
    int p_raw = (int)(g->pressure / (0.2f * 0.0689476f) + 0.5f);
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
    // b[6] flags kept as-captured (decoder does not validate it).
    b[7] = subghz_protocol_blocks_crc8(b, 7, 0x07, 0x00);

    uint64_t data = 0;
    for(int i = 0; i < 8; i++) data = (data << 8) | b[i];
    g->data = data;
    g->data_count_bit = HONDA_DATA_BITS;
}

// -----------------------------------------------------------------------------
// Encoder
// -----------------------------------------------------------------------------
//
// FSK. Frame = inter-frame gap + RAW 23-bit desync marker 0x6D71AA (one raw
// half-symbol per bit, MSB first -- NOT Manchester) + 64 Manchester-coded data
// bits. The decoder pairs the payload's raw half-symbols and takes the 2nd half
// of each transitioning pair as the data bit (data_second=false path for the
// normal marker), so the Manchester convention here is the inverse of Citroen's:
//   bit 1 -> 1st half LOW,  2nd half HIGH   (LOW-then-HIGH)
//   bit 0 -> 1st half HIGH, 2nd half LOW    (HIGH-then-LOW)
// build_frame recomputes the CRC so a modified/0-PSI frame always validates.
//
// Worst-case entries: 1 gap + 23 raw preamble + 2*64 data + 1 = 153.
#define TPMS_HONDA_ENCODER_UPLOAD_SIZE 200

void* tpms_protocol_encoder_honda_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderHonda* instance = malloc(sizeof(TPMSProtocolEncoderHonda));
    instance->base.protocol = &tpms_protocol_honda;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_HONDA_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_honda_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderHonda* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_honda_stop(void* context) {
    TPMSProtocolEncoderHonda* instance = context;
    instance->encoder.is_running = false;
}

// Rebuild the full 8-byte frame from generic.data and RECOMPUTE the CRC so an
// edited/0-PSI frame is always valid (decoder's CRC check passes).
static void tpms_protocol_encoder_honda_build_frame(TPMSBlockGeneric* generic, uint8_t b[8]) {
    for(int i = 0; i < 8; i++) {
        b[i] = (uint8_t)((generic->data >> (8 * (7 - i))) & 0xFF);
    }
    b[7] = subghz_protocol_blocks_crc8(b, 7, 0x07, 0x00);
}

static bool tpms_protocol_encoder_honda_get_upload(TPMSProtocolEncoderHonda* instance) {
    furi_assert(instance);
    const uint32_t te = tpms_protocol_honda_const.te_short;

    uint8_t frame[8];
    tpms_protocol_encoder_honda_build_frame(&instance->generic, frame);

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

// Manchester data bit (Honda/rtl_433 convention: the decoder takes the 2nd half of
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

    // Raw 23-bit desync marker 0x6D71AA -- NOT Manchester. The decoder hunts this
    // pattern one raw half-symbol at a time in a 23-bit window, so each marker bit
    // is ONE raw half-symbol (MSB first). Once the 23rd bit matches, the decoder
    // starts pairing the data half-symbols that follow.
    for(int b = 22; b >= 0; b--) {
        EMIT(((HONDA_MARKER >> b) & 0x1UL) ? true : false, te);
    }

    // 64 data bits, MSB first (frame[0]..frame[7])
    for(int i = 0; i < HONDA_DATA_BITS; i++) {
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
    tpms_protocol_encoder_honda_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderHonda* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_honda_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_HONDA_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_honda_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_honda_yield(void* context) {
    TPMSProtocolEncoderHonda* instance = context;

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
