/**
 * @file honda_v6.c
 * @brief Decoder for Honda FSK keyfob signals
 *
 * Supports three capture modes:
 *
 * 1) 434 MHz FSK (2FSK preset): Manchester-encoded at te=100us (10 kbps)
 *    - Preamble: 20+ alternating ~100us pulses
 *    - Sync: long-long (200us + 200us)
 *    - Data: 45 bits Manchester encoded
 *
 * 2) 313 MHz FSK (2FSK preset): 3-level timing at te=800us
 *    - Preamble: 20+ alternating ~800us pulses
 *    - Sync: first M/L timing (>1000us) after preamble
 *    - Data: ~48 timings classified as S(800)/M(1200)/L(1600)us
 *    - Encoded as 2-bit trits: S=0, M=1, L=2
 *
 * 3) OOK capture: Signal identification only (carrier bursts).
 */

#include "honda_v6.h"

#include <lib/toolbox/manchester_decoder.h>

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#define TAG "SubGhzProtocolHondaV6"

/* 434 MHz Manchester timing */
static const SubGhzBlockConst subghz_protocol_honda_v6_const = {
    .te_short = 100,
    .te_long = 200,
    .te_delta = 45,
    .min_count_bit_for_found = 43,
};

/* 313 MHz timing constants */
#define HONDA_V6_313_TE_SHORT 800U
#define HONDA_V6_313_TE_MID   1200U
#define HONDA_V6_313_TE_LONG  1600U
#define HONDA_V6_313_TE_DELTA 250U
#define HONDA_V6_313_MIN_PREAMBLE 15
#define HONDA_V6_313_MIN_TRITS 30
#define HONDA_V6_313_MAX_TRITS 60
#define HONDA_V6_313_GAP 2000U

/* Preamble: minimum alternating short pulses before sync (434 MHz).
 * Standard Honda fobs emit 38-62 preamble half-bits; Pandora TS clones
 * emit only ~22. Threshold 20 covers both without admitting noise runs. */
#define HONDA_V6_MIN_PREAMBLE 20

/* OOK carrier detection threshold */
#define HONDA_V6_CARRIER_MIN 15000U
#define HONDA_V6_OOK_MIN_CARRIERS 5
#define HONDA_V6_SESSION_GAP 80000U

// ============================================================================
// DATA STRUCTURES
// ============================================================================

typedef enum {
    HondaV6DecoderStepReset = 0,
    /* 434 MHz Manchester states */
    HondaV6DecoderStepCheckPreamble,
    HondaV6DecoderStepCollectData,
    /* 313 MHz 3-level states */
    HondaV6DecoderStep313Preamble,
    HondaV6DecoderStep313Data,
} HondaV6DecoderStep;

struct SubGhzProtocolDecoderHondaV6 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    ManchesterState manchester_saved_state;
    uint16_t header_count;

    /* Manchester decoded data (434 MHz).
     * For Pandora TS variant the frame is ~120-137 bits split across
     * key_data_hi + key_data so we can store it in generic.data_2/data. */
    uint64_t key_data;
    uint64_t key_data_hi;
    uint8_t bit_count;
    uint8_t halfa_bits;  /* remembered at the midgap — used to close the frame */
    bool midgap_crossed;

    /* 313 MHz trit accumulator: 2 bits per trit packed into data+data_2 */
    uint8_t trit_count;

    /* OOK carrier accumulator */
    uint8_t ook_carriers;
    bool ook_detected;
};

struct SubGhzProtocolEncoderHondaV6 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
};

// ============================================================================
// PROTOCOL DEFINITION
// ============================================================================

static SubGhzBlockGeneric* subghz_protocol_decoder_honda_v6_get_generic(void* context) {
    SubGhzProtocolDecoderHondaV6* instance = context;
    return &instance->generic;
}

const SubGhzProtocolDecoder subghz_protocol_honda_v6_decoder = {
    .alloc = subghz_protocol_decoder_honda_v6_alloc,
    .free = subghz_protocol_decoder_honda_v6_free,
    .feed = subghz_protocol_decoder_honda_v6_feed,
    .reset = subghz_protocol_decoder_honda_v6_reset,
    .get_hash_data = subghz_protocol_decoder_honda_v6_get_hash_data,
    .serialize = subghz_protocol_decoder_honda_v6_serialize,
    .deserialize = subghz_protocol_decoder_honda_v6_deserialize,
    .get_string = subghz_protocol_decoder_honda_v6_get_string,
    .get_generic = subghz_protocol_decoder_honda_v6_get_generic,
};

const SubGhzProtocolEncoder subghz_protocol_honda_v6_encoder = {
    .alloc = subghz_protocol_encoder_honda_v6_alloc,
    .free = subghz_protocol_encoder_honda_v6_free,
    .deserialize = subghz_protocol_encoder_honda_v6_deserialize,
    .stop = subghz_protocol_encoder_honda_v6_stop,
    .yield = subghz_protocol_encoder_honda_v6_yield,
};

const SubGhzProtocol subghz_protocol_honda_v6 = {
    .name = SUBGHZ_PROTOCOL_HONDA_V6_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_315 |
            SubGhzProtocolFlag_AM | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_honda_v6_decoder,
    .encoder = &subghz_protocol_honda_v6_encoder,
};

// ============================================================================
// DECODER - ALLOC / FREE / RESET
// ============================================================================

void* subghz_protocol_decoder_honda_v6_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderHondaV6* instance = malloc(sizeof(SubGhzProtocolDecoderHondaV6));
    memset(instance, 0, sizeof(SubGhzProtocolDecoderHondaV6));
    instance->base.protocol = &subghz_protocol_honda_v6;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_honda_v6_free(void* context) {
    furi_assert(context);
    free(context);
}

void subghz_protocol_decoder_honda_v6_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaV6* instance = context;
    instance->decoder.parser_step = HondaV6DecoderStepReset;
    instance->decoder.te_last = 0;
    instance->header_count = 0;
    instance->key_data = 0;
    instance->key_data_hi = 0;
    instance->bit_count = 0;
    instance->midgap_crossed = false;
    instance->halfa_bits = 0;
    instance->trit_count = 0;
    instance->ook_carriers = 0;
    instance->ook_detected = false;
    manchester_advance(
        instance->manchester_saved_state,
        ManchesterEventReset,
        &instance->manchester_saved_state,
        NULL);
}

// ============================================================================
// DECODER - OOK CARRIER DETECTION
// ============================================================================

static void honda_v6_process_ook(SubGhzProtocolDecoderHondaV6* instance) {
    /* OOK carrier detection formerly emitted a "use FSK preset" advisory through
     * the decode callback (data_count_bit=0, empty serial). But that fired on ANY
     * OOK signal with >= HONDA_V6_OOK_MIN_CARRIERS carrier bursts, so it
     * false-matched the vast majority of OOK car remotes across every make
     * (88 of 91 HondaV6 corpus hits were this advisory, not a real decode). Honda
     * V6 only yields real data from an FSK-preset capture, so the OOK path must
     * never report a decode. Keep the detector inert to avoid polluting the
     * receiver with false HondaV6 identifications. */
    instance->ook_carriers = 0;
}

// ============================================================================
// DECODER - 434 MHz FSK MANCHESTER PROCESSING
// ============================================================================

#define HONDA_V6_DATA_BITS 45

/* Pandora TS clone variants: Manchester 100us, split by ~1.5ms mid-gap.
 * Two known frame sizes observed:
 *   cell_0 style: Half A ≈ 57 bits, total ≈ 135
 *   cell_2 style: Half A ≈ 48 bits, total ≈ 120
 * On midgap we remember Half-A length and terminate at Half-A + 78 bits. */
#define HONDA_V6_PANDORA_MIN_BITS       118
#define HONDA_V6_PANDORA_MAX_BITS       140
#define HONDA_V6_PANDORA_HALFA_MIN      45
#define HONDA_V6_PANDORA_HALFA_MAX      65
#define HONDA_V6_PANDORA_HALFB_BITS     78  /* constant across cell_0/cell_2 */
#define HONDA_V6_PANDORA_MIDGAP_MIN     1000U
#define HONDA_V6_PANDORA_MIDGAP_MAX     2000U
#define HONDA_V6_SEED_PANDORA           0xB02A

#define HONDA_V6_BTN_LOCK   0x01
#define HONDA_V6_BTN_UNLOCK 0x02
#define HONDA_V6_BTN_TRUNK  0x03
#define HONDA_V6_BTN_BCM    0x10

static uint8_t honda_v6_identify_button(uint64_t hop) {
    uint8_t b34 = (hop >> 34) & 1;
    uint8_t b31 = (hop >> 31) & 1;
    uint8_t b29 = (hop >> 29) & 1;

    if(b34) return HONDA_V6_BTN_UNLOCK;
    if(b29) return HONDA_V6_BTN_LOCK;
    if(b31) return HONDA_V6_BTN_BCM;
    return HONDA_V6_BTN_TRUNK;
}

static const char* honda_v6_button_name(uint8_t btn) {
    switch(btn) {
    case HONDA_V6_BTN_LOCK: return "Lock";
    case HONDA_V6_BTN_UNLOCK: return "Unlock";
    case HONDA_V6_BTN_TRUNK: return "Trunk";
    case HONDA_V6_BTN_BCM: return "BCM";
    default: return "Unknown";
    }
}

/* Cascade-shift one Manchester bit into the 128-bit accumulator.
 * First 64 bits received land in key_data_hi, next 64 in key_data.
 * Bits beyond 128 are silently dropped. */
static void honda_v6_append_bit(SubGhzProtocolDecoderHondaV6* instance, bool bit) {
    if(instance->bit_count < 128) {
        instance->key_data_hi =
            (instance->key_data_hi << 1) | (instance->key_data >> 63);
        instance->key_data = (instance->key_data << 1) | (bit ? 1 : 0);
    }
    instance->bit_count++;
}

static void honda_v6_extract_fields(SubGhzBlockGeneric* generic) {
    uint64_t norm = generic->data & 0x1FFFFFFFFFFFULL;
    generic->data = norm;
    generic->data_count_bit = HONDA_V6_DATA_BITS;
    generic->cnt = (uint16_t)((norm >> 41) & 0xF);
    uint64_t hop = norm & 0x1FFFFFFFFFF;
    generic->btn = honda_v6_identify_button(hop);
    generic->serial = (uint32_t)(hop >> 9);
}

/* Pandora TS variants share a 119-bit cipher+header tail; the counter is the
 * variable-width prefix. Empirically observed widths:
 *   bit_count 135 (cell_0)          → 16-bit counter
 *   bit_count 133 (Mitino_OEM #1)   → 14-bit counter
 *   bit_count 129 (Mitino_OEM #2)   → 10-bit counter
 *   bit_count 128 (cell_2)          →  9-bit counter
 * Reading a fixed 16 bits (hi >> 48) drags cipher noise into the counter
 * field for all but the 135-bit variant. */
static uint8_t honda_v6_pandora_counter_width(uint16_t bit_count) {
    if(bit_count < 119) return 0;
    uint16_t w = bit_count - 119;
    return (w > 16) ? 16 : (uint8_t)w;
}

static uint16_t honda_v6_pandora_counter(uint64_t hi, uint16_t bit_count) {
    uint8_t w = honda_v6_pandora_counter_width(bit_count);
    if(w == 0) return 0;
    uint16_t n = (bit_count >= 128) ? 128 : bit_count;
    uint8_t shift = (uint8_t)(n - 64 - w);
    return (uint16_t)((hi >> shift) & (((uint32_t)1 << w) - 1));
}

/* Pandora TS cell_0 Manchester-drift recovery.
 *
 * When the FSK demod loses N bits at the front of the frame (capture noise),
 * the decoder sees bit_count = 135 - N and the whole 128-bit storage is the
 * real frame shifted LEFT by N. Recover by shifting back RIGHT by N and
 * validating against the fob-independent cell_0 frame skeleton:
 *
 *   lo[63:40] == 0x1E1E1F   (Pandora TS header magic, top 3 bytes)
 *   lo[7:0]   == 0x51       (header tail byte)
 *   hi[7:0]   == 0xFE       (frame tail)
 *
 * That is a 40-bit constant check → false-accept rate ~1e-12, so a passing
 * shift is the true alignment. Recovers ~100% of the 130–134-bit drift frames
 * (verified: corrected counter follows the clean -1 decrement sequence). */
static bool honda_v6_pandora_undrift(uint64_t* hi, uint64_t* lo, uint16_t bit_count) {
    if(bit_count >= 135 || bit_count < HONDA_V6_PANDORA_MIN_BITS) return false;
    uint8_t shift = (uint8_t)(135 - bit_count);
    if(shift > 16) return false;

    /* 128-bit storage = hi:lo, shift right by `shift` */
    uint64_t new_lo = (*lo >> shift) | (*hi << (64 - shift));
    uint64_t new_hi = *hi >> shift;

    bool header_ok = ((new_lo >> 40) & 0xFFFFFF) == 0x1E1E1FULL &&
                     (new_lo & 0xFF) == 0x51ULL;
    bool tail_ok = (new_hi & 0xFF) == 0xFEULL;
    if(header_ok && tail_ok) {
        *hi = new_hi;
        *lo = new_lo;
        return true;
    }
    return false;
}

static void honda_v6_process_fsk_frame(SubGhzProtocolDecoderHondaV6* instance) {
    bool is_stock = instance->bit_count >=
                        subghz_protocol_honda_v6_const.min_count_bit_for_found &&
                    instance->bit_count <= 48;
    bool is_pandora = instance->bit_count >= HONDA_V6_PANDORA_MIN_BITS &&
                      instance->bit_count <= HONDA_V6_PANDORA_MAX_BITS;
    if(!is_stock && !is_pandora) {
        return;
    }

    /* Pandora TS frames MUST carry the 40-bit frame skeleton (header magic
     * 0x1E1E1F..0x51 + tail 0xFE). Drifted 130–134-bit frames are realigned
     * to the clean 135-bit cell_0 layout by undrift(); a clean 135-bit frame
     * is validated in place. A frame matching neither skeleton is not a
     * Pandora frame — reject it (mandatory false-positive gate). */
    if(is_pandora) {
        uint64_t rec_hi = instance->key_data_hi;
        uint64_t rec_lo = instance->key_data;
        if(instance->bit_count < 135) {
            if(!honda_v6_pandora_undrift(&rec_hi, &rec_lo, instance->bit_count)) {
                return;
            }
            instance->key_data_hi = rec_hi;
            instance->key_data = rec_lo;
            instance->bit_count = 135;
        } else {
            bool header_ok = ((rec_lo >> 40) & 0xFFFFFF) == 0x1E1E1FULL &&
                             (rec_lo & 0xFF) == 0x51ULL;
            bool tail_ok = (rec_hi & 0xFF) == 0xFEULL;
            if(!(header_ok && tail_ok)) {
                return;
            }
        }
    }

    instance->generic.data = instance->key_data;
    instance->generic.data_count_bit = instance->bit_count;
    instance->generic.extra_check = true;

    if(is_stock) {
        instance->generic.data_2 = 0;
        instance->generic.seed = 0;
        honda_v6_extract_fields(&instance->generic);
        /* Plausibility gate: a genuine fob has a non-zero serial. Reject the
         * near-zero frames that OOK/Manchester noise decodes into. */
        if(instance->generic.serial == 0) {
            return;
        }
    } else {
        /* Pandora TS family — variable-width counter prefix at the start of
         * the frame. Counter width = bit_count - 119 (fits cell_0 16b,
         * Mitino_OEM #1 14b, Mitino_OEM #2 10b, cell_2 9b).
         * key_data_hi = first 64 bits received, key_data = next 64 bits. */
        instance->generic.data_2 = instance->key_data_hi;
        instance->generic.data = instance->key_data;
        instance->generic.seed = HONDA_V6_SEED_PANDORA;
        instance->generic.serial = (uint32_t)(instance->key_data >> 32);
        instance->generic.cnt =
            honda_v6_pandora_counter(instance->key_data_hi, instance->bit_count);
        instance->generic.btn = 0;
    }

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

// ============================================================================
// DECODER - 313 MHz 3-LEVEL PROCESSING
// ============================================================================

static void honda_v6_313_add_trit(SubGhzProtocolDecoderHondaV6* instance, uint8_t trit) {
    if(instance->trit_count >= HONDA_V6_313_MAX_TRITS) return;

    /* Pack 2-bit trit into data_2 (first 32 trits) then data (remaining) */
    uint8_t idx = instance->trit_count;
    if(idx < 32) {
        instance->generic.data_2 |= ((uint64_t)(trit & 3)) << (62 - idx * 2);
    } else if(idx < 64) {
        instance->generic.data |= ((uint64_t)(trit & 3)) << (62 - (idx - 32) * 2);
    }
    instance->trit_count++;
}

static void honda_v6_313_process_frame(SubGhzProtocolDecoderHondaV6* instance) {
    if(instance->trit_count < HONDA_V6_313_MIN_TRITS) {
        return;
    }

    instance->generic.data_count_bit = instance->trit_count;
    instance->generic.serial = (uint32_t)(instance->generic.data_2 >> 32);
    instance->generic.cnt = instance->trit_count;
    instance->generic.btn = 0;
    instance->generic.seed = 0x313;  /* marker: 313 MHz mode */
    instance->generic.extra_check = true;

    /* Plausibility gate: reject frames whose serial trits are all zero
     * (empty/idle capture) rather than reporting a phantom 313 MHz decode. */
    if(instance->generic.serial == 0) {
        return;
    }

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

static uint8_t honda_v6_313_classify(uint32_t duration) {
    if(DURATION_DIFF(duration, HONDA_V6_313_TE_SHORT) <= HONDA_V6_313_TE_DELTA)
        return 0; /* S */
    if(DURATION_DIFF(duration, HONDA_V6_313_TE_MID) <= HONDA_V6_313_TE_DELTA)
        return 1; /* M */
    if(DURATION_DIFF(duration, HONDA_V6_313_TE_LONG) <= HONDA_V6_313_TE_DELTA)
        return 2; /* L */
    return 3; /* invalid */
}

// ============================================================================
// DECODER - FEED
// ============================================================================

void subghz_protocol_decoder_honda_v6_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaV6* instance = context;

    /* --- OOK carrier detection (runs in parallel) --- */
    if(level && duration >= HONDA_V6_CARRIER_MIN) {
        instance->ook_carriers++;
    }
    if(!level && duration >= HONDA_V6_SESSION_GAP) {
        if(instance->ook_carriers >= HONDA_V6_OOK_MIN_CARRIERS) {
            honda_v6_process_ook(instance);
        }
    }

    /* --- Classify timing for both modes --- */
    /* 434 MHz Manchester */
    bool is_short_434 = DURATION_DIFF(duration, subghz_protocol_honda_v6_const.te_short) <=
                        subghz_protocol_honda_v6_const.te_delta;
    bool is_long_434 = DURATION_DIFF(duration, subghz_protocol_honda_v6_const.te_long) <=
                       subghz_protocol_honda_v6_const.te_delta;

    /* 313 MHz 3-level */
    bool is_short_313 = DURATION_DIFF(duration, HONDA_V6_313_TE_SHORT) <= HONDA_V6_313_TE_DELTA;
    uint8_t trit_313 = honda_v6_313_classify(duration);

    switch(instance->decoder.parser_step) {
    case HondaV6DecoderStepReset:
        /* Try 434 MHz preamble */
        if(level && is_short_434) {
            instance->decoder.parser_step = HondaV6DecoderStepCheckPreamble;
            instance->header_count = 0;
            instance->key_data = 0;
            instance->key_data_hi = 0;
            instance->bit_count = 0;
            instance->midgap_crossed = false;
            manchester_advance(
                instance->manchester_saved_state,
                ManchesterEventReset,
                &instance->manchester_saved_state,
                NULL);
        }
        /* Try 313 MHz preamble */
        if(is_short_313 && !is_short_434) {
            instance->decoder.parser_step = HondaV6DecoderStep313Preamble;
            instance->header_count = 1;
        }
        break;

    // ================================================================
    // 434 MHz Manchester path
    // ================================================================
    case HondaV6DecoderStepCheckPreamble:
        if(is_short_434) {
            instance->header_count++;
        } else if(is_long_434 && instance->header_count >= HONDA_V6_MIN_PREAMBLE) {
            instance->decoder.parser_step = HondaV6DecoderStepCollectData;
            instance->key_data = 0;
            instance->key_data_hi = 0;
            instance->bit_count = 0;
            instance->midgap_crossed = false;
            instance->halfa_bits = 0;
            manchester_advance(
                instance->manchester_saved_state,
                ManchesterEventReset,
                &instance->manchester_saved_state,
                NULL);
            {
                ManchesterEvent sync_event =
                    level ? ManchesterEventLongHigh : ManchesterEventLongLow;
                bool data;
                bool data_ok = manchester_advance(
                    instance->manchester_saved_state,
                    sync_event,
                    &instance->manchester_saved_state,
                    &data);
                if(data_ok) {
                    honda_v6_append_bit(instance, data);
                }
            }
        } else if(!is_short_434) {
            instance->decoder.parser_step = HondaV6DecoderStepReset;
        }
        break;

    case HondaV6DecoderStepCollectData: {
        ManchesterEvent event = ManchesterEventReset;

        if(is_short_434) {
            event = level ? ManchesterEventShortHigh : ManchesterEventShortLow;
        } else if(is_long_434) {
            event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
        } else {
            /* Invalid timing — Pandora TS midgap recovery first. The clone
             * splits its 137-bit payload with a ~1.5ms low gap; on seeing it
             * we re-seed the Manchester FSM and keep collecting Data B. */
            bool is_midgap = !level && duration >= HONDA_V6_PANDORA_MIDGAP_MIN &&
                             duration <= HONDA_V6_PANDORA_MIDGAP_MAX;
            if(is_midgap && !instance->midgap_crossed &&
               instance->bit_count >= HONDA_V6_PANDORA_HALFA_MIN &&
               instance->bit_count <= HONDA_V6_PANDORA_HALFA_MAX) {
                instance->midgap_crossed = true;
                instance->halfa_bits = instance->bit_count;
                manchester_advance(
                    instance->manchester_saved_state,
                    ManchesterEventReset,
                    &instance->manchester_saved_state,
                    NULL);
                break;
            }

            if(!level) {
                bool flush_data;
                bool flush_ok = manchester_advance(
                    instance->manchester_saved_state,
                    ManchesterEventLongLow,
                    &instance->manchester_saved_state,
                    &flush_data);
                if(flush_ok) {
                    honda_v6_append_bit(instance, flush_data);
                }
            }
            honda_v6_process_fsk_frame(instance);
            instance->decoder.parser_step = HondaV6DecoderStepReset;
            break;
        }

        bool data;
        bool data_ok = manchester_advance(
            instance->manchester_saved_state, event, &instance->manchester_saved_state, &data);

        if(data_ok) {
            honda_v6_append_bit(instance, data);

            /* Pandora: frame length = halfa_bits + HALFB_BITS. halfa_bits is
             * remembered at the mid-gap transition above (varies per variant:
             * cell_0 ≈ 57, cell_2 ≈ 48). Half B is a fixed 78 bits. */
            if(instance->midgap_crossed && instance->halfa_bits > 0 &&
               instance->bit_count >=
                   (uint16_t)instance->halfa_bits + HONDA_V6_PANDORA_HALFB_BITS) {
                honda_v6_process_fsk_frame(instance);
                instance->decoder.parser_step = HondaV6DecoderStepReset;
            }
        }
    } break;

    // ================================================================
    // 313 MHz 3-level path
    // ================================================================
    case HondaV6DecoderStep313Preamble:
        if(is_short_313) {
            instance->header_count++;
        } else if(instance->header_count >= HONDA_V6_313_MIN_PREAMBLE && trit_313 <= 2) {
            /* First non-short timing after preamble = sync start */
            instance->decoder.parser_step = HondaV6DecoderStep313Data;
            instance->trit_count = 0;
            instance->generic.data = 0;
            instance->generic.data_2 = 0;
            /* Record sync timing as first trit */
            honda_v6_313_add_trit(instance, trit_313);
        } else {
            instance->decoder.parser_step = HondaV6DecoderStepReset;
        }
        break;

    case HondaV6DecoderStep313Data:
        if(trit_313 <= 2) {
            honda_v6_313_add_trit(instance, trit_313);
        } else {
            /* Gap or invalid timing — process frame */
            honda_v6_313_process_frame(instance);
            instance->decoder.parser_step = HondaV6DecoderStepReset;
        }
        break;
    }

    instance->decoder.te_last = duration;
}

// ============================================================================
// DECODER - HASH / SERIALIZE / DESERIALIZE
// ============================================================================

uint8_t subghz_protocol_decoder_honda_v6_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaV6* instance = context;
    return (uint8_t)(instance->generic.data >> 40);
}

SubGhzProtocolStatus subghz_protocol_decoder_honda_v6_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaV6* instance = context;
    // Only the 434 MHz stock 45-bit Manchester frame can be re-transmitted; the
    // 313 MHz trit, Pandora TS and OOK-carrier captures are not re-encodable.
    if(instance->generic.data_count_bit != HONDA_V6_DATA_BITS || instance->generic.seed != 0) {
        return SubGhzProtocolStatusError;
    }
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_honda_v6_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaV6* instance = context;
    instance->generic.seed = 0;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, HONDA_V6_DATA_BITS);
}

// ============================================================================
// DECODER - GET STRING
// ============================================================================

void subghz_protocol_decoder_honda_v6_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderHondaV6* instance = context;

    if(instance->generic.seed == 0x313) {
        /* 313 MHz 3-level frame */
        furi_string_cat_printf(
            output,
            "%s 313MHz %ut\r\n"
            "D2:%08lX%08lX\r\n"
            "D1:%08lX%08lX\r\n",
            instance->generic.protocol_name,
            instance->generic.data_count_bit,
            (unsigned long)(instance->generic.data_2 >> 32),
            (unsigned long)(instance->generic.data_2 & 0xFFFFFFFF),
            (unsigned long)(instance->generic.data >> 32),
            (unsigned long)(instance->generic.data & 0xFFFFFFFF));
    } else if(instance->generic.seed == HONDA_V6_SEED_PANDORA) {
        /* Pandora TS family — variable-width counter prefix at frame start,
         * width = bit_count - 119. Serial (LO 64b) stable per fob. */
        uint64_t hi = instance->generic.data_2;
        uint64_t lo = instance->generic.data;
        uint16_t bits = instance->generic.data_count_bit;
        uint8_t cnt_w = honda_v6_pandora_counter_width(bits);
        uint16_t cnt = honda_v6_pandora_counter(hi, bits);
        uint64_t rolling = (hi >> 8) & 0xFFFFFFFFFFULL;
        uint8_t tail = (uint8_t)(hi & 0xFF);
        furi_string_cat_printf(
            output,
            "%s Pandora %ubit\r\n"
            "Ser:%08lX%08lX\r\n"
            "Cnt:%u (%ub) Roll:%02lX%08lX Tail:%02X\r\n",
            instance->generic.protocol_name,
            bits,
            (unsigned long)(lo >> 32),
            (unsigned long)(lo & 0xFFFFFFFF),
            cnt,
            cnt_w,
            (unsigned long)(rolling >> 32),
            (unsigned long)(rolling & 0xFFFFFFFF),
            tail);
    } else if(instance->generic.data_count_bit > 0) {
        /* 434 MHz FSK decoded frame */
        honda_v6_extract_fields(&instance->generic);

        uint64_t hop = instance->generic.data & 0x1FFFFFFFFFF;
        uint32_t hop_hi = (uint32_t)(hop >> 32);
        uint32_t hop_lo = (uint32_t)(hop & 0xFFFFFFFF);

        furi_string_cat_printf(
            output,
            "%s %ubit\r\n"
            "Hop:%01lX%08lX\r\n"
            "Btn:%s Cnt:%lu\r\n",
            instance->generic.protocol_name,
            instance->generic.data_count_bit,
            (unsigned long)hop_hi,
            (unsigned long)hop_lo,
            honda_v6_button_name(instance->generic.btn),
            (unsigned long)instance->generic.cnt);
    } else {
        /* OOK carrier identification */
        furi_string_cat_printf(
            output,
            "%s (Honda FSK)\r\n"
            "Carriers:%lu\r\n"
            "OOK capture - use FSK\r\n"
            "preset to decode bits\r\n",
            instance->generic.protocol_name,
            (unsigned long)instance->generic.cnt);
    }
}

// ============================================================================
// ENCODER (434 MHz stock 45-bit Manchester)
//
// Reproduces the waveform the 434 MHz path accepts:
//   [preamble short pairs][trigger LONG-LOW][45 Manchester bits][gap]
// The Manchester state machine is the standard Flipper OOK-Manchester encoder
// (identical to audi_v1/audi_v2), whose output the shared manchester_advance
// decoder reads back verbatim. The trigger LONG-LOW yields no data bit (it only
// re-seeds the FSM), and the terminator ends HIGH so the decoder's LONG-LOW
// flush never appends a phantom bit — giving an exact 45-bit round-trip.
// ============================================================================

#define HONDA_V6_ENC_PREAMBLE_PAIRS 30
#define HONDA_V6_ENC_UPLOAD_SIZE    (HONDA_V6_ENC_PREAMBLE_PAIRS * 2 + HONDA_V6_DATA_BITS * 2 + 4)

void* subghz_protocol_encoder_honda_v6_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderHondaV6* instance = malloc(sizeof(SubGhzProtocolEncoderHondaV6));
    memset(instance, 0, sizeof(SubGhzProtocolEncoderHondaV6));
    instance->base.protocol = &subghz_protocol_honda_v6;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->encoder.repeat = 3;
    instance->encoder.size_upload = HONDA_V6_ENC_UPLOAD_SIZE;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_honda_v6_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderHondaV6* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

static void subghz_protocol_encoder_honda_v6_get_upload(SubGhzProtocolEncoderHondaV6* instance) {
    const uint32_t te_s = (uint32_t)subghz_protocol_honda_v6_const.te_short;
    const uint32_t te_l = (uint32_t)subghz_protocol_honda_v6_const.te_long;
    size_t index = 0;
    const size_t enc_cap = HONDA_V6_ENC_UPLOAD_SIZE;

    // Preamble: LOW-HIGH short pairs. The leading LOW is harmlessly skipped by
    // the decoder (Reset needs a HIGH short); the run ends HIGH so the trigger
    // LONG-LOW alternates cleanly.
    for(int p = 0; p < HONDA_V6_ENC_PREAMBLE_PAIRS; p++) {
        if(index < enc_cap) instance->encoder.upload[index++] = level_duration_make(false, te_s);
        if(index < enc_cap) instance->encoder.upload[index++] = level_duration_make(true, te_s);
    }

    // Trigger LONG-LOW (re-seeds the FSM, produces no data bit).
    if(index < enc_cap) instance->encoder.upload[index++] = level_duration_make(false, te_l);

    // 45 Manchester data bits, MSB first, standard Flipper OOK-Manchester FSM.
    uint8_t state = ManchesterStateMid1;
    for(int i = HONDA_V6_DATA_BITS - 1; i >= 0; i--) {
        bool b = (instance->generic.data >> i) & 1;
        if(state == ManchesterStateMid1) {
            if(!b) {
                if(index < enc_cap)
                    instance->encoder.upload[index++] = level_duration_make(true, te_l);
                state = ManchesterStateMid0;
            } else {
                if(index < enc_cap)
                    instance->encoder.upload[index++] = level_duration_make(true, te_s);
                if(index < enc_cap)
                    instance->encoder.upload[index++] = level_duration_make(false, te_s);
                state = ManchesterStateMid1;
            }
        } else { // ManchesterStateMid0
            if(!b) {
                if(index < enc_cap)
                    instance->encoder.upload[index++] = level_duration_make(false, te_s);
                if(index < enc_cap)
                    instance->encoder.upload[index++] = level_duration_make(true, te_s);
                state = ManchesterStateMid0;
            } else {
                if(index < enc_cap)
                    instance->encoder.upload[index++] = level_duration_make(false, te_l);
                state = ManchesterStateMid1;
            }
        }
    }

    // Terminate on a HIGH gap so the decoder's LONG-LOW flush is skipped.
    // Mid0 => last pulse was HIGH: insert a LOW short (Mid0+ShortLow -> Start0,
    // no bit) to flip the level before the HIGH gap.
    if(state == ManchesterStateMid0) {
        if(index < enc_cap) instance->encoder.upload[index++] = level_duration_make(false, te_s);
    }
    if(index < enc_cap) instance->encoder.upload[index++] = level_duration_make(true, (uint32_t)6000);

    instance->encoder.size_upload = index;
}

SubGhzProtocolStatus
    subghz_protocol_encoder_honda_v6_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderHondaV6* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        if(subghz_block_generic_deserialize_check_count_bit(
               &instance->generic, flipper_format, HONDA_V6_DATA_BITS) != SubGhzProtocolStatusOk) {
            break;
        }
        flipper_format_read_uint32(
            flipper_format, "Repeat", (uint32_t*)&instance->encoder.repeat, 1);
        subghz_protocol_encoder_honda_v6_get_upload(instance);
        instance->encoder.front = 0;
        instance->encoder.is_running = true;
        res = SubGhzProtocolStatusOk;
    } while(false);
    return res;
}

void subghz_protocol_encoder_honda_v6_stop(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderHondaV6* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_honda_v6_yield(void* context) {
    SubGhzProtocolEncoderHondaV6* instance = context;
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
