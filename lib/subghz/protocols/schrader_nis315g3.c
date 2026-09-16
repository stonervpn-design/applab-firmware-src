#include "schrader_nis315g3.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include "tpms_generic.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "SchraderNIS315G3"

// Port of the schrader_NIS315G3_decode() path (schrader_SMD3MA4_family_decode
// with model == SCHRADER_NIS315G3) in rtl_433/src/devices/schraeder.c
// (Copyright 2016 Benjamin Larsson / 2017 Christian W. Zuckschwerdt, GPL-2+).
//
// TPMS Model: Schrader MRXNIS315G3, 3039 (Infiniti, Nissan, Renault), also
// sold as Redi-Sensor SE10001HP / SE10001HPR. Sibling of the SMD3MA4 (Subaru)
// decoder in the same file: identical wire format, the ONLY difference is the
// pressure scale (NIS315G3 = 0.25 PSI/bit, SMD3MA4 = 0.2 PSI/bit).
//
// Frequency: 315 MHz (also seen at 433.92 MHz on some markets).
// Modulation: OOK, rtl_433 OOK_PULSE_PCM short=long=120 us, reset_limit 480 us,
//             the raw 120 us NRZ bits are then Manchester-decoded (one data bit
//             per two raw bits -> full-bit ~240 us). This is the same low-level
//             OOK/Manchester timing family as the Schrader MRX-GG4 / EG53MA4
//             already ported here, so we reuse their proven Manchester-II
//             convention: decode Manchester-I, then invert the output bit.
//
// Preamble (rtl_433): full sync word 0xF5555555E; the decoder searches for its
// last 16 bits {0x55,0x5E}. The 0x5555.. run is simply the Manchester clock
// (an alternating half-symbol train), which a constant run of Manchester-0 bits
// reproduces exactly on the wire -- so we emit a short run of sync zeros as the
// preamble and keep the trailing data-word aligned by the fixed leading 1 bit.
//
// Data word: NUM_BITS_DATA = 38 bits, MSB first:
//   1 FFF SSSSSSSSSSSSSSSSSSSSSSSS PPPPPPPP XX
//   |  |  |                        |        |
//   |  |  |                        |        +-- 2-bit checksum
//   |  |  |                        +----------- 8-bit pressure raw (PSI = raw*0.25)
//   |  |  +------------------------------------ 24-bit sensor ID
//   |  +--------------------------------------- 3-bit flags (0x7 = normal drive)
//   +------------------------------------------ fixed 1 bit
//
// Byte view used by rtl_433 (b[0..4], the 38 bits left-aligned, low 2 bits of
// b[4] are zero padding):
//   b[0] = 1FFF SSSS
//   b[1] = SSSS SSSS
//   b[2] = SSSS SSSS
//   b[3] = SSSS PPPP
//   b[4] = PPPP XX00
//   flags     = (b[0] & 0x70) >> 4
//   serial_id = ((b[0]&0x0f)<<20) | (b[1]<<12) | (b[2]<<4) | (b[3]>>4)
//   pressure  = ((b[3]&0x0f)<<4) | (b[4]>>4)
//
// Checksum (rtl_433 "PARITY"): add all five bytes' four 2-bit groups (the fixed
// leading 1 bit and the two zero-pad bits included) modulo 4; a valid frame
// yields 1. Plus the all-zero sanity guard (reject b[0..3] all 0x00).
//
// There is NO temperature and NO battery data in this protocol.

#define NIS315G3_DATA_BITS 38
// Sync/preamble Manchester zeros emitted before the data word so the decoder's
// Manchester lock settles; the receiver keeps only the trailing 38 data bits.
#define NIS315G3_SYNC_BITS 8
#define NIS315G3_TOTAL_BITS (NIS315G3_SYNC_BITS + NIS315G3_DATA_BITS)
#define NIS315G3_DATA_MASK ((1ULL << NIS315G3_DATA_BITS) - 1)

static const SubGhzBlockConst tpms_protocol_schrader_nis315g3_const = {
    .te_short = 120,
    .te_long = 240,
    .te_delta = 55, // ~50% of te_short (Schrader OOK has poor sensitivity)
    .min_count_bit_for_found = NIS315G3_DATA_BITS,
};

typedef enum {
    Nis315g3DecoderStepReset = 0,
    Nis315g3DecoderStepData,
} Nis315g3DecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    TPMSBlockGeneric generic;
    ManchesterState manchester_saved_state;
} TPMSProtocolDecoderNis315g3;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    TPMSBlockGeneric generic;
} TPMSProtocolEncoderNis315g3;

// -- decoder forward declarations (file-static) ------------------------------
static void* tpms_protocol_decoder_schrader_nis315g3_alloc(SubGhzEnvironment* environment);
static void tpms_protocol_decoder_schrader_nis315g3_free(void* context);
static void tpms_protocol_decoder_schrader_nis315g3_reset(void* context);
static void
    tpms_protocol_decoder_schrader_nis315g3_feed(void* context, bool level, uint32_t duration);
static uint8_t tpms_protocol_decoder_schrader_nis315g3_get_hash_data(void* context);
static SubGhzProtocolStatus tpms_protocol_decoder_schrader_nis315g3_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
static SubGhzProtocolStatus tpms_protocol_decoder_schrader_nis315g3_deserialize(
    void* context,
    FlipperFormat* flipper_format);
static void tpms_protocol_decoder_schrader_nis315g3_get_string(void* context, FuriString* output);

// -- encoder forward declarations --------------------------------------------
static void* tpms_protocol_encoder_schrader_nis315g3_alloc(SubGhzEnvironment* environment);
static void tpms_protocol_encoder_schrader_nis315g3_free(void* context);
static void tpms_protocol_encoder_schrader_nis315g3_stop(void* context);
static SubGhzProtocolStatus
    tpms_protocol_encoder_schrader_nis315g3_deserialize(void* context, FlipperFormat* flipper_format);
static LevelDuration tpms_protocol_encoder_schrader_nis315g3_yield(void* context);

const SubGhzProtocolDecoder tpms_protocol_schrader_nis315g3_decoder = {
    .alloc = tpms_protocol_decoder_schrader_nis315g3_alloc,
    .free = tpms_protocol_decoder_schrader_nis315g3_free,
    .feed = tpms_protocol_decoder_schrader_nis315g3_feed,
    .reset = tpms_protocol_decoder_schrader_nis315g3_reset,
    .get_hash_data = tpms_protocol_decoder_schrader_nis315g3_get_hash_data,
    .serialize = tpms_protocol_decoder_schrader_nis315g3_serialize,
    .deserialize = tpms_protocol_decoder_schrader_nis315g3_deserialize,
    .get_string = tpms_protocol_decoder_schrader_nis315g3_get_string,
};

const SubGhzProtocolEncoder tpms_protocol_schrader_nis315g3_encoder = {
    .alloc = tpms_protocol_encoder_schrader_nis315g3_alloc,
    .free = tpms_protocol_encoder_schrader_nis315g3_free,
    .deserialize = tpms_protocol_encoder_schrader_nis315g3_deserialize,
    .stop = tpms_protocol_encoder_schrader_nis315g3_stop,
    .yield = tpms_protocol_encoder_schrader_nis315g3_yield,
};

const SubGhzProtocol tpms_protocol_schrader_nis315g3 = {
    .name = TPMS_PROTOCOL_SCHRADER_NIS315G3_NAME,
    .type = SubGhzProtocolTypeTpms,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Sensors |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Send,
    .decoder = &tpms_protocol_schrader_nis315g3_decoder,
    .encoder = &tpms_protocol_schrader_nis315g3_encoder,
};

// Split the 38-bit data word into the five bytes rtl_433 works with (the word
// left-aligned in 40 bits; the low 2 bits of b[4] are zero padding).
static void nis315g3_get_bytes(uint64_t data, uint8_t b[5]) {
    uint64_t d = data & NIS315G3_DATA_MASK;
    b[0] = (uint8_t)((d >> 30) & 0xFF);
    b[1] = (uint8_t)((d >> 22) & 0xFF);
    b[2] = (uint8_t)((d >> 14) & 0xFF);
    b[3] = (uint8_t)((d >> 6) & 0xFF);
    b[4] = (uint8_t)((d << 2) & 0xFF);
}

// rtl_433 parity: sum of every 2-bit group of b[0..4] modulo 4 must be 1.
static uint8_t nis315g3_checksum_sum(const uint8_t b[5]) {
    int sum = 0;
    for(int i = 0; i < 5; ++i) {
        sum += ((b[i] >> 0) & 0x3) + ((b[i] >> 2) & 0x3) + ((b[i] >> 4) & 0x3) +
               ((b[i] >> 6) & 0x3);
    }
    return (uint8_t)(sum & 0x3);
}

static void tpms_protocol_schrader_nis315g3_analyze(TPMSBlockGeneric* instance) {
    uint8_t b[5];
    nis315g3_get_bytes(instance->data, b);

    // 24-bit sensor ID (rtl_433 serial_id).
    instance->id =
        ((uint32_t)(b[0] & 0x0f) << 20) | ((uint32_t)b[1] << 12) | ((uint32_t)b[2] << 4) |
        ((uint32_t)b[3] >> 4);

    // 8-bit raw pressure. rtl_433: PSI = raw * 0.25 (NIS315G3). Framework stores
    // pressure in bar: bar = PSI * 0.0689476.
    uint8_t pressure_raw = (uint8_t)(((b[3] & 0x0f) << 4) | (b[4] >> 4));
    instance->pressure = (float)pressure_raw * 0.25f * 0.0689476f;

    // No temperature and no battery flag are transmitted by this protocol.
    instance->temperature = 0.0f;
    instance->battery_low = TPMS_NO_BATT;
}

static void* tpms_protocol_decoder_schrader_nis315g3_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolDecoderNis315g3* instance = calloc(1, sizeof(TPMSProtocolDecoderNis315g3));
    instance->base.protocol = &tpms_protocol_schrader_nis315g3;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->manchester_saved_state = ManchesterStateStart1;
    return instance;
}

static void tpms_protocol_decoder_schrader_nis315g3_free(void* context) {
    furi_assert(context);
    free(context);
}

static void tpms_protocol_decoder_schrader_nis315g3_reset(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderNis315g3* instance = context;
    instance->decoder.parser_step = Nis315g3DecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->manchester_saved_state = ManchesterStateStart1;
}

static ManchesterEvent nis315g3_evt(bool level, uint32_t duration) {
    bool is_long = false;
    if(DURATION_DIFF(duration, tpms_protocol_schrader_nis315g3_const.te_long) <
       tpms_protocol_schrader_nis315g3_const.te_delta) {
        is_long = true;
    } else if(
        DURATION_DIFF(duration, tpms_protocol_schrader_nis315g3_const.te_short) <
        tpms_protocol_schrader_nis315g3_const.te_delta) {
        is_long = false;
    } else {
        return ManchesterEventReset;
    }
    if(level)
        return is_long ? ManchesterEventLongHigh : ManchesterEventShortHigh;
    else
        return is_long ? ManchesterEventLongLow : ManchesterEventShortLow;
}

// Validate the trailing 38-bit data word and, if good, fill generic + fire.
static bool nis315g3_try_decode(TPMSProtocolDecoderNis315g3* instance) {
    uint8_t b[5];
    nis315g3_get_bytes(instance->decoder.decode_data, b);

    // Fixed leading bit must be 1 (rtl_433 layout "1FFF...").
    if(!(b[0] & 0x80)) return false;

    // rtl_433 sanity: reject an all-zero data word.
    if(!b[0] && !b[1] && !b[2] && !b[3]) return false;

    // rtl_433 parity: sum of 2-bit groups modulo 4 must equal 1.
    if(nis315g3_checksum_sum(b) != 1) return false;

    instance->generic.data = instance->decoder.decode_data & NIS315G3_DATA_MASK;
    instance->generic.data_count_bit = NIS315G3_DATA_BITS;
    tpms_protocol_schrader_nis315g3_analyze(&instance->generic);

    if(instance->base.callback) instance->base.callback(&instance->base, instance->base.context);
    return true;
}

static void
    tpms_protocol_decoder_schrader_nis315g3_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    TPMSProtocolDecoderNis315g3* instance = context;

    ManchesterEvent event = nis315g3_evt(level, duration);

    if(event == ManchesterEventReset) {
        // End of burst: the data word is the last 38 bits before this gap.
        if(instance->decoder.parser_step == Nis315g3DecoderStepData &&
           instance->decoder.decode_count_bit >= NIS315G3_DATA_BITS) {
            nis315g3_try_decode(instance);
        }
        instance->decoder.parser_step = Nis315g3DecoderStepReset;
        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
        return;
    }

    bool bit = false;
    bool have_bit = manchester_advance(
        instance->manchester_saved_state, event, &instance->manchester_saved_state, &bit);
    if(!have_bit) return;

    // Manchester-II: decoder emits Manchester-I, so invert (matches GG4/EG53MA4).
    bit = !bit;

    if(instance->decoder.parser_step == Nis315g3DecoderStepReset) {
        instance->decoder.parser_step = Nis315g3DecoderStepData;
        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;
    }

    subghz_protocol_blocks_add_bit(&instance->decoder, bit ? 1 : 0);

    // Opportunistic check at the nominal frame length (absorbs +/-1 Manchester
    // "zerobit" phase ambiguity). The trailing-gap path above is the reliable
    // one; this covers a burst whose final bit lands right before the gap.
    uint16_t n = instance->decoder.decode_count_bit;
    if(n == NIS315G3_TOTAL_BITS || n == NIS315G3_TOTAL_BITS + 1 || n == NIS315G3_TOTAL_BITS + 2) {
        if(nis315g3_try_decode(instance)) {
            instance->decoder.parser_step = Nis315g3DecoderStepReset;
            instance->decoder.decode_data = 0;
            instance->decoder.decode_count_bit = 0;
            instance->manchester_saved_state = ManchesterStateStart1;
            return;
        }
    }

    // Runaway guard: decode_count_bit is a uint8_t (wraps at 256), re-sync well
    // before that on a long noise burst.
    if(n >= 160) {
        instance->decoder.parser_step = Nis315g3DecoderStepReset;
        instance->decoder.decode_data = 0;
        instance->decoder.decode_count_bit = 0;
        instance->manchester_saved_state = ManchesterStateStart1;
    }
}

static uint8_t tpms_protocol_decoder_schrader_nis315g3_get_hash_data(void* context) {
    furi_assert(context);
    TPMSProtocolDecoderNis315g3* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

static SubGhzProtocolStatus tpms_protocol_decoder_schrader_nis315g3_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    TPMSProtocolDecoderNis315g3* instance = context;
    return tpms_block_generic_serialize(&instance->generic, flipper_format, preset);
}

static SubGhzProtocolStatus tpms_protocol_decoder_schrader_nis315g3_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolDecoderNis315g3* instance = context;
    return tpms_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, NIS315G3_DATA_BITS);
}

static void tpms_protocol_decoder_schrader_nis315g3_get_string(void* context, FuriString* output) {
    furi_assert(context);
    TPMSProtocolDecoderNis315g3* instance = context;
    uint8_t b[5];
    nis315g3_get_bytes(instance->generic.data, b);
    int flags = (b[0] & 0x70) >> 4;
    float psi = instance->generic.pressure / 0.0689476f;
    furi_string_printf(
        output,
        "%s\r\n"
        "Id:0x%06lX Fl:%X\r\n"
        "%2.1f PSI Bar:%2.2f",
        instance->generic.protocol_name,
        instance->generic.id,
        flags,
        (double)psi,
        (double)instance->generic.pressure);
}

// -----------------------------------------------------------------------------
// Encoder
// -----------------------------------------------------------------------------
//
// Same OOK / Manchester timing family and Manchester-II half-symbol convention
// as the decoder above (the decoder reads Manchester I and inverts):
//   bit 0 -> first half LOW, second half HIGH
//   bit 1 -> first half HIGH, second half LOW
//
// Frame on the wire (half-symbols te_short unless run-length merged):
//   [inter-frame gap LOW]
//   [HIGH pilot te_long*2 = ~480 us]  -- a reset event that re-arms the decoder
//                                        Manchester state (mirrors GG4/EG53MA4)
//   [NIS315G3_SYNC_BITS x 0]          -- Manchester clock preamble (= 0x5555..)
//   [38 data bits, MSB first]
//
// Worst-case size: NIS315G3_TOTAL_BITS * 2 half-symbols = 92, plus gap + pilot
// + trailing flush. 160 LevelDuration entries is ample before run-length merge.
#define TPMS_SCHRADER_NIS315G3_ENCODER_UPLOAD_SIZE 160

// Build the 38-bit data word from the generic fields and RECOMPUTE the 2-bit
// checksum so an edited / 0-PSI frame is always valid.
static uint64_t nis315g3_build_word(const TPMSBlockGeneric* generic) {
    uint32_t serial = generic->id & 0x00FFFFFFUL; // 24-bit ID

    // rtl_433: PSI = raw * 0.25; framework pressure is bar (PSI = bar/0.0689476).
    // raw = round(bar / 0.0689476 / 0.25).
    int32_t pressure_raw = (int32_t)((generic->pressure / 0.0689476f) / 0.25f + 0.5f);
    if(pressure_raw < 0) pressure_raw = 0;
    if(pressure_raw > 0xFF) pressure_raw = 0xFF;

    // Preserve the flags nibble already carried in generic.data (default 0x7 =
    // normal driving is set by pack() for a fresh frame).
    uint8_t prev_b0 = (uint8_t)((generic->data >> 30) & 0xFF);
    int flags = (prev_b0 & 0x70) >> 4;

    uint8_t b[5];
    // b[0] = 1 FFF SSSS : fixed 1, 3 flag bits, top ID nibble.
    b[0] = 0x80 | ((uint8_t)flags << 4) | ((serial >> 20) & 0x0F);
    b[1] = (serial >> 12) & 0xFF;
    b[2] = (serial >> 4) & 0xFF;
    b[3] = (uint8_t)(((serial & 0x0F) << 4) | (((uint8_t)pressure_raw >> 4) & 0x0F));
    // b[4] = PPPP XX00 : low pressure nibble, 2 checksum bits, 2 zero-pad bits.
    b[4] = (uint8_t)(((uint8_t)pressure_raw & 0x0F) << 4);

    // Choose the 2-bit checksum so the total 2-bit-group sum modulo 4 equals 1.
    uint8_t base = nis315g3_checksum_sum(b); // b[4] currently has XX = 0
    uint8_t check = (uint8_t)((1 - base) & 0x3);
    b[4] |= (uint8_t)(check << 2);

    uint64_t val40 = ((uint64_t)b[0] << 32) | ((uint64_t)b[1] << 24) | ((uint64_t)b[2] << 16) |
                     ((uint64_t)b[3] << 8) | (uint64_t)b[4];
    return (val40 >> 2) & NIS315G3_DATA_MASK; // drop the 2 zero-pad bits -> 38 bits
}

static void* tpms_protocol_encoder_schrader_nis315g3_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    TPMSProtocolEncoderNis315g3* instance = malloc(sizeof(TPMSProtocolEncoderNis315g3));
    instance->base.protocol = &tpms_protocol_schrader_nis315g3;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = TPMS_SCHRADER_NIS315G3_ENCODER_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

static void tpms_protocol_encoder_schrader_nis315g3_free(void* context) {
    furi_assert(context);
    TPMSProtocolEncoderNis315g3* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

static void tpms_protocol_encoder_schrader_nis315g3_stop(void* context) {
    TPMSProtocolEncoderNis315g3* instance = context;
    instance->encoder.is_running = false;
}

static bool
    tpms_protocol_encoder_schrader_nis315g3_get_upload(TPMSProtocolEncoderNis315g3* instance) {
    furi_assert(instance);

    const uint32_t te_short = tpms_protocol_schrader_nis315g3_const.te_short;
    const uint32_t te_long = tpms_protocol_schrader_nis315g3_const.te_long;

    // Recompute the word (and checksum) from whatever generic currently holds.
    uint64_t word = nis315g3_build_word(&instance->generic);

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

    // HIGH pilot (~480 us = reset_limit): a reset event for the decoder, which
    // re-arms its Manchester state and prevents the first data LOW from merging
    // into the gap.
    EMIT(true, te_long * 2);

    // Manchester-clock preamble (all zeros = 0x5555.. on the wire).
    for(int i = 0; i < NIS315G3_SYNC_BITS; i++) {
        EMIT(false, te_short);
        EMIT(true, te_short);
    }

    // 38 data bits, MSB first, Manchester-II.
    for(int bit = NIS315G3_DATA_BITS - 1; bit >= 0; bit--) {
        if((word >> bit) & 0x1ULL) {
            EMIT(true, te_short);
            EMIT(false, te_short);
        } else {
            EMIT(false, te_short);
            EMIT(true, te_short);
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

static SubGhzProtocolStatus tpms_protocol_encoder_schrader_nis315g3_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    TPMSProtocolEncoderNis315g3* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        res = tpms_block_generic_deserialize_check_count_bit(
            &instance->generic, flipper_format, NIS315G3_DATA_BITS);
        if(res != SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }

        // Optional repeat override.
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        // Restore allocator-side upload capacity before regenerating the upload.
        instance->encoder.size_upload = TPMS_SCHRADER_NIS315G3_ENCODER_UPLOAD_SIZE;
        if(!tpms_protocol_encoder_schrader_nis315g3_get_upload(instance)) {
            res = SubGhzProtocolStatusErrorEncoderGetUpload;
            break;
        }
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
    } while(false);
    return res;
}

static LevelDuration tpms_protocol_encoder_schrader_nis315g3_yield(void* context) {
    TPMSProtocolEncoderNis315g3* instance = context;

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

void tpms_protocol_schrader_nis315g3_pack(TPMSBlockGeneric* g) {
    furi_assert(g);

    // Fresh frame: force the fixed leading 1 bit and default flags = 0x7
    // (normal driving) into the flags nibble that nis315g3_build_word reads
    // back, so build_word starts from a valid header instead of stale data.
    g->data = ((uint64_t)(0x80 | (0x7 << 4)) << 30);
    g->data = nis315g3_build_word(g);
    g->data_count_bit = NIS315G3_DATA_BITS;
}
