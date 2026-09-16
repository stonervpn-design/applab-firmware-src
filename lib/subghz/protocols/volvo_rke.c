#include "volvo_rke.h"
#include "protocols_common.h"

#include <string.h>

#define TAG "VolvoRke"

// ---- Timing (Olsson 2013, Chalmers EX012/2013) -----------------------------
// 2-GFSK, Manchester, 5.76 kbit/s -> bit period 173.6 us, half-symbol 86.8 us.
// The CC1101 FSK demod squares the signal to GDO0 transitions; feed() sees the
// half-symbol pulse train, exactly like tpms_trw_fsk.c. te = one half-symbol.
static const SubGhzBlockConst subghz_protocol_volvo_rke_const = {
    .te_short = 87, // half-symbol (~86.8 us)
    .te_long = 174, // full bit (two half-symbols)
    .te_delta = 34, // ~40% of te_short
    .min_count_bit_for_found = 48, // reject sub-frame noise bursts
};

// Frame window = the last (tail) VOLVO_RKE_MAX_BITS Manchester bits before the
// inter-burst gap. The payload + footer + EOM sit at the END of the burst, so a
// tail-aligned window keeps the payload at stable bit offsets from the LSB even
// as the leading wake-up (WUP) count varies press to press.
#define VOLVO_RKE_MAX_BITS    128U
#define VOLVO_RKE_GAP_TE_MULT 3U // gap >= 3*te ends the burst (like trw_fsk)

typedef struct SubGhzProtocolDecoderVolvoRke {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder; // used only for get_hash_data / generic.data
    SubGhzBlockGeneric generic;

    // ---- Manchester half-symbol pairing (rtl_433 zerobit / 802.3) ----
    bool have_half;
    uint8_t first_half;

    // ---- Tail-aligned 128-bit sliding frame window (LSB = most-recent bit) ----
    uint64_t f_lo; // live per-burst window, frame bits [63:0]
    uint64_t f_hi; // live per-burst window, frame bits [127:64]
    uint32_t total_bits; // total Manchester bits this burst (saturating)

    // ---- Last FINALIZED frame (survives the post-burst reset) ----
    uint64_t frame_lo, frame_hi; // read by get_string / serialize / hash

    // ---- Session stability diff (same fob assumed within one RX session) ----
    bool has_prev; // a previous frame is held
    uint16_t prev_bits; // its bit count
    uint64_t prev_lo, prev_hi; // its frame
    uint32_t frames_seen; // frames finalized this session
    uint64_t stable_lo, stable_hi; // 1 = bit constant across every frame so far
    uint16_t frame_bits; // bit count of the CURRENT finalized frame
    // Diff summary of the latest pair (valid when frames_seen >= 2):
    uint16_t changed_bits; // popcount(cur ^ prev) over the compared window
    uint16_t aes_win_off; // LSB offset of the best 64-bit high-entropy window
    uint16_t aes_win_flips; // flips inside that window
} SubGhzProtocolDecoderVolvoRke;

typedef enum {
    VolvoRkeDecoderStepIdle = 0,
} VolvoRkeDecoderStep;

// Shift one demodulated Manchester data bit into the tail-aligned 128-bit window.
static void volvo_rke_shift_in(SubGhzProtocolDecoderVolvoRke* instance, uint8_t bit) {
    instance->f_hi = (instance->f_hi << 1) | (instance->f_lo >> 63);
    instance->f_lo = (instance->f_lo << 1) | (bit & 1U);
    if(instance->total_bits < 0xFFFFFFFFU) instance->total_bits++;
}

// popcount over a 128-bit value split as (hi, lo).
static uint16_t volvo_rke_popcount128(uint64_t hi, uint64_t lo) {
    return (uint16_t)(__builtin_popcountll(hi) + __builtin_popcountll(lo));
}

// Read a 64-bit window starting `off` bits above the LSB from a 128-bit (hi,lo).
static uint64_t volvo_rke_window64(uint64_t hi, uint64_t lo, uint16_t off) {
    if(off == 0) return lo;
    if(off >= 64) return hi >> (off - 64);
    // off in 1..63: high (64-off) bits of lo in the low part, low `off` bits of hi up top.
    return (lo >> off) | (hi << (64 - off));
}

// Analyse the current frame against the previous one: refresh the constant-bit
// map and locate the 64-bit sub-window with the most flips (candidate AES block,
// which is high-entropy and changes ~50% of its bits every press).
static void volvo_rke_analyze(SubGhzProtocolDecoderVolvoRke* instance) {
    if(!instance->has_prev || instance->prev_bits != instance->frame_bits) {
        // First frame of a (possibly new-length) session: seed the stable map to
        // "all constant" and record nothing to diff yet.
        instance->stable_lo = ~0ULL;
        instance->stable_hi = ~0ULL;
        instance->changed_bits = 0;
        instance->aes_win_off = 0;
        instance->aes_win_flips = 0;
        return;
    }

    const uint64_t chg_lo = instance->f_lo ^ instance->prev_lo;
    const uint64_t chg_hi = instance->f_hi ^ instance->prev_hi;

    // A bit stays "stable" only if it was stable before AND did not change now.
    instance->stable_lo &= ~chg_lo;
    instance->stable_hi &= ~chg_hi;

    // Restrict the maps to the valid frame width (bits at/above frame_bits are 0).
    uint16_t width = instance->frame_bits;
    if(width > VOLVO_RKE_MAX_BITS) width = VOLVO_RKE_MAX_BITS;
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

    instance->changed_bits = volvo_rke_popcount128(chg_hi, chg_lo);

    // Best 64-bit high-entropy window: slide the 64-bit window over offsets that
    // fit inside the frame width and pick the one with the most changed bits.
    uint16_t best_off = 0, best_flips = 0;
    uint16_t max_off = (width > 64) ? (uint16_t)(width - 64) : 0;
    for(uint16_t off = 0; off <= max_off; off++) {
        uint64_t w = volvo_rke_window64(chg_hi, chg_lo, off);
        uint16_t f = (uint16_t)__builtin_popcountll(w);
        if(f > best_flips) {
            best_flips = f;
            best_off = off;
        }
    }
    instance->aes_win_off = best_off;
    instance->aes_win_flips = best_flips;
}

static void volvo_rke_finalize(SubGhzProtocolDecoderVolvoRke* instance) {
    uint16_t bits = (instance->total_bits > VOLVO_RKE_MAX_BITS) ?
                        (uint16_t)VOLVO_RKE_MAX_BITS :
                        (uint16_t)instance->total_bits;
    instance->frame_bits = bits;

    // Surface the low 64 bits through the generic block (persisted as Key).
    instance->generic.data = instance->f_lo;
    instance->generic.data_count_bit = bits;
    instance->generic.serial = 0; // no published ID offset -> not a field, see get_string
    instance->generic.btn = 0;
    instance->generic.cnt = 0;
    instance->generic.crc_check = false; // decode-only: no crypto/CRC validated

    instance->decoder.decode_data = instance->f_lo;
    instance->decoder.decode_count_bit = (bits > 64) ? 64 : (uint8_t)bits;

    volvo_rke_analyze(instance);

    instance->frames_seen++;

    // Persist the finalized frame: the burst accumulator (f_lo/f_hi) is zeroed by
    // volvo_rke_burst_reset() right after this returns, so get_string/serialize
    // must read these retained copies instead.
    instance->frame_lo = instance->f_lo;
    instance->frame_hi = instance->f_hi;

    // Roll the current frame into "previous" for the next press's diff.
    instance->has_prev = true;
    instance->prev_bits = bits;
    instance->prev_lo = instance->f_lo;
    instance->prev_hi = instance->f_hi;
}

// Feed one demodulated Manchester data bit (2nd half of each transitioning pair).
static void volvo_rke_emit_data_bit(SubGhzProtocolDecoderVolvoRke* instance, uint8_t bit) {
    volvo_rke_shift_in(instance, bit);
}

// Consume one raw half-symbol: pair half-symbols into Manchester data bits; a
// same-level pair is a phase error -> slip one half-symbol to re-align.
static void volvo_rke_emit_raw_half(SubGhzProtocolDecoderVolvoRke* instance, uint8_t bit) {
    bit &= 1U;
    if(!instance->have_half) {
        instance->first_half = bit;
        instance->have_half = true;
        return;
    }
    instance->have_half = false;
    if(instance->first_half == bit) {
        instance->first_half = bit;
        instance->have_half = true;
        return;
    }
    volvo_rke_emit_data_bit(instance, bit); // data bit = 2nd half
}

// Reset ONLY the per-burst accumulator (keeps the cross-press session diff).
static void volvo_rke_burst_reset(SubGhzProtocolDecoderVolvoRke* instance) {
    instance->have_half = false;
    instance->first_half = 0;
    instance->f_lo = 0;
    instance->f_hi = 0;
    instance->total_bits = 0;
    instance->decoder.parser_step = VolvoRkeDecoderStepIdle;
}

const SubGhzProtocolDecoder subghz_protocol_volvo_rke_decoder = {
    .alloc = subghz_protocol_decoder_volvo_rke_alloc,
    .free = subghz_protocol_decoder_volvo_rke_free,
    .feed = subghz_protocol_decoder_volvo_rke_feed,
    .reset = subghz_protocol_decoder_volvo_rke_reset,
    .get_hash_data = subghz_protocol_decoder_volvo_rke_get_hash_data,
    .serialize = subghz_protocol_decoder_volvo_rke_serialize,
    .deserialize = subghz_protocol_decoder_volvo_rke_deserialize,
    .get_string = subghz_protocol_decoder_volvo_rke_get_string,
};

// ---- Encoder: verbatim Manchester replay of the captured tail window --------
// A rolling receiver accepts a captured frame at most once (the SI counter has
// already advanced); this is a one-shot replay, never a prediction.
#ifdef ENABLE_EMULATE_FEATURE
typedef struct SubGhzProtocolEncoderVolvoRke {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
    uint64_t f_lo, f_hi;
    uint16_t bits;
} SubGhzProtocolEncoderVolvoRke;

#define VOLVO_RKE_TX_PREAMBLE_BITS 24U // Manchester clock lead-in for RX lock
#define VOLVO_RKE_UPLOAD_CAPACITY \
    ((VOLVO_RKE_TX_PREAMBLE_BITS * 2U) + (VOLVO_RKE_MAX_BITS * 2U) + 2U)
_Static_assert(
    VOLVO_RKE_UPLOAD_CAPACITY <= PP_SHARED_UPLOAD_CAPACITY,
    "VOLVO_RKE_UPLOAD_CAPACITY exceeds shared upload slab");

static bool volvo_rke_encoder_build_upload(SubGhzProtocolEncoderVolvoRke* instance) {
    furi_check(instance);
    LevelDuration* up = instance->encoder.upload;
    const size_t cap = VOLVO_RKE_UPLOAD_CAPACITY;
    const uint32_t te = subghz_protocol_volvo_rke_const.te_short;
    size_t index = 0;

    // Manchester clock lead-in (alternating 1s -> steady transition run) so the
    // FSK demod + RX can establish half-symbol phase before the frame.
    for(uint32_t i = 0; i < VOLVO_RKE_TX_PREAMBLE_BITS; i++) {
        index = pp_emit_manchester_bit(up, index, cap, true, te);
    }

    // Frame bits MSB-first from the tail window (bit (bits-1) .. bit 0).
    uint16_t bits = instance->bits;
    if(bits > VOLVO_RKE_MAX_BITS) bits = VOLVO_RKE_MAX_BITS;
    for(int32_t i = (int32_t)bits - 1; i >= 0; i--) {
        bool b = (i < 64) ? (((instance->f_lo >> i) & 1U) != 0U) :
                            (((instance->f_hi >> (i - 64)) & 1U) != 0U);
        index = pp_emit_manchester_bit(up, index, cap, b, te);
    }

    if(index > cap) return false;
    instance->encoder.front = 0;
    instance->encoder.size_upload = index;
    return true;
}

void* subghz_protocol_encoder_volvo_rke_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderVolvoRke* instance = calloc(1, sizeof(SubGhzProtocolEncoderVolvoRke));
    furi_check(instance);
    instance->base.protocol = &volvo_rke_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 10;
    instance->encoder.front = 0;
    instance->encoder.is_running = false;
    pp_encoder_buffer_ensure(instance, VOLVO_RKE_UPLOAD_CAPACITY);
    return instance;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_volvo_rke_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolEncoderVolvoRke* instance = context;
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
        if(!volvo_rke_encoder_build_upload(instance)) break;
        if(instance->encoder.size_upload == 0) break;
        instance->encoder.is_running = true;
        ret = SubGhzProtocolStatusOk;
    } while(false);
    return ret;
}

const SubGhzProtocolEncoder subghz_protocol_volvo_rke_encoder = {
    .alloc = subghz_protocol_encoder_volvo_rke_alloc,
    .free = pp_encoder_free,
    .deserialize = subghz_protocol_encoder_volvo_rke_deserialize,
    .stop = pp_encoder_stop,
    .yield = pp_encoder_yield,
};
#else
const SubGhzProtocolEncoder subghz_protocol_volvo_rke_encoder = {
    .alloc = NULL,
    .free = NULL,
    .deserialize = NULL,
    .stop = NULL,
    .yield = NULL,
};
#endif

const SubGhzProtocol volvo_rke_protocol = {
    .name = VOLVO_RKE_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_volvo_rke_decoder,
    .encoder = &subghz_protocol_volvo_rke_encoder,
};

void* subghz_protocol_decoder_volvo_rke_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderVolvoRke* instance = calloc(1, sizeof(SubGhzProtocolDecoderVolvoRke));
    furi_check(instance);
    instance->base.protocol = &volvo_rke_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_volvo_rke_free(void* context) {
    furi_check(context);
    free(context);
}

void subghz_protocol_decoder_volvo_rke_reset(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderVolvoRke* instance = context;
    volvo_rke_burst_reset(instance);
    // Fresh RX session: drop the cross-press stability diff too.
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
    instance->aes_win_off = 0;
    instance->aes_win_flips = 0;
}

// Reject a burst whose tail window is a degenerate/boring pattern -- the
// signature of noise or a preamble/clock lock, not a real RKE frame. A Flipper
// RAW capture has strictly alternating levels, so any long noise burst pairs
// into alternating Manchester bits (0x5555.../0xAAAA...); with no CRC to check,
// the decoder used to report a bogus "Volvo RKE 5555555555...". Real Volvo
// frames carry a high-entropy rolling/AES block, so an all-same-byte, all-zero
// or all-one window is never legitimate.
static bool volvo_rke_frame_plausible(uint64_t hi, uint64_t lo, uint32_t total_bits) {
    uint16_t w = (total_bits > VOLVO_RKE_MAX_BITS) ? VOLVO_RKE_MAX_BITS : (uint16_t)total_bits;
    if(w < subghz_protocol_volvo_rke_const.min_count_bit_for_found) return false;

    // Mask the tail-aligned window to its valid width.
    uint64_t mlo = lo, mhi = hi;
    if(w <= 64) {
        mlo &= (w == 64) ? ~0ULL : ((1ULL << w) - 1U);
        mhi = 0;
    } else {
        mhi &= (w == 128) ? ~0ULL : ((1ULL << (w - 64)) - 1U);
    }

    // (1) all-zero / all-one -> not a frame.
    uint16_t ones = volvo_rke_popcount128(mhi, mlo);
    if(ones == 0 || ones == w) return false;

    // (2) every 8-bit group identical (catches 0x55/0xAA/0x00/0xFF/0x33/... and
    //     the alternating-noise pattern). The final partial group is compared
    //     only over its valid bits. Byte groups never straddle the 64-bit split
    //     because the step is 8 and 64 is a multiple of 8.
    uint8_t b0 = (uint8_t)(mlo & 0xFFU);
    bool byte_repeat = true;
    for(uint16_t i = 8; i < w; i += 8) {
        uint8_t bi = (i < 64) ? (uint8_t)((mlo >> i) & 0xFFU) :
                                (uint8_t)((mhi >> (i - 64)) & 0xFFU);
        uint16_t rem = (uint16_t)(w - i);
        if(rem < 8) {
            uint8_t m = (uint8_t)((1U << rem) - 1U);
            if((bi & m) != (b0 & m)) {
                byte_repeat = false;
                break;
            }
        } else if(bi != b0) {
            byte_repeat = false;
            break;
        }
    }
    if(byte_repeat) return false;

    return true;
}

void subghz_protocol_decoder_volvo_rke_feed(void* context, bool level, uint32_t duration) {
    furi_check(context);
    SubGhzProtocolDecoderVolvoRke* instance = context;

    const uint32_t te = subghz_protocol_volvo_rke_const.te_short;

    // Inter-burst gap: finalize the tail window if it is a plausible frame.
    if(duration >= te * VOLVO_RKE_GAP_TE_MULT) {
        // Validate the raw tail window BEFORE finalize so a rejected noise burst
        // does not pollute the cross-press stability diff or fire the callback.
        if(instance->total_bits >= subghz_protocol_volvo_rke_const.min_count_bit_for_found &&
           volvo_rke_frame_plausible(instance->f_hi, instance->f_lo, instance->total_bits)) {
            volvo_rke_finalize(instance);
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        volvo_rke_burst_reset(instance);
        return;
    }

    // Expand this pulse into 1 or 2 raw half-symbols of the current level.
    uint32_t n = (duration + te / 2) / te;
    if(n < 1) n = 1;
    if(n > 2) n = 2;
    for(uint32_t k = 0; k < n; k++) {
        volvo_rke_emit_raw_half(instance, level ? 1U : 0U);
    }
}

uint8_t subghz_protocol_decoder_volvo_rke_get_hash_data(void* context) {
    furi_check(context);
    SubGhzProtocolDecoderVolvoRke* instance = context;
    uint8_t hash = subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
    hash ^= (uint8_t)(instance->frame_hi & 0xFFU);
    return hash;
}

SubGhzProtocolStatus subghz_protocol_decoder_volvo_rke_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_check(context);
    SubGhzProtocolDecoderVolvoRke* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret == SubGhzProtocolStatusOk) {
        flipper_format_rewind(flipper_format);
        // Persist the high 64 bits of the tail window as two u32 halves.
        pp_flipper_update_or_insert_u32(
            flipper_format, "DataHi", (uint32_t)(instance->frame_hi & 0xFFFFFFFFU));
        pp_flipper_update_or_insert_u32(
            flipper_format, "DataHi2", (uint32_t)(instance->frame_hi >> 32));
    }
    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_volvo_rke_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_check(context);
    SubGhzProtocolDecoderVolvoRke* instance = context;

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
        // A loaded .sub is a single frame: no cross-press diff is possible.
        instance->has_prev = false;
        instance->frames_seen = 1;
        instance->stable_lo = 0;
        instance->stable_hi = 0;
        instance->changed_bits = 0;
        instance->aes_win_off = 0;
        instance->aes_win_flips = 0;
    }
    return ret;
}

void subghz_protocol_decoder_volvo_rke_get_string(void* context, FuriString* output) {
    furi_check(context);
    SubGhzProtocolDecoderVolvoRke* instance = context;

    uint16_t bits = instance->frame_bits;
    furi_string_cat_printf(
        output, "%s %ubit\r\n", instance->generic.protocol_name, (unsigned)bits);

    // Frame tail window as hex (high half then low half), MSB-first.
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
            "to map ID vs counter\r\n"
            "AES block stays opaque\r\n");
        return;
    }

    uint16_t stable = volvo_rke_popcount128(instance->stable_hi, instance->stable_lo);
    furi_string_cat_printf(
        output,
        "Frames:%lu Const:%ub Chg:%ub\r\n"
        "AES?win@%ub flips:%u/64\r\n"
        "~ID/hdr const; AES opaque\r\n"
        "No key: replay-only\r\n",
        (unsigned long)instance->frames_seen,
        (unsigned)stable,
        (unsigned)instance->changed_bits,
        (unsigned)instance->aes_win_off,
        (unsigned)instance->aes_win_flips);
}
