#include "schrader_eg53ma4.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include "tpms_generic.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "SchraderEG53MA4"

// Port of the schrader_EG53MA4_decode() path in
// rtl_433/src/devices/schraeder.c (Copyright 2016 Benjamin Larsson /
// 2017 Christian W. Zuckschwerdt, GPL-2+). Contributed there by Leonardo
// Hamada (hkazu).
//
// TPMS Model: Schrader Electronics EG53MA4. Also Schrader Opel OEM
// 13348393 (Saab, Opel, Vauxhall, Chevrolet) and GM (Chevrolet) OEM
// 13540600 for 2006-2025 GM. GM/North-America transmits at 315 MHz,
// Opel/Vauxhall at 433.92 MHz.
//
// Modulation: OOK / AM, Manchester-coded (rtl_433 OOK_PULSE_MANCHESTER_ZEROBIT,
//             short_width 123 us -> half-bit ~123 us, full-bit ~246 us,
//             reset_limit 300 us). Same low-level timing family as the
//             Schrader MRX-GG4 already ported here (schrader_gg4.c), so we
//             reuse its proven Manchester-II convention (decode Manchester-I,
//             then invert the output bit).
//
// Frame: 120 Manchester bits total. rtl_433 discards the first 40 (sync /
// preamble / status) and keeps the trailing 80 bits = 10 payload bytes:
//
//   b[0..3] = flags / status (32-bit, not further decoded)
//   b[4..6] = 24-bit ID
//   b[7]    = pressure raw, 25 mbar per bit  (kPa = raw * 2.5)
//   b[8]    = temperature in degrees FAHRENHEIT (direct byte)
//   b[9]    = checksum: (sum of bytes b[0..8]) & 0xFF
//
// Sanity guard (from rtl_433): reject if b[1],b[2],b[4],b[5],b[7],b[8] are all
// zero. Combined with the 8-bit byte-sum checksum this keeps noise from
// false-triggering.
//
// This is a passive receive / decode-only port. No encoder (all-NULL stub).
//
// NOTE: no explicit start pulse delimits the frame, so we track the number of
// Manchester bits and validate the *last 80 bits* preceding the end-of-burst
// gap (and opportunistically at the nominal 120-bit count). If a real capture's
// Manchester polarity is opposite to the assumed (GG4-proven) convention the
// byte-sum checksum simply will not match and nothing is emitted -- it fails
// safe rather than false-positive.

#define EG53MA4_TOTAL_BITS   120
#define EG53MA4_PAYLOAD_BITS 80
// The receiver discards the leading 40 Manchester bits (sync); we emit them as
// a run of zero bits so the decoder's Manchester lock settles well before the
// 80-bit payload begins (total 40 + 80 = 120, matching EG53MA4_TOTAL_BITS).
#define EG53MA4_SYNC_BITS    40

static const SubGhzBlockConst tpms_protocol_schrader_eg53ma4_const = {
    .te_short = 123,
    .te_long = 246,
    .te_delta = 60, // ~50% of te_short (Schrader OOK has poor sensitivity)
    .min_count_bit_for_found = EG53MA4_TOTAL_BITS,
};

typedef enum {
    Eg53ma4DecoderStepReset = 0,
    Eg53ma4DecoderStepData,
} Eg53ma4DecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    ManchesterState manchester_saved_state;
    uint64_t head_64_bit;
} TPMSProtocolDecoderEg53ma4;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderEg53ma4;

// -- forward declarations (decoder functions are file-static) ----------------
static void* tpms_protocol_decoder_schrader_eg53ma4_alloc(SubGhzEnvironment* environment);
static void tpms_protocol_decoder_schrader_eg53ma4_free(void* context);
static void tpms_protocol_decoder_schrader_eg53ma4_reset(void* context);
static void
    tpms_protocol_decoder_schrader_eg53ma4_feed(void* context, bool level, uint32_t duration);
static uint8_t tpms_protocol_decoder_schrader_eg53ma4_get_hash_data(void* context);
static SubGhzProtocolStatus tpms_protocol_decoder_schrader_eg53ma4_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
static SubGhzProtocolStatus tpms_protocol_decoder_schrader_eg53ma4_deserialize(
    void* context,
    FlipperFormat* flipper_format);
static void tpms_protocol_decoder_schrader_eg53ma4_get_string(void* context, FuriString* output);

// -- encoder forward declarations --------------------------------------------
static void* tpms_protocol_encoder_schrader_eg53ma4_alloc(SubGhzEnvironment* environment);
static void tpms_protocol_encoder_schrader_eg53ma4_free(void* context);
static void tpms_protocol_encoder_schrader_eg53ma4_stop(void* context);
static SubGhzProtocolStatus
    tpms_protocol_encoder_schrader_eg53ma4_deserialize(void* context, FlipperFormat* flipper_format);
static LevelDuration tpms_protocol_encoder_schrader_eg53ma4_yield(void* context);

const SubGhzProtocolDecoder tpms_protocol_schrader_eg53ma4_decoder = {
    .alloc = tpms_protocol_decoder_schrader_eg53ma4_alloc,
    .free = tpms_protocol_decoder_schrader_eg53ma4_free,
    .feed = tpms_protocol_decoder_schrader_eg53ma4_feed,
    .reset = tpms_protocol_decoder_schrader_eg53ma4_reset,
    .get_hash_data = tpms_protocol_decoder_schrader_eg53ma4_get_hash_data,
    .serialize = tpms_protocol_decoder_schrader_eg53ma4_serialize,
    .deserialize = tpms_protocol_decoder_schrader_eg53ma4_deserialize,
    .get_string = tpms_protocol_decoder_schrader_eg53ma4_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_schrader_eg53ma4_encoder = {
    .alloc = tpms_protocol_encoder_schrader_eg53ma4_alloc,
    .free = tpms_protocol_encoder_schrader_eg53ma4_free,
    .deserialize = tpms_protocol_encoder_schrader_eg53ma4_deserialize,
    .stop = tpms_protocol_encoder_schrader_eg53ma4_stop,
    .yield = tpms_protocol_encoder_schrader_eg53ma4_yield,
};

const SubGhzProtocol tpms_protocol_schrader_eg53ma4 = {
    .name = TPMS_PROTOCOL_SCHRADER_EG53MA4_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_schrader_eg53ma4_decoder,
    .encoder = &tpms_protocol_schrader_eg53ma4_encoder,
};

static void* tpms_protocol_decoder_schrader_eg53ma4_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderEg53ma4* instance = calloc(1, sizeof(TPMSProtocolDecoderEg53ma4));
    instance->base.protocol = &tpms_protocol_schrader_eg53ma4;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->manchester_saved_state = ManchesterStateStart1;
    return instance;
}

static void tpms_protocol_decoder_schrader_eg53ma4_free(void* context) {
    furi_assert(context);
    free(context);
}

static void tpms_protocol_decoder_schrader_eg53ma4_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderEg53ma4* instance = context;
    instance->decoder.parser_step = Eg53ma4DecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->head_64_bit = 0;
    instance->manchester_saved_state = ManchesterStateStart1;
}

static ManchesterEvent eg53ma4_evt(bool level, uint32_t duration) {
    bool is_long = false;
    if(DURATION_DIFF(duration, tpms_protocol_schrader_eg53ma4_const.te_long) <
       tpms_protocol_schrader_eg53ma4_const.te_delta) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_schrader_eg53ma4_const.te_short) <
        tpms_protocol_schrader_eg53ma4_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }
    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

// Extract the trailing 80 payload bits (10 bytes) from the rolling accumulator.
// After N (>= 80) Manchester bits, decode_data holds the last 64 bits (b[2..9])
// and head_64_bit's low 16 bits hold the 16 bits just before them (b[0..1]),
// so this always yields the last 80 bits regardless of how many preamble bits
// were counted -- exactly rtl_433's "discard everything but the last 80".
static void eg53ma4_get_bytes(TPMSProtocolDecoderEg53ma4* instance, uint8_t b[10]) {
    b[0] = (uint8_t)((instance->head_64_bit >> 8) & 0xFF);
    b[1] = (uint8_t)(instance->head_64_bit & 0xFF);
    for(int i = 0; i < 8; i++) {
        b[i + 2] = (instance->decoder.decode_data >> (8 * (7 - i))) & 0xFF;
    }
}

// Validate the last-80-bit frame and, if good, fill generic + fire callback.
// Returns true on a valid decode.
static bool eg53ma4_try_decode(TPMSProtocolDecoderEg53ma4* instance) {
    uint8_t b[10];
    eg53ma4_get_bytes(instance, b);

    // rtl_433 sanity: reject if all the meaningful bytes are zero.
    if(!b[1] && !b[2] && !b[4] && !b[5] && !b[7] && !b[8]) return false;

    // Checksum: (sum of b[0..8]) & 0xFF == b[9].
    if(subghz_protocol_blocks_add_bytes(b, 9) != b[9]) return false;

    instance->generic.data = instance->decoder.decode_data;
    instance->generic.data_count_bit = EG53MA4_PAYLOAD_BITS;
    instance->generic.id = ((uint32_t)b[4] << 16) | ((uint32_t)b[5] << 8) | b[6];
    // Pressure: 25 mbar per bit -> kPa (raw*2.5) -> bar (kPa*0.01).
    instance->generic.pressure = (float)b[7] * 2.5f * 0.01f;
    // Source temperature is in Fahrenheit; store Celsius for a unified view.
    instance->generic.temperature = ((float)b[8] - 32.0f) * (5.0f / 9.0f);
    instance->generic.battery_low = TPMS_NO_BATT;

    if(instance->base.callback) instance->base.callback(&instance->base, instance->base.context);
    return true;
}

static void
    tpms_protocol_decoder_schrader_eg53ma4_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderEg53ma4* instance = context;

    ManchesterEvent event = eg53ma4_evt(level, duration);

    if(event == ManchesterEventReset) {
        // End of burst: the payload is always the last 80 bits before this gap,
        // no matter how many preamble bits Manchester lock happened to count, so
        // any burst that yielded at least a full payload's worth is a candidate.
        if(instance->decoder.parser_step == Eg53ma4DecoderStepData &&
           instance->decoder.decode_count_bit >= EG53MA4_PAYLOAD_BITS) {
            eg53ma4_try_decode(instance);
        }
        instance->decoder.parser_step = Eg53ma4DecoderStepReset;
        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;
        instance->head_64_bit = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        return;
    }

    bool bit = false;
    bool have_bit = manchester_advance(
        instance->manchester_saved_state, event, &instance->manchester_saved_state, &bit);
    if(!have_bit) return;

    // Manchester-II: decoder emits Manchester-I, so invert (matches GG4).
    bit = !bit;

    if(instance->decoder.parser_step == Eg53ma4DecoderStepReset) {
        instance->decoder.parser_step = Eg53ma4DecoderStepData;
        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;
        instance->head_64_bit = 0;
    }

    subghz_protocol_blocks_add_to_128_bit(&instance->decoder, bit ? 1 : 0, &instance->head_64_bit);

    // Opportunistic check at the nominal frame length (covers a clean 120-bit
    // burst whose final bit lands before the trailing gap). A few counts are
    // tried to absorb the +/-1 Manchester "zerobit" phase ambiguity.
    uint16_t n = instance->decoder.decode_count_bit;
    if(n == EG53MA4_TOTAL_BITS || n == EG53MA4_TOTAL_BITS + 1 || n == EG53MA4_TOTAL_BITS + 2) {
        if(eg53ma4_try_decode(instance)) {
            instance->decoder.parser_step = Eg53ma4DecoderStepReset;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->head_64_bit = 0;
            instance->manchester_saved_state = ManchesterStateStart1;
            return;
        }
    }

    // Runaway guard: decode_count_bit is a uint8_t (wraps at 256), so re-sync
    // well before that on a long noise burst. Normal frames are delimited by
    // the trailing gap (handled above) long before this fires.
    if(n >= 160) {
        instance->decoder.parser_step = Eg53ma4DecoderStepReset;
        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;
        instance->head_64_bit = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
    }
}

static uint8_t tpms_protocol_decoder_schrader_eg53ma4_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderEg53ma4* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

static SubGhzProtocolStatus tpms_protocol_decoder_schrader_eg53ma4_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderEg53ma4* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

static SubGhzProtocolStatus tpms_protocol_decoder_schrader_eg53ma4_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderEg53ma4* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, EG53MA4_PAYLOAD_BITS);
}

static void tpms_protocol_decoder_schrader_eg53ma4_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderEg53ma4* instance = context;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%06lX\r\n"
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
// Mirrors the Schrader MRX-GG4 encoder (schrader_gg4.c) -- same OOK / Manchester
// timing family and the same Manchester-II half-symbol convention as the decoder
// above (the decoder reads Manchester I and inverts the output bit):
//
//   bit 0 -> first half LOW, second half HIGH
//   bit 1 -> first half HIGH, second half LOW
//
// Frame on the wire (all half-symbols te_short unless run-length merged):
//   [inter-frame gap LOW]
//   [HIGH pilot te_long*2]   -- separates the gap from the first data half-symbol
//                               so the leading LOW does not merge into the gap;
//                               the decoder classifies it as a reset event and
//                               re-arms its Manchester state at Start1 (exactly
//                               like GG4 after its start pulse).
//   [EG53MA4_SYNC_BITS x 0]  -- 40-bit sync/preamble, discarded by the receiver.
//   [80 payload bits b[0..9], MSB first]
//
// Because the receiver keeps only the trailing 80 decoded bits, the 40 sync
// zeros give the Manchester decoder ample runway to lock before the payload.

// Worst-case: 120 bits * 2 half-symbols = 240, plus gap + pilot + trailing
// flush. 300 LevelDuration entries is plenty even before run-length merging.
#define TPMS_SCHRADER_EG53MA4_ENCODER_UPLOAD_SIZE 300

// Rebuild the full 10-byte payload from the generic fields and RECOMPUTE the
// checksum so an edited / 0-PSI frame is always valid.
//   b[0..1] : high flag bytes -- not carried in the 64-bit generic.data word,
//             defaulted to 0 (the receiver re-derives them from the wire and the
//             byte-sum checksum stays consistent because we transmit them too).
//   b[2..3] : low flag bytes, preserved from generic.data.
//   b[4..6] : 24-bit ID from generic.id.
//   b[7]    : pressure raw (already in generic.data from pack()).
//   b[8]    : temperature (degrees F, already in generic.data from pack()).
//   b[9]    : checksum = (sum of b[0..8]) & 0xFF.
static void eg53ma4_build_frame(const TPMSBlockGeneric* generic, uint8_t b[10]) {
    for(int i = 0; i < 8; i++) {
        b[i + 2] = (uint8_t)((generic->data >> (8 * (7 - i))) & 0xFF);
    }
    b[0] = 0;
    b[1] = 0;
    b[4] = (uint8_t)((generic->id >> 16) & 0xFF);
    b[5] = (uint8_t)((generic->id >> 8) & 0xFF);
    b[6] = (uint8_t)(generic->id & 0xFF);
    b[9] = subghz_protocol_blocks_add_bytes(b, 9);
}

static void* tpms_protocol_encoder_schrader_eg53ma4_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderEg53ma4* instance = malloc(sizeof(TPMSProtocolEncoderEg53ma4));
    instance->base.protocol = &tpms_protocol_schrader_eg53ma4;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_SCHRADER_EG53MA4_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

static void tpms_protocol_encoder_schrader_eg53ma4_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderEg53ma4* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

static void tpms_protocol_encoder_schrader_eg53ma4_stop(void* context) {
    TPMSProtocolEncoderEg53ma4* instance = context;
    instance->encoder.is_running = false;
}

static bool
    tpms_protocol_encoder_schrader_eg53ma4_get_upload(TPMSProtocolEncoderEg53ma4* instance) {
    furi_assert(instance);

    const uint32_t te_short = tpms_protocol_schrader_eg53ma4_const.te_short;
    const uint32_t te_long = tpms_protocol_schrader_eg53ma4_const.te_long;

    uint8_t b[10];
    eg53ma4_build_frame(&instance->generic, b);

    size_t index = 0;
    const size_t max = instance->encoder.size_upload;
    bool last_level = false;
    uint32_t last_duration = 0;

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

    // Inter-frame gap before the pilot so consecutive repeats are framed.
    EMIT(false, te_short * 50);

    // HIGH pilot (~492 us): a reset event for the decoder, which re-arms its
    // Manchester state and prevents the first data LOW from merging into the gap.
    EMIT(true, te_long * 2);

    // 40-bit sync/preamble (all zeros): decoder discards the leading 40 bits.
    for(int i = 0; i < EG53MA4_SYNC_BITS; i++) {
        EMIT(false, te_short);
        EMIT(true, te_short);
    }

    // 80 payload bits (b[0..9]), MSB first, Manchester-II.
    for(int byte = 0; byte < 10; byte++) {
        for(int bit = 7; bit >= 0; bit--) {
            if((b[byte] >> bit) & 0x1) {
                EMIT(true, te_short);
                EMIT(false, te_short);
            } else {
                EMIT(false, te_short);
                EMIT(true, te_short);
            }
        }
    }

    // Flush trailing entry.
    if(last_duration != 0) {
        if(index >= max) return false;
        instance->encoder.upload[index++] = level_duration_make(last_level, last_duration);
    }

#undef EMIT

    instance->encoder.size_upload = index;
    return true;
}

static SubGhzProtocolStatus tpms_protocol_encoder_schrader_eg53ma4_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderEg53ma4* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic, flipper_format, EG53MA4_PAYLOAD_BITS);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }

        // Optional repeat override.
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        // Restore allocator-side upload capacity before regenerating the upload.
        instance->encoder.size_upload = TPMS_SCHRADER_EG53MA4_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_schrader_eg53ma4_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

static LevelDuration tpms_protocol_encoder_schrader_eg53ma4_yield(void* context) {
    TPMSProtocolEncoderEg53ma4* instance = context;

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

void tpms_protocol_schrader_eg53ma4_pack(TPMSBlockGeneric* g) {
    furi_assert(g);

    // Preserve captured flag/data bytes b[2..9] from the 64-bit generic.data.
    uint8_t b[10];
    for(int i = 0; i < 8; i++) {
        b[i + 2] = (uint8_t)((g->data >> (8 * (7 - i))) & 0xFF);
    }

    // High flag bytes are not carried in generic.data; default to 0.
    b[0] = 0;
    b[1] = 0;

    // 24-bit ID -> b[4..6].
    b[4] = (uint8_t)((g->id >> 16) & 0xFF);
    b[5] = (uint8_t)((g->id >> 8) & 0xFF);
    b[6] = (uint8_t)(g->id & 0xFF);

    // Pressure: decoder computes bar = raw * 2.5 * 0.01 (25 mbar/bit),
    // so raw = bar / 0.025. Clamp to a byte (0 => 0 PSI).
    int32_t pressure_raw = (int32_t)(g->pressure / 0.025f + 0.5f);
    if(pressure_raw < 0) pressure_raw = 0;
    if(pressure_raw > 0xFF) pressure_raw = 0xFF;
    b[7] = (uint8_t)pressure_raw;

    // Temperature: decoder stores Celsius from a Fahrenheit byte,
    // C = (F - 32) * 5/9, so F = C * 9/5 + 32.
    int32_t temperature_raw = (int32_t)(g->temperature * 9.0f / 5.0f + 32.0f + 0.5f);
    if(temperature_raw < 0) temperature_raw = 0;
    if(temperature_raw > 0xFF) temperature_raw = 0xFF;
    b[8] = (uint8_t)temperature_raw;

    // Checksum: (sum of b[0..8]) & 0xFF.
    b[9] = subghz_protocol_blocks_add_bytes(b, 9);

    // Store b[2..9] back into generic.data (64 bits, as the decoder holds it).
    uint64_t data = 0;
    for(int i = 0; i < 8; i++) {
        data = (data << 8) | b[i + 2];
    }
    g->data = data;
    g->data_count_bit = EG53MA4_PAYLOAD_BITS;
}
