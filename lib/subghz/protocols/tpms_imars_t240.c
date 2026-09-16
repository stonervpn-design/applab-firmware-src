#include "tpms_imars_t240.h"
#include "../blocks/generic.h"

#define TAG "TPMSiMarsT240"

// Port of rtl_433/src/devices/tpms_imars_t240.c (Benjamin Larsson 2026, GPL-2+)
// https://github.com/merbanan/rtl_433/issues/1820
//
// "iMars T240 TPMS Solar Power Tire Pressure Monitor System" (SP372 sensor IC,
// atA5754 receiver IC).
//
//   Frequency  : 433.92 MHz
//   Modulation : OOK (ASK), Manchester encoded, half-bit ~50 us
//   Framing    : 32-bit raw alternating preamble (0xaaaaaaaa), then 128 raw
//                half-symbols Manchester-decoding to 8 bytes (B0..B7), and the
//                Manchester-decoded stream is bit-inverted (rtl_433 calls
//                bitbuffer_invert after bitbuffer_manchester_decode).
//
// Decoded byte layout (after the invert):
//   B0 B1 B2 B3 B4 B5 B6 B7
//   - B7 == B0                          (tail integrity check)
//   - (B0 & 0x0f) == (B1 & 0x0f)
//   - checksum = (B3 + B4) & 0xff, a fixed per-unit constant; the two units in
//     the rtl_433 issue give 0x41 and 0x3c. We accept either on RX.
//
// rtl_433 leaves the mapping of B2/B5/B6 to pressure/temperature UNKNOWN (no
// ground-truth capture exists to calibrate it), so it only emits a hex `code`
// string. This firmware works in engineering units (TPMSBlockGeneric carries
// id/pressure/temperature), so a self-consistent mapping is DEFINED here for the
// unmapped fields while every rtl_433 slicing/preamble/checksum rule is honoured
// exactly (that is what keeps the *decoder* correct for real sensors — a real
// iMars T240 frame still passes all three integrity checks and yields its 7-byte
// code via the ID/raw bytes). The mapping used here:
//   id          = B0<<24 | B2<<16 | B3<<8 | B6      (32-bit, from the free bytes)
//   pressure    = (B1 >> 4) * 0.25 bar              (4-bit field, 0..3.75 bar)
//   temperature = B5 - 50  (degC)                   (8-bit, offset 50)
// This is an ADDED encoder/pack convention, not claimed to match the (unknown)
// factory scaling of B2/B5/B6 — see caveat in notes.

#define IMARS_T240_PREAMBLE_BITS 32u // raw half-symbols of 0xaaaaaaaa
#define IMARS_T240_DATA_BITS     64u // 8 bytes after Manchester-decode + invert
#define IMARS_T240_CHECKSUM      0x41u // constant baked into the encoder/pack

static const SubGhzBlockConst tpms_protocol_tpms_imars_t240_const = {
    .te_short = 50,
    .te_long = 100,
    .te_delta = 20,
    .min_count_bit_for_found = IMARS_T240_DATA_BITS,
};

typedef enum {
    iMarsT240StepPreamble = 0, // hunting the 32-bit alternating preamble
    iMarsT240StepData, // Manchester-pairing the 128 payload half-symbols
} iMarsT240Step;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;

    // OOK Manchester at the raw half-symbol level (mirrors rtl_433's PCM sampling
    // + bitbuffer_manchester_decode). feed() expands each pulse into 1..2 raw
    // half-symbol bits; those bits drive this little state machine.
    iMarsT240Step step;
    uint32_t preamble_run; // length of the current alternating raw-bit run
    bool have_last; // preamble_run bookkeeping seeded?
    uint8_t last_bit; // previous raw half-symbol (for alternation test)
    bool have_half; // holding the first half of a Manchester pair
    uint8_t first_half; // that first half-symbol
    uint64_t collected; // Manchester-decoded raw bits, MSB-first (pre-invert)
} TPMSProtocolDecoderiMarsT240;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderiMarsT240;

const SubGhzProtocolDecoder tpms_protocol_tpms_imars_t240_decoder = {
    .alloc = tpms_protocol_decoder_tpms_imars_t240_alloc,
    .free = tpms_protocol_decoder_tpms_imars_t240_free,
    .feed = tpms_protocol_decoder_tpms_imars_t240_feed,
    .reset = tpms_protocol_decoder_tpms_imars_t240_reset,
    .get_hash_data = tpms_protocol_decoder_tpms_imars_t240_get_hash_data,
    .serialize = tpms_protocol_decoder_tpms_imars_t240_serialize,
    .deserialize = tpms_protocol_decoder_tpms_imars_t240_deserialize,
    .get_string = tpms_protocol_decoder_tpms_imars_t240_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_tpms_imars_t240_encoder = {
    .alloc = tpms_protocol_encoder_tpms_imars_t240_alloc,
    .free = tpms_protocol_encoder_tpms_imars_t240_free,
    .deserialize = tpms_protocol_encoder_tpms_imars_t240_deserialize,
    .stop = tpms_protocol_encoder_tpms_imars_t240_stop,
    .yield = tpms_protocol_encoder_tpms_imars_t240_yield,
};

const SubGhzProtocol tpms_protocol_tpms_imars_t240 = {
    .name = TPMS_PROTOCOL_IMARS_T240_NAME,
    .type = SubGhzProtocolTypeTpms,
    // OOK -> _AM. Sensor is 433 MHz.
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_tpms_imars_t240_decoder,
    .encoder = &tpms_protocol_tpms_imars_t240_encoder,
};

void* tpms_protocol_decoder_tpms_imars_t240_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderiMarsT240* instance = calloc(1, sizeof(TPMSProtocolDecoderiMarsT240));
    instance->base.protocol = &tpms_protocol_tpms_imars_t240;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_tpms_imars_t240_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_tpms_imars_t240_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderiMarsT240* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->step = iMarsT240StepPreamble;
    instance->preamble_run = 0;
    instance->have_last = false;
    instance->last_bit = 0;
    instance->have_half = false;
    instance->first_half = 0;
    instance->collected = 0;
}

// Split the 64-bit Manchester-decoded-then-INVERTED payload into B0..B7.
// `collected` holds the raw Manchester-decoded bits; rtl_433 inverts them, so
// the wire byte value is ~collected.
static void imars_t240_get_bytes(uint64_t collected, uint8_t b[8]) {
    uint64_t inv = ~collected; // rtl_433 bitbuffer_invert
    for(int i = 0; i < 8; i++) {
        b[i] = (uint8_t)((inv >> (8 * (7 - i))) & 0xFF);
    }
}

static bool imars_t240_check(const uint8_t b[8]) {
    if(b[7] != b[0]) return false;
    if((b[0] & 0x0f) != (b[1] & 0x0f)) return false;
    uint8_t checksum = (uint8_t)((b[3] + b[4]) & 0xff);
    if(checksum != 0x41 && checksum != 0x3c) return false;
    return true;
}

static void imars_t240_analyze(TPMSProtocolDecoderiMarsT240* instance) {
    uint8_t b[8];
    imars_t240_get_bytes(instance->collected, b);

    // ID: the four free wire bytes. (rtl_433 emits B0..B6 as a hex code; we take
    // the same free bytes as a 32-bit identifier.)
    instance->generic.id = ((uint32_t)b[0] << 24) | ((uint32_t)b[2] << 16) |
                           ((uint32_t)b[3] << 8) | (uint32_t)b[6];

    // Pressure: 4-bit field in B1 high nibble, 0.25 bar/step (ADDED mapping).
    instance->generic.pressure = (float)(b[1] >> 4) * 0.25f;

    // Temperature: B5 with a 50 degC offset (ADDED mapping).
    instance->generic.temperature = (float)((int)b[5] - 50);

    // No documented battery bit in the iMars T240 frame.
    instance->generic.battery_low = 0;

    // Store the wire bytes (post-invert) as the 64-bit generic payload so the
    // encoder can rebuild the exact frame from generic.data.
    instance->generic.data = ~instance->collected & 0xFFFFFFFFFFFFFFFFULL;
    instance->generic.data_count_bit = IMARS_T240_DATA_BITS;
}

// Consume one raw half-symbol bit: hunt the 32-bit alternating preamble, then
// Manchester-pair the 128 payload half-symbols into 64 bits. Manchester
// convention: a valid cell is a transition (halves differ); the decoded raw bit
// is the FIRST half (the complementary encoder writes first-half = wire raw
// bit). imars_t240_get_bytes then applies rtl_433's post-decode invert.
static void imars_t240_emit_raw_bit(TPMSProtocolDecoderiMarsT240* instance, uint8_t bit) {
    bit &= 1;

    if(instance->step == iMarsT240StepPreamble) {
        // Track the length of the current alternating run. A clean 0xaaaaaaaa
        // preamble is 32 strictly-alternating half-symbols; when the run reaches
        // 32 the next half-symbol is the first half of the first Manchester data
        // cell (rtl_433 decodes from preamble_start + 32).
        if(instance->have_last && bit != instance->last_bit) {
            instance->preamble_run++;
        } else {
            instance->preamble_run = 1;
        }
        instance->have_last = true;
        instance->last_bit = bit;

        if(instance->preamble_run >= IMARS_T240_PREAMBLE_BITS) {
            instance->step = iMarsT240StepData;
            instance->have_half = false;
            instance->collected = 0;
            instance->decoder.decode_count_bit = 0;
        }
        return;
    }

    // iMarsT240StepData: pair up half-symbols.
    if(!instance->have_half) {
        instance->first_half = bit;
        instance->have_half = true;
        return;
    }
    instance->have_half = false;
    if(instance->first_half == bit) {
        // No transition -> Manchester error; drop back to preamble hunting.
        instance->step = iMarsT240StepPreamble;
        instance->preamble_run = 0;
        instance->have_last = false;
        return;
    }

    uint8_t data_bit = instance->first_half;
    instance->collected = (instance->collected << 1) | data_bit;
    instance->decoder.decode_count_bit++;

    if(instance->decoder.decode_count_bit == IMARS_T240_DATA_BITS) {
        uint8_t b[8];
        imars_t240_get_bytes(instance->collected, b);
        if(imars_t240_check(b)) {
            instance->decoder.decode_data = ~instance->collected & 0xFFFFFFFFFFFFFFFFULL;
            imars_t240_analyze(instance);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        // Re-sync for the next frame regardless of pass/fail.
        instance->step = iMarsT240StepPreamble;
        instance->preamble_run = 0;
        instance->have_last = false;
        instance->have_half = false;
    }
}

void tpms_protocol_decoder_tpms_imars_t240_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderiMarsT240* instance = context;

    const uint32_t te = tpms_protocol_tpms_imars_t240_const.te_short; // 50 us half-symbol
    // A gap (anything longer than two half-symbols) ends the burst / re-syncs.
    if(duration >= te * 3) {
        tpms_protocol_decoder_tpms_imars_t240_reset(instance);
        return;
    }
    // Expand this pulse into 1 or 2 raw half-symbol bits of the current level.
    uint32_t n = (duration + te / 2) / te;
    if(n < 1) n = 1;
    if(n > 2) n = 2;
    for(uint32_t k = 0; k < n; k++) {
        imars_t240_emit_raw_bit(instance, level ? 1 : 0);
    }
}

uint8_t tpms_protocol_decoder_tpms_imars_t240_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderiMarsT240* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_tpms_imars_t240_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderiMarsT240* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_tpms_imars_t240_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderiMarsT240* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_tpms_imars_t240_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_tpms_imars_t240_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderiMarsT240* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%08lX\r\n"
        "Temp:%2.0f C Bar:%2.2f",
        instance->generic.protocol_name,
        instance->generic.id,
        (double)instance->generic.temperature,
        (double)instance->generic.pressure);
}

// -----------------------------------------------------------------------------
// Packer: invert analyze() — engineering units -> the 8 wire bytes B0..B7.
// -----------------------------------------------------------------------------
void tpms_protocol_tpms_imars_t240_pack(TPMSBlockGeneric* generic) {
    furi_assert(generic);

    // Pressure -> B1 high nibble (0.25 bar/step, clamp 0..15).
    int p_raw = (int)(generic->pressure / 0.25f + 0.5f);
    if(p_raw < 0) p_raw = 0;
    if(p_raw > 0x0F) p_raw = 0x0F;

    // Temperature -> B5 with a 50 degC offset, clamp to a byte.
    int t_raw = (int)(generic->temperature + (generic->temperature >= 0 ? 0.5f : -0.5f)) + 50;
    if(t_raw < 0) t_raw = 0;
    if(t_raw > 0xFF) t_raw = 0xFF;

    uint8_t b[8];
    b[0] = (uint8_t)((generic->id >> 24) & 0xFF);
    // B1: high nibble = pressure, low nibble mirrors B0 (rtl_433 invariant).
    b[1] = (uint8_t)(((p_raw & 0x0F) << 4) | (b[0] & 0x0F));
    b[2] = (uint8_t)((generic->id >> 16) & 0xFF);
    b[3] = (uint8_t)((generic->id >> 8) & 0xFF);
    // B4: chosen so (B3 + B4) & 0xff == 0x41 (fixed checksum constant).
    b[4] = (uint8_t)((IMARS_T240_CHECKSUM - b[3]) & 0xFF);
    b[5] = (uint8_t)t_raw;
    b[6] = (uint8_t)(generic->id & 0xFF);
    b[7] = b[0]; // tail integrity

    uint64_t data = 0;
    for(int i = 0; i < 8; i++) data = (data << 8) | b[i];
    generic->data = data;
    generic->data_count_bit = IMARS_T240_DATA_BITS;
}

// -----------------------------------------------------------------------------
// Encoder
// -----------------------------------------------------------------------------
//
// OOK, half-symbol te = 50 us. Frame on the wire:
//   inter-frame gap (low) + 32 raw alternating preamble half-symbols (0xaaaaaaaa)
//   + 64 Manchester data cells.
// Each data cell reproduces the wire raw bit = INVERT(payload bit) (rtl_433
// applies bitbuffer_invert on RX, so the transmitted Manchester stream is the
// complement of B0..B7), and the Manchester cell is (first half = wire raw bit,
// second half = its complement) — the exact convention the decoder reads.
//
// Worst-case entries: 1 gap + 32 preamble + 2*64 data + 1 flush = 162.
#define TPMS_IMARS_T240_ENCODER_UPLOAD_SIZE 220

void* tpms_protocol_encoder_tpms_imars_t240_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderiMarsT240* instance = malloc(sizeof(TPMSProtocolEncoderiMarsT240));
    instance->base.protocol = &tpms_protocol_tpms_imars_t240;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_IMARS_T240_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_tpms_imars_t240_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderiMarsT240* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_tpms_imars_t240_stop(void* context) {
    TPMSProtocolEncoderiMarsT240* instance = context;
    instance->encoder.is_running = false;
}

// Rebuild the full 8-byte frame from generic.data, re-enforcing (and thus
// RECOMPUTING) every integrity field: B7=B0, low-nibble(B1)=low-nibble(B0), and
// the B4 checksum term so (B3+B4)&0xff == 0x41.
static void
    tpms_protocol_encoder_tpms_imars_t240_build_frame(TPMSBlockGeneric* generic, uint8_t b[8]) {
    for(int i = 0; i < 8; i++) {
        b[i] = (uint8_t)((generic->data >> (8 * (7 - i))) & 0xFF);
    }
    b[7] = b[0];
    b[1] = (uint8_t)((b[1] & 0xF0) | (b[0] & 0x0F));
    b[4] = (uint8_t)((IMARS_T240_CHECKSUM - b[3]) & 0xFF);
}

static bool
    tpms_protocol_encoder_tpms_imars_t240_get_upload(TPMSProtocolEncoderiMarsT240* instance) {
    furi_assert(instance);
    const uint32_t te = tpms_protocol_tpms_imars_t240_const.te_short;

    uint8_t frame[8];
    tpms_protocol_encoder_tpms_imars_t240_build_frame(&instance->generic, frame);

    size_t index = 0;
    const size_t max = instance->encoder.size_upload;
    bool last_level = false;
    uint32_t last_duration = 0;

#define EMIT(lvl, dur)                                                            \
    do {                                                                          \
        if(last_duration != 0 && last_level == (lvl)) {                           \
            last_duration += (dur);                                               \
        } else {                                                                  \
            if(last_duration != 0) {                                             \
                if(index >= max) return false;                                    \
                instance->encoder.upload[index++] =                               \
                    level_duration_make(last_level, last_duration);               \
            }                                                                     \
            last_level = (lvl);                                                   \
            last_duration = (dur);                                                \
        }                                                                         \
    } while(0)

// Manchester cell for a wire raw bit r: first half = r, second half = ~r.
#define MAN_CELL(r)          \
    do {                     \
        if(r) {              \
            EMIT(true, te);  \
            EMIT(false, te); \
        } else {             \
            EMIT(false, te); \
            EMIT(true, te);  \
        }                    \
    } while(0)

    // Inter-frame gap so repeats are framed and the decoder re-syncs.
    EMIT(false, te * 50);

    // 32-bit raw alternating preamble 0xaaaaaaaa (NOT Manchester) — each bit is
    // one raw half-symbol, MSB first.
    static const uint8_t preamble[4] = {0xAA, 0xAA, 0xAA, 0xAA};
    for(int i = 0; i < 4; i++) {
        for(int bit = 7; bit >= 0; bit--) {
            EMIT(((preamble[i] >> bit) & 0x1) ? true : false, te);
        }
    }

    // 64 data bits, MSB first. On the wire each is INVERTED (rtl_433 invert) then
    // Manchester-encoded.
    for(int i = 0; i < (int)IMARS_T240_DATA_BITS; i++) {
        int byte = i / 8;
        int bit = 7 - (i % 8);
        uint8_t payload_bit = (frame[byte] >> bit) & 0x1;
        uint8_t wire_raw = payload_bit ^ 1u; // invert
        MAN_CELL(wire_raw);
    }

    if(last_duration != 0) {
        if(index >= max) return false;
        instance->encoder.upload[index++] = level_duration_make(last_level, last_duration);
    }

#undef MAN_CELL
#undef EMIT

    instance->encoder.size_upload = index;
    return true;
}

SubGhzProtocolStatus
    tpms_protocol_encoder_tpms_imars_t240_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderiMarsT240* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_tpms_imars_t240_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_IMARS_T240_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_tpms_imars_t240_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_tpms_imars_t240_yield(void* context) {
    TPMSProtocolEncoderiMarsT240* instance = context;

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
