#include "bmw_ook.h"
#include "protocols_common.h"

#include <string.h>

#define TAG "BmwOok"

// ---- Timing (measured; rtl_433 -A confirmed: pulse 252us, gaps 492/1492us) ----
// OOK Pulse-Position: constant ~250us pulse, then a gap; short gap=0, long gap=1.
static const SubGhzBlockConst subghz_protocol_bmw_ook_const = {
    .te_short = 250, // carrier pulse (constant)
    .te_long = 1500, // long gap (bit 1)
    .te_delta = 120,
    .min_count_bit_for_found = 60, // real frame is 64; accept 60..128
};

#define BMW_OOK_MAX_BITS    128U
#define BMW_OOK_BOUNDARY_US 2500U // any pulse/gap >= this ends the burst

// data-pulse acceptance window
#define BMW_OOK_PULSE_MIN 100U
#define BMW_OOK_PULSE_MAX 450U
// gap windows
#define BMW_OOK_GAP0_MIN 250U
#define BMW_OOK_GAP0_MAX 900U
#define BMW_OOK_GAP1_MIN 1100U
#define BMW_OOK_GAP1_MAX 2200U

typedef struct SubGhzProtocolDecoderBmwOok {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    // ---- PPM front-end state ----
    bool pulse_ok; // a valid ~250us carrier pulse is pending its gap

    // ---- Tail-aligned 128-bit sliding frame window (LSB = most-recent bit) ----
    uint64_t f_lo;
    uint64_t f_hi;
    uint32_t total_bits;

    // ---- Last FINALIZED frame (survives the post-burst reset) ----
    uint64_t frame_lo, frame_hi;

    // ---- Cross-press stability diff (same fob within one RX session) ----
    bool has_prev;
    uint16_t prev_bits;
    uint64_t prev_lo, prev_hi;
    uint32_t frames_seen;
    uint64_t stable_lo, stable_hi; // 1 = bit constant across every frame so far
    uint16_t frame_bits;
    uint16_t changed_bits;
    uint16_t roll_win_off; // best 32-bit high-entropy window offset
    uint16_t roll_win_flips;
} SubGhzProtocolDecoderBmwOok;

// Shift one demodulated data bit into the tail-aligned 128-bit window.
static void bmw_ook_shift_in(SubGhzProtocolDecoderBmwOok* instance, uint8_t bit) {
    instance->f_hi = (instance->f_hi << 1) | (instance->f_lo >> 63);
    instance->f_lo = (instance->f_lo << 1) | (bit & 1U);
    if(instance->total_bits < 0xFFFFFFFFU) instance->total_bits++;
}

static uint16_t bmw_ook_popcount128(uint64_t hi, uint64_t lo) {
    return (uint16_t)(__builtin_popcountll(hi) + __builtin_popcountll(lo));
}

static uint64_t bmw_ook_window32(uint64_t hi, uint64_t lo, uint16_t off) {
    uint64_t w;
    if(off == 0)
        w = lo;
    else if(off >= 64)
        w = hi >> (off - 64);
    else
        w = (lo >> off) | (hi << (64 - off));
    return w & 0xFFFFFFFFULL;
}

// Diff the current frame vs the previous: refresh the constant-bit map and find
// the 32-bit sub-window with the most flips (candidate rolling/encrypted block).
static void bmw_ook_analyze(SubGhzProtocolDecoderBmwOok* instance) {
    if(!instance->has_prev || instance->prev_bits != instance->frame_bits) {
        instance->stable_lo = ~0ULL;
        instance->stable_hi = ~0ULL;
        instance->changed_bits = 0;
        instance->roll_win_off = 0;
        instance->roll_win_flips = 0;
        return;
    }

    const uint64_t chg_lo = instance->f_lo ^ instance->prev_lo;
    const uint64_t chg_hi = instance->f_hi ^ instance->prev_hi;

    instance->stable_lo &= ~chg_lo;
    instance->stable_hi &= ~chg_hi;

    uint16_t width = instance->frame_bits;
    if(width > BMW_OOK_MAX_BITS) width = BMW_OOK_MAX_BITS;
    if(width < 128) {
        if(width <= 64) {
            uint64_t m = (width == 64) ? ~0ULL : ((1ULL << width) - 1U);
            instance->stable_lo &= m;
            instance->stable_hi = 0;
        } else {
            uint64_t mh = (width == 128) ? ~0ULL : ((1ULL << (width - 64)) - 1U);
            instance->stable_hi &= mh;
        }
    }

    instance->changed_bits = bmw_ook_popcount128(chg_hi, chg_lo);

    uint16_t best_off = 0, best_flips = 0;
    uint16_t max_off = (width > 32) ? (uint16_t)(width - 32) : 0;
    for(uint16_t off = 0; off <= max_off; off++) {
        uint64_t w = bmw_ook_window32(chg_hi, chg_lo, off);
        uint16_t f = (uint16_t)__builtin_popcountll(w);
        if(f > best_flips) {
            best_flips = f;
            best_off = off;
        }
    }
    instance->roll_win_off = best_off;
    instance->roll_win_flips = best_flips;
}

static void bmw_ook_finalize(SubGhzProtocolDecoderBmwOok* instance) {
    uint16_t bits = (instance->total_bits > BMW_OOK_MAX_BITS) ? (uint16_t)BMW_OOK_MAX_BITS :
                                                               (uint16_t)instance->total_bits;
    instance->frame_bits = bits;

    instance->generic.data = instance->f_lo;
    instance->generic.data_count_bit = bits;
    instance->generic.serial = 0;
    instance->generic.btn = 0;
    instance->generic.cnt = 0;
    instance->generic.crc_check = false; // decode-only: no crypto/CRC validated

    instance->decoder.decode_data = instance->f_lo;
    instance->decoder.decode_count_bit = (bits > 64) ? 64 : (uint8_t)bits;

    bmw_ook_analyze(instance);

    instance->frames_seen++;

    instance->frame_lo = instance->f_lo;
    instance->frame_hi = instance->f_hi;

    instance->has_prev = true;
    instance->prev_bits = bits;
    instance->prev_lo = instance->f_lo;
    instance->prev_hi = instance->f_hi;
}

// Reset ONLY the per-burst accumulator (keeps the cross-press session diff).
static void bmw_ook_burst_reset(SubGhzProtocolDecoderBmwOok* instance) {
    instance->pulse_ok = false;
    instance->f_lo = 0;
    instance->f_hi = 0;
    instance->total_bits = 0;
    instance->decoder.parser_step = 0;
}

// Content gate: real BMW frames carry the marker byte 0x45 immediately followed
// by a command byte whose high nibble is 0x1 (0x13 lock / 0x12 unlock). Scan
// every byte-aligned MSB-first position of the finalized {hi:lo} frame for that
// pair, and reject degenerate all-zero / all-ones frames (the Fiat Panda FP is
// an all-zero 128-bit frame). This keeps the 60..128-bit sliding window: it
// gates emission on structure, it does NOT force a fixed bit count.
static bool bmw_ook_has_marker(uint64_t hi, uint64_t lo, uint16_t bits) {
    if(bits == 0 || (hi == 0 && lo == 0)) return false; // empty / all-zero
    if(bits >= 64) {
        uint64_t mh = (bits >= 128) ? ~0ULL : ((1ULL << (bits - 64)) - 1U);
        if(lo == ~0ULL && hi == mh) return false; // all-ones across used width
    } else {
        uint64_t ml = (1ULL << bits) - 1U;
        if(hi == 0 && lo == ml) return false; // all-ones across used width
    }
    uint16_t nbytes = (uint16_t)((bits + 7U) / 8U);
    if(nbytes > 16U) nbytes = 16U;
    // Byte i of the {hi:lo} store: i in [0,7] -> lo, [8,15] -> hi. MSB-first, so
    // the marker byte sits at a higher index than the command byte it precedes.
    for(uint16_t i = 1; i < nbytes; i++) {
        uint8_t cur = (uint8_t)((i < 8) ? (lo >> (8U * i)) : (hi >> (8U * (i - 8U))));
        uint16_t j = (uint16_t)(i - 1U);
        uint8_t nxt = (uint8_t)((j < 8) ? (lo >> (8U * j)) : (hi >> (8U * (j - 8U))));
        if(cur == 0x45U && (nxt & 0xF0U) == 0x10U) return true;
    }
    return false;
}

const SubGhzProtocolDecoder subghz_protocol_bmw_ook_decoder = {
    .alloc = subghz_protocol_decoder_bmw_ook_alloc,
    .free = subghz_protocol_decoder_bmw_ook_free,
    .feed = subghz_protocol_decoder_bmw_ook_feed,
    .reset = subghz_protocol_decoder_bmw_ook_reset,
    .get_hash_data = subghz_protocol_decoder_bmw_ook_get_hash_data,
    .serialize = subghz_protocol_decoder_bmw_ook_serialize,
    .deserialize = subghz_protocol_decoder_bmw_ook_deserialize,
    .get_string = subghz_protocol_decoder_bmw_ook_get_string,
};

// ---- Encoder: verbatim PPM replay of the captured tail window --------------
#ifdef ENABLE_EMULATE_FEATURE
typedef struct SubGhzProtocolEncoderBmwOok {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
    uint64_t f_lo, f_hi;
    uint16_t bits;
} SubGhzProtocolEncoderBmwOok;

#define BMW_OOK_UPLOAD_CAPACITY ((BMW_OOK_MAX_BITS * 2U) + 4U)
_Static_assert(
    BMW_OOK_UPLOAD_CAPACITY <= PP_SHARED_UPLOAD_CAPACITY,
    "BMW_OOK_UPLOAD_CAPACITY exceeds shared upload slab");

static bool bmw_ook_encoder_build_upload(SubGhzProtocolEncoderBmwOok* instance) {
    furi_check(instance);
    LevelDuration* up = instance->encoder.upload;
    const size_t cap = BMW_OOK_UPLOAD_CAPACITY;
    const uint32_t pulse = subghz_protocol_bmw_ook_const.te_short; // 250
    size_t index = 0;

    uint16_t bits = instance->bits;
    if(bits > BMW_OOK_MAX_BITS) bits = BMW_OOK_MAX_BITS;
    for(int32_t i = (int32_t)bits - 1; i >= 0; i--) {
        bool b = (i < 64) ? (((instance->f_lo >> i) & 1U) != 0U) :
                            (((instance->f_hi >> (i - 64)) & 1U) != 0U);
        index = pp_emit(up, index, cap, true, pulse); // carrier pulse
        index = pp_emit(up, index, cap, false, b ? 1500U : 500U); // gap encodes bit
    }
    index = pp_emit(up, index, cap, false, 20000U); // inter-frame gap before repeat

    if(index > cap) return false;
    instance->encoder.front = 0;
    instance->encoder.size_upload = index;
    return true;
}

void* subghz_protocol_encoder_bmw_ook_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderBmwOok* instance = calloc(1, sizeof(SubGhzProtocolEncoderBmwOok));
    furi_check(instance);
    instance->base.protocol = &bmw_ook_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.front = 0;
    instance->encoder.is_running = false;
    pp_encoder_buffer_ensure(instance, BMW_OOK_UPLOAD_CAPACITY);
    return instance;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_bmw_ook_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderBmwOok* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    instance->encoder.is_running = false;
    instance->encoder.front = 0;
    do {
        if(pp_verify_protocol_name(flipper_format, instance->base.protocol->name) !=
           SubGhzProtocolStatusOk)
            break;
        flipper_format_rewind(flipper_format);
        if(subghz_block_generic_deserialize(&instance->generic, flipper_format) !=
           SubGhzProtocolStatusOk)
            break;

        instance->f_lo = instance->generic.data;
        instance->bits = (uint16_t)instance->generic.data_count_bit;

        uint32_t hi = 0, hi2 = 0;
        flipper_format_rewind(flipper_format);
        flipper_format_read_uint32(flipper_format, "DataHi", &hi, 1);
        flipper_format_rewind(flipper_format);
        flipper_format_read_uint32(flipper_format, "DataHi2", &hi2, 1);
        instance->f_hi = ((uint64_t)hi2 << 32) | hi;

        instance->encoder.repeat = pp_encoder_read_repeat(flipper_format, 10);
        if(!bmw_ook_encoder_build_upload(instance)) break;
        if(instance->encoder.size_upload == 0) break;
        instance->encoder.is_running = true;
        ret = SubGhzProtocolStatusOk;
    } while(false);
    return ret;
}

const SubGhzProtocolEncoder subghz_protocol_bmw_ook_encoder = {
    .alloc = subghz_protocol_encoder_bmw_ook_alloc,
    .free = pp_encoder_free,
    .deserialize = subghz_protocol_encoder_bmw_ook_deserialize,
    .stop = pp_encoder_stop,
    .yield = pp_encoder_yield,
};
#else
const SubGhzProtocolEncoder subghz_protocol_bmw_ook_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};
#endif

const SubGhzProtocol bmw_ook_protocol = {
    .name = BMW_OOK_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_bmw_ook_decoder,
    .encoder = &subghz_protocol_bmw_ook_encoder,
};

void* subghz_protocol_decoder_bmw_ook_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderBmwOok* instance = calloc(1, sizeof(SubGhzProtocolDecoderBmwOok));
    furi_check(instance);
    instance->base.protocol = &bmw_ook_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_bmw_ook_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_bmw_ook_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderBmwOok* instance = context;
    bmw_ook_burst_reset(instance);
    instance->frame_lo = 0;
    instance->frame_hi = 0;
    instance->has_prev = false;
    instance->prev_bits = 0;
    instance->prev_lo = 0;
    instance->prev_hi = 0;
    instance->frames_seen = 0;
    instance->stable_lo = 0;
    instance->stable_hi = 0;
    instance->frame_bits = 0;
    instance->changed_bits = 0;
    instance->roll_win_off = 0;
    instance->roll_win_flips = 0;
}

void subghz_protocol_decoder_bmw_ook_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderBmwOok* instance = context;

    // End-of-burst: any long pulse or gap terminates the frame.
    if(duration >= BMW_OOK_BOUNDARY_US) {
        if(instance->total_bits >= subghz_protocol_bmw_ook_const.min_count_bit_for_found) {
            bmw_ook_finalize(instance);
            if(instance->base.callback &&
               bmw_ook_has_marker(
                   instance->frame_hi, instance->frame_lo, instance->frame_bits))
                instance->base.callback(&instance->base, instance->base.context);
        }
        bmw_ook_burst_reset(instance);
        return;
    }

    if(level) {
        // Expect a constant ~250us carrier pulse; anything else = framing lost.
        if(duration >= BMW_OOK_PULSE_MIN && duration <= BMW_OOK_PULSE_MAX) {
            instance->pulse_ok = true;
        } else {
            bmw_ook_burst_reset(instance);
        }
        return;
    }

    // Gap: only meaningful right after a valid carrier pulse.
    if(!instance->pulse_ok) return;
    instance->pulse_ok = false;

    if(duration >= BMW_OOK_GAP0_MIN && duration <= BMW_OOK_GAP0_MAX) {
        bmw_ook_shift_in(instance, 0);
    } else if(duration >= BMW_OOK_GAP1_MIN && duration <= BMW_OOK_GAP1_MAX) {
        bmw_ook_shift_in(instance, 1);
    } else {
        bmw_ook_burst_reset(instance); // out-of-spec gap = framing lost
    }
}

uint8_t subghz_protocol_decoder_bmw_ook_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderBmwOok* instance = context;
    uint8_t hash = subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
    hash ^= (uint8_t)(instance->frame_hi & 0xFFU);
    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_bmw_ook_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderBmwOok* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret == SubGhzProtocolStatusOk) {
        flipper_format_rewind(flipper_format);
        pp_flipper_update_or_insert_u32(
            flipper_format, "DataHi", (uint32_t)(instance->frame_hi & 0xFFFFFFFFU));
        pp_flipper_update_or_insert_u32(
            flipper_format, "DataHi2", (uint32_t)(instance->frame_hi >> 32));
    }
    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_bmw_ook_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderBmwOok* instance = context;

    SubGhzProtocolStatus ret = subghz_block_generic_deserialize(&instance->generic, flipper_format);
    if(ret == SubGhzProtocolStatusOk) {
        instance->f_lo = instance->generic.data;
        instance->frame_bits = (uint16_t)instance->generic.data_count_bit;
        instance->total_bits = instance->frame_bits;

        uint32_t hi = 0, hi2 = 0;
        flipper_format_rewind(flipper_format);
        flipper_format_read_uint32(flipper_format, "DataHi", &hi, 1);
        flipper_format_rewind(flipper_format);
        flipper_format_read_uint32(flipper_format, "DataHi2", &hi2, 1);
        instance->f_hi = ((uint64_t)hi2 << 32) | hi;
        instance->frame_lo = instance->f_lo;
        instance->frame_hi = instance->f_hi;

        instance->generic.crc_check = false;
        instance->has_prev = false;
        instance->frames_seen = 1;
        instance->stable_lo = 0;
        instance->stable_hi = 0;
        instance->changed_bits = 0;
        instance->roll_win_off = 0;
        instance->roll_win_flips = 0;
    }
    return ret;
}

void subghz_protocol_decoder_bmw_ook_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderBmwOok* instance = context;

    uint16_t bits = instance->frame_bits;
    furi_string_cat_printf(
        output, "%s %ubit\r\n", instance->generic.protocol_name, (unsigned)bits);

    if(bits > 64) {
        furi_string_cat_printf(
            output,
            "Frame:%08lX%08lX\r\n      %08lX%08lX\r\n",
            (unsigned long)(instance->frame_hi >> 32),
            (unsigned long)(instance->frame_hi & 0xFFFFFFFFU),
            (unsigned long)(instance->frame_lo >> 32),
            (unsigned long)(instance->frame_lo & 0xFFFFFFFFU));
    } else {
        furi_string_cat_printf(
            output,
            "Frame:%08lX%08lX\r\n",
            (unsigned long)(instance->frame_lo >> 32),
            (unsigned long)(instance->frame_lo & 0xFFFFFFFFU));
    }

    if(instance->frames_seen < 2 || !instance->has_prev) {
        furi_string_cat_printf(
            output,
            "Press again (same fob)\r\n"
            "to map fixed vs rolling\r\n"
            "Replay-only, no key\r\n");
        return;
    }

    uint16_t stable = bmw_ook_popcount128(instance->stable_hi, instance->stable_lo);
    if(instance->changed_bits == 0) {
        furi_string_cat_printf(
            output,
            "Frames:%lu identical\r\n"
            "=same press or FIXED\r\n"
            "space presses to tell\r\n",
            (unsigned long)instance->frames_seen);
    } else {
        furi_string_cat_printf(
            output,
            "Frames:%lu Const:%ub Chg:%ub\r\n"
            "roll?win@%ub flips:%u/32\r\n"
            "ROLLING: const=ID/hdr\r\n"
            "No key: replay-only\r\n",
            (unsigned long)instance->frames_seen,
            (unsigned)stable,
            (unsigned)instance->changed_bits,
            (unsigned)instance->roll_win_off,
            (unsigned)instance->roll_win_flips);
    }
}
