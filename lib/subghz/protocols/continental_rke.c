#include "continental_rke.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include "../defines.h" /* ENABLE_EMULATE_FEATURE (gates the TX encoder) */
#include "protocols_common.h"

/*
 * Continental RKE — Continental KR5V2X car remote keyfob decoder.
 *
 * Ported from the rtl_433 device "continental_car_remote"
 * (src/devices/continental_car_remote.c, merbanan/rtl_433).
 *
 * RF / demod (from the rtl_433 r_device):
 *   - Carrier ~313.8 MHz, 2-FSK  (FSK_PULSE_MANCHESTER_ZEROBIT)
 *   - Manchester-coded, half-bit ~100us, full-bit ~200us, reset gap 1500us
 *
 * Frame (after the 20-bit preamble 0xF0F06 == {0xF0,0xF0,0x60}[:20]):
 *   112 payload bits = 14 bytes, extracted MSB-first:
 *     bytes[0..3]   Remote ID (32-bit)
 *     bytes[4]      unknown
 *     bytes[5]>>4   Button (4-bit)
 *     bytes[6..8]   Sequence / rolling counter (24-bit)
 *     bytes[9..12]  Encrypted rolling code (32-bit)
 *     bytes[13]     XOR checksum
 *   Validation: xor_bytes(bytes, 14) == 0  (XOR of all 14 bytes is zero).
 *
 * Flipper feed() re-implements the rtl_433 manchester-zerobit pulse slicer as a
 * streaming state machine over LevelDuration pulses: it accumulates half-bit
 * durations, emits a bit on every data edge (falling edge = 1, rising = 0),
 * scans a rolling 20-bit window for the preamble, then collects 112 payload
 * bits and gates on the XOR checksum before emitting. The checksum gate is what
 * keeps this out of the false-positive territory the codebase has fought.
 *
 * Rolling-code / encrypted: the 32-bit authenticator (bytes[9..12]) is encrypted
 * under an unknown key, so the next code cannot be predicted (no predictor). The
 * encoder is therefore a ONE-SHOT verbatim replay of the captured frame, gated
 * under ENABLE_EMULATE_FEATURE (see the encoder block below).
 */

#define TAG "ContinentalRKE"

/* Timing, microseconds (rtl_433: short_width=100, long_width=200, reset=1500) */
#define CONTINENTAL_RKE_TE_SHORT   100
#define CONTINENTAL_RKE_TE_LONG    200
#define CONTINENTAL_RKE_TE_DELTA   40 /* ~+/-30-40% tolerance per symbol */
#define CONTINENTAL_RKE_TE_RESET   1500
/* Data-edge threshold: rtl_433 uses short_width * 1.5 */
#define CONTINENTAL_RKE_TE_THRESH  (CONTINENTAL_RKE_TE_SHORT + (CONTINENTAL_RKE_TE_SHORT / 2))
/* Upper bound on a single valid mark/space (a long symbol + tolerance) */
#define CONTINENTAL_RKE_TE_MAX     (CONTINENTAL_RKE_TE_LONG + CONTINENTAL_RKE_TE_DELTA)

/* 20-bit preamble: {0xF0,0xF0,0x60} first 20 bits = 0xF0F06 */
#define CONTINENTAL_RKE_PREAMBLE_20 0xF0F06UL
#define CONTINENTAL_RKE_PREAMBLE_MASK 0xFFFFFUL

#define CONTINENTAL_RKE_PAYLOAD_BYTES 14
#define CONTINENTAL_RKE_PAYLOAD_BITS  112 /* 14 bytes */
#define CONTINENTAL_RKE_KEY_BITS      64  /* MSB 8 bytes kept as the Key */

static const SubGhzBlockConst subghz_protocol_continental_rke_const = {
    .te_short = CONTINENTAL_RKE_TE_SHORT,
    .te_long = CONTINENTAL_RKE_TE_LONG,
    .te_delta = CONTINENTAL_RKE_TE_DELTA,
    .min_count_bit_for_found = CONTINENTAL_RKE_KEY_BITS,
};

// ============================================================================
// Instance state
// ============================================================================

struct SubGhzProtocolDecoderContinentalRke {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint32_t te_accum; /* accumulated half-bit duration (rtl_433 time_since_last) */
    uint32_t preamble_shift; /* rolling 20-bit window while hunting the preamble */
    bool collecting; /* true once preamble matched, collecting payload bits */
    uint16_t frame_bit; /* payload bits collected so far */
    uint8_t frame[CONTINENTAL_RKE_PAYLOAD_BYTES];
};


// ============================================================================
// Protocol registration
// ============================================================================

const SubGhzProtocolDecoder subghz_protocol_continental_rke_decoder = {
    .alloc = subghz_protocol_decoder_continental_rke_alloc,
    .free = subghz_protocol_decoder_continental_rke_free,
    .feed = subghz_protocol_decoder_continental_rke_feed,
    .reset = subghz_protocol_decoder_continental_rke_reset,
    .get_hash_data = subghz_protocol_decoder_continental_rke_get_hash_data,
    .serialize = subghz_protocol_decoder_continental_rke_serialize,
    .deserialize = subghz_protocol_decoder_continental_rke_deserialize,
    .get_string = subghz_protocol_decoder_continental_rke_get_string,
};

// ============================================================================
// Encoder: ONE-SHOT verbatim replay (rolling authenticator is encrypted under
// an unknown key, so NO predictor -- see the classification note at the bottom
// of this block). Rebuilds the exact 14-byte on-air frame from the retained
// decode state (Key + Cnt + Enc) and re-emits it as the same rtl_433
// Manchester-zerobit waveform this file's feed() slices, so a re-decode of the
// TX reproduces the captured frame byte-for-byte.
// ============================================================================

#ifdef ENABLE_EMULATE_FEATURE

#define CONTINENTAL_RKE_PREAMBLE_BITS 20U
#define CONTINENTAL_RKE_TOTAL_BITS \
    (CONTINENTAL_RKE_PREAMBLE_BITS + CONTINENTAL_RKE_PAYLOAD_BITS) /* 20 + 112 = 132 */
/* Worst case every bit needs 2 half-symbol pulses; + 1 trailing reset gap. */
#define CONTINENTAL_RKE_UPLOAD_CAPACITY ((CONTINENTAL_RKE_TOTAL_BITS * 2U) + 2U)
#define CONTINENTAL_RKE_TX_REPEAT       5U

_Static_assert(
    CONTINENTAL_RKE_UPLOAD_CAPACITY <= PP_SHARED_UPLOAD_CAPACITY,
    "CONTINENTAL_RKE_UPLOAD_CAPACITY exceeds shared upload slab");

typedef struct SubGhzProtocolEncoderContinentalRke {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
} SubGhzProtocolEncoderContinentalRke;

/* Emit one data bit as the inverse of feed()'s slicer. feed() emits a bit each
 * time accumulated duration crosses ~1.5*TE_SHORT, valuing it at the level of
 * the pulse that crossed. Because the RF envelope's pulses strictly alternate
 * level and accum is always 0 at a bit boundary, a bit whose value matches the
 * next-available level is a single TE_LONG pulse; a mismatched bit is a
 * non-triggering TE_SHORT pulse (flips parity) followed by a triggering
 * TE_SHORT pulse. *cur_level tracks the level available for the next pulse. */
static size_t continental_rke_emit_bit(
    LevelDuration* up,
    size_t idx,
    size_t cap,
    bool* cur_level,
    bool bit) {
    if(*cur_level == bit) {
        idx = pp_emit(up, idx, cap, bit, CONTINENTAL_RKE_TE_LONG);
        *cur_level = !bit;
    } else {
        idx = pp_emit(up, idx, cap, *cur_level, CONTINENTAL_RKE_TE_SHORT);
        *cur_level = !*cur_level; /* now equals bit */
        idx = pp_emit(up, idx, cap, bit, CONTINENTAL_RKE_TE_SHORT);
        *cur_level = !bit;
    }
    return idx;
}

static bool continental_rke_build_upload(
    SubGhzProtocolEncoderContinentalRke* instance,
    const uint8_t frame[CONTINENTAL_RKE_PAYLOAD_BYTES]) {
    furi_check(instance);

    LevelDuration* up = instance->encoder.upload;
    const size_t cap = CONTINENTAL_RKE_UPLOAD_CAPACITY;
    size_t idx = 0;

    /* 20-bit preamble 0xF0F06, MSB-first. Seed cur_level with the first bit so
     * it comes out as a single TE_LONG pulse (clean start after the reset gap). */
    bool first_bit = (bool)((CONTINENTAL_RKE_PREAMBLE_20 >> 19) & 1U);
    bool cur_level = first_bit;
    for(int8_t i = 19; i >= 0; i--) {
        idx = continental_rke_emit_bit(
            up, idx, cap, &cur_level, (bool)((CONTINENTAL_RKE_PREAMBLE_20 >> i) & 1U));
    }

    /* 112 payload bits (14 bytes), MSB-first. */
    for(size_t byte = 0; byte < CONTINENTAL_RKE_PAYLOAD_BYTES; byte++) {
        for(int8_t b = 7; b >= 0; b--) {
            idx = continental_rke_emit_bit(
                up, idx, cap, &cur_level, (bool)((frame[byte] >> b) & 1U));
        }
    }

    /* Terminate with a low reset gap (>= TE_RESET) so feed() re-arms cleanly
     * between repeats. */
    idx = pp_emit(up, idx, cap, false, CONTINENTAL_RKE_TE_RESET + 500U);

    if(idx > cap) {
        return false;
    }

    instance->encoder.front = 0;
    instance->encoder.size_upload = idx;
    return true;
}

static void* subghz_protocol_encoder_continental_rke_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderContinentalRke* instance =
        calloc(1, sizeof(SubGhzProtocolEncoderContinentalRke));
    furi_check(instance);
    instance->base.protocol = &subghz_protocol_continental_rke;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = CONTINENTAL_RKE_TX_REPEAT;
    instance->encoder.front = 0;
    instance->encoder.is_running = false;
    pp_encoder_buffer_ensure(instance, CONTINENTAL_RKE_UPLOAD_CAPACITY);
    return instance;
}

static SubGhzProtocolStatus subghz_protocol_encoder_continental_rke_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderContinentalRke* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    instance->encoder.is_running = false;
    instance->encoder.front = 0;

    do {
        if(pp_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
           SubGhzProtocolStatusOk) {
            break;
        }

        flipper_format_rewind(flipper_format);
        SubGhzProtocolStatus load_st = subghz_block_generic_deserialize_check_count_bit(
            &instance->generic,
            flipper_format,
            subghz_protocol_continental_rke_const.min_count_bit_for_found);
        if(load_st != SubGhzProtocolStatusOk) {
            ret = load_st;
            break;
        }

        /* Recover the counter LSB byte and the 32-bit encrypted authenticator the
         * decoder persisted alongside the 64-bit Key. Absent (older capture) =>
         * default 0: the frame still self-checks (XOR recomputed) but will not
         * match a real fob -- best-effort replay only. */
        uint32_t cnt = 0;
        uint32_t enc = 0;
        flipper_format_rewind(flipper_format);
        flipper_format_read_uint32(flipper_format, FF_CNT, &cnt, 1);
        flipper_format_rewind(flipper_format);
        flipper_format_read_uint32(flipper_format, "Enc", &enc, 1);
        instance->generic.cnt = cnt;
        instance->generic.seed = enc;

        instance->encoder.repeat =
            pp_encoder_read_repeat(flipper_format, CONTINENTAL_RKE_TX_REPEAT);

        /* Rebuild the exact 14-byte on-air frame:
         *   bytes[0..7] = Key (MSB-first 64-bit)
         *   byte[8]     = counter LSB (bytes[6],[7] already live in the Key)
         *   bytes[9..12]= 32-bit encrypted rolling code (MSB-first)
         *   byte[13]    = XOR checksum so xor_bytes(frame,14) == 0 */
        uint8_t frame[CONTINENTAL_RKE_PAYLOAD_BYTES];
        pp_u64_to_bytes_be(instance->generic.data, frame);
        frame[8] = (uint8_t)(cnt & 0xFFU);
        frame[9] = (uint8_t)((enc >> 24) & 0xFFU);
        frame[10] = (uint8_t)((enc >> 16) & 0xFFU);
        frame[11] = (uint8_t)((enc >> 8) & 0xFFU);
        frame[12] = (uint8_t)(enc & 0xFFU);
        uint8_t xor_sum = 0;
        for(size_t i = 0; i < CONTINENTAL_RKE_PAYLOAD_BYTES - 1; i++) {
            xor_sum ^= frame[i];
        }
        frame[13] = xor_sum;

        if(!continental_rke_build_upload(instance, frame)) {
            break;
        }
        if(instance->encoder.size_upload == 0) {
            break;
        }

        instance->encoder.is_running = true;
        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

const SubGhzProtocolEncoder subghz_protocol_continental_rke_encoder = {
    .alloc = subghz_protocol_encoder_continental_rke_alloc,
    .free = pp_encoder_free,
    .deserialize = subghz_protocol_encoder_continental_rke_deserialize,
    .stop = pp_encoder_stop,
    .yield = pp_encoder_yield,
};

#else

const SubGhzProtocolEncoder subghz_protocol_continental_rke_encoder = {
    // ENCRYPTED_ROLLING: verbatim one-shot replay only, gated by the emulate
    // feature. Without it, decode-only. NULL ops, no stub funcs.
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};

#endif

const SubGhzProtocol subghz_protocol_continental_rke = {
    .name = CONTINENTAL_RKE_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_continental_rke_decoder,
    .encoder = &subghz_protocol_continental_rke_encoder,
};

// ============================================================================
// Frame processing
// ============================================================================

static void
    subghz_protocol_continental_rke_process_frame(SubGhzProtocolDecoderContinentalRke* instance) {
    /* rtl_433: if (xor_bytes(bytes, 14)) return DECODE_FAIL_MIC; */
    uint8_t xor_sum = 0;
    for(size_t i = 0; i < CONTINENTAL_RKE_PAYLOAD_BYTES; i++) {
        xor_sum ^= instance->frame[i];
    }
    if(xor_sum != 0) {
        return; /* checksum fail — reject (prevents false positives) */
    }

    const uint8_t* b = instance->frame;

    uint32_t id = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) |
                  (uint32_t)b[3];
    uint8_t button = b[5] >> 4;
    uint32_t sequence = ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 8) | (uint32_t)b[8];
    uint32_t encrypted = ((uint32_t)b[9] << 24) | ((uint32_t)b[10] << 16) |
                         ((uint32_t)b[11] << 8) | (uint32_t)b[12];

    /* Content gate: the XOR checksum alone passes for degenerate / periodic bit
     * streams (14 equal bytes XOR to 0), which is exactly how a foreign OOK/FSK
     * burst false-decodes here once the 20-bit preamble accidentally aligns.
     * Require the protocol's documented fixed structure before accepting:
     *   - a non-zero 32-bit Remote ID,
     *   - a Button nibble from the known KR5V2X set, and
     *   - a non-degenerate (not all-equal) 14-byte frame. */
    if(id == 0) {
        return; /* real fobs carry a non-zero remote ID */
    }
    switch(button) {
    case 0x1: /* Lock */
    case 0x3: /* Unlock */
    case 0x9: /* Trunk(hold) */
    case 0xA: /* Trunk/Panic */
    case 0xB: /* Panic(hold) */
        break;
    default:
        return; /* button not in the documented set — reject */
    }
    bool frame_degenerate = true;
    for(size_t i = 1; i < CONTINENTAL_RKE_PAYLOAD_BYTES; i++) {
        if(instance->frame[i] != instance->frame[0]) {
            frame_degenerate = false;
            break;
        }
    }
    if(frame_degenerate) {
        return; /* constant / all-equal frame (all-00, all-FF, all-AA, ...) */
    }

    /* Keep the most-significant 64 bits (bytes 0..7 = ID + button + high counter)
     * as the stable Key for hashing / save / load. */
    uint64_t data = 0;
    for(size_t i = 0; i < 8; i++) {
        data = (data << 8) | (uint64_t)b[i];
    }

    instance->generic.data = data;
    instance->generic.data_count_bit = CONTINENTAL_RKE_KEY_BITS;
    instance->generic.serial = id;
    instance->generic.btn = button;
    instance->generic.cnt = sequence;
    instance->generic.seed = encrypted;

    /* Feed the block decoder so the hash covers the key. */
    instance->decoder.decode_data = data;
    instance->decoder.decode_count_bit = CONTINENTAL_RKE_KEY_BITS;

    FURI_LOG_D(
        TAG,
        "ID:%08lX Btn:%X Cnt:%06lX Enc:%08lX",
        (unsigned long)id,
        button,
        (unsigned long)sequence,
        (unsigned long)encrypted);

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

// ============================================================================
// Bit assembly: preamble hunt + payload collection
// ============================================================================

static void
    subghz_protocol_continental_rke_add_bit(SubGhzProtocolDecoderContinentalRke* instance, uint8_t bit) {
    bit &= 1;

    if(!instance->collecting) {
        instance->preamble_shift =
            ((instance->preamble_shift << 1) | bit) & CONTINENTAL_RKE_PREAMBLE_MASK;
        if(instance->preamble_shift == CONTINENTAL_RKE_PREAMBLE_20) {
            instance->collecting = true;
            instance->frame_bit = 0;
            for(size_t i = 0; i < CONTINENTAL_RKE_PAYLOAD_BYTES; i++) {
                instance->frame[i] = 0;
            }
        }
        return;
    }

    /* Store payload bits MSB-first into the byte buffer. */
    uint16_t idx = instance->frame_bit;
    if(bit) {
        instance->frame[idx >> 3] |= (uint8_t)(0x80U >> (idx & 7U));
    }
    instance->frame_bit++;

    if(instance->frame_bit >= CONTINENTAL_RKE_PAYLOAD_BITS) {
        subghz_protocol_continental_rke_process_frame(instance);
        /* Re-arm to hunt for the next preamble in the same burst. */
        instance->collecting = false;
        instance->preamble_shift = 0;
        instance->frame_bit = 0;
    }
}

static void
    subghz_protocol_continental_rke_restart(SubGhzProtocolDecoderContinentalRke* instance) {
    instance->te_accum = 0;
    instance->preamble_shift = 0;
    instance->collecting = false;
    instance->frame_bit = 0;
}







// ============================================================================
// Decoder: alloc / free / reset
// ============================================================================

void* subghz_protocol_decoder_continental_rke_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderContinentalRke* instance =
        malloc(sizeof(SubGhzProtocolDecoderContinentalRke));
    instance->base.protocol = &subghz_protocol_continental_rke;
    instance->generic.protocol_name = subghz_protocol_continental_rke.name;
    subghz_protocol_continental_rke_restart(instance);
    return instance;
}

void subghz_protocol_decoder_continental_rke_free(void* context) {
    furi_assert(context);
    free(context);
}

void subghz_protocol_decoder_continental_rke_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderContinentalRke* instance = context;
    subghz_protocol_continental_rke_restart(instance);
}

// ============================================================================
// Decoder: feed
//
// Streaming re-implementation of rtl_433 pulse_slicer_manchester_zerobit.
// Each level==true duration is a mark (pulse), level==false is a space (gap).
// A half-symbol accumulates into te_accum; when accum+duration crosses the
// short*1.5 threshold a data edge occurred: falling edge (end of mark) = 1,
// rising edge (end of space) = 0. The prepended zero bit of the original
// slicer is unnecessary here because we hunt the preamble in a rolling window.
// ============================================================================

void subghz_protocol_decoder_continental_rke_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderContinentalRke* instance = context;

    /* Long silence ends the message / burst. */
    if(!level && duration >= CONTINENTAL_RKE_TE_RESET) {
        subghz_protocol_continental_rke_restart(instance);
        return;
    }

    /* Anything well outside a valid half/full symbol is noise — drop the row. */
    if(duration > CONTINENTAL_RKE_TE_MAX ||
       duration < (uint32_t)(CONTINENTAL_RKE_TE_SHORT - CONTINENTAL_RKE_TE_DELTA)) {
        subghz_protocol_continental_rke_restart(instance);
        return;
    }

    if(instance->te_accum + duration > CONTINENTAL_RKE_TE_THRESH) {
        /* Data edge: falling (end of mark) => 1, rising (end of space) => 0. */
        subghz_protocol_continental_rke_add_bit(instance, level ? 1 : 0);
        instance->te_accum = 0;
    } else {
        /* Half symbol — keep accumulating toward a full bit period. */
        instance->te_accum += duration;
    }
}

// ============================================================================
// Decoder: hash / serialize / deserialize / string
// ============================================================================

uint8_t subghz_protocol_decoder_continental_rke_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderContinentalRke* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_continental_rke_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderContinentalRke* instance = context;
    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret == SubGhzProtocolStatusOk) {
        /* Persist the counter LSB byte + 32-bit encrypted rolling code alongside
         * the 64-bit Key so a later verbatim replay can rebuild the full 14-byte
         * frame. These are extra fields; the accept/decode path ignores them. */
        uint32_t cnt = instance->generic.cnt;
        uint32_t enc = instance->generic.seed;
        if(!flipper_format_write_uint32(flipper_format, FF_CNT, &cnt, 1)) {
            return SubGhzProtocolStatusErrorParserOthers;
        }
        if(!flipper_format_write_uint32(flipper_format, "Enc", &enc, 1)) {
            return SubGhzProtocolStatusErrorParserOthers;
        }
    }
    return ret;
}

SubGhzProtocolStatus subghz_protocol_decoder_continental_rke_deserialize(
    void* context,
    FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderContinentalRke* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic,
        flipper_format,
        subghz_protocol_continental_rke_const.min_count_bit_for_found);
}

static const char*
    subghz_protocol_continental_rke_button_name(uint8_t btn) {
    switch(btn) {
    case 0x1:
        return "Lock";
    case 0x3:
        return "Unlock";
    case 0x9:
        return "Trunk(hold)";
    case 0xA:
        return "Trunk/Panic";
    case 0xB:
        return "Panic(hold)";
    default:
        return "?";
    }
}

void subghz_protocol_decoder_continental_rke_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderContinentalRke* instance = context;

    /* Re-derive ID + button from the stored 64-bit Key so this is stable across
     * save/load (bytes 0..3 = ID, byte 5 high-nibble = button). */
    uint64_t data = instance->generic.data;
    uint32_t id = (uint32_t)(data >> 32);
    uint8_t button = (uint8_t)((data >> 20) & 0x0F);

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%016llX\r\n"
        "ID:%08lX Btn:%X %s\r\n"
        "Cnt:%06lX Enc:%08lX",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long long)data,
        (unsigned long)id,
        button,
        subghz_protocol_continental_rke_button_name(button),
        (unsigned long)instance->generic.cnt,
        (unsigned long)instance->generic.seed);
}
