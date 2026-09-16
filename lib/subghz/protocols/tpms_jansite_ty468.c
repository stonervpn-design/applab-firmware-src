#include "tpms_jansite_ty468.h"
#include "../blocks/generic.h"

#define TAG "TPMSJansiteTy468"

// Port of rtl_433/src/devices/tpms_jansite_ty468.c
//   (Jansite TPMS TY-468-eu2 / KKMOON TPMS, Benjamin Larsson 2026, GPL-2+)
//
// Same SP372-chip-family encoding as tpms_imars_t240 (rtl_433 #1820 / #2025):
//   OOK, Manchester-encoded, ~50 us half-bit (OOK_PULSE_PCM short=long=50).
//
// Wire structure:
//   32-bit raw alternating preamble (0xaaaaaaaa), then 128 raw bits that
//   Manchester-decode to 8 bytes B0..B7 (the decoded stream is then inverted).
//
// Decoded byte layout (B0..B7):
//   - B7 repeats B0                       (tail integrity check)
//   - (B0 & 0x0f) == (B1 & 0x0f)          (nibble integrity check)
//   - checksum = (B3 + B4) & 0xff selects the per-unit calibration; two real
//     sensor units are confirmed in rtl_433 (cross-checked against ground-truth
//     PSI/C in issue #2025):
//         checksum  temp_offset  pressure_offset
//         0xfb      224          273
//         0x64      153          201
//   - temperature_C = temp_offset     - ((B2 + B5) & 0xff)
//   - pressure_kPa  = (pressure_offset - ((B5 + B6) & 0xff)) * 2.5
//
// rtl_433 exposes no sensor ID (only a 7-byte "code"). We derive a stable
// per-sensor 28-bit id from the invariant bytes that do NOT feed the
// pressure/temperature sums: B0 (8), high nibble of B1 (4), B3 (8), B5 (8):
//   id = (B0 << 20) | ((B1 >> 4) << 16) | (B3 << 8) | B5
// (B2, B4, B6 are compensation bytes tied to temp/pressure/checksum, so they
// are recovered on transmit by the packer rather than carrying id bits.)
//
// Preamble hunt: the raw alternating run toggles a 24-bit window between
// 0xAAAAAA and 0x555555 each chip; rtl_433 searches {0xaa,0xaa,0xaa} then jumps
// to pos+32 (skip the rest of the 32-bit preamble). We mirror that: lock on the
// first 0xAAAAAA (24 chips), skip 8 more preamble chips, then Manchester-decode
// the 64-bit payload starting on the (even) pair grid.

#define JANSITE_TY468_DATA_BITS 64
#define JANSITE_TY468_PREAMBLE  0xAAAAAAUL // 24 alternating chips, rtl_433 {aa,aa,aa}
#define JANSITE_TY468_SKIP_TAIL 8 // chips from first 24-match to the 32-chip end

static const SubGhzBlockConst tpms_protocol_jansite_ty468_const = {
    .te_short = 50,
    .te_long = 100,
    .te_delta = 25,
    .min_count_bit_for_found = JANSITE_TY468_DATA_BITS,
};

typedef enum {
    Ty468StepHuntPreamble = 0,
    Ty468StepSkipTail,
    Ty468StepCollectData,
} Ty468DecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    uint32_t raw_shift; // rolling 24-bit raw-chip window for preamble hunt
    uint8_t step; // Ty468DecoderStep
    uint8_t skip_count; // preamble-tail chips remaining to discard
    bool have_half; // holding the first chip of the current Manchester pair
    uint8_t first_half; // that first chip
} TPMSProtocolDecoderJansiteTy468;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderJansiteTy468;

const SubGhzProtocolDecoder tpms_protocol_tpms_jansite_ty468_decoder = {
    .alloc = tpms_protocol_decoder_jansite_ty468_alloc,
    .free = tpms_protocol_decoder_jansite_ty468_free,
    .feed = tpms_protocol_decoder_jansite_ty468_feed,
    .reset = tpms_protocol_decoder_jansite_ty468_reset,
    .get_hash_data = tpms_protocol_decoder_jansite_ty468_get_hash_data,
    .serialize = tpms_protocol_decoder_jansite_ty468_serialize,
    .deserialize = tpms_protocol_decoder_jansite_ty468_deserialize,
    .get_string = tpms_protocol_decoder_jansite_ty468_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_tpms_jansite_ty468_encoder = {
    .alloc = tpms_protocol_encoder_jansite_ty468_alloc,
    .free = tpms_protocol_encoder_jansite_ty468_free,
    .deserialize = tpms_protocol_encoder_jansite_ty468_deserialize,
    .stop = tpms_protocol_encoder_jansite_ty468_stop,
    .yield = tpms_protocol_encoder_jansite_ty468_yield,
};

const SubGhzProtocol tpms_protocol_tpms_jansite_ty468 = {
    .name = TPMS_PROTOCOL_JANSITE_TY468_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Sensors | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load |
            SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_tpms_jansite_ty468_decoder,
    .encoder = &tpms_protocol_tpms_jansite_ty468_encoder,
};

void* tpms_protocol_decoder_jansite_ty468_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderJansiteTy468* instance = calloc(1, sizeof(TPMSProtocolDecoderJansiteTy468));
    instance->base.protocol = &tpms_protocol_tpms_jansite_ty468;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void tpms_protocol_decoder_jansite_ty468_free(void* context) {
    furi_assert(context);
    free(context);
}

void tpms_protocol_decoder_jansite_ty468_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteTy468* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->raw_shift = 0;
    instance->step = Ty468StepHuntPreamble;
    instance->skip_count = 0;
    instance->have_half = false;
    instance->first_half = 0;
}

// Two calibration units are known; both share the 2.5 kPa/step scale.
static bool jansite_ty468_calib(uint8_t checksum, int* temp_offset, int* pressure_offset) {
    if(checksum == 0xfb) {
        *temp_offset = 224;
        *pressure_offset = 273;
        return true;
    }
    if(checksum == 0x64) {
        *temp_offset = 153;
        *pressure_offset = 201;
        return true;
    }
    return false;
}

static void jansite_ty468_get_bytes(TPMSProtocolDecoderJansiteTy468* instance, uint8_t b[8]) {
    for(int i = 0; i < 8; i++) {
        b[i] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

// Mirror rtl_433's sanity/checksum gate: tail repeat, nibble echo, valid unit.
static bool jansite_ty468_check(TPMSProtocolDecoderJansiteTy468* instance) {
    uint8_t b[8];
    jansite_ty468_get_bytes(instance, b);
    if(b[7] != b[0]) return false;
    if((b[0] & 0x0F) != (b[1] & 0x0F)) return false;
    int to = 0, po = 0;
    return jansite_ty468_calib((b[3] + b[4]) & 0xFF, &to, &po);
}

static void jansite_ty468_analyze(TPMSProtocolDecoderJansiteTy468* instance) {
    uint8_t b[8];
    jansite_ty468_get_bytes(instance, b);

    int to = 0, po = 0;
    jansite_ty468_calib((b[3] + b[4]) & 0xFF, &to, &po);

    instance->generic.id = ((uint32_t)b[0] << 20) | ((uint32_t)(b[1] >> 4) << 16) |
                           ((uint32_t)b[3] << 8) | (uint32_t)b[5];

    instance->generic.temperature = (float)(to - ((b[2] + b[5]) & 0xFF));
    // rtl_433 prints pressure in kPa; TPMSBlockGeneric stores bar (kPa / 100).
    float pressure_kPa = (float)(po - ((b[5] + b[6]) & 0xFF)) * 2.5f;
    instance->generic.pressure = pressure_kPa * 0.01f;
    // ty468 carries no battery/flag bits.
    instance->generic.battery_low = 0;
}

// Inverse of jansite_ty468_analyze(): id/pressure/temp -> the 8 wire bytes.
// Uses the 0x64 unit (temp_offset 153, pressure_offset 201) whose pressure span
// includes 0 kPa. B2/B4/B6 are compensation bytes so temp/pressure/checksum come
// out exactly; B0/B1hi/B3/B5 carry the 28-bit id.
void tpms_protocol_tpms_jansite_ty468_pack(TPMSBlockGeneric* generic) {
    furi_assert(generic);
    const int checksum = 0x64;
    const int temp_offset = 153;
    const int pressure_offset = 201;

    uint32_t id = generic->id & 0x0FFFFFFFUL; // 28-bit id space

    int temp_c = (int)(generic->temperature + (generic->temperature >= 0 ? 0.5f : -0.5f));
    float pressure_kPa = generic->pressure * 100.0f; // bar -> kPa
    int p_raw = (int)(pressure_kPa / 2.5f + 0.5f); // (b5+b6) sum term

    uint8_t b[8];
    b[0] = (uint8_t)((id >> 20) & 0xFF);
    b[1] = (uint8_t)((((id >> 16) & 0x0F) << 4) | (b[0] & 0x0F)); // nibble echo
    b[3] = (uint8_t)((id >> 8) & 0xFF);
    b[5] = (uint8_t)(id & 0xFF);
    b[4] = (uint8_t)((checksum - b[3]) & 0xFF); // (b3+b4)&0xff == checksum
    // temperature: (b2+b5)&0xff == temp_offset - temp_c
    b[2] = (uint8_t)((((temp_offset - temp_c) & 0xFF) - b[5]) & 0xFF);
    // pressure: (b5+b6)&0xff == pressure_offset - p_raw
    b[6] = (uint8_t)((((pressure_offset - p_raw) & 0xFF) - b[5]) & 0xFF);
    b[7] = b[0]; // tail integrity

    uint64_t data = 0;
    for(int i = 0; i < 8; i++) data = (data << 8) | b[i];
    generic->data = data;
    generic->data_count_bit = JANSITE_TY468_DATA_BITS;
}

// Consume one raw 50 us chip: hunt the alternating preamble, skip its tail, then
// Manchester-decode the 64-bit payload. rtl_433 adds the FIRST chip of each pair
// then inverts the whole packet -> decoded bit = !first_chip.
static void jansite_ty468_emit_chip(TPMSProtocolDecoderJansiteTy468* instance, uint8_t chip) {
    chip &= 1;
    switch(instance->step) {
    case Ty468StepHuntPreamble:
        instance->raw_shift = ((instance->raw_shift << 1) | chip) & 0xFFFFFFUL;
        if(instance->raw_shift == JANSITE_TY468_PREAMBLE) {
            instance->step = Ty468StepSkipTail;
            instance->skip_count = JANSITE_TY468_SKIP_TAIL;
        }
        return;
    case Ty468StepSkipTail:
        if(--instance->skip_count == 0) {
            instance->step = Ty468StepCollectData;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->have_half = false;
        }
        return;
    default: // Ty468StepCollectData
        if(!instance->have_half) {
            instance->first_half = chip;
            instance->have_half = true;
            return;
        }
        instance->have_half = false;
        if(instance->first_half == chip) {
            // No mid-pair transition: framing lost -> resync on preamble.
            instance->step = Ty468StepHuntPreamble;
            instance->raw_shift = 0;
            return;
        }
        subghz_protocol_blocks_add_bit(&instance->decoder, instance->first_half ? 0 : 1);
        if(instance->decoder.decode_count_bit == JANSITE_TY468_DATA_BITS) {
            if(jansite_ty468_check(instance)) {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                jansite_ty468_analyze(instance);
                if(instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }
            instance->step = Ty468StepHuntPreamble;
            instance->raw_shift = 0;
        }
        return;
    }
}

void tpms_protocol_decoder_jansite_ty468_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteTy468* instance = context;

    const uint32_t te = tpms_protocol_jansite_ty468_const.te_short; // 50 us chip
    // A gap (or any pulse longer than two chips) ends the burst.
    if(duration >= te * 3) {
        tpms_protocol_decoder_jansite_ty468_reset(instance);
        return;
    }
    // Expand this pulse into 1 or 2 raw chips of the current level.
    uint32_t n = (duration + te / 2) / te;
    if(n < 1) n = 1;
    if(n > 2) n = 2;
    for(uint32_t k = 0; k < n; k++) {
        jansite_ty468_emit_chip(instance, level ? 1 : 0);
    }
}

uint8_t tpms_protocol_decoder_jansite_ty468_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteTy468* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus tpms_protocol_decoder_jansite_ty468_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteTy468* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    tpms_protocol_decoder_jansite_ty468_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteTy468* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        tpms_protocol_jansite_ty468_const.min_count_bit_for_found);
}

void tpms_protocol_decoder_jansite_ty468_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderJansiteTy468* instance = context;
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
// Encoder
// -----------------------------------------------------------------------------
//
// OOK PCM at 50 us/chip. Frame: inter-frame gap + 32-chip alternating preamble
// (0xaaaaaaaa) + 64 Manchester-encoded data bits (each -> 2 chips). Matching the
// decoder's !first_chip convention, data bit V -> chips (!V, V).
//
// Worst-case entries: 1 gap + 32 preamble + 2*64 data + 1 flush = 162.
#define TPMS_JANSITE_TY468_ENCODER_UPLOAD_SIZE 200

void* tpms_protocol_encoder_jansite_ty468_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderJansiteTy468* instance = malloc(sizeof(TPMSProtocolEncoderJansiteTy468));
    instance->base.protocol = &tpms_protocol_tpms_jansite_ty468;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_JANSITE_TY468_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void tpms_protocol_encoder_jansite_ty468_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderJansiteTy468* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void tpms_protocol_encoder_jansite_ty468_stop(void* context) {
    TPMSProtocolEncoderJansiteTy468* instance = context;
    instance->encoder.is_running = false;
}

// Extract the 8 wire bytes from generic.data and recompute the tail integrity
// byte B7 = B0 (ty468's only recomputable integrity field; the (B3+B4) checksum
// is a per-unit calibration constant carried in the payload, not a CRC).
static void
    tpms_protocol_encoder_jansite_ty468_build_frame(TPMSBlockGeneric* generic, uint8_t b[8]) {
    for(int i = 0; i < 8; i++) {
        b[i] = (generic->data >> (8 * (7 - i))) & 0xFF;
    }
    b[7] = b[0];
}

static bool
    tpms_protocol_encoder_jansite_ty468_get_upload(TPMSProtocolEncoderJansiteTy468* instance) {
    furi_assert(instance);
    const uint32_t te = tpms_protocol_jansite_ty468_const.te_short;

    uint8_t frame[8];
    tpms_protocol_encoder_jansite_ty468_build_frame(&instance->generic, frame);

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

// Manchester chip pair for data bit V (decoder reads !first_chip): (!V, V).
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

    // Inter-frame gap so repeats reset the decoder.
    EMIT(false, te * 50);

    // 32-chip alternating preamble 0xaaaaaaaa (raw, NOT Manchester). MSB-first:
    // chip i level = (i even) -> high, matching the {0xaa,..} the decoder hunts.
    for(int i = 0; i < 32; i++) {
        EMIT(((i & 1) == 0) ? true : false, te);
    }

    // 64 data bits, MSB first (frame[0]..frame[7]).
    for(int i = 0; i < JANSITE_TY468_DATA_BITS; i++) {
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
    tpms_protocol_encoder_jansite_ty468_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderJansiteTy468* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            tpms_protocol_jansite_ty468_const.min_count_bit_for_found);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        instance->encoder.size_upload = TPMS_JANSITE_TY468_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_jansite_ty468_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

LevelDuration tpms_protocol_encoder_jansite_ty468_yield(void* context) {
    TPMSProtocolEncoderJansiteTy468* instance = context;

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
