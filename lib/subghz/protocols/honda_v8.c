/*
 * honda_v8.c — Honda KeeLoq Inverted PWM Protocol
 * Source: Pandora firmware — Crypto_Scramble_Finalize_Keeloq @ 0x9C00
 *
 * KeeLoq decryption: key_bit = (15 - round) & 0x3F  (NOT round & 0x3F)
 * Firmware uses RSB R0, R5, #0xF.
 *
 * Bit 1 produces a short HIGH (inverted PWM).
 * Verified against Lock/Unlock/Hondaaa.sub.
 */

#include "honda_v8.h"

#include <furi.h>
#include <furi_hal.h>
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h>
#include <lib/subghz/blocks/custom_btn_i.h>

#define TAG "HondaV8"

/* ══════════════════════════════════════════════════════════════════
 * MANUFACTURER KEY TABLE
 * Source: Pandora firmware @ 0x14D60 (20×8 bytes)
 * Test order optimized: [14,1,0] are the most common
 * ══════════════════════════════════════════════════════════════════ */

const HondaV8MfrKey honda_v8_mfr_keys[HV8_MFR_KEY_COUNT] = {
    {0x8DAA5CDB, 0xA8F5DFFC},  /*  0 main firmware @ 0x9EB4               */
    {0x10C1F940, 0xABBA94D4},  /*  1 ROM[0]  Hondaaa.sub ✓                */
    {0x460114B7, 0xDCF40FCD},  /*  2 ROM[1]                               */
    {0xE9B977B4, 0x56EB02D8},  /*  3 ROM[2]                               */
    {0x3F26FA21, 0xA8507BA5},  /*  4 ROM[3]                               */
    {0xA4C5C542, 0x6857C1C5},  /*  5 ROM[4]                               */
    {0xEBBB753E, 0x54E900DA},  /*  6 ROM[5]                               */
    {0x6A67C523, 0x5683FC9B},  /*  7 ROM[6]                               */
    {0xECBC7263, 0x53EE07DD},  /*  8 ROM[7]                               */
    {0xE1CA6024, 0x5D973077},  /*  9 ROM[8]                               */
    {0xEDBD737C, 0x52EF06DC},  /* 10 ROM[9]                               */
    {0x6011AC25, 0x36714027},  /* 11 ROM[10]                              */
    {0xEEBE7059, 0x51EC05DF},  /* 12 ROM[11]                              */
    {0x9BB87A26, 0x1F4DAA96},  /* 13 ROM[12]                              */
    {0xEFBF713C, 0x50ED04DE},  /* 14 ROM[13] Lock/Unlock ✓                */
    {0xF39FD627, 0xDC8DC8EA},  /* 15 ROM[14]                              */
    {0x610A14EB, 0x30850A82},  /* 16 ROM[15]                              */
    {0x1719FF41, 0xD5204919},  /* 17 ROM[16]                              */
    {0x779F14DA, 0x558EA3B0},  /* 18 ROM[17]                              */
    {0x47454DE2, 0x54454441},  /* 19 ROM[18]                              */
    {0x0514F348, 0x40802014},  /* 20 ROM[19]                              */
};

static const uint8_t kl_key_probe_order[HV8_MFR_KEY_COUNT] = {
    14, 1, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 17, 18, 19, 20
};

static const char* hv8_btn_name(uint8_t btn) {
    switch(btn) {
    case HV8_BTN_PANIC:        return "Panic";
    case HV8_BTN_LOCK:         return "Lock";
    case HV8_BTN_TRUNK:        return "Trunk/Hatch";
    case HV8_BTN_REMOTE_START: return "Remote Start";
    case HV8_BTN_UNLOCK:       return "Unlock";
    case HV8_BTN_UNLOCK2:      return "Unlock x2";
    default:                   return "Unknown";
    }
}

static uint8_t hv8_btn_to_custom(uint8_t btn) {
    switch(btn) {
    case HV8_BTN_LOCK:         return 1;
    case HV8_BTN_UNLOCK:       return 2;
    case HV8_BTN_TRUNK:        return 3;
    case HV8_BTN_PANIC:        return 4;
    case HV8_BTN_REMOTE_START: return 5;
    default:                   return 1;
    }
}

static uint8_t hv8_custom_to_btn(uint8_t custom) {
    switch(custom) {
    case 1: return HV8_BTN_LOCK;
    case 2: return HV8_BTN_UNLOCK;
    case 3: return HV8_BTN_TRUNK;
    case 4: return HV8_BTN_PANIC;
    case 5: return HV8_BTN_REMOTE_START;
    default: return HV8_BTN_LOCK;
    }
}

static void hv8_register_btn(uint8_t btn) {
    uint8_t custom = hv8_btn_to_custom(btn);
    if(subghz_custom_btn_get_original() == 0)
        subghz_custom_btn_set_original(custom);
    subghz_custom_btn_set_max(HV8_BTN_CUSTOM_MAX);
}

static uint8_t hv8_get_active_btn(void) {
    uint8_t ac = subghz_custom_btn_get();
    if(ac == SUBGHZ_CUSTOM_BTN_OK)
        return hv8_custom_to_btn(subghz_custom_btn_get_original());
    return hv8_custom_to_btn(ac);
}

static uint64_t hv8_pack(
    uint32_t serial, uint8_t btn,
    uint16_t rolling, uint8_t discr_mode,
    uint8_t key_idx,  uint8_t proto_mode, uint8_t flags)
{
    return ((uint64_t)(serial      & 0x0FFFFFFFu) << 36) |
           ((uint64_t)(btn         & 0x0Fu)        << 32) |
           ((uint64_t)rolling                      << 16) |
           ((uint64_t)discr_mode                   <<  8) |
           ((uint64_t)(key_idx     & 0x0Fu)        <<  4) |
           ((uint64_t)(proto_mode  & 0x03u)        <<  2) |
           ((uint64_t)(flags       & 0x03u));
}

static void hv8_unpack(
    uint64_t raw,
    uint32_t* serial, uint8_t* btn,
    uint16_t* rolling, uint8_t* discr_mode,
    uint8_t* key_idx,  uint8_t* proto_mode, uint8_t* flags)
{
    *serial     = (uint32_t)((raw >> 36) & 0x0FFFFFFFu);
    *btn        = (uint8_t) ((raw >> 32) & 0x0Fu);
    *rolling    = (uint16_t)((raw >> 16) & 0xFFFFu);
    *discr_mode = (uint8_t) ((raw >>  8) & 0xFFu);
    *key_idx    = (uint8_t) ((raw >>  4) & 0x0Fu);
    *proto_mode = (uint8_t) ((raw >>  2) & 0x03u);
    *flags      = (uint8_t) ((raw      ) & 0x03u);
}

/* ══════════════════════════════════════════════════════════════════
 * KEELOQ ENGINE
 * Source: Crypto.LFSR.Rolling_Code @0x9EB8
 * NLF: 0x3A5C742E
 * Rounds: 528
 * Taps: bits 1, 9, 20, 26, 31
 * ══════════════════════════════════════════════════════════════════ */

static inline uint32_t kl_nlf(uint32_t x) {
    uint32_t idx =
        (((x >>  1) & 1u)      ) |
        (((x >>  9) & 1u) << 1u) |
        (((x >> 20) & 1u) << 2u) |
        (((x >> 26) & 1u) << 3u) |
        (((x >> 31) & 1u) << 4u);
    return (HV8_NLF >> idx) & 1u;
}

static uint32_t kl_decrypt(uint32_t ciphertext, uint64_t key) {
    uint32_t x = ciphertext;
    for(int i = 0; i < HV8_ROUNDS; i++) {
        uint32_t kb  = (uint32_t)((key >> ((15 - i) & 0x3F)) & 1u);
        uint32_t nlf = kl_nlf(x);
        uint32_t msb = x & 1u;
        uint32_t b15 = (x >> 15) & 1u;
        uint32_t fb  = msb ^ nlf ^ b15 ^ kb;
        x = (x >> 1) | (fb << 31);
    }
    return x;
}

/* True inverse of kl_decrypt: kl_decrypt() runs 528 right-shift rounds
 * R_0..R_527, so the inverse applies the inverted rounds R_527..R_0 in
 * reverse. Each decrypt round is s' = (s>>1) | (fb<<31) with
 * fb = (s&1) ^ nlf(s) ^ ((s>>15)&1) ^ kb. Inverting: bits 31..1 of s come
 * from bits 30..0 of s'; the NLF taps (1,9,20,26,31) never read bit0, so
 * nlf(s) and s[15] are recoverable from (s'<<1), and bit0 is solved as
 * s0 = fb ^ nlf ^ s15 ^ kb. This guarantees kl_decrypt(kl_encrypt(p)) == p
 * so an emulated frame re-decodes to the same plaintext (a plain forward
 * KeeLoq round is NOT the inverse of this decryptor). */
static uint32_t kl_encrypt(uint32_t plaintext, uint64_t key) {
    uint32_t x = plaintext;
    for(int i = HV8_ROUNDS - 1; i >= 0; i--) {
        uint32_t kb  = (uint32_t)((key >> ((15 - i) & 0x3F)) & 1u);
        uint32_t tmp = (x << 1);
        uint32_t nlf = kl_nlf(tmp);
        uint32_t s15 = (tmp >> 15) & 1u;
        uint32_t fb  = (x >> 31) & 1u;
        uint32_t s0  = fb ^ nlf ^ s15 ^ kb;
        x = tmp | s0;
    }
    return x;
}

static bool kl_btn_is_known(uint8_t btn) {
    switch(btn) {
    case HV8_BTN_PANIC:
    case HV8_BTN_LOCK:
    case HV8_BTN_TRUNK:
    case HV8_BTN_REMOTE_START:
    case HV8_BTN_UNLOCK:
    case HV8_BTN_UNLOCK2:
        return true;
    default:
        return false;
    }
}

static int8_t kl_find_key(
    uint32_t  enc,
    uint32_t  serial,
    uint8_t   btn,
    uint32_t* out_dec,
    uint16_t* out_rc)
{
    /* Honda KeeLoq requires plaintext discriminant = (serial >> 20) & 0xFF
     * (see encoder kl_build_frame). Reject decryptions that don't pin to
     * the observed serial — removes ~99% of random-noise false positives. */
    uint8_t expected_disc = (uint8_t)((serial >> 20) & 0xFFu);

    for(int probe = 0; probe < HV8_MFR_KEY_COUNT; probe++) {
        uint8_t i = kl_key_probe_order[probe];
        uint64_t k = ((uint64_t)honda_v8_mfr_keys[i].high << 32) |
                      (uint64_t)honda_v8_mfr_keys[i].low;
        uint32_t dec = kl_decrypt(enc, k);

        if((dec & HV8_DEC_FUNC_MASK) != (uint32_t)btn) continue;

        uint8_t  disc = (uint8_t) ((dec & HV8_DEC_DISC_MASK)
                                        >> HV8_DEC_DISC_SHIFT);
        if(disc != expected_disc) continue;

        uint16_t rc   = (uint16_t)((dec & HV8_DEC_RC_MASK)
                                        >> HV8_DEC_RC_SHIFT);
        if(rc == 0 && disc == 0) continue;

        *out_dec = dec;
        *out_rc  = rc;
        return (int8_t)i;
    }
    return -1;
}

static void kl_build_frame(
    bool     bits[HV8_FRAME_BITS],
    uint64_t key,
    uint32_t serial,
    uint8_t  btn,
    uint8_t  status,
    uint16_t rc)
{
    uint8_t  discr = (uint8_t)((serial >> 20) & 0xFFu);
    uint32_t plain = ((uint32_t)(btn & 0xFu))    |
                     ((uint32_t)discr      << 4)  |
                     ((uint32_t)rc         << 16);
    uint32_t enc = kl_encrypt(plain, key);

    /* The decoder shifts the 64 data bits in (bit0 = first on-air) then
     * bit-reverses them into `raw`, extracting enc=raw[0:31],
     * serial=raw[32:59], btn=raw[60:63]. To reproduce that exact frame the
     * air order must be MSB-first over the natural 64-bit value
     *   raw = (btn << 60) | (serial << 32) | enc
     * i.e. bits[0] carries raw bit63. (A LSB-first fill would make the
     * decoder recover byte-reversed, scrambled fields.) */
    uint64_t raw = ((uint64_t)(btn    & 0x0Fu)      << 60) |
                   ((uint64_t)(serial & 0x0FFFFFFFu) << 32) |
                   (uint64_t)enc;

    for(int i = 0; i < 64; i++) bits[i]    = (bool)((raw >> (63 - i)) & 1u);
    for(int i = 0; i < 2;  i++) bits[64+i] = (bool)((status >> i) & 1u);
}

const SubGhzProtocolDecoder subghz_protocol_honda_v8_decoder;
const SubGhzProtocolEncoder subghz_protocol_honda_v8_encoder;
const SubGhzProtocol        subghz_protocol_honda_v8;

static void kl_process_frame(SubGhzProtocolDecoderHondaV8* inst) {
    uint64_t raw = 0;
    uint64_t tmp = inst->decoder.decode_data;
    for(int i = 0; i < 64; i++) {
        raw = (raw << 1) | (tmp & 1u);
        tmp >>= 1;
    }

    uint32_t enc    = (uint32_t)(raw & 0xFFFFFFFFu);
    uint32_t serial = (uint32_t)((raw >> 32) & 0x0FFFFFFFu);
    uint8_t  btn    = (uint8_t) ((raw >> 60) & 0x0Fu);

    if(!kl_btn_is_known(btn)) {
        FURI_LOG_D(TAG, "[KL] unknown btn=0x%X", btn);
        return;
    }
    if(serial == 0u || serial == 0x0FFFFFFFu) {
        FURI_LOG_D(TAG, "[KL] trivial serial=%07lX", (unsigned long)serial);
        return;
    }

    uint32_t dec = 0;
    uint16_t rc  = 0;
    int8_t   ki  = kl_find_key(enc, serial, btn, &dec, &rc);

    if(ki < 0) {
        FURI_LOG_D(TAG, "[KL] no key enc=%08lX btn=%u ser=%07lX",
                   (unsigned long)enc, btn, (unsigned long)serial);
        return;
    }

    if(inst->has_last_rolling) {
        uint16_t diff = (uint16_t)(rc - inst->last_rolling);
        if(diff == 0 || diff > HV8_ROLLING_WINDOW) {
            FURI_LOG_D(TAG, "[KL] RC out window rc=%u last=%u",
                       rc, inst->last_rolling);
            return;
        }
    }

    uint8_t discr = (uint8_t)((dec & HV8_DEC_DISC_MASK)
                                   >> HV8_DEC_DISC_SHIFT);

    inst->frame.encrypted    = enc;
    inst->frame.serial       = serial;
    inst->frame.button       = btn;
    inst->frame.status       = 0u;
    inst->frame.decrypted    = dec;
    inst->frame.rolling_code = rc;
    inst->frame.discriminant = discr;
    inst->frame.key_idx      = ki;
    inst->frame_valid        = true;
    inst->last_rolling       = rc;
    inst->has_last_rolling   = true;

    inst->generic.data = hv8_pack(
        serial, btn, rc, discr,
        (uint8_t)ki, HV8_PROTO_MODE, 0u);
    inst->generic.data_count_bit = HV8_FRAME_BITS;
    inst->generic.serial = serial;
    inst->generic.btn    = btn;
    inst->generic.cnt    = rc;

    hv8_register_btn(btn);

    FURI_LOG_I(TAG,
        "[KL] VALID key[%d] ser=%07lX btn=%u rc=%04X enc=%08lX dec=%08lX",
        ki, (unsigned long)serial, btn, rc,
        (unsigned long)enc, (unsigned long)dec);

    if(inst->base.callback)
        inst->base.callback(&inst->base, inst->base.context);
}

void* subghz_protocol_decoder_honda_v8_alloc(SubGhzEnvironment* env) {
    UNUSED(env);
    SubGhzProtocolDecoderHondaV8* inst =
        malloc(sizeof(SubGhzProtocolDecoderHondaV8));
    furi_check(inst);
    memset(inst, 0, sizeof(SubGhzProtocolDecoderHondaV8));
    inst->base.protocol         = &subghz_protocol_honda_v8;
    inst->generic.protocol_name = inst->base.protocol->name;
    inst->result_str            = furi_string_alloc();
    inst->frame.key_idx         = -1;
    inst->has_last_rolling      = false;
    inst->frame_valid           = false;
    FURI_LOG_I(TAG, "[KL] decoder alloc");
    return inst;
}

void subghz_protocol_decoder_honda_v8_free(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV8* inst = ctx;
    furi_string_free(inst->result_str);
    free(inst);
}

void subghz_protocol_decoder_honda_v8_reset(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV8* inst = ctx;
    inst->decoder.parser_step      = 0;
    inst->decoder.decode_data      = 0;
    inst->decoder.decode_count_bit = 0;
}

/*
 * State 0 (RESET): wait for LOW >= HV8_SYNC_MIN_US
 * State 1 (DATA):
 *   LOW >= SYNC_MIN  → inter-repeat sync gap
 *   LOW in [250..550]→ bit 0 (LOW 400µs ± delta)
 *   LOW < 250        → bit 1 (PWM inverted, LOW cut off)
 *   Bit 64 reached   → process frame
 *
 * Shift register accumulates MSB-first. kl_process_frame() reverts
 * to LSB-first to extract fields.
 */
void subghz_protocol_decoder_honda_v8_feed(
    void* ctx, bool level, uint32_t duration)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV8* inst = ctx;

    switch(inst->decoder.parser_step) {

    case 0:
        if(!level && duration >= HV8_SYNC_MIN_US) {
            inst->decoder.decode_data      = 0;
            inst->decoder.decode_count_bit = 0;
            inst->decoder.parser_step      = 1;
            FURI_LOG_D(TAG, "[KL] sync dur=%lu", (unsigned long)duration);
        }
        break;

    case 1:
        if(!level) {
            if(duration >= HV8_SYNC_MIN_US) {
                if(inst->decoder.decode_count_bit == HV8_FRAME_BITS) {
                    kl_process_frame(inst);
                } else if(inst->decoder.decode_count_bit > 60u) {
                    kl_process_frame(inst);
                }
                inst->decoder.decode_data      = 0;
                inst->decoder.decode_count_bit = 0;
                break;
            }

            uint8_t bit_val;
            if(duration + HV8_TE_DELTA >= HV8_TE_BASE &&
               duration <= HV8_TE_BASE + HV8_TE_DELTA) {
                bit_val = 0u;
            } else {
                bit_val = 1u;
            }

            if(inst->decoder.decode_count_bit < 64u) {
                inst->decoder.decode_data >>= 1;
                if(bit_val)
                    inst->decoder.decode_data |= (1ULL << 63);
            }
            inst->decoder.decode_count_bit++;

            if(inst->decoder.decode_count_bit >= HV8_FRAME_BITS) {
                kl_process_frame(inst);
                inst->decoder.decode_data      = 0;
                inst->decoder.decode_count_bit = 0;
                inst->decoder.parser_step      = 0;
            }
        }

        break;

    default:
        inst->decoder.parser_step = 0;
        break;
    }

    inst->decoder.te_last = duration;
}

uint8_t subghz_protocol_decoder_honda_v8_get_hash_data(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV8* inst = ctx;
    return (uint8_t)(
         inst->frame.serial ^
        (inst->frame.serial >>  8) ^
        (inst->frame.serial >> 16) ^
         inst->frame.button ^
        (uint8_t)(inst->frame.key_idx >= 0 ? inst->frame.key_idx : 0));
}

SubGhzProtocolStatus subghz_protocol_decoder_honda_v8_serialize(
    void* ctx, FlipperFormat* ff, SubGhzRadioPreset* preset)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV8* inst = ctx;
    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&inst->generic, ff, preset);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint32_t v = inst->frame.serial;
    if(!flipper_format_write_uint32(ff, "Serial", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    v = inst->frame.rolling_code;
    if(!flipper_format_write_uint32(ff, "RollingCode", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    v = inst->frame.button;
    if(!flipper_format_write_uint32(ff, "Button", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    v = (inst->frame.key_idx >= 0) ?
        (uint32_t)(uint8_t)inst->frame.key_idx : 0u;
    if(!flipper_format_write_uint32(ff, "KeyIdx", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus subghz_protocol_decoder_honda_v8_deserialize(
    void* ctx, FlipperFormat* ff)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV8* inst = ctx;
    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize_check_count_bit(
            &inst->generic, ff, HV8_FRAME_BITS);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint32_t v = 0;
    if(!flipper_format_read_uint32(ff, "Serial", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.serial = v;

    if(!flipper_format_read_uint32(ff, "RollingCode", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.rolling_code = (uint16_t)v;

    if(!flipper_format_read_uint32(ff, "Button", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.button = (uint8_t)v;

    if(!flipper_format_read_uint32(ff, "KeyIdx", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.key_idx = (int8_t)v;

    inst->frame.discriminant  = (uint8_t)((inst->frame.serial >> 20) & 0xFFu);
    inst->frame_valid         = true;
    inst->has_last_rolling    = true;
    inst->last_rolling        = inst->frame.rolling_code;
    inst->generic.serial      = inst->frame.serial;
    inst->generic.btn         = inst->frame.button;
    inst->generic.cnt         = inst->frame.rolling_code;
    hv8_register_btn(inst->frame.button);
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_honda_v8_get_string(
    void* ctx, FuriString* out)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV8* inst = ctx;

    if(!inst->frame_valid && inst->generic.data != 0) {
        uint8_t pm, ki, fl, dm;
        uint16_t roll;
        hv8_unpack(inst->generic.data,
                   &inst->frame.serial,
                   &inst->frame.button,
                   &roll, &dm, &ki, &pm, &fl);
        inst->frame.rolling_code  = roll;
        inst->frame.discriminant  = dm;
        inst->frame.key_idx       = (int8_t)ki;
        inst->frame_valid         = true;
    }

    furi_string_cat_printf(
        out,
        "%s %ubit\r\n"
        "Btn:%s (0x%X)\r\n"
        "Ser:%07lX\r\n"
        "RC:%04X Key[%d]\r\n"
        "Dis:%02X Enc:%08lX\r\n",
        SUBGHZ_PROTOCOL_HONDA_V8_NAME,
        inst->generic.data_count_bit,
        hv8_btn_name(inst->frame.button),
        inst->frame.button,
        (unsigned long)inst->frame.serial,
        inst->frame.rolling_code,
        inst->frame.key_idx,
        inst->frame.discriminant,
        (unsigned long)inst->frame.encrypted);
}

static void kl_build_upload(SubGhzProtocolEncoderHondaV8* inst) {
    LevelDuration* buf = inst->encoder.upload;
    size_t idx = 0;

    for(uint8_t rep = 0;
        rep < HV8_REPEAT && idx < HV8_ENC_BUF_SIZE - 200u;
        rep++)
    {

        for(uint8_t p = 0; p < HV8_PREAMBLE_COUNT; p++) {
            buf[idx++] = level_duration_make(true,  HV8_TE_BASE);
            buf[idx++] = level_duration_make(false, HV8_TE_BASE);
        }

        buf[idx++] = level_duration_make(false, HV8_SYNC_GAP_US);

        for(uint8_t b = 0;
            b < HV8_FRAME_BITS && idx < HV8_ENC_BUF_SIZE - 4u;
            b++) {
            bool bit = inst->frame_bits[b];
            buf[idx++] = level_duration_make(true, HV8_TE_BASE);
            if(bit) {
                /* Inverted PWM 1-bit: a single SHORT low, then the rest of the
                 * bit cell is held HIGH. The decoder keys off the LOW width only
                 * and ignores HIGH durations, so the cell must contain exactly
                 * ONE low per bit; emitting the filler as a second LOW would be
                 * counted as a spurious extra bit and desync the frame. */
                buf[idx++] = level_duration_make(false, HV8_TE_BIT1_LOW);
                buf[idx++] = level_duration_make(true,
                    HV8_TE_BASE - HV8_TE_BIT1_LOW);
            } else {
                buf[idx++] = level_duration_make(false, HV8_TE_BASE);
            }
        }

        buf[idx++] = level_duration_make(false, HV8_TRAIL_US);

        if(rep + 1u < HV8_REPEAT) {
            inst->rolling_code++;
            kl_build_frame(
                inst->frame_bits,
                inst->key,
                inst->serial,
                inst->active_button,
                inst->status,
                inst->rolling_code);
        }
    }

    inst->encoder.size_upload = idx;
    inst->encoder.front       = 0;
    FURI_LOG_D(TAG, "[KL] upload %u symbols", (unsigned)idx);
}

void* subghz_protocol_encoder_honda_v8_alloc(SubGhzEnvironment* env) {
    UNUSED(env);
    SubGhzProtocolEncoderHondaV8* inst =
        malloc(sizeof(SubGhzProtocolEncoderHondaV8));
    furi_check(inst);
    memset(inst, 0, sizeof(SubGhzProtocolEncoderHondaV8));
    inst->base.protocol         = &subghz_protocol_honda_v8;
    inst->generic.protocol_name = inst->base.protocol->name;
    inst->encoder.repeat        = (int32_t)1;
    inst->encoder.upload        =
        malloc(HV8_ENC_BUF_SIZE * sizeof(LevelDuration));
    furi_check(inst->encoder.upload);
    inst->encoder.is_running    = false;
    return inst;
}

void subghz_protocol_encoder_honda_v8_free(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolEncoderHondaV8* inst = ctx;
    free(inst->encoder.upload);
    free(inst);
}

void subghz_protocol_encoder_honda_v8_stop(void* ctx) {
    furi_assert(ctx);
    ((SubGhzProtocolEncoderHondaV8*)ctx)->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_honda_v8_yield(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolEncoderHondaV8* inst = ctx;
    if(!inst->encoder.is_running || inst->encoder.repeat == 0)
        return level_duration_reset();
    LevelDuration ret = inst->encoder.upload[inst->encoder.front];
    if(++inst->encoder.front >= inst->encoder.size_upload) {
        inst->encoder.repeat--;
        inst->encoder.front = 0;
    }
    return ret;
}

SubGhzProtocolStatus subghz_protocol_encoder_honda_v8_deserialize(
    void* ctx, FlipperFormat* ff)
{
    furi_assert(ctx);
    SubGhzProtocolEncoderHondaV8* inst = ctx;
    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize(&inst->generic, ff);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint32_t v = 0;
    if(!flipper_format_read_uint32(ff, "Serial", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->serial = v;

    if(!flipper_format_read_uint32(ff, "RollingCode", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->rolling_code = (uint16_t)v;

    if(!flipper_format_read_uint32(ff, "Button", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->btn = (uint8_t)v;

    if(!flipper_format_read_uint32(ff, "KeyIdx", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    int8_t ki = (int8_t)v;
    if(ki < 0 || ki >= HV8_MFR_KEY_COUNT)
        return SubGhzProtocolStatusErrorParserOthers;

    inst->key = ((uint64_t)honda_v8_mfr_keys[ki].high << 32) |
                 (uint64_t)honda_v8_mfr_keys[ki].low;

    hv8_register_btn(inst->btn);
    inst->active_button = hv8_get_active_btn();
    inst->status        = 0u;

    inst->rolling_code = (uint16_t)(
        inst->rolling_code +
        (uint16_t)furi_hal_subghz_get_rolling_counter_mult());

    kl_build_frame(
        inst->frame_bits,
        inst->key,
        inst->serial,
        inst->active_button,
        inst->status,
        inst->rolling_code);

    inst->generic.cnt = inst->rolling_code;
    inst->generic.btn = inst->active_button;

    flipper_format_rewind(ff);
    uint8_t kd[8];
    uint64_t packed = hv8_pack(
        inst->serial, inst->active_button,
        inst->rolling_code,
        (uint8_t)((inst->serial >> 20) & 0xFFu),
        (uint8_t)ki, HV8_PROTO_MODE, 0u);
    for(int i = 0; i < 8; i++)
        kd[i] = (uint8_t)(packed >> (56 - i * 8));
    flipper_format_update_hex(ff, "Key", kd, 8);

    kl_build_upload(inst);
    inst->encoder.is_running = true;

    FURI_LOG_I(TAG, "[KL] encoder ready ser=%07lX btn=%u rc=%04X key=%016llX",
               (unsigned long)inst->serial,
               inst->active_button,
               inst->rolling_code,
               (unsigned long long)inst->key);
    return SubGhzProtocolStatusOk;
}

static SubGhzBlockGeneric* subghz_protocol_decoder_honda_v8_get_generic(void* context) {
    SubGhzProtocolDecoderHondaV8* instance = context;
    return &instance->generic;
}

const SubGhzProtocolDecoder subghz_protocol_honda_v8_decoder = {
    .alloc         = subghz_protocol_decoder_honda_v8_alloc,
    .free          = subghz_protocol_decoder_honda_v8_free,
    .feed          = subghz_protocol_decoder_honda_v8_feed,
    .reset         = subghz_protocol_decoder_honda_v8_reset,
    .get_hash_data = subghz_protocol_decoder_honda_v8_get_hash_data,
    .serialize     = subghz_protocol_decoder_honda_v8_serialize,
    .deserialize   = subghz_protocol_decoder_honda_v8_deserialize,
    .get_string    = subghz_protocol_decoder_honda_v8_get_string,
    .get_generic   = subghz_protocol_decoder_honda_v8_get_generic,
};

const SubGhzProtocolEncoder subghz_protocol_honda_v8_encoder = {
    .alloc       = subghz_protocol_encoder_honda_v8_alloc,
    .free        = subghz_protocol_encoder_honda_v8_free,
    .deserialize = subghz_protocol_encoder_honda_v8_deserialize,
    .stop        = subghz_protocol_encoder_honda_v8_stop,
    .yield       = subghz_protocol_encoder_honda_v8_yield,
};

const SubGhzProtocol subghz_protocol_honda_v8 = {
    .name    = SUBGHZ_PROTOCOL_HONDA_V8_NAME,
    .type    = SubGhzProtocolTypeDynamic,
    .flag    = SubGhzProtocolFlag_433 |
               SubGhzProtocolFlag_315 |
               SubGhzProtocolFlag_AM  |
               SubGhzProtocolFlag_Decodable |
               SubGhzProtocolFlag_Load |
               SubGhzProtocolFlag_Save |
               SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_honda_v8_decoder,
    .encoder = &subghz_protocol_honda_v8_encoder,
};
