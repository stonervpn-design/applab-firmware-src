/**
 * Audi V3 SubGHz Protocol — 2-FSK Manchester rolling code (Q5 8R, 2014)
 *
 * Physical: 2-FSK @ ~433.89 MHz, FuriHalSubGhzPreset2FSKDev12KAsync
 * Manchester: te_short (half-bit) ~383us, te_long (full bit) ~766us.
 *   (FSK-demodulated baseband: marks/spaces both cluster at ~375-400us short,
 *    ~750-775us long; a single ~580us split separates short/long.)
 *
 * Capture characteristics: like every FSK keyfob recording, the discriminator
 * emits continuous noise between bursts, so most of the file is garbage. The
 * real packet is found by a NARROW preamble gate: >=20 consecutive pulses in
 * [220,470]us (the clean alternating preamble), after which the first LONG
 * pulse triggers data collection.
 *
 * Frame (96 Manchester bits collected after the trigger LONG):
 *   bit  0.. 7 : BUTTON   — 0x0C = Unlock/Open, 0x0D = Lock/Close.
 *   bit  8..63 : HOP      — 56-bit encrypted rolling code.
 *   bit 64..87 : COUNTER  — 24-bit plaintext event counter, increments +1 per
 *                           transmitted RF frame and is SHARED across buttons
 *                           (open burst 0x0875FA..0x0875FF continues into close
 *                           burst 0x087613..). Captured frames step by +1/+2
 *                           because the noisy FSK capture drops some of the
 *                           per-press RF repeats. Verified across 29 frames.
 *   bit 88..95 : CHK      — 8-bit trailer; not a plain CRC-8 over the cleartext
 *                           bytes (brute-forced all poly/init/refl) -> treated
 *                           as a proprietary checksum / encrypted trailer byte.
 *
 * The 56-bit hop and the trailer are encrypted (no cleartext serial is
 * transmitted), so only Button + Counter are decoded; Hop + Chk are shown raw
 * (decode-only dev).
 *
 * NB: the earlier 32-bit "counter" reading (bits 64..95) was wrong — it folded
 * the random trailer byte into the low 8 bits, which is why it appeared to jump
 * by ~+400 per frame instead of +1.
 *
 * Distinct from existing Audi decoders: audi_v1 is OOK Manchester 260/520 (88-bit),
 * audi_v2 is OOK Manchester 280/560 (72-bit). This is 2-FSK 383/766 (96-bit).
 *
 * Source signals: Audi Q5 (2014) keyfob, Q5_2014_open_x15 / Q5_2014_close.
 */

#include "audi_v3.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include <furi.h>
#include <furi_hal.h>

#define TAG "SubGhzProtocolAudiV3"

// Timing bands (us)
#define AUDI_V3_PRE_LO  220 // preamble short: lower bound
#define AUDI_V3_PRE_HI  470 // preamble short: upper bound
#define AUDI_V3_LONG_LO 580 // full-bit (LONG): lower bound
#define AUDI_V3_LONG_HI 980 // full-bit (LONG): upper bound
#define AUDI_V3_GAP     980 // >= this ends a burst
#define AUDI_V3_MIN_PRE 20  // minimum clean preamble pulses before trigger

// Frame layout
#define AUDI_V3_BUTTON_BITS  8
#define AUDI_V3_HOP_BITS     56
#define AUDI_V3_COUNTER_BITS 24
#define AUDI_V3_CHK_BITS     8
#define AUDI_V3_FRAME_BITS                                                          \
    (AUDI_V3_BUTTON_BITS + AUDI_V3_HOP_BITS + AUDI_V3_COUNTER_BITS + AUDI_V3_CHK_BITS) // 96
#define AUDI_V3_HOP_BYTES (AUDI_V3_HOP_BITS / 8) // 7

// Bit offsets
#define AUDI_V3_HOP_OFF     (AUDI_V3_BUTTON_BITS)                       // 8
#define AUDI_V3_COUNTER_OFF (AUDI_V3_BUTTON_BITS + AUDI_V3_HOP_BITS)    // 64
#define AUDI_V3_CHK_OFF     (AUDI_V3_COUNTER_OFF + AUDI_V3_COUNTER_BITS) // 88

// Known button codes
#define AUDI_V3_BTN_UNLOCK 0x0C
#define AUDI_V3_BTN_LOCK   0x0D

static const SubGhzBlockConst subghz_protocol_audi_v3_const __attribute__((unused)) = {
    .te_short = 383,
    .te_long = 766,
    .te_delta = 180,
    .min_count_bit_for_found = AUDI_V3_FRAME_BITS,
};

// ============================================================================
// DATA STRUCTURES
// ============================================================================

typedef enum {
    AudiV3StepReset = 0,
    AudiV3StepPreamble,
    AudiV3StepData,
} AudiV3Step;

struct SubGhzProtocolDecoderAudiV3 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    AudiV3Step step;
    uint16_t preamble_count;
    bool half_bit;
    uint8_t all_bits[AUDI_V3_FRAME_BITS + 8];
    uint16_t bit_count;

    uint8_t button;                    // bits 0..7
    uint8_t hop[AUDI_V3_HOP_BYTES];    // bits 8..63 (56-bit rolling code)
    uint32_t counter;                  // bits 64..87 (24-bit plaintext counter)
    uint8_t chk;                       // bits 88..95 (trailer / checksum byte)
};

struct SubGhzProtocolEncoderAudiV3 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint8_t button;                 // bits 0..7
    uint8_t hop[AUDI_V3_HOP_BYTES]; // bits 8..63
    uint32_t counter;               // bits 64..87
    uint8_t chk;                    // bits 88..95
};

// ============================================================================
// PROTOCOL DEFINITION
// ============================================================================

static SubGhzBlockGeneric* subghz_protocol_decoder_audi_v3_get_generic(void* context) {
    SubGhzProtocolDecoderAudiV3* instance = context;
    return &instance->generic;
}

const SubGhzProtocolDecoder subghz_protocol_audi_v3_decoder = {
    .alloc = subghz_protocol_decoder_audi_v3_alloc,
    .free = subghz_protocol_decoder_audi_v3_free,
    .feed = subghz_protocol_decoder_audi_v3_feed,
    .reset = subghz_protocol_decoder_audi_v3_reset,
    .get_hash_data = subghz_protocol_decoder_audi_v3_get_hash_data,
    .serialize = subghz_protocol_decoder_audi_v3_serialize,
    .deserialize = subghz_protocol_decoder_audi_v3_deserialize,
    .get_string = subghz_protocol_decoder_audi_v3_get_string,
    .get_generic = subghz_protocol_decoder_audi_v3_get_generic,
};

const SubGhzProtocolEncoder subghz_protocol_audi_v3_encoder = {
    .alloc = subghz_protocol_encoder_audi_v3_alloc,
    .free = subghz_protocol_encoder_audi_v3_free,
    .deserialize = subghz_protocol_encoder_audi_v3_deserialize,
    .stop = subghz_protocol_encoder_audi_v3_stop,
    .yield = subghz_protocol_encoder_audi_v3_yield,
};

const SubGhzProtocol subghz_protocol_audi_v3 = {
    .name = SUBGHZ_PROTOCOL_AUDI_V3_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_audi_v3_decoder,
    .encoder = &subghz_protocol_audi_v3_encoder,
};

// ============================================================================
// DECODER
// ============================================================================

void* subghz_protocol_decoder_audi_v3_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderAudiV3* instance = malloc(sizeof(SubGhzProtocolDecoderAudiV3));
    instance->base.protocol = &subghz_protocol_audi_v3;
    instance->generic.protocol_name = instance->base.protocol->name;
    subghz_protocol_decoder_audi_v3_reset(instance);
    return instance;
}

void subghz_protocol_decoder_audi_v3_free(void* context) {
    furi_assert(context);
    free(context);
}

void subghz_protocol_decoder_audi_v3_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderAudiV3* instance = context;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->step = AudiV3StepReset;
    instance->preamble_count = 0;
    instance->half_bit = false;
    instance->bit_count = 0;
    instance->button = 0;
    instance->counter = 0;
    instance->chk = 0;
}

/**
 * Validate and emit a completed 96-bit frame.
 * Splits into button (bits 0..7), 56-bit hop (8..63), 24-bit counter (64..87)
 * and an 8-bit trailer/checksum (88..95).
 * Degenerate frames with a uniform 00.. / FF.. hop (false triggers in FSK
 * noise) are dropped.
 */
static void audi_v3_process_frame(SubGhzProtocolDecoderAudiV3* instance) {
    if(instance->bit_count < AUDI_V3_FRAME_BITS) return;

    // Button: bits 0..7
    uint8_t button = 0;
    for(int i = 0; i < AUDI_V3_BUTTON_BITS; i++) {
        button = (uint8_t)((button << 1) | instance->all_bits[i]);
    }

    // Hop: bits 8..63 -> 7 bytes, MSB first
    uint8_t hop[AUDI_V3_HOP_BYTES];
    for(int b = 0; b < AUDI_V3_HOP_BYTES; b++) {
        uint8_t v = 0;
        for(int k = 0; k < 8; k++) {
            v = (uint8_t)((v << 1) | instance->all_bits[AUDI_V3_HOP_OFF + b * 8 + k]);
        }
        hop[b] = v;
    }

    // Counter: bits 64..87 (24-bit plaintext)
    uint32_t counter = 0;
    for(int i = 0; i < AUDI_V3_COUNTER_BITS; i++) {
        counter = (counter << 1) | instance->all_bits[AUDI_V3_COUNTER_OFF + i];
    }

    // Trailer / checksum: bits 88..95
    uint8_t chk = 0;
    for(int i = 0; i < AUDI_V3_CHK_BITS; i++) {
        chk = (uint8_t)((chk << 1) | instance->all_bits[AUDI_V3_CHK_OFF + i]);
    }

    // Reject obvious noise frames (false trigger inside FSK noise / trailing
    // preamble). A real rolling code never decodes to a uniform 00.. / FF.. hop.
    bool hop_all_ff = true, hop_all_00 = true;
    for(int b = 0; b < AUDI_V3_HOP_BYTES; b++) {
        if(hop[b] != 0xFF) hop_all_ff = false;
        if(hop[b] != 0x00) hop_all_00 = false;
    }
    if(hop_all_ff || hop_all_00) {
        FURI_LOG_D(TAG, "Rejected degenerate frame (uniform hop)");
        return;
    }

    instance->button = button;
    for(int b = 0; b < AUDI_V3_HOP_BYTES; b++) instance->hop[b] = hop[b];
    instance->counter = counter;
    instance->chk = chk;

    // generic.data = first 64 bits (button + hop) for hashing / dedup
    uint64_t data = button;
    for(int b = 0; b < AUDI_V3_HOP_BYTES; b++) {
        data = (data << 8) | hop[b];
    }
    instance->generic.data = data;
    instance->generic.data_count_bit = AUDI_V3_FRAME_BITS;
    instance->generic.serial = 0;
    instance->generic.btn = button;
    instance->generic.cnt = counter;
    instance->decoder.decode_data = data;
    instance->decoder.decode_count_bit = AUDI_V3_FRAME_BITS;

    FURI_LOG_I(
        TAG,
        "Decoded Btn:%02X Cnt:%06lX Chk:%02X Hop:%02X%02X%02X...",
        button,
        (unsigned long)counter,
        chk,
        hop[0],
        hop[1],
        hop[2]);

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

static void audi_v3_restart(SubGhzProtocolDecoderAudiV3* instance) {
    instance->step = AudiV3StepReset;
    instance->preamble_count = 0;
    instance->half_bit = false;
    instance->bit_count = 0;
}

void subghz_protocol_decoder_audi_v3_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderAudiV3* instance = context;

    bool in_pre = (duration >= AUDI_V3_PRE_LO && duration <= AUDI_V3_PRE_HI);
    bool is_long = (duration >= AUDI_V3_LONG_LO && duration <= AUDI_V3_LONG_HI);
    bool is_short = (duration < AUDI_V3_LONG_LO);

    switch(instance->step) {
    case AudiV3StepReset:
        if(in_pre) {
            instance->preamble_count = 1;
            instance->step = AudiV3StepPreamble;
        }
        break;

    case AudiV3StepPreamble:
        if(in_pre) {
            instance->preamble_count++;
        } else if(is_long && instance->preamble_count >= AUDI_V3_MIN_PRE) {
            // First LONG after a clean preamble = first data bit.
            // Manchester polarity is inverted for this signal: bit = !level
            // (verified against Btn 0x0C/0x0D + monotonic counter; level convention
            //  yields the bitwise complement).
            instance->step = AudiV3StepData;
            instance->bit_count = 0;
            instance->half_bit = false;
            instance->all_bits[instance->bit_count++] = level ? 0 : 1;
        } else {
            audi_v3_restart(instance);
        }
        break;

    case AudiV3StepData:
        if(duration >= AUDI_V3_GAP) {
            // burst ended -> emit what we have, restart
            audi_v3_process_frame(instance);
            audi_v3_restart(instance);
            break;
        }
        if(is_long) {
            if(instance->half_bit) {
                // Manchester violation -> frame boundary
                audi_v3_process_frame(instance);
                audi_v3_restart(instance);
            } else {
                instance->all_bits[instance->bit_count++] = level ? 0 : 1;
            }
        } else if(is_short) {
            if(instance->half_bit) {
                instance->all_bits[instance->bit_count++] = level ? 0 : 1;
                instance->half_bit = false;
            } else {
                instance->half_bit = true;
            }
        }

        if(instance->bit_count >= AUDI_V3_FRAME_BITS) {
            audi_v3_process_frame(instance);
            audi_v3_restart(instance);
        }
        break;

    default:
        audi_v3_restart(instance);
        break;
    }
}

// ============================================================================
// HASH
// ============================================================================

uint8_t subghz_protocol_decoder_audi_v3_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderAudiV3* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

// ============================================================================
// SERIALIZATION
// ============================================================================

SubGhzProtocolStatus subghz_protocol_decoder_audi_v3_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderAudiV3* instance = context;

    SubGhzProtocolStatus res =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(res != SubGhzProtocolStatusOk) return res;

    if(!flipper_format_write_hex(flipper_format, "Hop", instance->hop, AUDI_V3_HOP_BYTES))
        return SubGhzProtocolStatusErrorParserOthers;

    // 24-bit counter -> 3 bytes
    uint8_t cnt[3] = {
        (uint8_t)(instance->counter >> 16),
        (uint8_t)(instance->counter >> 8),
        (uint8_t)(instance->counter)};
    if(!flipper_format_write_hex(flipper_format, "Counter", cnt, 3))
        return SubGhzProtocolStatusErrorParserOthers;

    if(!flipper_format_write_hex(flipper_format, "Chk", &instance->chk, 1))
        return SubGhzProtocolStatusErrorParserOthers;

    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_audi_v3_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderAudiV3* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize(&instance->generic, flipper_format);
    if(ret != SubGhzProtocolStatusOk) return ret;

    flipper_format_read_hex(flipper_format, "Hop", instance->hop, AUDI_V3_HOP_BYTES);
    uint8_t cnt[3] = {0};
    if(flipper_format_read_hex(flipper_format, "Counter", cnt, 3)) {
        instance->counter =
            ((uint32_t)cnt[0] << 16) | ((uint32_t)cnt[1] << 8) | cnt[2];
    }
    flipper_format_read_hex(flipper_format, "Chk", &instance->chk, 1);
    // generic deserialize restores only data (not btn); button is the top byte of
    // the packed 64-bit data (button<<56 | hop56), so derive it from there.
    instance->button = (uint8_t)(instance->generic.data >> 56);
    instance->generic.protocol_name = instance->base.protocol->name;
    return SubGhzProtocolStatusOk;
}

// ============================================================================
// DISPLAY
// ============================================================================

void subghz_protocol_decoder_audi_v3_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderAudiV3* instance = context;

    const char* btn_name = "?";
    switch(instance->button) {
    case AUDI_V3_BTN_UNLOCK:
        btn_name = "Unlock";
        break;
    case AUDI_V3_BTN_LOCK:
        btn_name = "Lock";
        break;
    default:
        btn_name = "?";
        break;
    }

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Btn:%02X [%s]\r\n"
        "Cnt:%06lX Chk:%02X\r\n"
        "Hop:",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        instance->button,
        btn_name,
        (unsigned long)instance->counter,
        instance->chk);

    for(int b = 0; b < AUDI_V3_HOP_BYTES; b++) {
        furi_string_cat_printf(output, "%02X", instance->hop[b]);
    }
}

// ============================================================================
// ENCODER
//
// Reproduces the exact 2-FSK Manchester waveform the decoder accepts:
//   [>=MIN_PRE short preamble pulses][trigger LONG][96 Manchester bits][gap]
// Each data bit is one symbol: a single LONG pulse, or a SHORT,SHORT pair.
// Polarity is inverted (recorded bit = !level). For every bit exactly one of
// {LONG, SHORT+SHORT} records the wanted value given the current line level, so
// the symbol choice is forced — this round-trips the decoder bit-for-bit.
// ============================================================================

// preamble margin above the decoder minimum
#define AUDI_V3_ENC_PREAMBLE (AUDI_V3_MIN_PRE + 6)
// worst case: preamble + trigger + 2*(FRAME_BITS-1) + gap
#define AUDI_V3_ENC_UPLOAD_SIZE (AUDI_V3_ENC_PREAMBLE + 2 + 2 * AUDI_V3_FRAME_BITS)

void* subghz_protocol_encoder_audi_v3_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderAudiV3* instance = malloc(sizeof(SubGhzProtocolEncoderAudiV3));
    instance->base.protocol = &subghz_protocol_audi_v3;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 3;
    instance->encoder.size_upload = AUDI_V3_ENC_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_audi_v3_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderAudiV3* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

static void subghz_protocol_encoder_audi_v3_get_upload(SubGhzProtocolEncoderAudiV3* instance) {
    furi_assert(instance);

    // Rebuild the 96-bit frame (MSB first per field).
    uint8_t bits[AUDI_V3_FRAME_BITS];
    for(int i = 0; i < AUDI_V3_BUTTON_BITS; i++)
        bits[i] = (instance->button >> (7 - i)) & 1;
    for(int b = 0; b < AUDI_V3_HOP_BYTES; b++)
        for(int k = 0; k < 8; k++)
            bits[AUDI_V3_HOP_OFF + b * 8 + k] = (instance->hop[b] >> (7 - k)) & 1;
    for(int i = 0; i < AUDI_V3_COUNTER_BITS; i++)
        bits[AUDI_V3_COUNTER_OFF + i] =
            (instance->counter >> (AUDI_V3_COUNTER_BITS - 1 - i)) & 1;
    for(int i = 0; i < AUDI_V3_CHK_BITS; i++)
        bits[AUDI_V3_CHK_OFF + i] = (instance->chk >> (7 - i)) & 1;

    const uint32_t te_s = (uint32_t)subghz_protocol_audi_v3_const.te_short;
    const uint32_t te_l = (uint32_t)subghz_protocol_audi_v3_const.te_long;
    size_t index = 0;
    const size_t enc_cap = AUDI_V3_ENC_UPLOAD_SIZE;

    // Trigger LONG records bit0 = !level -> level = !bits[0].
    bool tl = !(bits[0] != 0);

    // Preamble: alternating short pulses ending on !tl so the trigger alternates.
    const int P = AUDI_V3_ENC_PREAMBLE;
    bool lvl = (P & 1) ? !tl : tl;
    for(int p = 0; p < P; p++) {
        if(index < enc_cap) instance->encoder.upload[index++] = level_duration_make(lvl, te_s);
        lvl = !lvl;
    }

    // Trigger LONG (bit0).
    bool cur = tl;
    if(index < enc_cap) instance->encoder.upload[index++] = level_duration_make(cur, te_l);
    cur = !cur;

    // Data bits 1..95.
    for(int i = 1; i < AUDI_V3_FRAME_BITS; i++) {
        bool want = (bits[i] != 0);
        bool long_records = !cur; // inverted polarity
        if(long_records == want) {
            if(index < enc_cap) instance->encoder.upload[index++] = level_duration_make(cur, te_l);
            cur = !cur;
        } else {
            if(index < enc_cap) instance->encoder.upload[index++] = level_duration_make(cur, te_s);
            cur = !cur;
            if(index < enc_cap) instance->encoder.upload[index++] = level_duration_make(cur, te_s);
            cur = !cur;
        }
    }

    // End gap (>= AUDI_V3_GAP) — alternates from the last data pulse.
    if(index < enc_cap) instance->encoder.upload[index++] = level_duration_make(cur, (uint32_t)20000);

    instance->encoder.size_upload = index;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_audi_v3_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderAudiV3* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        if(subghz_block_generic_deserialize(&instance->generic, flipper_format) !=
           SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        if(instance->generic.data_count_bit != AUDI_V3_FRAME_BITS) {
            FURI_LOG_E(
                TAG, "Invalid bit count: %u", instance->generic.data_count_bit);
            res = SubGhzProtocolStatusErrorValueBitCount;
            break;
        }

        // Recover the extra serialized fields. generic restores only data/bit;
        // the button is the top byte of the packed 64-bit data (button<<56|hop56).
        flipper_format_rewind(flipper_format);
        flipper_format_read_hex(flipper_format, "Hop", instance->hop, AUDI_V3_HOP_BYTES);
        uint8_t cnt[3] = {0};
        if(flipper_format_read_hex(flipper_format, "Counter", cnt, 3)) {
            instance->counter =
                ((uint32_t)cnt[0] << 16) | ((uint32_t)cnt[1] << 8) | cnt[2];
        }
        flipper_format_read_hex(flipper_format, "Chk", &instance->chk, 1);
        instance->button = (uint8_t)(instance->generic.data >> 56);

        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        subghz_protocol_encoder_audi_v3_get_upload(instance);
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
        res = SubGhzProtocolStatusOk;
    } while(false);
    return res;
}

void subghz_protocol_encoder_audi_v3_stop(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderAudiV3* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_audi_v3_yield(void* context) {
    SubGhzProtocolEncoderAudiV3* instance = context;

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
