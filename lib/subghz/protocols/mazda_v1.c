/**
 * Mazda V1 SubGHz Protocol — 2-FSK Manchester
 *
 * Two captured remote generations share the same physical layer and frame
 * structure, so this decoder handles both:
 *   - "V1 classic" — Mazda RX-8 2003 (OEM#1 + OEM#2)
 *   - "V3 variant" — newer Mazda fob (4+ buttons, e.g. btn5/Trunk)
 *
 * Physical: 2-FSK @ 433.92 MHz, FuriHalSubGhzPreset2FSKDev476Async
 * CC1101 demodulates to baseband → raw timings = Manchester edges.
 *
 * Manchester: te_short=500-510us, te_long=1000-1020us (te_delta widened to
 * 200us so both generations match cleanly)
 * Preamble: 20+ alternating SHORT pairs (Manchester zeros)
 * Frame: 71 bits, sent back-to-back twice per burst (verified at Δ=71
 * across all V1 captures and 526 V3 captures).
 *
 * Bit layout:
 *   bit  0..7  : 0xEC sync (always)
 *   bit  8..15 : Button (± bit7 repeat-flag)
 *                  0x06 = Lock           (V1+V3)
 *                  0x0A = Unlock         (V1+V3)
 *                  0x0E = Trunk          (V1)
 *                  0x26 = Btn5/Trunk     (V3)
 *   bit 16..21 : per-remote constant — V1 uses bits 16..17 for 2-bit Serial
 *                + bits 18..21 = 1000 fixed marker; V3 uses these 6 bits as
 *                a per-remote constant ("serial") with NO 1000 marker. The
 *                decoder treats them as a 6-bit serial in both cases.
 *   bit 22..69 : 48-bit rolling-code region (the "Hop"). One of these (bit 37)
 *                is a redundant parity bit linearly determined by the others —
 *                for V1 it reads 0, for V3 it varies (≈ ~counter-bit-2 for some
 *                fobs, constant for others). The Hop field excludes it → 47
 *                independent bits, no information lost.
 *   bit 70     : 1  (constant frame-end marker — both generations)
 *
 * → 47 bits encrypted (Hop) carry the counter, the per-fob serial (XOR-spread
 *   across ~13 linear functionals) and a ~16-22-bit nonlinear cipher core.
 *   Not pure XOR / not a bit-permuted counter / not an LFSR (all tested).
 *
 * Press counter — the low 5 bits are recoverable in the clear from the
 * rolling-code region (the cipher only diffuses ~22 of the 47 bits; the low
 * counter bits are linearly spread, XOR-mask 0). Verified to increment +1
 * per press across 4 fobs (key0 1665x gapless, key1 145x, key3/key5 30x
 * consecutive, key0 524x):
 *   counter bit 0 = frame bit 53
 *   counter bit 1 = frame bit 52
 *   counter bit 2 = frame bit 51
 *   counter bit 3 = frame bit 50
 *   counter bit 4 = frame bit 53 ⊕ frame bit 49
 * Higher counter bits are present but woven into the per-fob serial whitening
 * (not universally extractable). generic.cnt holds counter & 0x1F.
 */

#include "mazda_v1.h"

#include <lib/toolbox/manchester_decoder.h>
#include <lib/toolbox/manchester_encoder.h>

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include <furi.h>
#include <furi_hal.h>

#define TAG "SubGhzProtocolMazdaV1"

#define MAZDA_V1_DATA_BITS       71
#define MAZDA_V1_MIN_PREAMBLE    20
#define MAZDA_V1_SYNC_BYTE       0xEC
#define MAZDA_V1_TOTAL_BURSTS    3
#define MAZDA_V1_PREAMBLE_PAIRS  40

static const SubGhzBlockConst subghz_protocol_mazda_v1_const = {
    .te_short = 505,
    .te_long = 1010,
    .te_delta = 200,
    .min_count_bit_for_found = MAZDA_V1_DATA_BITS,
};

// ============================================================================
// DATA STRUCTURES
// ============================================================================

typedef enum {
    MazdaV1DecoderStepReset = 0,
    MazdaV1DecoderStepCollect,
} MazdaV1DecoderStep;

struct SubGhzProtocolDecoderMazdaV1 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    bool half_bit;
    bool half_bit_level;

    uint8_t all_bits[400];
    uint16_t all_bit_count;
    int16_t last_callback_pos;  // last frame's sync_pos, -1 = none yet

    uint32_t data_32; // bits 64-70 (last 7 bits, left-aligned)
};

struct SubGhzProtocolEncoderMazdaV1 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint32_t data_32;
};

// ============================================================================
// PROTOCOL DEFINITION
// ============================================================================

static SubGhzBlockGeneric* subghz_protocol_decoder_mazda_v1_get_generic(void* context) {
    SubGhzProtocolDecoderMazdaV1* instance = context;
    return &instance->generic;
}

const SubGhzProtocolDecoder subghz_protocol_mazda_v1_decoder = {
    .alloc = subghz_protocol_decoder_mazda_v1_alloc,
    .free = subghz_protocol_decoder_mazda_v1_free,
    .feed = subghz_protocol_decoder_mazda_v1_feed,
    .reset = subghz_protocol_decoder_mazda_v1_reset,
    .get_hash_data = subghz_protocol_decoder_mazda_v1_get_hash_data,
    .serialize = subghz_protocol_decoder_mazda_v1_serialize,
    .deserialize = subghz_protocol_decoder_mazda_v1_deserialize,
    .get_string = subghz_protocol_decoder_mazda_v1_get_string,
    .get_generic = subghz_protocol_decoder_mazda_v1_get_generic,
};

const SubGhzProtocolEncoder subghz_protocol_mazda_v1_encoder = {
    .alloc = subghz_protocol_encoder_mazda_v1_alloc,
    .free = subghz_protocol_encoder_mazda_v1_free,
    .deserialize = subghz_protocol_encoder_mazda_v1_deserialize,
    .stop = subghz_protocol_encoder_mazda_v1_stop,
    .yield = subghz_protocol_encoder_mazda_v1_yield,
};

const SubGhzProtocol subghz_protocol_mazda_v1 = {
    .name = SUBGHZ_PROTOCOL_MAZDA_V1_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_mazda_v1_decoder,
    .encoder = &subghz_protocol_mazda_v1_encoder,
};

// ============================================================================
// DECODER
// ============================================================================

void* subghz_protocol_decoder_mazda_v1_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderMazdaV1* instance = malloc(sizeof(SubGhzProtocolDecoderMazdaV1));
    instance->base.protocol = &subghz_protocol_mazda_v1;
    instance->generic.protocol_name = instance->base.protocol->name;
    /* Seed decoder state so a feed() before the first reset() can't run the
     * state machine with a garbage parser_step / all_bit_count. */
    subghz_protocol_decoder_mazda_v1_reset(instance);
    return instance;
}

void subghz_protocol_decoder_mazda_v1_free(void* context) {
    furi_assert(context);
    free(context);
}

void subghz_protocol_decoder_mazda_v1_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderMazdaV1* instance = context;
    instance->decoder.parser_step = MazdaV1DecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->half_bit = false;
    instance->all_bit_count = 0;
    instance->last_callback_pos = -32767;
    instance->data_32 = 0;
}

/**
 * Recover the low 5 bits of the press counter from generic.data (which holds
 * frame bits 0..63, MSB-first). See header comment for the bit mapping.
 */
static uint32_t mazda_v1_extract_cnt(uint64_t data) {
    uint8_t fb53 = (uint8_t)((data >> (63 - 53)) & 1);
    uint8_t fb52 = (uint8_t)((data >> (63 - 52)) & 1);
    uint8_t fb51 = (uint8_t)((data >> (63 - 51)) & 1);
    uint8_t fb50 = (uint8_t)((data >> (63 - 50)) & 1);
    uint8_t fb49 = (uint8_t)((data >> (63 - 49)) & 1);
    uint8_t b4 = fb53 ^ fb49;
    return (uint32_t)((b4 << 4) | (fb50 << 3) | (fb51 << 2) | (fb52 << 1) | fb53);
}

/**
 * Validate frame at given sync_pos for the given polarity. If valid (sync EC,
 * known button, format markers bit37=0/bit70=1), extract & callback. Returns
 * true if a frame was emitted.
 */
static bool mazda_v1_try_emit_at(
    SubGhzProtocolDecoderMazdaV1* instance, int sync_pos, int polarity) {
    if(sync_pos < 0) return false;
    if(sync_pos + MAZDA_V1_DATA_BITS > instance->all_bit_count) return false;

    static const uint8_t sync_ec[8] = {1,1,1,0,1,1,0,0};

    // Check sync pattern (with polarity)
    for(int j = 0; j < 8; j++) {
        uint8_t b = instance->all_bits[sync_pos + j];
        if(polarity == 1) b ^= 1;
        if(b != sync_ec[j]) return false;
    }

    // Check button
    uint8_t btn = 0;
    for(int j = 0; j < 8; j++) {
        uint8_t b = instance->all_bits[sync_pos + 8 + j];
        if(polarity == 1) b ^= 1;
        btn = (uint8_t)((btn << 1) | b);
    }
    uint8_t btn_base = btn & 0x7F;
    // 0x06/0x0A/0x0E = V1 (Lock/Unlock/Trunk), 0x26 = V3 (Btn5/Trunk)
    if(btn_base != 0x06 && btn_base != 0x0A && btn_base != 0x0E && btn_base != 0x26) return false;

    // Frame-end marker (bit 70 = 1) — holds in BOTH V1 and V3 across all captures
    uint8_t bit70 = instance->all_bits[sync_pos + 70];
    if(polarity == 1) bit70 ^= 1;
    if(bit70 != 1) return false;

    // Format-marker check: V1 has bit37=0 AND bits18-21=1000.
    // V3 has bit37 button-dependent (1 for btn=06/0A, 0 for btn=26) and
    // bits18-21 ≠ 1000. So accept either of:
    //   - V1 path: bit37=0 AND bits18-21==1000
    //   - V3 path: bits18-21 != 1000  (bit37 is unconstrained)
    uint8_t bit18 = instance->all_bits[sync_pos + 18];
    uint8_t bit19 = instance->all_bits[sync_pos + 19];
    uint8_t bit20 = instance->all_bits[sync_pos + 20];
    uint8_t bit21 = instance->all_bits[sync_pos + 21];
    uint8_t bit37 = instance->all_bits[sync_pos + 37];
    if(polarity == 1) { bit18 ^= 1; bit19 ^= 1; bit20 ^= 1; bit21 ^= 1; bit37 ^= 1; }
    bool v1_marker = (bit18 == 1 && bit19 == 0 && bit20 == 0 && bit21 == 0 && bit37 == 0);
    bool v3_marker = !(bit18 == 1 && bit19 == 0 && bit20 == 0 && bit21 == 0);
    if(!v1_marker && !v3_marker) return false;

    // Apply polarity inversion in-place if needed (for downstream extraction)
    if(polarity == 1) {
        for(int k = sync_pos; k < sync_pos + MAZDA_V1_DATA_BITS; k++) {
            instance->all_bits[k] ^= 1;
        }
    }

    // Extract generic.data (first 64 bits = sync + btn + 6 hop bytes)
    instance->generic.data = 0;
    for(int i = 0; i < 64; i++) {
        instance->generic.data = (instance->generic.data << 1) | instance->all_bits[sync_pos + i];
    }

    // Extract data_32 (last 7 bits = bits 64..70, left-aligned in 32-bit field)
    instance->data_32 = 0;
    for(int i = 64; i < MAZDA_V1_DATA_BITS; i++) {
        instance->data_32 = (instance->data_32 << 1) | instance->all_bits[sync_pos + i];
    }
    instance->data_32 <<= (32 - (MAZDA_V1_DATA_BITS - 64));

    instance->generic.data_count_bit = MAZDA_V1_DATA_BITS;
    instance->decoder.decode_data = instance->generic.data;
    instance->decoder.decode_count_bit = MAZDA_V1_DATA_BITS;

    // Button: byte 1 (bits 8..15)
    instance->generic.btn = (uint8_t)((instance->generic.data >> 48) & 0xFF);
    // Serial: bits 16..21 of frame (top 6 bits of byte 2). Captures the
    // V1 2-bit serial + 1000 marker as a 6-bit value (==0x20 for V1) and
    // the V3 6-bit per-remote constant in the same field.
    uint8_t b2 = (uint8_t)((instance->generic.data >> 40) & 0xFF);
    instance->generic.serial = (uint32_t)(b2 >> 2) & 0x3F;
    // Low 5 bits of the press counter are recoverable in the clear
    instance->generic.cnt = mazda_v1_extract_cnt(instance->generic.data);

    FURI_LOG_D(TAG, "Frame @bit %d (%s): Btn=0x%02X Sn=0x%02lX Cnt=0x%02lX",
        sync_pos, polarity ? "inverted" : "normal",
        instance->generic.btn, (unsigned long)instance->generic.serial,
        (unsigned long)instance->generic.cnt);

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
    return true;
}

/**
 * Streaming check: if a complete frame ends at the most recent bit, validate
 * and emit. Called after every bit append. Dedup via last_callback_pos so the
 * back-to-back repeat in each burst doesn't fire twice.
 */
static void mazda_v1_streaming_check(SubGhzProtocolDecoderMazdaV1* instance) {
    if(instance->all_bit_count < MAZDA_V1_DATA_BITS) return;
    int sync_pos = instance->all_bit_count - MAZDA_V1_DATA_BITS;
    // Skip if the candidate sync would overlap with or be the back-to-back
    // repeat of the previously emitted frame. V1 sends each frame twice
    // back-to-back at +71 bit offset; only the first counts.
    if(instance->last_callback_pos >= 0 &&
       sync_pos < instance->last_callback_pos + 2 * MAZDA_V1_DATA_BITS) return;
    for(int polarity = 0; polarity < 2; polarity++) {
        if(mazda_v1_try_emit_at(instance, sync_pos, polarity)) {
            instance->last_callback_pos = sync_pos;
            return;
        }
    }
}

/**
 * End-of-burst sweep: scan for any not-yet-emitted frame in the current
 * buffer. Used as a fallback when streaming detection didn't fire (rare).
 */
static void mazda_v1_process_frame(SubGhzProtocolDecoderMazdaV1* instance) {
    if(instance->all_bit_count < MAZDA_V1_DATA_BITS) return;
    int search_end = instance->all_bit_count - MAZDA_V1_DATA_BITS;
    for(int i = 0; i <= search_end; i++) {
        // Skip the immediate back-to-back repeat of the previous frame
        if(instance->last_callback_pos >= 0 &&
           i < instance->last_callback_pos + 2 * MAZDA_V1_DATA_BITS) continue;
        for(int polarity = 0; polarity < 2; polarity++) {
            if(mazda_v1_try_emit_at(instance, i, polarity)) {
                instance->last_callback_pos = i;
                break;
            }
        }
    }
}

void subghz_protocol_decoder_mazda_v1_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderMazdaV1* instance = context;

    uint32_t te_s = subghz_protocol_mazda_v1_const.te_short;
    uint32_t te_l = subghz_protocol_mazda_v1_const.te_long;
    uint32_t te_d = subghz_protocol_mazda_v1_const.te_delta;

    switch(instance->decoder.parser_step) {
    case MazdaV1DecoderStepReset:
        if(DURATION_DIFF(duration, te_s) <= te_d) {
            instance->decoder.parser_step = MazdaV1DecoderStepCollect;
            instance->all_bit_count = 0;
            instance->last_callback_pos = -32767;
            instance->half_bit = true;
            instance->half_bit_level = level;
        }
        break;

    case MazdaV1DecoderStepCollect: {
        if(instance->all_bit_count >= 380) {
            mazda_v1_process_frame(instance);
            instance->decoder.parser_step = MazdaV1DecoderStepReset;
            break;
        }

        bool is_short = DURATION_DIFF(duration, te_s) <= te_d;
        bool is_long = DURATION_DIFF(duration, te_l) <= te_d;

        bool bit_added = false;
        if(is_long) {
            // LONG = produces a bit; reset half_bit
            // (When half=true, LONG completes the pair; when half=false, LONG is a full bit)
            if(instance->all_bit_count < sizeof(instance->all_bits)) {
                instance->all_bits[instance->all_bit_count++] = level ? 1 : 0;
                bit_added = true;
            }
            instance->half_bit = false;
        } else if(is_short) {
            if(instance->half_bit) {
                if(instance->all_bit_count < sizeof(instance->all_bits)) {
                    instance->all_bits[instance->all_bit_count++] = level ? 1 : 0;
                    bit_added = true;
                }
                instance->half_bit = false;
            } else {
                instance->half_bit = true;
                instance->half_bit_level = level;
            }
        } else {
            mazda_v1_process_frame(instance);
            instance->decoder.parser_step = MazdaV1DecoderStepReset;
        }
        // Streaming check: emit frame as soon as the last bit completes one
        if(bit_added) mazda_v1_streaming_check(instance);
    } break;

    default:
        instance->decoder.parser_step = MazdaV1DecoderStepReset;
        break;
    }
}

// ============================================================================
// HASH
// ============================================================================

uint8_t subghz_protocol_decoder_mazda_v1_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderMazdaV1* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

// ============================================================================
// SERIALIZATION
// ============================================================================

SubGhzProtocolStatus subghz_protocol_decoder_mazda_v1_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderMazdaV1* instance = context;

    SubGhzProtocolStatus res =
        subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
    if(res != SubGhzProtocolStatusOk) return res;

    FuriString* disp = furi_string_alloc();
    furi_string_printf(disp, "Mazda RX-8 - Sn:%02lX Btn:%02X",
        (unsigned long)instance->generic.serial,
        instance->generic.btn);
    flipper_format_write_string_cstr(flipper_format, "Disp", furi_string_get_cstr(disp));
    furi_string_free(disp);

    uint32_t k2 = instance->data_32;
    if(!flipper_format_rewind(flipper_format)) return SubGhzProtocolStatusErrorParserOthers;
    if(!flipper_format_insert_or_update_uint32(flipper_format, "Key2", &k2, 1))
        return SubGhzProtocolStatusErrorParserOthers;

    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_mazda_v1_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderMazdaV1* instance = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize(&instance->generic, flipper_format);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint32_t k2 = 0;
    if(!flipper_format_read_uint32(flipper_format, "Key2", &k2, 1))
        return SubGhzProtocolStatusError;

    instance->data_32 = k2;

    // Restore fields from generic.data (matches process_frame layout)
    instance->generic.btn = (uint8_t)((instance->generic.data >> 48) & 0xFF);
    {
        uint8_t b2 = (uint8_t)((instance->generic.data >> 40) & 0xFF);
        instance->generic.serial = (uint32_t)(b2 >> 2) & 0x3F;
    }
    instance->generic.cnt = mazda_v1_extract_cnt(instance->generic.data);
    instance->generic.protocol_name = instance->base.protocol->name;

    return SubGhzProtocolStatusOk;
}

// ============================================================================
// DISPLAY
// ============================================================================

void subghz_protocol_decoder_mazda_v1_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderMazdaV1* instance = context;

    uint32_t key_hi = (uint32_t)(instance->generic.data >> 32);
    uint32_t key_lo = (uint32_t)(instance->generic.data & 0xFFFFFFFF);

    // Hop: 47 encrypted bits = frame bits 22..69 with format markers (37, 70)
    // skipped. Packed left-aligned into 48 bits for hex display.
    uint64_t hop = 0;
    int hop_bits = 0;
    for(int i = 22; i < 71; i++) {
        if(i == 37 || i == 70) continue; // skip constant format markers
        bool b;
        if(i < 64) b = (instance->generic.data >> (63 - i)) & 1;
        else       b = (instance->data_32 >> (31 - (i - 64))) & 1;
        hop = (hop << 1) | (b ? 1 : 0);
        hop_bits++;
    }
    hop <<= (48 - hop_bits); // left-align in 48-bit field (12 hex chars)

    const char* btn_name = "Unknown";
    switch(instance->generic.btn & 0x7F) {
    case 0x06: btn_name = "Lock"; break;
    case 0x0A: btn_name = "Unlock"; break;
    case 0x0E: btn_name = "Trunk"; break;
    case 0x26: btn_name = "Btn5/Trunk"; break;
    }

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%08lX%08lX\r\n"
        "Key2:%08lX Sn:%lX Btn:%s\r\n"
        "Cnt:%02lX (low 5 bits) Hop:%012llX (47 enc bits)",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        (unsigned long)key_hi,
        (unsigned long)key_lo,
        (unsigned long)instance->data_32,
        (unsigned long)instance->generic.serial,
        btn_name,
        (unsigned long)instance->generic.cnt,
        (unsigned long long)hop);
}

// ============================================================================
// ENCODER
// ============================================================================

static bool mazda_v1_get_data_bit(SubGhzProtocolEncoderMazdaV1* instance, uint8_t idx) {
    if(idx < 64) {
        return (instance->generic.data >> (63 - idx)) & 1;
    } else {
        return (instance->data_32 >> (31 - (idx - 64))) & 1;
    }
}

void* subghz_protocol_encoder_mazda_v1_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderMazdaV1* instance = malloc(sizeof(SubGhzProtocolEncoderMazdaV1));
    instance->base.protocol = &subghz_protocol_mazda_v1;
    instance->generic.protocol_name = instance->base.protocol->name;

    instance->encoder.repeat = MAZDA_V1_TOTAL_BURSTS;
    instance->encoder.size_upload = 512;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_mazda_v1_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderMazdaV1* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

/**
 * Build one complete frame:
 * [PREAMBLE: 40x SHORT pairs] [71 Manchester bits] [71 Manchester bits repeat] [END GAP]
 */
static void mazda_v1_build_upload(SubGhzProtocolEncoderMazdaV1* instance) {
    furi_assert(instance);
    size_t index = 0;

    uint32_t te_s = subghz_protocol_mazda_v1_const.te_short;
    uint32_t te_l = subghz_protocol_mazda_v1_const.te_long;

    // PREAMBLE: alternating SHORT pairs (produces Manchester zeros)
    for(int p = 0; p < MAZDA_V1_PREAMBLE_PAIRS; p++) {
        instance->encoder.upload[index++] = level_duration_make(true, te_s);
        instance->encoder.upload[index++] = level_duration_make(false, te_s);
    }

    // Send data twice (as observed in captures)
    for(int repeat = 0; repeat < 2; repeat++) {
        uint8_t manchester_state = ManchesterStateMid0; // preamble ends in mid-0

        for(int i = 0; i < MAZDA_V1_DATA_BITS; i++) {
            bool b = mazda_v1_get_data_bit(instance, i);

            switch(manchester_state) {
            case ManchesterStateMid1:
                if(b == 0) {
                    instance->encoder.upload[index++] = level_duration_make(true, te_l);
                    manchester_state = ManchesterStateMid0;
                } else {
                    instance->encoder.upload[index++] = level_duration_make(true, te_s);
                    instance->encoder.upload[index++] = level_duration_make(false, te_s);
                    manchester_state = ManchesterStateMid1;
                }
                break;
            case ManchesterStateMid0:
                if(b == 0) {
                    instance->encoder.upload[index++] = level_duration_make(false, te_s);
                    instance->encoder.upload[index++] = level_duration_make(true, te_s);
                    manchester_state = ManchesterStateMid0;
                } else {
                    instance->encoder.upload[index++] = level_duration_make(false, te_l);
                    manchester_state = ManchesterStateMid1;
                }
                break;
            default:
                manchester_state = ManchesterStateMid0;
                break;
            }
        }
    }

    // END: guard pulse + gap
    instance->encoder.upload[index++] = level_duration_make(true, 3000);

    instance->encoder.size_upload = index;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_mazda_v1_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderMazdaV1* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;

    do {
        if(subghz_block_generic_deserialize(&instance->generic, flipper_format) !=
           SubGhzProtocolStatusOk) {
            break;
        }

        uint32_t k2 = 0;
        if(!flipper_format_read_uint32(flipper_format, "Key2", &k2, 1)) break;
        instance->data_32 = k2;

        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);

        mazda_v1_build_upload(instance);
        instance->encoder.is_running = true;
        res = SubGhzProtocolStatusOk;
    } while(false);

    return res;
}

void subghz_protocol_encoder_mazda_v1_stop(void* context) {
    SubGhzProtocolEncoderMazdaV1* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_mazda_v1_yield(void* context) {
    SubGhzProtocolEncoderMazdaV1* instance = context;

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
