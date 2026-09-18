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

const SubGhzProtocol subghz_protocol_audi_v3 = {
    .name = SUBGHZ_PROTOCOL_AUDI_V3_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save,
    .decoder = &subghz_protocol_audi_v3_decoder,
    .encoder = NULL,
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

    // Reject unknown buttons: real frames carry only Unlock(0x0C) / Lock(0x0D)
    if(button != AUDI_V3_BTN_UNLOCK && button != AUDI_V3_BTN_LOCK) {
        FURI_LOG_D(TAG, "Rejected frame (unknown button 0x%02X)", button);
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
    instance->button = (uint8_t)instance->generic.btn;
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
