/**
 * @file fiat_v2_v3_v5.c
 * @brief Combined Fiat V2/V3/V5 SubGHz Protocol — single "Fiat" protocol
 *
 * One protocol registration, variant detected at runtime from signal timing.
 * V2: 102-bit frame (16 preamble + 32 fix + 32 hop + 22 tail), te_short=100us
 * V3: 70-bit frame (preamble + 22 fix + 2 btn + 2 status + 44 hop), te_short=460us
 * V5: 97-bit frame (preamble + 32 fix + 8 type + 8 btn + 8 seed + 8 cnt + 32 hop + 1 trail), te_short=200us
 */

#include "fiat_v2_v3_v5.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"

#include "../blocks/custom_btn_i.h"

#define TAG "SubGhzProtocolFiat"

// ============================================================================
// TIMING CONSTANTS
// ============================================================================

// V2
#define FIAT_V2_TE_SHORT       100
#define FIAT_V2_TE_LONG        200
#define FIAT_V2_TE_DELTA       50
#define FIAT_V2_MIN_GAP        500
#define FIAT_V2_FRAME_BITS     102
#define FIAT_V2_MIN_FRAME_BITS 90
#define FIAT_V2_MAX_BITS       150
#define FIAT_V2_PREAMBLE_BITS  16

static const SubGhzBlockConst subghz_protocol_fiat_v2_const = {
    .te_short = FIAT_V2_TE_SHORT,
    .te_long = FIAT_V2_TE_LONG,
    .te_delta = FIAT_V2_TE_DELTA,
    .min_count_bit_for_found = FIAT_V2_MIN_FRAME_BITS,
};

// V3
#define FIAT_V3_TE_SHORT         460
#define FIAT_V3_TE_LONG          920
#define FIAT_V3_TE_DELTA         200
#define FIAT_V3_MIN_GAP          1500
#define FIAT_V3_FRAME_BITS       70
#define FIAT_V3_MIN_FRAME_BITS   60
#define FIAT_V3_MAX_BITS         100
#define FIAT_V3_PREAMBLE_MIN     4
#define FIAT_V3_PREAMBLE_MAX     20
#define FIAT_V3_PREAMBLE_BITS    13
#define FIAT_V3_MAX_TIMINGS      170
#define FIAT_V3_MIN_PREAMBLE_GAP 400

static const SubGhzBlockConst subghz_protocol_fiat_v3_const = {
    .te_short = FIAT_V3_TE_SHORT,
    .te_long = FIAT_V3_TE_LONG,
    .te_delta = FIAT_V3_TE_DELTA,
    .min_count_bit_for_found = FIAT_V3_MIN_FRAME_BITS,
};

// V5
#define FIAT_V5_TE_SHORT       200
#define FIAT_V5_TE_LONG        400
#define FIAT_V5_TE_DELTA       100
#define FIAT_V5_MIN_GAP        2000
#define FIAT_V5_FRAME_BITS     97
#define FIAT_V5_MIN_FRAME_BITS 90
#define FIAT_V5_MAX_BITS       130
#define FIAT_V5_PREAMBLE_MIN   10
#define FIAT_V5_PREAMBLE_BITS  15

static const SubGhzBlockConst subghz_protocol_fiat_v5_const = {
    .te_short = FIAT_V5_TE_SHORT,
    .te_long = FIAT_V5_TE_LONG,
    .te_delta = FIAT_V5_TE_DELTA,
    .min_count_bit_for_found = FIAT_V5_MIN_FRAME_BITS,
};

// ============================================================================
// V2 CUSTOM HITAG2 CIPHER (Ducato/FCA variant)
// ============================================================================
// Verified from D70F3631 V850ES firmware (Ducato_Arseniy).
// Custom truth tables and tap mask — NOT standard Hitag2.
// Master key from EEPROM (group-reversed by boot code).
// UID = serial from fix code (fix >> 4), iv0 = 0x20.

#define FIAT_V2_MASK48    0xFFFFFFFFFFFFULL
#define FIAT_V2_FA        0x2C79U
#define FIAT_V2_FB        0x6671U
#define FIAT_V2_FC        0x7907287BUL
#define FIAT_V2_TAP_MASK  0xB38083220073ULL
#define FIAT_V2_BIT(s, n) (((s) >> (n)) & 1)

static const uint8_t FIAT_V2_MASTER_KEY[6] = {0x71, 0x53, 0x73, 0x3D, 0x0E, 0xB2};
#define FIAT_V2_IV0 0x20

static inline int fiat_v2_filter(uint64_t s) {
    int g1 = (FIAT_V2_BIT(s, 41) << 3) | (FIAT_V2_BIT(s, 42) << 2) |
             (FIAT_V2_BIT(s, 44) << 1) | FIAT_V2_BIT(s, 45);
    int g2 = (FIAT_V2_BIT(s, 32) << 3) | (FIAT_V2_BIT(s, 33) << 2) |
             (FIAT_V2_BIT(s, 35) << 1) | FIAT_V2_BIT(s, 39);
    int g3 = (FIAT_V2_BIT(s, 21) << 3) | (FIAT_V2_BIT(s, 24) << 2) |
             (FIAT_V2_BIT(s, 26) << 1) | FIAT_V2_BIT(s, 30);
    int g4 = (FIAT_V2_BIT(s, 14) << 3) | (FIAT_V2_BIT(s, 16) << 2) |
             (FIAT_V2_BIT(s, 18) << 1) | FIAT_V2_BIT(s, 19);
    int g5 = (FIAT_V2_BIT(s, 1) << 3) | (FIAT_V2_BIT(s, 3) << 2) |
             (FIAT_V2_BIT(s, 4) << 1) | FIAT_V2_BIT(s, 13);
    int r0 = (FIAT_V2_FA >> g1) & 1;
    int r1 = (FIAT_V2_FB >> g2) & 1;
    int r2 = (FIAT_V2_FB >> g3) & 1;
    int r3 = (FIAT_V2_FB >> g4) & 1;
    int r4 = (FIAT_V2_FA >> g5) & 1;
    return (int)((FIAT_V2_FC >> (r0 | (r1 << 1) | (r2 << 2) | (r3 << 3) | (r4 << 4))) & 1);
}

static inline int fiat_v2_parity64(uint64_t x) {
    x ^= x >> 32;
    x ^= x >> 16;
    x ^= x >> 8;
    x ^= x >> 4;
    x ^= x >> 2;
    x ^= x >> 1;
    return (int)(x & 1);
}

/**
 * Initialize cipher state from UID, key and IV.
 * state[47:16] = uid, state[15:0] = key[4:5]
 * Then 32 rounds mixing filter ^ key_bit ^ iv_bit.
 */
static uint64_t fiat_v2_cipher_init(uint32_t uid, const uint8_t key[6], const uint8_t iv[4]) {
    uint64_t s = ((uint64_t)uid << 16) | ((uint64_t)key[4] << 8) | (uint64_t)key[5];
    for(int i = 0; i < 32; i++) {
        int f = fiat_v2_filter(s);
        int kb = (key[i / 8] >> (7 - (i % 8))) & 1;
        int ib = (iv[i / 8] >> (7 - (i % 8))) & 1;
        s = ((s << 1) & FIAT_V2_MASK48) | (uint64_t)(f ^ kb ^ ib);
    }
    return s;
}

/**
 * Generate n bits of keystream. Since plaintext=0, keystream IS the hop code.
 */
static uint32_t fiat_v2_cipher_ks(uint64_t state, int n) {
    uint32_t ks = 0;
    for(int i = 0; i < n; i++) {
        ks = (ks << 1) | (uint32_t)fiat_v2_filter(state);
        state = ((state << 1) & FIAT_V2_MASK48) |
                (uint64_t)fiat_v2_parity64(state & FIAT_V2_TAP_MASK);
    }
    return ks;
}

/**
 * Build IV from counter and fix code.
 * iv[0] = iv0_base (constant per remote, typically 0x20)
 * iv[1] = counter >> 2
 * iv[2] = ((counter & 3) << 6) | ds_low6
 * iv[3] = ds_byte
 * where ds is derived from fix[2:3] (the data_struct halfword in MCU).
 */
static void fiat_v2_build_iv(uint16_t counter, uint8_t iv0, uint32_t fix, uint8_t iv[4]) {
    // fix MSB-first: fix[2] = (fix >> 8) & 0xFF, fix[3] = fix & 0xFF
    uint8_t fix2 = (fix >> 8) & 0xFF;
    uint8_t fix3 = fix & 0xFF;
    // LE halfword as V850 would load it
    uint16_t hw = (uint16_t)fix2 | ((uint16_t)fix3 << 8);
    uint8_t ds_low6 = (hw >> 4) & 0x3F;
    uint8_t ds_byte = ((hw & 0xF) << 4) | ((hw >> 12) & 0xF);

    iv[0] = iv0;
    iv[1] = counter >> 2;
    iv[2] = ((counter & 3) << 6) | ds_low6;
    iv[3] = ds_byte;
}

/**
 * Compute V2 hop code using the custom Hitag2 cipher.
 * UID = fix >> 4 (28-bit serial from fix code).
 * Uses hardcoded master key and iv0=0x20.
 * Returns the 32-bit keystream (= hop, since plaintext=0).
 */
static uint32_t fiat_v2_compute_hop(uint16_t counter, uint32_t fix) {
    uint32_t uid = fix >> 4;
    uint8_t iv[4];
    fiat_v2_build_iv(counter, FIAT_V2_IV0, fix, iv);
    uint64_t state = fiat_v2_cipher_init(uid, FIAT_V2_MASTER_KEY, iv);
    return fiat_v2_cipher_ks(state, 32);
}

// ============================================================================
// MANCHESTER TABLE & SHARED DECODE
// ============================================================================

static const uint8_t fiat_manchester_table[4] = {0x01, 0x91, 0x9B, 0xFB};

static bool fiat_manchester_decode(uint8_t* state, uint8_t timing_type, uint8_t* bit_out) {
    if(timing_type == 8) {
        *state = 1;
        return false;
    }
    if(*state >= 4) {
        *state = 1;
        return false;
    }

    uint8_t entry = fiat_manchester_table[*state];
    uint8_t new_state = (entry >> timing_type) & 0x3;

    if(*state == new_state) {
        return false;
    }

    *state = new_state;

    if(new_state == 2) {
        if(bit_out) *bit_out = 0;
        return true;
    } else if(new_state == 1) {
        if(bit_out) *bit_out = 1;
        return true;
    }

    return false;
}

// ============================================================================
// VARIANT ENUM & PARAMETERS TABLE
// ============================================================================

typedef enum {
    FiatVariantV2 = 2,
    FiatVariantV3 = 3,
    FiatVariantV5 = 5,
} FiatVariant;

// Index for the params table (0,1,2)
#define FIAT_VARIANT_IDX(v) ((v) == FiatVariantV2 ? 0 : (v) == FiatVariantV3 ? 1 : 2)
#define FIAT_VARIANT_COUNT  3

typedef struct {
    FiatVariant variant;
    const SubGhzBlockConst* block_const;
    uint32_t min_gap;
    uint16_t frame_bits;
    uint16_t max_bits;
    uint8_t preamble_bits;
    uint8_t btn_max;
    bool has_counter;
    uint8_t cnt_length_bit;
    uint8_t btn_length_bit;
    const char* display_name;
} FiatVariantParams;

static const FiatVariantParams fiat_variant_params[FIAT_VARIANT_COUNT] = {
    {
        .variant = FiatVariantV2,
        .block_const = &subghz_protocol_fiat_v2_const,
        .min_gap = FIAT_V2_MIN_GAP,
        .frame_bits = FIAT_V2_FRAME_BITS,
        .max_bits = FIAT_V2_MAX_BITS,
        .preamble_bits = FIAT_V2_PREAMBLE_BITS,
        .btn_max = 3,
        .has_counter = true,
        .cnt_length_bit = 10,
        .btn_length_bit = 4,
        //Alfa Romeo 2002
        .display_name = "Fiat V2",
    },
    {
        .variant = FiatVariantV3,
        .block_const = &subghz_protocol_fiat_v3_const,
        .min_gap = FIAT_V3_MIN_GAP,
        .frame_bits = FIAT_V3_FRAME_BITS,
        .max_bits = FIAT_V3_MAX_BITS,
        .preamble_bits = FIAT_V3_PREAMBLE_BITS,
        .btn_max = 3,
        .has_counter = false,
        .cnt_length_bit = 0,
        .btn_length_bit = 2,
        .display_name = "Fiat V3",
    },
    {
        .variant = FiatVariantV5,
        .block_const = &subghz_protocol_fiat_v5_const,
        .min_gap = FIAT_V5_MIN_GAP,
        .frame_bits = FIAT_V5_FRAME_BITS,
        .max_bits = FIAT_V5_MAX_BITS,
        .preamble_bits = FIAT_V5_PREAMBLE_BITS,
        .btn_max = 3,
        .has_counter = true,
        .cnt_length_bit = 8,
        .btn_length_bit = 8,
        .display_name = "Fiat V5",
    },
};

static const FiatVariantParams* fiat_get_params(FiatVariant v) {
    return &fiat_variant_params[FIAT_VARIANT_IDX(v)];
}

// ============================================================================
// BUTTON CONSTANTS
// ============================================================================

// V2 buttons: one-hot encoded in fix bottom nibble
#define FIAT_V2_BTN_TRUNK  0x02
#define FIAT_V2_BTN_LOCK   0x04
#define FIAT_V2_BTN_UNLOCK 0x08

// V3 buttons
#define FIAT_V3_BTN_LOCK   0x00
#define FIAT_V3_BTN_UNLOCK 0x01
#define FIAT_V3_BTN_TRUNK  0x02

// V5 buttons
#define FIAT_V5_BTN_OPEN  0x80
#define FIAT_V5_BTN_TRUNK 0x40
#define FIAT_V5_BTN_CLOSE 0x20

static const uint8_t fiat_v2_btns[] = {FIAT_V2_BTN_LOCK, FIAT_V2_BTN_UNLOCK, FIAT_V2_BTN_TRUNK};
static const uint8_t fiat_v3_btns[] = {FIAT_V3_BTN_LOCK, FIAT_V3_BTN_UNLOCK, FIAT_V3_BTN_TRUNK};
static const uint8_t fiat_v5_btns[] = {FIAT_V5_BTN_CLOSE, FIAT_V5_BTN_OPEN, FIAT_V5_BTN_TRUNK};

// ============================================================================
// UNIFIED BUTTON FUNCTIONS
// ============================================================================

static FiatVariant fiat_current_variant = FiatVariantV2;

static const char* fiat_get_button_name(FiatVariant variant, uint8_t btn);

static const char* fiat_get_button_name_simple(uint8_t btn) {
    return fiat_get_button_name(fiat_current_variant, btn);
}

static const char* fiat_get_button_name(FiatVariant variant, uint8_t btn) {
    switch(variant) {
    case FiatVariantV2:
        switch(btn) {
        case FIAT_V2_BTN_LOCK:
            return "LOCK";
        case FIAT_V2_BTN_UNLOCK:
            return "UNLOCK";
        case FIAT_V2_BTN_TRUNK:
            return "TRUNK";
        default: {
            static char unknown_buf[8];
            snprintf(unknown_buf, sizeof(unknown_buf), "0x%02X", btn);
            return unknown_buf;
        }
        }
    case FiatVariantV3:
        switch(btn) {
        case FIAT_V3_BTN_LOCK:
            return "LOCK";
        case FIAT_V3_BTN_UNLOCK:
            return "UNLOCK";
        case FIAT_V3_BTN_TRUNK:
            return "TRUNK";
        default:
            return "UNKNOWN";
        }
    case FiatVariantV5:
        switch(btn) {
        case FIAT_V5_BTN_OPEN:
            return "OPEN";
        case FIAT_V5_BTN_TRUNK:
            return "TRUNK";
        case FIAT_V5_BTN_CLOSE:
            return "CLOSE";
        default:
            return "UNKNOWN";
        }
    }
    return "UNKNOWN";
}

static uint8_t fiat_get_btn_code(FiatVariant variant, uint8_t original_btn) {
    uint8_t btn = original_btn;
    uint8_t custom_id = subghz_custom_btn_get();
    switch(variant) {
    case FiatVariantV2:
        subghz_custom_btn_apply_positional(
            &btn, original_btn, custom_id, fiat_v2_btns, COUNT_OF(fiat_v2_btns));
        break;
    case FiatVariantV3:
        subghz_custom_btn_apply_positional(
            &btn, original_btn, custom_id, fiat_v3_btns, COUNT_OF(fiat_v3_btns));
        break;
    case FiatVariantV5:
        subghz_custom_btn_apply_positional(
            &btn, original_btn, custom_id, fiat_v5_btns, COUNT_OF(fiat_v5_btns));
        break;
    }
    return btn;
}

static bool fiat_v5_is_valid_button(uint8_t btn) {
    return (btn == FIAT_V5_BTN_OPEN || btn == FIAT_V5_BTN_TRUNK || btn == FIAT_V5_BTN_CLOSE);
}

// ============================================================================
// DATA STRUCTURES
// ============================================================================

typedef enum {
    FiatDecoderStepReset = 0,
    FiatDecoderStepData = 1,
} FiatDecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    FiatVariant variant;
    uint8_t manchester_state;
    uint8_t bits[150];
    uint16_t bit_count;

    union {
        struct {
            uint32_t fix_value;
            uint32_t hop_value;
            uint32_t tail;
            uint8_t btn;
            uint16_t counter;
        } v2;
        struct {
            uint32_t fix_value;
            uint8_t btn;
            uint8_t status_bits;
            uint64_t hop_value;
            uint8_t hop_low;
            uint16_t timing_count;
            uint32_t preamble_gap_sum;
            uint16_t preamble_gap_count;
            bool preamble_ended;
        } v3;
        struct {
            uint32_t fix_value;
            uint8_t type_byte;
            uint8_t btn;
            uint8_t seed;
            uint8_t cnt;
            uint32_t hop_value;
        } v5;
    } extra;
} SubGhzProtocolDecoderFiat;

typedef struct {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    FiatVariant variant;

    union {
        struct {
            uint32_t fix_value;
            uint32_t hop_value;
            uint32_t tail;
            uint8_t btn;
            uint16_t counter;
        } v2;
        struct {
            uint32_t fix_value;
            uint8_t btn;
            uint8_t status_bits;
            uint64_t hop_value;
            uint8_t hop_low;
        } v3;
        struct {
            uint32_t fix_value;
            uint8_t type_byte;
            uint8_t btn;
            uint8_t seed;
            uint8_t cnt;
            uint32_t hop_value;
        } v5;
    } extra;
} SubGhzProtocolEncoderFiat;

// ============================================================================
// PROTOCOL DEFINITION — single VTable, single registration
// ============================================================================

static SubGhzBlockGeneric* subghz_protocol_decoder_fiat_get_generic(void* context) {
    SubGhzProtocolDecoderFiat* instance = context;
    return &instance->generic;
}

const SubGhzProtocolDecoder subghz_protocol_fiat_decoder = {
    .alloc = subghz_protocol_decoder_fiat_alloc,
    .free = subghz_protocol_decoder_fiat_free,
    .feed = subghz_protocol_decoder_fiat_feed,
    .reset = subghz_protocol_decoder_fiat_reset,
    .get_hash_data = subghz_protocol_decoder_fiat_get_hash_data,
    .serialize = subghz_protocol_decoder_fiat_serialize,
    .deserialize = subghz_protocol_decoder_fiat_deserialize,
    .get_string = subghz_protocol_decoder_fiat_get_string,
    .get_generic = subghz_protocol_decoder_fiat_get_generic,
};

const SubGhzProtocolEncoder subghz_protocol_fiat_encoder = {
    .alloc = subghz_protocol_encoder_fiat_alloc,
    .free = subghz_protocol_encoder_fiat_free,
    .deserialize = subghz_protocol_encoder_fiat_deserialize,
    .stop = subghz_protocol_encoder_fiat_stop,
    .yield = subghz_protocol_encoder_fiat_yield,
};

const SubGhzProtocol subghz_protocol_fiat_v2_v3_v5 = {
    .name = SUBGHZ_PROTOCOL_FIAT_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_fiat_decoder,
    .encoder = &subghz_protocol_fiat_encoder,
};

// ============================================================================
// V3 HOP LINEAR CRYPTANALYSIS
// ============================================================================

/**
 * Extract button value from V3 hop code using linear cryptanalysis.
 *   btn = XOR of bits {4,5,12,13,20,21,28,29,36,37}
 *
 * Returns 0 for LOCK, 1 for UNLOCK
 */
static uint8_t fiat_v3_hop_btn_extract(uint64_t hop) {
    /* Mask selects bit-pairs (4,5) in each of the 5 full bytes */
    const uint64_t mask = (3ULL << 4) | (3ULL << 12) | (3ULL << 20) | (3ULL << 28) | (3ULL << 36);
    uint64_t masked = hop & mask;

    /* popcount parity: XOR of all selected bits = popcount mod 2 */
    masked ^= masked >> 32;
    masked ^= masked >> 16;
    masked ^= masked >> 8;
    masked ^= masked >> 4;
    masked ^= masked >> 2;
    masked ^= masked >> 1;
    return (uint8_t)(masked & 1);
}

/**
 * Validate V3 hop against the 5 known cipher invariants.
 */
static bool fiat_v3_hop_invariants_check(uint64_t hop) {
    /* R1: XOR bits {5,6,13,14,21,22,29,30,37,38} = 0 */
    const uint64_t r1 = (3ULL << 5) | (3ULL << 13) | (3ULL << 21) | (3ULL << 29) | (3ULL << 37);
    /* R2: XOR bits {5,7,13,15,21,23,29,31,37,39} = 0 */
    const uint64_t r2 = (5ULL << 5) | (5ULL << 13) | (5ULL << 21) | (5ULL << 29) | (5ULL << 37);
    /* R3: XOR bits {5,8,13,16,21,24,29,32,37,40} = 0 */
    const uint64_t r3 = (1ULL << 5) | (1ULL << 8) | (1ULL << 13) | (1ULL << 16) |
                         (1ULL << 21) | (1ULL << 24) | (1ULL << 29) | (1ULL << 32) |
                         (1ULL << 37) | (1ULL << 40);
    /* R4: XOR bits {1,9,17,25,33,41} = 0 */
    const uint64_t r4 = (1ULL << 1) | (1ULL << 9) | (1ULL << 17) | (1ULL << 25) |
                         (1ULL << 33) | (1ULL << 41);

    const uint64_t masks[] = {r1, r2, r3, r4};
    for(int i = 0; i < 4; i++) {
        uint64_t v = hop & masks[i];
        v ^= v >> 32;
        v ^= v >> 16;
        v ^= v >> 8;
        v ^= v >> 4;
        v ^= v >> 2;
        v ^= v >> 1;
        if(v & 1) return false;
    }
    return true;
}

// ============================================================================
// V3 PACK / UNPACK helpers
// ============================================================================

static void fiat_v3_pack_generic(
    SubGhzBlockGeneric* generic,
    uint32_t fix,
    uint8_t btn,
    uint64_t hop,
    uint8_t* hop_low_out) {
    generic->data = ((uint64_t)(fix & 0x3FFFFF) << 42) | ((uint64_t)(btn & 0x3) << 40) |
                    ((hop >> 4) & 0xFFFFFFFFFFULL);
    generic->data_count_bit = FIAT_V3_FRAME_BITS;
    generic->serial = fix;
    generic->btn = btn;
    generic->cnt = 0;
    if(hop_low_out) *hop_low_out = hop & 0xF;
}

static void fiat_v3_unpack_generic(
    const SubGhzBlockGeneric* generic,
    uint8_t hop_low,
    uint32_t* fix_out,
    uint8_t* btn_out,
    uint64_t* hop_out) {
    uint32_t fix = (generic->data >> 42) & 0x3FFFFF;
    uint8_t btn = (generic->data >> 40) & 0x3;
    uint64_t hop = ((generic->data & 0xFFFFFFFFFFULL) << 4) | (hop_low & 0xF);
    if(fix_out) *fix_out = fix;
    if(btn_out) *btn_out = btn;
    if(hop_out) *hop_out = hop;
}

// ============================================================================
// V5 PACK / UNPACK helpers
// ============================================================================

static void fiat_v5_pack_generic(
    SubGhzBlockGeneric* generic,
    uint32_t fix,
    uint8_t btn,
    uint8_t cnt,
    uint32_t hop) {
    generic->data = ((uint64_t)fix << 32) | ((uint64_t)hop & 0xFFFFFFFF);
    generic->data_count_bit = FIAT_V5_FRAME_BITS;
    generic->serial = fix;
    generic->btn = btn;
    generic->cnt = cnt;
}

static void fiat_v5_unpack_generic(
    const SubGhzBlockGeneric* generic,
    uint32_t* fix_out,
    uint32_t* hop_out) {
    if(fix_out) *fix_out = (uint32_t)(generic->data >> 32);
    if(hop_out) *hop_out = (uint32_t)(generic->data & 0xFFFFFFFF);
}

// ============================================================================
// UNIFIED FRAME PROCESSING
// ============================================================================

static void fiat_process_frame(SubGhzProtocolDecoderFiat* instance) {
    uint16_t start = 0;
    while(start < instance->bit_count && instance->bits[start] == 1) {
        start++;
    }
    uint16_t preamble_len = start;
    uint16_t data_len = instance->bit_count - start;

    switch(instance->variant) {
    case FiatVariantV2: {
        if(preamble_len < 8 || data_len < 64) return;

        uint32_t fix = 0;
        for(uint16_t i = 0; i < 32; i++) {
            fix = (fix << 1) | instance->bits[start + i];
        }
        uint32_t hop = 0;
        for(uint16_t i = 0; i < 32; i++) {
            hop = (hop << 1) | instance->bits[start + 32 + i];
        }
        uint32_t tail = 0;
        uint16_t tail_len = (data_len > 64) ? (data_len - 64) : 0;
        if(tail_len > 22) tail_len = 22;
        for(uint16_t i = 0; i < tail_len; i++) {
            tail = (tail << 1) | instance->bits[start + 64 + i];
        }

        uint8_t btn = fix & 0x0F;
        uint16_t counter = (hop >> 20) & 0x3FF;

        instance->extra.v2.fix_value = fix;
        instance->extra.v2.hop_value = hop;
        instance->extra.v2.tail = tail;
        instance->extra.v2.btn = btn;
        instance->extra.v2.counter = counter;
        instance->generic.data = ((uint64_t)fix << 32) | hop;
        instance->generic.data_count_bit = FIAT_V2_FRAME_BITS;
        instance->generic.serial = fix >> 4;
        instance->generic.btn = btn;
        instance->generic.cnt = counter;

        // Debug: dump all frame fields for cipher analysis
        {
            /*uint32_t serial = fix >> 4;
            uint8_t hop_flags = (hop >> 30) & 0x3;
            uint32_t hop_bottom20 = hop & 0xFFFFF;

            FURI_LOG_D(TAG, "=== V2 Frame Debug ===");
            FURI_LOG_D(
                TAG,
                "  Preamble: %d bits, Data: %d bits, Tail: %d bits",
                preamble_len,
                data_len,
                tail_len);
            FURI_LOG_D(
                TAG,
                "  Fix:     %08lX  Serial:%07lX  Btn:%02X",
                (unsigned long)fix,
                (unsigned long)serial,
                btn);
            FURI_LOG_D(
                TAG,
                "  Hop:     %08lX  Flags:%d  Cnt:%d  Bot20:%05lX",
                (unsigned long)hop,
                hop_flags,
                counter,
                (unsigned long)hop_bottom20);
            FURI_LOG_D(TAG, "  Tail:    %06lX (%d bits)", (unsigned long)tail, tail_len);*/

            // Counter extracted different ways
            /*uint16_t cnt_10bit = (hop >> 20) & 0x3FF;
            uint16_t cnt_12bit = (hop >> 20) & 0xFFF;
            uint16_t cnt_8bit = (hop >> 22) & 0xFF;
            uint16_t cnt_full16 = (hop >> 16) & 0xFFFF;
            FURI_LOG_D(
                TAG,
                "  Counter variants: 10bit=%d 12bit=%d 8bit=%d top16=%d",
                cnt_10bit,
                cnt_12bit,
                cnt_8bit,
                cnt_full16);*/

            // Hop bytes individually (as MCU data_struct[4:7] would contain them)
            /*uint8_t hb0 = (hop >> 24) & 0xFF;
            uint8_t hb1 = (hop >> 16) & 0xFF;
            uint8_t hb2 = (hop >> 8) & 0xFF;
            uint8_t hb3 = hop & 0xFF;
            FURI_LOG_D(
                TAG,
                "  Hop bytes: [%02X %02X %02X %02X]  reversed: [%02X %02X %02X %02X]",
                hb0,
                hb1,
                hb2,
                hb3,
                hb3,
                hb2,
                hb1,
                hb0);*/

            // Hop as bswap32 (byte-reversed)
            /*uint32_t hop_rev = ((hop >> 24) & 0xFF) | ((hop >> 8) & 0xFF00) |
                               ((hop << 8) & 0xFF0000) | ((hop << 24) & 0xFF000000);
            FURI_LOG_D(TAG, "  Hop bswap32: %08lX", (unsigned long)hop_rev);*/

            // Tail bytes
            /*uint8_t tb0 = (tail >> 16) & 0x1F; // 5 bits
            uint8_t tb1 = (tail >> 8) & 0xFF;
            uint8_t tb2 = tail & 0xFF;
            FURI_LOG_D(TAG, "  Tail bytes: [%02X %02X %02X] (21bit)", tb0, tb1, tb2);*/

            // Raw bits as hex for the full data payload
            /*uint8_t raw[11];
            raw[0] = (fix >> 24) & 0xFF;
            raw[1] = (fix >> 16) & 0xFF;
            raw[2] = (fix >> 8) & 0xFF;
            raw[3] = fix & 0xFF;
            raw[4] = (hop >> 24) & 0xFF;
            raw[5] = (hop >> 16) & 0xFF;
            raw[6] = (hop >> 8) & 0xFF;
            raw[7] = hop & 0xFF;
            raw[8] = (tail >> 14) & 0xFF;
            raw[9] = (tail >> 6) & 0xFF;
            raw[10] = (tail << 2) & 0xFC;*/
            /*FURI_LOG_D(
                TAG,
                "  Raw bytes: %02X %02X %02X %02X | %02X %02X %02X %02X | %02X %02X %02X",
                raw[0],
                raw[1],
                raw[2],
                raw[3],
                raw[4],
                raw[5],
                raw[6],
                raw[7],
                raw[8],
                raw[9],
                raw[10]);*/

            // data_struct interpretations for cipher IV:
            // If data_struct = [fix[0:3] | hop[0:3] | tail[0:2]+pad]:
            //   ds[2:3] = fix[2:3] = raw[2:3]
            /*uint16_t ds_hw_fix = (uint16_t)raw[2] | ((uint16_t)raw[3] << 8); // LE halfword
            uint8_t ds_low6_fix = (ds_hw_fix >> 4) & 0x3F;
            uint8_t ds_byte_fix = ((ds_hw_fix & 0xF) << 4) | ((ds_hw_fix >> 12) & 0xF);
            FURI_LOG_D(
                TAG,
                "  IV(fix[2:3]): hw=%04X ds_low6=%02X ds_byte=%02X",
                ds_hw_fix,
                ds_low6_fix,
                ds_byte_fix);*/

            // If data_struct bytes are reversed fix:
            /*uint16_t ds_hw_fixr = (uint16_t)raw[1] | ((uint16_t)raw[0] << 8);
            uint8_t ds_low6_fixr = (ds_hw_fixr >> 4) & 0x3F;
            uint8_t ds_byte_fixr = ((ds_hw_fixr & 0xF) << 4) | ((ds_hw_fixr >> 12) & 0xF);
            FURI_LOG_D(
                TAG,
                "  IV(fix[1:0]): hw=%04X ds_low6=%02X ds_byte=%02X",
                ds_hw_fixr,
                ds_low6_fixr,
                ds_byte_fixr);*/

            // If ds comes from tail bytes:
            /*uint16_t ds_hw_tail = (uint16_t)tb1 | ((uint16_t)tb2 << 8);
            uint8_t ds_low6_tail = (ds_hw_tail >> 4) & 0x3F;
            uint8_t ds_byte_tail = ((ds_hw_tail & 0xF) << 4) | ((ds_hw_tail >> 12) & 0xF);
            FURI_LOG_D(
                TAG,
                "  IV(tail[1:2]): hw=%04X ds_low6=%02X ds_byte=%02X",
                ds_hw_tail,
                ds_low6_tail,
                ds_byte_tail);*/

            // Hop+Tail together as 54-bit value
            /*uint64_t hop_tail = ((uint64_t)hop << 22) | tail;
            FURI_LOG_D(TAG, "  Hop+Tail (54bit): %014llX", (unsigned long long)hop_tail);*/

            // Dump individual bit fields for manual analysis
            /*FURI_LOG_D(
                TAG,
                "  Hop binary: %d%d %d%d%d%d%d%d%d%d%d%d %d%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d",
                (hop >> 31) & 1,
                (hop >> 30) & 1,
                (hop >> 29) & 1,
                (hop >> 28) & 1,
                (hop >> 27) & 1,
                (hop >> 26) & 1,
                (hop >> 25) & 1,
                (hop >> 24) & 1,
                (hop >> 23) & 1,
                (hop >> 22) & 1,
                (hop >> 21) & 1,
                (hop >> 20) & 1,
                (hop >> 19) & 1,
                (hop >> 18) & 1,
                (hop >> 17) & 1,
                (hop >> 16) & 1,
                (hop >> 15) & 1,
                (hop >> 14) & 1,
                (hop >> 13) & 1,
                (hop >> 12) & 1,
                (hop >> 11) & 1,
                (hop >> 10) & 1,
                (hop >> 9) & 1,
                (hop >> 8) & 1,
                (hop >> 7) & 1,
                (hop >> 6) & 1,
                (hop >> 5) & 1,
                (hop >> 4) & 1,
                (hop >> 3) & 1,
                (hop >> 2) & 1,
                (hop >> 1) & 1,
                (hop >> 0) & 1);*/

            // Dump all bits as string to verify alignment
            /*char bitstr[90];
            for(uint16_t i = 0; i < data_len && i < 86; i++) {
                bitstr[i] = '0' + instance->bits[start + i];
            }
            bitstr[data_len < 86 ? data_len : 86] = '\0';*/
            //FURI_LOG_D(TAG, "  All data bits: %s", bitstr);

            // UID candidates
            /*FURI_LOG_D(
                TAG,
                "  UID candidates: fix=%08lX fix_noBtn=%08lX serial=%07lX fix_bswap=%08lX",
                (unsigned long)fix,
                (unsigned long)(fix & 0xFFFFFFF0),
                (unsigned long)serial,
                (unsigned long)hop_rev);*/
        }

        // Validate: button must be a known V2 button
        bool valid_btn =
            (btn == FIAT_V2_BTN_LOCK || btn == FIAT_V2_BTN_UNLOCK || btn == FIAT_V2_BTN_TRUNK);
        instance->generic.extra_check = valid_btn;
        instance->generic.crc_check = valid_btn && (tail != 0);
        /* Drop frames with an unrecognized button instead of firing the shared
         * callback on a ~90-bit noise burst (V3/V5 already early-return). */
        if(!valid_btn) return;
    } break;

    case FiatVariantV3: {
        if(preamble_len < FIAT_V3_PREAMBLE_MIN || data_len < FIAT_V3_MIN_FRAME_BITS) return;
        if(preamble_len > FIAT_V3_PREAMBLE_MAX) return;
        if(instance->bit_count >= FIAT_V3_MAX_BITS) return;
        if(instance->extra.v3.timing_count > FIAT_V3_MAX_TIMINGS) return;

        if(instance->extra.v3.preamble_gap_count > 0) {
            uint32_t avg_gap =
                instance->extra.v3.preamble_gap_sum / instance->extra.v3.preamble_gap_count;
            if(avg_gap < FIAT_V3_MIN_PREAMBLE_GAP) return;
        }


        if(data_len > FIAT_V3_FRAME_BITS) data_len = FIAT_V3_FRAME_BITS;

        uint32_t fix = 0;
        for(uint16_t i = 0; i < 22 && i < data_len; i++) {
            fix = (fix << 1) | instance->bits[start + i];
        }
        uint8_t btn = 0;
        for(uint16_t i = 22; i < 24 && i < data_len; i++) {
            btn = (btn << 1) | instance->bits[start + i];
        }
        uint8_t status = 0;
        for(uint16_t i = 24; i < 26 && i < data_len; i++) {
            status = (status << 1) | instance->bits[start + i];
        }
        uint64_t hop = 0;
        for(uint16_t i = 26; i < 70 && i < data_len; i++) {
            hop = (hop << 1) | instance->bits[start + i];
        }

        // Require a COMPLETE, structurally-valid frame. Partial frames
        // (data_len < FRAME_BITS) previously skipped the hop stop-bit check, which
        // let other cars' signals (Kia V5, Isuzu, VW) false-match as Fiat. Real
        // Fiat fobs always transmit the full frame, so demand it.
        // hop[43:42] must be 00, hop[0] must be 1 (stop bit)
        if(data_len < FIAT_V3_FRAME_BITS) return;
        if((hop >> 42) != 0) return;
        if((hop & 1) != 1) return;

        instance->extra.v3.fix_value = fix;
        instance->extra.v3.btn = btn;
        instance->extra.v3.status_bits = status;
        instance->extra.v3.hop_value = hop;
        fiat_v3_pack_generic(&instance->generic, fix, btn, hop, &instance->extra.v3.hop_low);

        // Validate hop via linear cryptanalysis:
        // button extracted from hop must match frame button field
        uint8_t hop_btn = fiat_v3_hop_btn_extract(hop);
        if(hop_btn != btn) return;
        /* Require the hop invariants to hold before emitting, and only then
         * mark crc_check — previously a frame with failing invariants still
         * surfaced as a decode with crc_check=ERR. */
        if(!fiat_v3_hop_invariants_check(hop)) return;
        instance->generic.extra_check = true;
        instance->generic.crc_check = true;
    } break;

    case FiatVariantV5: {
        if(preamble_len < FIAT_V5_PREAMBLE_MIN || data_len < FIAT_V5_MIN_FRAME_BITS) return;
        if(data_len > FIAT_V5_FRAME_BITS) data_len = FIAT_V5_FRAME_BITS;

        uint32_t fix = 0;
        for(uint16_t i = 0; i < 32 && i < data_len; i++) {
            fix = (fix << 1) | instance->bits[start + i];
        }
        uint8_t type_byte = 0;
        for(uint16_t i = 32; i < 40 && i < data_len; i++) {
            type_byte = (type_byte << 1) | instance->bits[start + i];
        }
        uint8_t btn = 0;
        for(uint16_t i = 40; i < 48 && i < data_len; i++) {
            btn = (btn << 1) | instance->bits[start + i];
        }
        uint8_t seed = 0;
        for(uint16_t i = 48; i < 56 && i < data_len; i++) {
            seed = (seed << 1) | instance->bits[start + i];
        }
        uint8_t cnt = 0;
        for(uint16_t i = 56; i < 64 && i < data_len; i++) {
            cnt = (cnt << 1) | instance->bits[start + i];
        }
        uint32_t hop = 0;
        for(uint16_t i = 64; i < 96 && i < data_len; i++) {
            hop = (hop << 1) | instance->bits[start + i];
        }

        if(!fiat_v5_is_valid_button(btn)) return;

        // Require a COMPLETE frame (see V3): partial frames skip the hop stop-bit
        // check and let non-Fiat signals false-match. Real Fiat sends the full frame.
        if(data_len < FIAT_V5_FRAME_BITS) return;
        if((hop & 1) != 1) return;

        instance->extra.v5.fix_value = fix;
        instance->extra.v5.type_byte = type_byte;
        instance->extra.v5.btn = btn;
        instance->extra.v5.seed = seed;
        instance->extra.v5.cnt = cnt;
        instance->extra.v5.hop_value = hop;
        fiat_v5_pack_generic(&instance->generic, fix, btn, cnt, hop);
    } break;

    default:
        return;
    }

    // Set decode_data so get_hash_data returns a meaningful hash for deduplication
    instance->decoder.decode_data = instance->generic.data;

    if(instance->base.callback) {
        instance->base.callback(&instance->base, instance->base.context);
    }
}

// ============================================================================
// SHARED DECODE TIMING
// ============================================================================

static void
    fiat_decode_timing(SubGhzProtocolDecoderFiat* instance, bool level, uint32_t duration) {
    const FiatVariantParams* params = fiat_get_params(instance->variant);
    const SubGhzBlockConst* bc = params->block_const;

    uint8_t timing_type;
    bool valid = false;

    if(DURATION_DIFF(duration, bc->te_short) <= bc->te_delta) {
        timing_type = level ? 0 : 2;
        valid = true;
    } else if(DURATION_DIFF(duration, bc->te_long) <= bc->te_delta) {
        timing_type = level ? 4 : 6;
        valid = true;
    }

    if(valid) {
        // V3 preamble validation & timing quality tracking
        if(instance->variant == FiatVariantV3) {
            instance->extra.v3.timing_count++;
            if(!level && !instance->extra.v3.preamble_ended) {
                instance->extra.v3.preamble_gap_sum += duration;
                instance->extra.v3.preamble_gap_count++;
            }
            if(instance->extra.v3.timing_count > FIAT_V3_MAX_TIMINGS) {
                instance->decoder.parser_step = FiatDecoderStepReset;
                instance->bit_count = 0;
                instance->manchester_state = 1;
                instance->extra.v3.timing_count = 0;
                instance->extra.v3.preamble_gap_sum = 0;
                instance->extra.v3.preamble_gap_count = 0;
                instance->extra.v3.preamble_ended = false;
                return;
            }
        }

        uint8_t bit_value = 0;
        if(fiat_manchester_decode(&instance->manchester_state, timing_type, &bit_value)) {
            if(instance->bit_count < params->max_bits) {
                instance->bits[instance->bit_count++] = bit_value;
            }

            if(instance->variant == FiatVariantV3 && bit_value == 0 &&
               !instance->extra.v3.preamble_ended) {
                instance->extra.v3.preamble_ended = true;
            }

            if(instance->bit_count >= params->max_bits) {
                fiat_process_frame(instance);
                instance->decoder.parser_step = FiatDecoderStepReset;
                instance->bit_count = 0;
                instance->manchester_state = 1;
                if(instance->variant == FiatVariantV3) {
                    instance->extra.v3.timing_count = 0;
                    instance->extra.v3.preamble_gap_sum = 0;
                    instance->extra.v3.preamble_gap_count = 0;
                    instance->extra.v3.preamble_ended = false;
                }
            }
        }
    } else {
        if(instance->bit_count >= bc->min_count_bit_for_found) {
            fiat_process_frame(instance);
        }
        instance->decoder.parser_step = FiatDecoderStepReset;
        instance->bit_count = 0;
        instance->manchester_state = 1;
        if(instance->variant == FiatVariantV3) {
            instance->extra.v3.timing_count = 0;
            instance->extra.v3.preamble_gap_sum = 0;
            instance->extra.v3.preamble_gap_count = 0;
            instance->extra.v3.preamble_ended = false;
        }
    }
}

// ============================================================================
// DECODER — ALLOC / FREE / RESET
// ============================================================================

void* subghz_protocol_decoder_fiat_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);

    SubGhzProtocolDecoderFiat* instance = malloc(sizeof(SubGhzProtocolDecoderFiat));
    instance->base.protocol = &subghz_protocol_fiat_v2_v3_v5;
    instance->generic.protocol_name = subghz_protocol_fiat_v2_v3_v5.name;
    instance->variant = FiatVariantV2; // default, detected at runtime
    instance->manchester_state = 1;
    instance->bit_count = 0;
    memset(instance->bits, 0, sizeof(instance->bits));
    memset(&instance->extra, 0, sizeof(instance->extra));

    return instance;
}

void subghz_protocol_decoder_fiat_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderFiat* instance = context;
    free(instance);
}

void subghz_protocol_decoder_fiat_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderFiat* instance = context;

    instance->decoder.parser_step = FiatDecoderStepReset;
    instance->decoder.decode_data = 0;
    instance->decoder.decode_count_bit = 0;
    instance->manchester_state = 1;
    instance->bit_count = 0;
    memset(&instance->extra, 0, sizeof(instance->extra));
}

// ============================================================================
// DECODER — FEED (variant auto-detection from te_short timing)
// ============================================================================

void subghz_protocol_decoder_fiat_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderFiat* instance = context;

    switch(instance->decoder.parser_step) {
    case FiatDecoderStepReset: {
        // Detect variant from timing — best-fit te_short match
        // Manchester encoder always starts with te_short, so this
        // cleanly separates V2(100), V5(200), V3(460)
        int best_idx = -1;
        uint32_t best_diff = UINT32_MAX;

        for(int i = 0; i < FIAT_VARIANT_COUNT; i++) {
            const SubGhzBlockConst* bc = fiat_variant_params[i].block_const;
            uint32_t diff = DURATION_DIFF(duration, bc->te_short);
            if(diff <= bc->te_delta && diff < best_diff) {
                best_diff = diff;
                best_idx = i;
            }
        }

        if(best_idx >= 0) {
            instance->variant = fiat_variant_params[best_idx].variant;
            instance->decoder.parser_step = FiatDecoderStepData;
            instance->manchester_state = 1;
            instance->bit_count = 0;
            memset(&instance->extra, 0, sizeof(instance->extra));
            fiat_decode_timing(instance, level, duration);
        }
    } break;

    case FiatDecoderStepData: {
        const FiatVariantParams* params = fiat_get_params(instance->variant);

        // Frame boundary: gap > min_gap
        if(duration > params->min_gap) {
            if(instance->bit_count >= params->block_const->min_count_bit_for_found) {
                fiat_process_frame(instance);
            }
            instance->decoder.parser_step = FiatDecoderStepReset;
            instance->bit_count = 0;
            instance->manchester_state = 1;
            memset(&instance->extra, 0, sizeof(instance->extra));
            return;
        }

        fiat_decode_timing(instance, level, duration);
    } break;

    default:
        instance->decoder.parser_step = FiatDecoderStepReset;
        break;
    }
}

// ============================================================================
// DECODER — GET HASH DATA
// ============================================================================

uint8_t subghz_protocol_decoder_fiat_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderFiat* instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, sizeof(uint64_t));
}

// ============================================================================
// DECODER — SERIALIZE
// ============================================================================

SubGhzProtocolStatus subghz_protocol_decoder_fiat_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderFiat* dec = context;

    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&dec->generic, flipper_format, preset);
    if(ret != SubGhzProtocolStatusOk) return ret;

    // Write variant number
    uint32_t variant_temp = (uint32_t)dec->variant;
    if(!flipper_format_insert_or_update_uint32(flipper_format, "Variant", &variant_temp, 1)) {
        return SubGhzProtocolStatusError;
    }

    switch(dec->variant) {
    case FiatVariantV2: {
        uint32_t tail_temp = dec->extra.v2.tail;
        if(!flipper_format_insert_or_update_uint32(flipper_format, "Tail", &tail_temp, 1)) {
            return SubGhzProtocolStatusError;
        }
    } break;

    case FiatVariantV3: {
        uint32_t hop_low_temp = dec->extra.v3.hop_low;
        if(!flipper_format_insert_or_update_uint32(flipper_format, "HopLow", &hop_low_temp, 1)) {
            return SubGhzProtocolStatusError;
        }
        uint32_t status_temp = dec->extra.v3.status_bits;
        if(!flipper_format_insert_or_update_uint32(flipper_format, "Status", &status_temp, 1)) {
            return SubGhzProtocolStatusError;
        }
    } break;

    case FiatVariantV5: {
        uint32_t type_temp = dec->extra.v5.type_byte;
        if(!flipper_format_insert_or_update_uint32(flipper_format, "Type", &type_temp, 1)) {
            return SubGhzProtocolStatusError;
        }
        uint32_t seed_temp = dec->extra.v5.seed;
        if(!flipper_format_insert_or_update_uint32(flipper_format, "Seed", &seed_temp, 1)) {
            return SubGhzProtocolStatusError;
        }
        uint32_t btn_temp = dec->extra.v5.btn;
        if(!flipper_format_insert_or_update_uint32(flipper_format, "Btn", &btn_temp, 1)) {
            return SubGhzProtocolStatusError;
        }
        uint32_t cnt_temp = dec->extra.v5.cnt;
        if(!flipper_format_insert_or_update_uint32(flipper_format, "Cnt", &cnt_temp, 1)) {
            return SubGhzProtocolStatusError;
        }
    } break;
    }

    return SubGhzProtocolStatusOk;
}

// ============================================================================
// DECODER — DESERIALIZE
// ============================================================================

SubGhzProtocolStatus
    subghz_protocol_decoder_fiat_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderFiat* dec = context;

    // Read generic data first (Key, Bit)
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize(&dec->generic, flipper_format);
    if(ret != SubGhzProtocolStatusOk) return ret;

    // Read variant — or auto-detect from Bit count
    uint32_t variant_temp = 0;
    if(flipper_format_read_uint32(flipper_format, "Variant", &variant_temp, 1)) {
        dec->variant = (FiatVariant)variant_temp;
    } else {
        switch(dec->generic.data_count_bit) {
        case FIAT_V2_FRAME_BITS:
            dec->variant = FiatVariantV2;
            break;
        case FIAT_V3_FRAME_BITS:
            dec->variant = FiatVariantV3;
            break;
        case FIAT_V5_FRAME_BITS:
            dec->variant = FiatVariantV5;
            break;
        default:
            return SubGhzProtocolStatusErrorValueBitCount;
        }
    }

    const FiatVariantParams* params = fiat_get_params(dec->variant);

    // Validate bit count
    if(dec->generic.data_count_bit != params->frame_bits) {
        return SubGhzProtocolStatusErrorValueBitCount;
    }

    switch(dec->variant) {
    case FiatVariantV2: {
        uint32_t tail_temp = 0;
        if(flipper_format_read_uint32(flipper_format, "Tail", &tail_temp, 1)) {
            dec->extra.v2.tail = tail_temp & 0x3FFFFF;
        } else {
            dec->extra.v2.tail = 0;
        }

        dec->extra.v2.fix_value = (dec->generic.data >> 32) & 0xFFFFFFFF;
        dec->extra.v2.hop_value = dec->generic.data & 0xFFFFFFFF;
        dec->extra.v2.btn = dec->extra.v2.fix_value & 0x0F;
        dec->extra.v2.counter = (dec->extra.v2.hop_value >> 20) & 0x3FF;

        dec->generic.serial = dec->extra.v2.fix_value >> 4;
        dec->generic.btn = dec->extra.v2.btn;
        dec->generic.cnt = dec->extra.v2.counter;

        {
            bool valid_btn = (dec->extra.v2.btn == FIAT_V2_BTN_LOCK ||
                              dec->extra.v2.btn == FIAT_V2_BTN_UNLOCK ||
                              dec->extra.v2.btn == FIAT_V2_BTN_TRUNK);
            dec->generic.extra_check = valid_btn;
            dec->generic.crc_check = valid_btn && (dec->extra.v2.tail != 0);
        }

    } break;

    case FiatVariantV3: {
        uint32_t hop_low_temp = 0;
        flipper_format_read_uint32(flipper_format, "HopLow", &hop_low_temp, 1);
        dec->extra.v3.hop_low = hop_low_temp & 0xF;

        uint32_t status_temp = 0;
        flipper_format_read_uint32(flipper_format, "Status", &status_temp, 1);
        dec->extra.v3.status_bits = status_temp & 0x3;

        uint32_t fix;
        uint8_t btn;
        uint64_t hop;
        fiat_v3_unpack_generic(&dec->generic, dec->extra.v3.hop_low, &fix, &btn, &hop);
        dec->extra.v3.fix_value = fix;
        dec->extra.v3.btn = btn;
        dec->extra.v3.hop_value = hop;

        dec->generic.serial = fix;
        dec->generic.btn = btn;
    } break;

    case FiatVariantV5: {
        uint32_t type_temp = 0;
        flipper_format_read_uint32(flipper_format, "Type", &type_temp, 1);
        dec->extra.v5.type_byte = type_temp & 0xFF;

        uint32_t seed_temp = 0;
        flipper_format_read_uint32(flipper_format, "Seed", &seed_temp, 1);
        dec->extra.v5.seed = seed_temp & 0xFF;

        uint32_t btn_temp = 0;
        if(flipper_format_read_uint32(flipper_format, "Btn", &btn_temp, 1)) {
            dec->extra.v5.btn = btn_temp & 0xFF;
            dec->generic.btn = dec->extra.v5.btn;
        }

        uint32_t cnt_temp = 0;
        if(flipper_format_read_uint32(flipper_format, "Cnt", &cnt_temp, 1)) {
            dec->extra.v5.cnt = cnt_temp & 0xFF;
            dec->generic.cnt = dec->extra.v5.cnt;
        }

        fiat_v5_unpack_generic(&dec->generic, &dec->extra.v5.fix_value, &dec->extra.v5.hop_value);
        dec->generic.serial = dec->extra.v5.fix_value;
    } break;
    }

    subghz_custom_btn_set_original_if_unset(dec->generic.btn);
    subghz_custom_btn_set_max(params->btn_max);

    return SubGhzProtocolStatusOk;
}

// ============================================================================
// DECODER — GET STRING
// ============================================================================

void subghz_protocol_decoder_fiat_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderFiat* dec = context;
    const FiatVariantParams* params = fiat_get_params(dec->variant);

    // Re-extract fields from generic.data
    switch(dec->variant) {
    case FiatVariantV2:
        dec->extra.v2.fix_value = (dec->generic.data >> 32) & 0xFFFFFFFF;
        dec->extra.v2.hop_value = dec->generic.data & 0xFFFFFFFF;
        dec->extra.v2.btn = dec->extra.v2.fix_value & 0x0F;
        dec->extra.v2.counter = (dec->extra.v2.hop_value >> 20) & 0x3FF;
        dec->generic.serial = dec->extra.v2.fix_value >> 4;
        dec->generic.btn = dec->extra.v2.btn;
        dec->generic.cnt = dec->extra.v2.counter;
        {
            bool valid_btn = (dec->extra.v2.btn == FIAT_V2_BTN_LOCK ||
                              dec->extra.v2.btn == FIAT_V2_BTN_UNLOCK ||
                              dec->extra.v2.btn == FIAT_V2_BTN_TRUNK);
            dec->generic.extra_check = valid_btn;
            dec->generic.crc_check = valid_btn && (dec->extra.v2.tail != 0);
        }
        break;
    case FiatVariantV3: {
        uint32_t fix;
        uint8_t btn;
        uint64_t hop;
        fiat_v3_unpack_generic(&dec->generic, dec->extra.v3.hop_low, &fix, &btn, &hop);
        dec->extra.v3.fix_value = fix;
        dec->extra.v3.btn = btn;
        dec->extra.v3.hop_value = hop;
        dec->generic.serial = fix;
        dec->generic.btn = btn;

        uint8_t hop_btn = fiat_v3_hop_btn_extract(hop);
        dec->generic.extra_check = (hop_btn == btn);
        dec->generic.crc_check = fiat_v3_hop_invariants_check(hop);
    } break;
    case FiatVariantV5:
        fiat_v5_unpack_generic(&dec->generic, &dec->extra.v5.fix_value, &dec->extra.v5.hop_value);
        dec->extra.v5.btn = dec->generic.btn;
        dec->extra.v5.cnt = dec->generic.cnt;
        dec->generic.serial = dec->extra.v5.fix_value;
        break;
    }

    // Push UI globals
    subghz_block_generic_global.btn_is_available = true;
    subghz_block_generic_global.btn_length_bit = params->btn_length_bit;
    subghz_block_generic_global.current_btn = dec->generic.btn;
    fiat_current_variant = dec->variant;
    switch(dec->variant) {
    case FiatVariantV2:
        subghz_custom_btn_fill_labels_positional(
            dec->generic.btn, fiat_v2_btns, COUNT_OF(fiat_v2_btns), fiat_get_button_name_simple);
        break;
    case FiatVariantV3:
        subghz_custom_btn_fill_labels_positional(
            dec->generic.btn, fiat_v3_btns, COUNT_OF(fiat_v3_btns), fiat_get_button_name_simple);
        break;
    case FiatVariantV5:
        subghz_custom_btn_fill_labels_positional(
            dec->generic.btn, fiat_v5_btns, COUNT_OF(fiat_v5_btns), fiat_get_button_name_simple);
        break;
    }
    subghz_block_generic_global.cnt_is_available = params->has_counter;
    if(params->has_counter) {
        subghz_block_generic_global.cnt_length_bit = params->cnt_length_bit;
        subghz_block_generic_global.current_cnt = dec->generic.cnt;
    }

    // Display with variant-specific name
    switch(dec->variant) {
    case FiatVariantV2:
        furi_string_cat_printf(
            output,
            "%s %dbit\r\n"
            "Key:%08lX%08lX\r\n"
            "Tail:%06lX\r\n"
            "Sn:%07lX Btn:%02X [%s]\r\n"
            "Cnt:%u\r\n",
            params->display_name,
            dec->generic.data_count_bit,
            dec->extra.v2.fix_value,
            dec->extra.v2.hop_value,
            dec->extra.v2.tail,
            dec->generic.serial,
            dec->generic.btn,
            fiat_get_button_name(dec->variant, dec->generic.btn),
            dec->extra.v2.counter);
        break;

    case FiatVariantV3:
        furi_string_cat_printf(
            output,
            "%s %dbit\r\n"
            "Key:%016llX\r\n"
            "Fix:%06lX Bt:%02X [%s]\r\n"
            "Hop:%011llX\r\n"
            "HpBt:%s Inv:%s\r\n",
            params->display_name,
            dec->generic.data_count_bit,
            dec->generic.data,
            dec->extra.v3.fix_value,
            dec->extra.v3.btn,
            fiat_get_button_name(dec->variant, dec->extra.v3.btn),
            dec->extra.v3.hop_value,
            dec->generic.extra_check ? "OK" : "ERR",
            dec->generic.crc_check ? "OK" : "ERR");
        break;

    case FiatVariantV5:
        furi_string_cat_printf(
            output,
            "%s %dbit\r\n"
            "Key:%016llX\r\n"
            "Fix:%08lX Btn: %02X [%s]\r\n"
            "Hop:%08lX Cnt:%u\r\n"
            "Type:%02X Seed:%02X\r\n",
            params->display_name,
            dec->generic.data_count_bit,
            dec->generic.data,
            dec->extra.v5.fix_value,
            dec->extra.v5.btn,
            fiat_get_button_name(dec->variant, dec->extra.v5.btn),
            dec->extra.v5.hop_value,
            dec->extra.v5.cnt,
            dec->extra.v5.type_byte,
            dec->extra.v5.seed);
        break;
    }
}

// ============================================================================
// MANCHESTER ENCODER (shared, parametrized by te_short/te_long)
// ============================================================================

static size_t
    fiat_encode_bit(LevelDuration* buf, uint8_t* state, uint8_t bit, uint16_t te_s, uint16_t te_l) {
    size_t count = 0;

    switch(*state) {
    case 1:
        if(bit == 0) {
            buf[count++] = level_duration_make(false, (uint32_t)te_l);
            *state = 2;
        } else {
            buf[count++] = level_duration_make(false, (uint32_t)te_s);
            buf[count++] = level_duration_make(true, (uint32_t)te_s);
            *state = 1;
        }
        break;
    case 2:
        if(bit == 0) {
            buf[count++] = level_duration_make(true, (uint32_t)te_s);
            buf[count++] = level_duration_make(false, (uint32_t)te_s);
            *state = 2;
        } else {
            buf[count++] = level_duration_make(true, (uint32_t)te_l);
            *state = 1;
        }
        break;
    case 0:
        if(bit == 1) {
            buf[count++] = level_duration_make(true, (uint32_t)te_s);
            *state = 1;
        } else {
            buf[count++] = level_duration_make(true, (uint32_t)te_s);
            buf[count++] = level_duration_make(false, (uint32_t)te_l);
            *state = 2;
        }
        break;
    case 3:
        if(bit == 0) {
            buf[count++] = level_duration_make(false, (uint32_t)te_s);
            *state = 2;
        } else {
            buf[count++] = level_duration_make(false, (uint32_t)te_s);
            buf[count++] = level_duration_make(true, (uint32_t)te_l);
            *state = 1;
        }
        break;
    default:
        *state = 1;
        break;
    }

    return count;
}

// ============================================================================
// UNIFIED BUILD UPLOAD
// ============================================================================

static void fiat_build_upload(SubGhzProtocolEncoderFiat* enc) {
    LevelDuration* buf = enc->encoder.upload;
    size_t idx = 0;
    uint8_t state = 0;
    const FiatVariantParams* params = fiat_get_params(enc->variant);
    uint16_t te_s = params->block_const->te_short;
    uint16_t te_l = params->block_const->te_long;

    // Preamble
    for(int i = 0; i < params->preamble_bits; i++) {
        idx += fiat_encode_bit(&buf[idx], &state, 1, te_s, te_l);
    }

    switch(enc->variant) {
    case FiatVariantV2: {
        for(int bit = 31; bit >= 0; bit--) {
            idx += fiat_encode_bit(
                &buf[idx], &state, bit_read(enc->extra.v2.fix_value, bit), te_s, te_l);
        }
        for(int bit = 31; bit >= 0; bit--) {
            idx += fiat_encode_bit(
                &buf[idx], &state, bit_read(enc->extra.v2.hop_value, bit), te_s, te_l);
        }
        for(int bit = 21; bit >= 0; bit--) {
            idx +=
                fiat_encode_bit(&buf[idx], &state, bit_read(enc->extra.v2.tail, bit), te_s, te_l);
        }
        buf[idx++] = level_duration_make(true, 1200);
    } break;

    case FiatVariantV3: {
        for(int bit = 21; bit >= 0; bit--) {
            idx += fiat_encode_bit(
                &buf[idx], &state, bit_read(enc->extra.v3.fix_value, bit), te_s, te_l);
        }
        for(int bit = 1; bit >= 0; bit--) {
            idx +=
                fiat_encode_bit(&buf[idx], &state, bit_read(enc->extra.v3.btn, bit), te_s, te_l);
        }
        for(int bit = 1; bit >= 0; bit--) {
            idx += fiat_encode_bit(
                &buf[idx], &state, bit_read(enc->extra.v3.status_bits, bit), te_s, te_l);
        }
        for(int bit = 43; bit >= 0; bit--) {
            uint8_t b = (enc->extra.v3.hop_value >> bit) & 1;
            idx += fiat_encode_bit(&buf[idx], &state, b, te_s, te_l);
        }
        buf[idx++] = level_duration_make(true, FIAT_V3_MIN_GAP);
    } break;

    case FiatVariantV5: {
        for(int bit = 31; bit >= 0; bit--) {
            idx += fiat_encode_bit(
                &buf[idx], &state, bit_read(enc->extra.v5.fix_value, bit), te_s, te_l);
        }
        for(int bit = 7; bit >= 0; bit--) {
            idx += fiat_encode_bit(
                &buf[idx], &state, bit_read(enc->extra.v5.type_byte, bit), te_s, te_l);
        }
        for(int bit = 7; bit >= 0; bit--) {
            idx +=
                fiat_encode_bit(&buf[idx], &state, bit_read(enc->extra.v5.btn, bit), te_s, te_l);
        }
        for(int bit = 7; bit >= 0; bit--) {
            idx +=
                fiat_encode_bit(&buf[idx], &state, bit_read(enc->extra.v5.seed, bit), te_s, te_l);
        }
        for(int bit = 7; bit >= 0; bit--) {
            idx +=
                fiat_encode_bit(&buf[idx], &state, bit_read(enc->extra.v5.cnt, bit), te_s, te_l);
        }
        for(int bit = 31; bit >= 0; bit--) {
            idx += fiat_encode_bit(
                &buf[idx], &state, bit_read(enc->extra.v5.hop_value, bit), te_s, te_l);
        }
        idx += fiat_encode_bit(&buf[idx], &state, 0, te_s, te_l);
        buf[idx++] = level_duration_make(true, FIAT_V5_MIN_GAP);
    } break;
    }

    enc->encoder.size_upload = idx;
}

// ============================================================================
// ENCODER — ALLOC / FREE / STOP / YIELD
// ============================================================================

void* subghz_protocol_encoder_fiat_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);

    SubGhzProtocolEncoderFiat* instance = malloc(sizeof(SubGhzProtocolEncoderFiat));
    instance->base.protocol = &subghz_protocol_fiat_v2_v3_v5;
    instance->generic.protocol_name = subghz_protocol_fiat_v2_v3_v5.name;
    instance->variant = FiatVariantV2; // default, set during deserialize
    instance->encoder.repeat = 10;
    instance->encoder.size_upload = 768;
    instance->encoder.upload = malloc(instance->encoder.size_upload * sizeof(LevelDuration));
    instance->encoder.is_running = false;
    memset(&instance->extra, 0, sizeof(instance->extra));

    return instance;
}

void subghz_protocol_encoder_fiat_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderFiat* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

void subghz_protocol_encoder_fiat_stop(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderFiat* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_fiat_yield(void* context) {
    SubGhzProtocolEncoderFiat* instance = context;

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

// ============================================================================
// ENCODER — DESERIALIZE
// ============================================================================

SubGhzProtocolStatus
    subghz_protocol_encoder_fiat_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderFiat* enc = context;

    enc->encoder.is_running = false;
    enc->encoder.front = 0;

    SubGhzProtocolStatus res = SubGhzProtocolStatusError;

    do {
        if(SubGhzProtocolStatusOk !=
           subghz_block_generic_deserialize(&enc->generic, flipper_format)) {
            FURI_LOG_E(TAG, "DE"); // Deserialize error
            break;
        }

        // Read variant — or auto-detect from Bit count
        uint32_t variant_temp = 0;
        if(flipper_format_read_uint32(flipper_format, "Variant", &variant_temp, 1)) {
            enc->variant = (FiatVariant)variant_temp;
        } else {
            switch(enc->generic.data_count_bit) {
            case FIAT_V2_FRAME_BITS:
                enc->variant = FiatVariantV2;
                break;
            case FIAT_V3_FRAME_BITS:
                enc->variant = FiatVariantV3;
                break;
            case FIAT_V5_FRAME_BITS:
                enc->variant = FiatVariantV5;
                break;
            default:
                FURI_LOG_E(
                    TAG, "UVFBC %d", enc->generic.data_count_bit); // Unknown variant for bit count
                break;
            }
        }

        // Optional Repeat
        flipper_format_read_uint32(flipper_format, "Repeat", (uint32_t*)&enc->encoder.repeat, 1);

        // flipper_format_read_uint32 is forward-only and the optional reads above
        // (Variant/Repeat) leave the stream cursor at EOF when a key is absent, which
        // would make every variant-specific read below silently fail (dropping HopLow/
        // Status/Type/Seed/Btn/Cnt/Tail and corrupting the emitted frame). Rewind so the
        // per-variant fields are found from the top.
        flipper_format_rewind(flipper_format);

        switch(enc->variant) {
        case FiatVariantV2: {
            enc->extra.v2.fix_value = (enc->generic.data >> 32) & 0xFFFFFFFF;
            enc->extra.v2.hop_value = enc->generic.data & 0xFFFFFFFF;

            uint32_t tail_temp = 0;
            if(flipper_format_read_uint32(flipper_format, "Tail", &tail_temp, 1)) {
                enc->extra.v2.tail = tail_temp & 0x3FFFFF;
            } else {
                enc->extra.v2.tail = 0;
            }

            enc->extra.v2.btn = enc->extra.v2.fix_value & 0x0F;
            enc->extra.v2.counter = (enc->extra.v2.hop_value >> 20) & 0x3FF;

            // Button handling
            subghz_custom_btn_set_original_if_unset(enc->extra.v2.btn);
            subghz_custom_btn_set_max(3);

            {
                uint8_t btn = enc->extra.v2.btn;
                if(!subghz_block_generic_global_button_override_get(&btn)) {
                    uint8_t original = subghz_custom_btn_get_original();
                    btn = fiat_get_btn_code(enc->variant, original);
                }
                enc->extra.v2.btn = btn;
                // Update fix with new button (bottom nibble)
                enc->extra.v2.fix_value =
                    (enc->extra.v2.fix_value & 0xFFFFFFF0) | (btn & 0x0F);
            }

            // Counter override (10-bit)
            {
                uint32_t cnt32 = enc->extra.v2.counter;
                if(furi_hal_subghz_get_rolling_counter_mult() != -0x7FFFFFFF) {
                    if(!subghz_block_generic_global_counter_override_get(&cnt32)) {
                        if((cnt32 + furi_hal_subghz_get_rolling_counter_mult()) > 0x3FF) {
                            cnt32 = 0;
                        } else {
                            cnt32 += furi_hal_subghz_get_rolling_counter_mult();
                        }
                    }
                } else {
                    cnt32 = (cnt32 + 1) & 0x3FF;
                }
                enc->extra.v2.counter = cnt32 & 0x3FF;
            }

            // Compute hop via custom Hitag2 cipher (master key, UID from fix)
            enc->extra.v2.hop_value = fiat_v2_compute_hop(
                enc->extra.v2.counter, enc->extra.v2.fix_value);

            enc->generic.data = ((uint64_t)enc->extra.v2.fix_value << 32) |
                                enc->extra.v2.hop_value;
            enc->generic.serial = enc->extra.v2.fix_value >> 4;
            enc->generic.btn = enc->extra.v2.btn;
            enc->generic.cnt = enc->extra.v2.counter;
            enc->generic.data_count_bit = FIAT_V2_FRAME_BITS;
        } break;

        case FiatVariantV3: {
            uint32_t hop_low_temp = 0;
            flipper_format_read_uint32(flipper_format, "HopLow", &hop_low_temp, 1);
            enc->extra.v3.hop_low = hop_low_temp & 0xF;

            uint32_t status_temp = 0;
            flipper_format_read_uint32(flipper_format, "Status", &status_temp, 1);
            enc->extra.v3.status_bits = status_temp & 0x3;

            enc->extra.v3.fix_value = (enc->generic.data >> 42) & 0x3FFFFF;
            enc->extra.v3.btn = (enc->generic.data >> 40) & 0x3;
            enc->extra.v3.hop_value = ((enc->generic.data & 0xFFFFFFFFFFULL) << 4) |
                                      (enc->extra.v3.hop_low & 0xF);

            // Button handling
            subghz_custom_btn_set_original_if_unset(enc->extra.v3.btn);
            subghz_custom_btn_set_max(3);

            {
                uint8_t btn = enc->extra.v3.btn;
                if(!subghz_block_generic_global_button_override_get(&btn)) {
                    uint8_t original = subghz_custom_btn_get_original();
                    btn = fiat_get_btn_code(enc->variant, original);
                }
                enc->extra.v3.btn = btn;
            }

            fiat_v3_pack_generic(
                &enc->generic,
                enc->extra.v3.fix_value,
                enc->extra.v3.btn,
                enc->extra.v3.hop_value,
                &enc->extra.v3.hop_low);

            enc->generic.serial = enc->extra.v3.fix_value;
            enc->generic.btn = enc->extra.v3.btn;
            enc->generic.cnt = 0;
            enc->generic.data_count_bit = FIAT_V3_FRAME_BITS;
        } break;

        case FiatVariantV5: {
            uint32_t type_temp = 0;
            flipper_format_read_uint32(flipper_format, "Type", &type_temp, 1);
            enc->extra.v5.type_byte = type_temp & 0xFF;

            uint32_t seed_temp = 0;
            flipper_format_read_uint32(flipper_format, "Seed", &seed_temp, 1);
            enc->extra.v5.seed = seed_temp & 0xFF;

            fiat_v5_unpack_generic(
                &enc->generic, &enc->extra.v5.fix_value, &enc->extra.v5.hop_value);

            {
                uint32_t btn_temp = 0;
                if(flipper_format_read_uint32(flipper_format, "Btn", &btn_temp, 1)) {
                    enc->extra.v5.btn = btn_temp & 0xFF;
                } else {
                    enc->extra.v5.btn = 0;
                }

                uint32_t cnt_temp = 0;
                if(flipper_format_read_uint32(flipper_format, "Cnt", &cnt_temp, 1)) {
                    enc->extra.v5.cnt = cnt_temp & 0xFF;
                } else {
                    enc->extra.v5.cnt = 0;
                }
            }

            // Button handling
            subghz_custom_btn_set_original_if_unset(enc->extra.v5.btn);
            subghz_custom_btn_set_max(3);

            {
                uint8_t btn = enc->extra.v5.btn;
                if(!subghz_block_generic_global_button_override_get(&btn)) {
                    uint8_t original = subghz_custom_btn_get_original();
                    btn = fiat_get_btn_code(enc->variant, original);
                }
                enc->extra.v5.btn = btn;
            }

            // Counter override (8-bit)
            {
                uint32_t cnt32 = enc->extra.v5.cnt;
                if(furi_hal_subghz_get_rolling_counter_mult() != -0x7FFFFFFF) {
                    if(!subghz_block_generic_global_counter_override_get(&cnt32)) {
                        if((cnt32 + furi_hal_subghz_get_rolling_counter_mult()) > 0xFF) {
                            cnt32 = 0;
                        } else {
                            cnt32 += furi_hal_subghz_get_rolling_counter_mult();
                        }
                    }
                } else {
                    cnt32 = (cnt32 + 1) & 0xFF;
                }
                enc->extra.v5.cnt = cnt32 & 0xFF;
            }

            fiat_v5_pack_generic(
                &enc->generic,
                enc->extra.v5.fix_value,
                enc->extra.v5.btn,
                enc->extra.v5.cnt,
                enc->extra.v5.hop_value);
        } break;
        }

        fiat_build_upload(enc);

        // Write updated Key back
        if(!flipper_format_rewind(flipper_format)) {
            FURI_LOG_E(TAG, "RE"); // Rewind error
            break;
        }

        uint8_t key_data[sizeof(uint64_t)] = {0};
        for(size_t i = 0; i < sizeof(uint64_t); i++) {
            key_data[sizeof(uint64_t) - i - 1] = (enc->generic.data >> (i * 8)) & 0xFF;
        }
        if(!flipper_format_update_hex(flipper_format, "Key", key_data, sizeof(uint64_t))) {
            FURI_LOG_E(TAG, "UUK"); // Unable to update Key
            break;
        }

        enc->encoder.is_running = true;
        res = SubGhzProtocolStatusOk;

    } while(false);

    return res;
}
