/*
 * honda_v9.c — Honda FSK Manchester Extended Protocol
 * Source: Pandora firmware — Protocol_TX_Honda_Extended @ 0xEFDC
 *
 * FSK Manchester captured as OOK (~8x compressed):
 *   Half-bit SHORT: ~63µs in OOK capture
 *   Half-bit LONG:  ~126µs (two merged half-bits at the same level)
 * Manchester IEEE 802.3:
 *   bit 1: LOW→HIGH (rising edge, half-bits 0,1)
 *   bit 0: HIGH→LOW (falling edge, half-bits 1,0)
 * MSB-first, 112 bits (14 bytes).
 *
 * Checksum rolling (table @ 0xF0CE) for the extended mode.
 */

#include "honda_v9.h"

#include <furi.h>
#include <furi_hal.h>
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h>
#include <lib/subghz/blocks/custom_btn_i.h>

#define TAG "HondaV9"

static inline uint8_t popcount8(uint8_t x) {
    x = x - ((x >> 1) & 0x55);
    x = (x & 0x33) + ((x >> 2) & 0x33);
    return (x + (x >> 4)) & 0x0F;
}

static inline uint8_t hv9_bit_rev8(uint8_t v) {
    v = (uint8_t)(((v & 0xF0u) >> 4) | ((v & 0x0Fu) << 4));
    v = (uint8_t)(((v & 0xCCu) >> 2) | ((v & 0x33u) << 2));
    v = (uint8_t)(((v & 0xAAu) >> 1) | ((v & 0x55u) << 1));
    return v;
}

static inline uint8_t hv9_bit_rev4(uint8_t v) {
    return (uint8_t)(hv9_bit_rev8(v & 0x0Fu) >> 4);
}

static const uint8_t hv9_tbl_primary[16][16]   = { HV9_TABLE_PRIMARY };
static const uint8_t hv9_tbl_secondary[16][16] = { HV9_TABLE_SECONDARY };
static const uint8_t hv9_tbl_subst[16][16]     = { HV9_TABLE_SUBST };

static const char* hv9_btn_name(uint8_t btn) {
    switch(btn) {
    case HV9_BTN_PANIC:        return "Panic";
    case HV9_BTN_LOCK:         return "Lock";
    case HV9_BTN_TRUNK:        return "Trunk/Hatch";
    case HV9_BTN_REMOTE_START: return "Remote Start";
    case HV9_BTN_UNLOCK:       return "Unlock";
    case HV9_BTN_UNLOCK2:      return "Unlock x2";
    default:                   return "Unknown";
    }
}

static uint8_t hv9_btn_to_custom(uint8_t btn) {
    switch(btn) {
    case HV9_BTN_LOCK:         return 1;
    case HV9_BTN_UNLOCK:       return 2;
    case HV9_BTN_TRUNK:        return 3;
    case HV9_BTN_PANIC:        return 4;
    case HV9_BTN_REMOTE_START: return 5;
    default:                   return 1;
    }
}

static uint8_t hv9_custom_to_btn(uint8_t custom) {
    switch(custom) {
    case 1: return HV9_BTN_LOCK;
    case 2: return HV9_BTN_UNLOCK;
    case 3: return HV9_BTN_TRUNK;
    case 4: return HV9_BTN_PANIC;
    case 5: return HV9_BTN_REMOTE_START;
    default: return HV9_BTN_LOCK;
    }
}

static void hv9_register_btn(uint8_t btn) {
    uint8_t custom = hv9_btn_to_custom(btn);
    if(subghz_custom_btn_get_original() == 0)
        subghz_custom_btn_set_original(custom);
    subghz_custom_btn_set_max(HV9_BTN_CUSTOM_MAX);
}

static uint8_t hv9_get_active_btn(void) {
    uint8_t ac = subghz_custom_btn_get();
    if(ac == SUBGHZ_CUSTOM_BTN_OK)
        return hv9_custom_to_btn(subghz_custom_btn_get_original());
    return hv9_custom_to_btn(ac);
}

static uint64_t hv9_pack(
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

static void hv9_unpack(
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

const SubGhzProtocolDecoder subghz_protocol_honda_v9_decoder;
const SubGhzProtocolEncoder subghz_protocol_honda_v9_encoder;
const SubGhzProtocol        subghz_protocol_honda_v9;

static inline uint8_t fsk_classify(uint32_t dur) {
    if(dur + HV9_TE_DELTA >= HV9_TE_SHORT &&
       dur <= HV9_TE_SHORT + HV9_TE_DELTA) return 1u;
    if(dur + HV9_TE_DELTA >= HV9_TE_LONG &&
       dur <= HV9_TE_LONG  + HV9_TE_DELTA) return 2u;
    return 0u;
}

static uint8_t fsk_rolling_checksum(const uint8_t buf[HV9_FRAME_BYTES]) {
    uint8_t prev_lo = buf[8] & 0x0Fu;
    uint8_t prev_hi = (buf[8] >> 4) & 0x0Fu;
    uint8_t new_lo  = prev_lo;
    uint8_t new_hi  = prev_hi;

    uint8_t col_lo = hv9_bit_rev4(buf[3] & 0x0Fu);
    uint8_t col_hi = hv9_bit_rev4((buf[3] >> 4) & 0x0Fu);

    bool found = false;
    for(uint8_t row = 0; row < 16u && !found; row++) {
        if(hv9_tbl_primary[row][15] == prev_lo) {
            uint8_t target = hv9_tbl_primary[row][0];
            for(uint8_t r2 = 0; r2 < 16u; r2++) {
                if(hv9_tbl_subst[r2][col_lo] == target) {
                    uint8_t nc = (col_lo + 1u) & 0x0Fu;
                    new_lo = hv9_tbl_subst[r2][nc];
                    found = true;
                    break;
                }
            }
            if(!found) {
                new_lo = hv9_tbl_subst[row][col_lo];
                found = true;
            }
        }
    }
    if(!found) {
        for(uint8_t row = 0; row < 16u; row++) {
            if(hv9_tbl_secondary[row][15] == prev_lo) {
                uint8_t target = hv9_tbl_secondary[row][0];
                for(uint8_t r2 = 0; r2 < 16u; r2++) {
                    if(hv9_tbl_subst[r2][col_lo] == target) {
                        uint8_t nc = (col_lo + 1u) & 0x0Fu;
                        new_lo = hv9_tbl_subst[r2][nc];
                        break;
                    }
                }
                break;
            }
        }
    }

    found = false;
    for(uint8_t row = 0; row < 16u && !found; row++) {
        if(hv9_tbl_primary[row][15] == prev_hi) {
            uint8_t target = hv9_tbl_primary[row][0];
            for(uint8_t r2 = 0; r2 < 16u; r2++) {
                if(hv9_tbl_subst[r2][col_hi] == target) {
                    uint8_t nc = (col_hi + 1u) & 0x0Fu;
                    new_hi = hv9_tbl_subst[r2][nc];
                    found = true;
                    break;
                }
            }
            if(!found) {
                new_hi = hv9_tbl_subst[row][col_hi];
                found = true;
            }
        }
    }
    if(!found) {
        for(uint8_t row = 0; row < 16u; row++) {
            if(hv9_tbl_secondary[row][15] == prev_hi) {
                uint8_t target = hv9_tbl_secondary[row][0];
                for(uint8_t r2 = 0; r2 < 16u; r2++) {
                    if(hv9_tbl_subst[r2][col_hi] == target) {
                        uint8_t nc = (col_hi + 1u) & 0x0Fu;
                        new_hi = hv9_tbl_subst[r2][nc];
                        break;
                    }
                }
                break;
            }
        }
    }

    return (uint8_t)((new_hi << 4) | (new_lo & 0x0Fu));
}

static uint8_t fsk_xor_checksum(const uint8_t* data, uint8_t len) {
    uint8_t c = 0;
    for(uint8_t i = 0; i < len; i++) c ^= data[i];
    return c;
}

static void fsk_to_buf(
    const HondaV9Frame* f,
    uint8_t buf[HV9_FRAME_BYTES])
{
    memset(buf, 0, HV9_FRAME_BYTES);
    buf[0]  = (uint8_t)(((f->hdr & 0x0Fu) << 4) | (f->button & 0x0Fu));
    buf[1]  = (uint8_t)((f->serial >> 12) & 0xFFu);
    buf[2]  = (uint8_t)((f->serial >>  4) & 0xFFu);
    buf[3]  = (uint8_t)(((f->serial & 0x0Fu) << 4) | (f->flags & 0x0Fu));
    buf[4]  = (uint8_t)((f->fingerprint >> 24) & 0xFFu);
    buf[5]  = (uint8_t)((f->fingerprint >> 16) & 0xFFu);
    buf[6]  = (uint8_t)((f->fingerprint >>  8) & 0xFFu);
    buf[7]  = (uint8_t)( f->fingerprint        & 0xFFu);
    buf[8]  = f->checksum;
    buf[9]  = f->status;
    buf[10] = (uint8_t)((f->counter >> 8) & 0xFFu);
    buf[11] = (uint8_t)( f->counter       & 0xFFu);
    buf[12] = (uint8_t)((f->tail >> 8) & 0xFFu);
    buf[13] = (uint8_t)( f->tail       & 0xFFu);
}

static void fsk_from_buf(
    const uint8_t buf[HV9_FRAME_BYTES],
    HondaV9Frame* f)
{
    f->hdr         = (buf[0] >> 4) & 0x0Fu;
    f->button      =  buf[0]       & 0x0Fu;
    f->serial      = ((uint32_t)buf[1] << 12) |
                     ((uint32_t)buf[2] <<  4) |
                     ((uint32_t)(buf[3] >> 4) & 0x0Fu);
    f->flags       =  buf[3]       & 0x0Fu;
    f->fingerprint = ((uint32_t)buf[4] << 24) |
                     ((uint32_t)buf[5] << 16) |
                     ((uint32_t)buf[6] <<  8) |
                      (uint32_t)buf[7];
    f->checksum    = buf[8];
    f->status      = buf[9];
    f->counter     = (uint16_t)(((uint16_t)buf[10] << 8) | buf[11]);
    f->tail        = (uint16_t)(((uint16_t)buf[12] << 8) | buf[13]);
}

static void fsk_counter_increment(HondaV9Frame* f) {
    f->counter = (uint16_t)(f->counter + 1u);
}

/*
 * Manchester decoder for accumulated half-bits.
 *
 * Receives an array of half-bits (0=LOW, 1=HIGH).
 * Finds the end of the preamble (run alternating ≥ MIN_PREAMBLE).
 * Decodes pairs of half-bits:
 *   (0,1) → bit 1 (LOW→HIGH, rising)
 *   (1,0) → bit 0 (HIGH→LOW, falling)
 * MSB-first in decoded[].
 */
static bool fsk_manchester_decode(
    const uint8_t* hb, uint16_t hb_count,
    uint8_t* decoded, uint8_t* out_bits,
    uint8_t max_bits, bool invert)
{
    uint8_t bit_count = 0;
    memset(decoded, 0, (max_bits + 7u) / 8u);

    uint16_t alt_end = 1;
    while(alt_end < hb_count && hb[alt_end] != hb[alt_end-1]) alt_end++;

    if(alt_end == hb_count) {
        *out_bits = 0;
        return false;
    }
    if(alt_end < HV9_MIN_PREAMBLE) {
        *out_bits = 0;
        return false;
    }

    uint16_t i;
    if(alt_end >= (uint16_t)(HV9_PREAMBLE_CYCLES * 2u)) {
        /* Full preamble length reached — data bit 1 was 0, extending the
         * alternation. Anchor at fixed preamble end so no data bits are
         * absorbed into the preamble count. */
        i = (uint16_t)(HV9_PREAMBLE_CYCLES * 2u);
    } else {
        i = alt_end;
        while(i + 1 < hb_count && hb[i] == hb[i+1]) i++;
    }

    if(max_bits > (HV9_FRAME_BYTES + 2) * 8u) {
        max_bits = (HV9_FRAME_BYTES + 2) * 8u;
    }

    while(i + 1 < hb_count && bit_count < max_bits) {
        uint8_t h0 = hb[i];
        uint8_t h1 = hb[i+1];

        if(h0 == h1) break;

        uint8_t bit_val;
        if(!invert) {
            bit_val = (h0 == 0u && h1 == 1u) ? 1u : 0u;
        } else {
            bit_val = (h0 == 1u && h1 == 0u) ? 1u : 0u;
        }

        uint8_t byte_idx = bit_count / 8u;
        uint8_t bit_idx  = 7u - (bit_count % 8u);
        if(bit_val)
            decoded[byte_idx] |= (uint8_t)(1u << bit_idx);

        bit_count++;
        i += 2;
    }

    *out_bits = bit_count;
    return (bit_count >= HV9_MIN_BITS);
}

static bool fsk_try_decode_polarity(
    SubGhzProtocolDecoderHondaV9* inst, bool invert)
{
    uint8_t decoded[HV9_FRAME_BYTES + 2] = {0};
    uint8_t bit_count = 0;

    if(!fsk_manchester_decode(
        inst->half_bits, inst->hb_count,
        decoded, &bit_count,
        HV9_FRAME_BITS, invert)) {
        return false;
    }

    FURI_LOG_D(TAG,
        "[FSK] pol=%s bits=%u all14: %02X %02X %02X %02X %02X %02X %02X %02X  %02X  %02X %02X %02X %02X %02X",
        invert ? "INV" : "NOR", bit_count,
        decoded[0], decoded[1], decoded[2], decoded[3],
        decoded[4], decoded[5], decoded[6], decoded[7],
        decoded[8],
        decoded[9], decoded[10], decoded[11], decoded[12], decoded[13]);

    if(bit_count < HV9_FRAME_BITS) return false;

    HondaV9Frame f;
    fsk_from_buf(decoded, &f);
    uint8_t xchk = fsk_xor_checksum(decoded, 8u);
    uint8_t dist = popcount8(xchk ^ decoded[8]);

    FURI_LOG_D(TAG,
        "[FSK] try pol=%s hdr=%X btn=%X ser=%05lX flags=%X fp=%08lX "
        "chk=%02X(x=%02X d=%u) st=%02X cnt=%04X tail=%04X",
        invert?"INV":"NOR", f.hdr, f.button,
        (unsigned long)f.serial, f.flags,
        (unsigned long)f.fingerprint, f.checksum, xchk, dist,
        f.status, f.counter, f.tail);

    if(f.button > HV9_BTN_UNLOCK2) return false;
    if(f.serial == 0u || f.serial == 0x000FFFFFu) return false;
    if(f.fingerprint == 0u || f.fingerprint == 0xFFFFFFFFu) return false;

    if(xchk != decoded[8] && dist > 1) return false;

    inst->frame       = f;
    inst->frame_valid = true;
    FURI_LOG_I(TAG,
        "[FSK] decoded pol=%s hdr=%u btn=%u ser=%05lX fp=%08lX cnt=%04X",
        invert?"INV":"NOR", f.hdr, f.button,
        (unsigned long)f.serial,
        (unsigned long)f.fingerprint, f.counter);
    return true;
}

static bool fsk_try_decode(SubGhzProtocolDecoderHondaV9* inst) {
    if(inst->hb_count < (HV9_MIN_PREAMBLE + 16u)) return false;
    if(fsk_try_decode_polarity(inst, false)) return true;
    if(fsk_try_decode_polarity(inst, true))  return true;
    return false;
}

void* subghz_protocol_decoder_honda_v9_alloc(SubGhzEnvironment* env) {
    UNUSED(env);
    SubGhzProtocolDecoderHondaV9* inst =
        malloc(sizeof(SubGhzProtocolDecoderHondaV9));
    furi_check(inst);
    memset(inst, 0, sizeof(SubGhzProtocolDecoderHondaV9));
    inst->base.protocol         = &subghz_protocol_honda_v9;
    inst->generic.protocol_name = inst->base.protocol->name;
    inst->frame_valid           = false;
    FURI_LOG_I(TAG, "[FSK] decoder alloc");
    return inst;
}

void subghz_protocol_decoder_honda_v9_free(void* ctx) {
    furi_assert(ctx);
    free(ctx);
}

void subghz_protocol_decoder_honda_v9_reset(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV9* inst = ctx;
    inst->decoder.parser_step = 0;
    inst->decoder.te_last     = 0;
    inst->hb_count            = 0;
}

void subghz_protocol_decoder_honda_v9_feed(
    void* ctx, bool level, uint32_t duration)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV9* inst = ctx;
    uint8_t lvl = level ? 1u : 0u;
    uint8_t dc  = fsk_classify(duration);

    if(dc == 1u) {
        if(inst->hb_count < HV9_HALF_BIT_BUF)
            inst->half_bits[inst->hb_count++] = lvl;
    } else if(dc == 2u) {
        if(inst->hb_count + 2u <= HV9_HALF_BIT_BUF) {
            inst->half_bits[inst->hb_count++] = lvl;
            inst->half_bits[inst->hb_count++] = lvl;
        }
    } else {
        if(inst->hb_count >= (HV9_MIN_PREAMBLE + 16u)) {
            if(fsk_try_decode(inst)) {
                inst->generic.data = hv9_pack(
                    inst->frame.serial,
                    inst->frame.button,
                    inst->frame.counter,
                    inst->frame.flags,
                    inst->frame.hdr,
                    HV9_PROTO_MODE,
                    0u);
                inst->generic.data_count_bit = HV9_FRAME_BITS;
                inst->generic.serial = inst->frame.serial;
                inst->generic.btn    = inst->frame.button;
                inst->generic.cnt    = inst->frame.counter;
                hv9_register_btn(inst->frame.button);
                if(inst->base.callback)
                    inst->base.callback(&inst->base, inst->base.context);
            }
        }
        inst->hb_count = 0;
    }

    inst->decoder.te_last = duration;
}

uint8_t subghz_protocol_decoder_honda_v9_get_hash_data(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV9* inst = ctx;
    return (uint8_t)(inst->generic.data ^
                    (inst->generic.data >>  8) ^
                    (inst->generic.data >> 16) ^
                    (inst->generic.data >> 24) ^
                    (inst->generic.data >> 32));
}

SubGhzProtocolStatus subghz_protocol_decoder_honda_v9_serialize(
    void* ctx, FlipperFormat* ff, SubGhzRadioPreset* preset)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV9* inst = ctx;
    SubGhzProtocolStatus ret =
        subghz_block_generic_serialize(&inst->generic, ff, preset);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint32_t v = inst->frame.serial;
    if(!flipper_format_write_uint32(ff, "Serial", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    v = inst->frame.counter;
    if(!flipper_format_write_uint32(ff, "Counter", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    v = inst->frame.button;
    if(!flipper_format_write_uint32(ff, "Button", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    v = inst->frame.hdr;
    if(!flipper_format_write_uint32(ff, "Hdr", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    v = inst->frame.flags;
    if(!flipper_format_write_uint32(ff, "Flags", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    v = inst->frame.fingerprint;
    if(!flipper_format_write_uint32(ff, "Fingerprint", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    v = inst->frame.status;
    if(!flipper_format_write_uint32(ff, "Status", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    v = inst->frame.tail;
    if(!flipper_format_write_uint32(ff, "Tail", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus subghz_protocol_decoder_honda_v9_deserialize(
    void* ctx, FlipperFormat* ff)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV9* inst = ctx;
    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize_check_count_bit(
            &inst->generic, ff, HV9_FRAME_BITS);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint32_t v = 0;
    if(!flipper_format_read_uint32(ff, "Serial", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.serial = v;

    if(!flipper_format_read_uint32(ff, "Counter", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.counter = (uint16_t)v;

    if(!flipper_format_read_uint32(ff, "Button", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.button = (uint8_t)v;

    if(flipper_format_read_uint32(ff, "Hdr", &v, 1))
        inst->frame.hdr = (uint8_t)(v & 0x0Fu);
    else
        inst->frame.hdr = 0xDu;

    if(flipper_format_read_uint32(ff, "Flags", &v, 1))
        inst->frame.flags = (uint8_t)(v & 0x0Fu);
    else
        inst->frame.flags = 0x1u;

    if(flipper_format_read_uint32(ff, "Fingerprint", &v, 1))
        inst->frame.fingerprint = v;
    else
        inst->frame.fingerprint = 0u;

    if(flipper_format_read_uint32(ff, "Status", &v, 1))
        inst->frame.status = (uint8_t)v;
    else
        inst->frame.status = 0u;

    if(flipper_format_read_uint32(ff, "Tail", &v, 1))
        inst->frame.tail = (uint16_t)v;
    else
        inst->frame.tail = 0u;

    inst->frame.checksum    = 0u;
    inst->frame_valid       = true;
    inst->generic.serial    = inst->frame.serial;
    inst->generic.btn       = inst->frame.button;
    inst->generic.cnt       = inst->frame.counter;
    hv9_register_btn(inst->frame.button);
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_honda_v9_get_string(
    void* ctx, FuriString* out)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV9* inst = ctx;

    if(!inst->frame_valid && inst->generic.data != 0) {
        uint8_t pm, ki, fl, dm;
        uint16_t roll;
        hv9_unpack(inst->generic.data,
                   &inst->frame.serial, &inst->frame.button,
                   &roll, &dm, &ki, &pm, &fl);
        inst->frame.counter     = roll;
        inst->frame.flags       = dm;
        inst->frame.hdr         = ki;
        inst->frame_valid       = true;
    }

    furi_string_cat_printf(
        out,
        "%s %ubit\r\n"
        "Btn:%s (0x%X) Hdr:%X Flags:%X\r\n"
        "Ser:%05lX Fp:%08lX\r\n"
        "Cnt:%04X Tail:%04X St:%02X\r\n",
        SUBGHZ_PROTOCOL_HONDA_V9_NAME,
        inst->generic.data_count_bit,
        hv9_btn_name(inst->frame.button),
        inst->frame.button,
        inst->frame.hdr,
        inst->frame.flags,
        (unsigned long)inst->frame.serial,
        (unsigned long)inst->frame.fingerprint,
        inst->frame.counter,
        inst->frame.tail,
        inst->frame.status);
}

static void fsk_build_upload(SubGhzProtocolEncoderHondaV9* inst) {
    LevelDuration* buf = inst->encoder.upload;
    size_t idx = 0;

    buf[idx++] = level_duration_make(false, HV9_GUARD_TIME_US);

    for(uint16_t p = 0;
        p < (uint16_t)(HV9_PREAMBLE_CYCLES * 2u) &&
        idx < HV9_ENC_BUF_SIZE - 10u;
        p++) {
        buf[idx++] = level_duration_make((p & 1u) == 0u, HV9_TE_SHORT);
    }

    uint8_t frame_buf[HV9_FRAME_BYTES];
    inst->frame.button = inst->active_button;
    fsk_to_buf(&inst->frame, frame_buf);

    for(uint8_t b = 0;
        b < HV9_FRAME_BITS && idx < HV9_ENC_BUF_SIZE - 4u;
        b++) {
        uint8_t byte_idx = b / 8u;
        uint8_t bit_idx  = 7u - (b % 8u);  /* MSB-first */
        uint8_t bit      = (frame_buf[byte_idx] >> bit_idx) & 1u;

        if(bit) {
            /* bit 1: LOW→HIGH */
            buf[idx++] = level_duration_make(false, HV9_TE_SHORT);
            buf[idx++] = level_duration_make(true,  HV9_TE_SHORT);
        } else {
            /* bit 0: HIGH→LOW */
            buf[idx++] = level_duration_make(true,  HV9_TE_SHORT);
            buf[idx++] = level_duration_make(false, HV9_TE_SHORT);
        }
    }

    /* Guard HIGH pulse so the final data bit doesn't merge into the gap
     * when it ends LOW — otherwise the re-decoder sees one bit less. */
    if(idx < HV9_ENC_BUF_SIZE - 2u)
        buf[idx++] = level_duration_make(true, HV9_TE_SHORT);
    buf[idx++] = level_duration_make(false, HV9_GUARD_TIME_US);

    inst->encoder.size_upload = idx;
    inst->encoder.front       = 0;
    FURI_LOG_D(TAG, "[FSK] upload %u symbols", (unsigned)idx);
}

void* subghz_protocol_encoder_honda_v9_alloc(SubGhzEnvironment* env) {
    UNUSED(env);
    SubGhzProtocolEncoderHondaV9* inst =
        malloc(sizeof(SubGhzProtocolEncoderHondaV9));
    furi_check(inst);
    memset(inst, 0, sizeof(SubGhzProtocolEncoderHondaV9));
    inst->base.protocol         = &subghz_protocol_honda_v9;
    inst->generic.protocol_name = inst->base.protocol->name;
    inst->encoder.repeat        = 3;
    inst->encoder.upload        =
        malloc(HV9_ENC_BUF_SIZE * sizeof(LevelDuration));
    furi_check(inst->encoder.upload);
    inst->encoder.is_running    = false;
    return inst;
}

void subghz_protocol_encoder_honda_v9_free(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolEncoderHondaV9* inst = ctx;
    free(inst->encoder.upload);
    free(inst);
}

void subghz_protocol_encoder_honda_v9_stop(void* ctx) {
    furi_assert(ctx);
    ((SubGhzProtocolEncoderHondaV9*)ctx)->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_honda_v9_yield(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolEncoderHondaV9* inst = ctx;
    if(!inst->encoder.is_running || inst->encoder.repeat == 0)
        return level_duration_reset();
    LevelDuration ret = inst->encoder.upload[inst->encoder.front];
    if(++inst->encoder.front >= inst->encoder.size_upload) {
        inst->encoder.repeat--;
        inst->encoder.front = 0;
    }
    return ret;
}

SubGhzProtocolStatus subghz_protocol_encoder_honda_v9_deserialize(
    void* ctx, FlipperFormat* ff)
{
    furi_assert(ctx);
    SubGhzProtocolEncoderHondaV9* inst = ctx;
    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize(&inst->generic, ff);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint32_t v = 0;
    if(!flipper_format_read_uint32(ff, "Serial", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.serial = v;

    if(!flipper_format_read_uint32(ff, "Counter", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.counter = (uint16_t)v;

    if(!flipper_format_read_uint32(ff, "Button", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.button = (uint8_t)v;

    if(flipper_format_read_uint32(ff, "Hdr", &v, 1))
        inst->frame.hdr = (uint8_t)(v & 0x0Fu);
    else
        inst->frame.hdr = 0xDu;

    if(flipper_format_read_uint32(ff, "Flags", &v, 1))
        inst->frame.flags = (uint8_t)(v & 0x0Fu);
    else
        inst->frame.flags = 0x1u;

    if(flipper_format_read_uint32(ff, "Fingerprint", &v, 1))
        inst->frame.fingerprint = v;
    else
        inst->frame.fingerprint = 0u;

    if(flipper_format_read_uint32(ff, "Status", &v, 1))
        inst->frame.status = (uint8_t)v;
    else
        inst->frame.status = 0u;

    if(flipper_format_read_uint32(ff, "Tail", &v, 1))
        inst->frame.tail = (uint16_t)v;
    else
        inst->frame.tail = 0u;

    hv9_register_btn(inst->frame.button);
    inst->active_button = hv9_get_active_btn();

    inst->frame.counter = (uint16_t)(inst->frame.counter +
        (uint16_t)furi_hal_subghz_get_rolling_counter_mult());
    inst->frame.button = inst->active_button;

    uint8_t tmp[HV9_FRAME_BYTES];
    fsk_to_buf(&inst->frame, tmp);
    /* Decoder validates via fsk_xor_checksum over bytes 0..7 — keep encoder
     * consistent so roundtrip matches. Real-capture checksum algorithm is
     * not yet reverse-engineered. */
    inst->frame.checksum = fsk_xor_checksum(tmp, 8u);

    inst->generic.cnt  = inst->frame.counter;
    inst->generic.btn  = inst->active_button;
    inst->generic.data = hv9_pack(
        inst->frame.serial, inst->active_button,
        inst->frame.counter,
        inst->frame.flags, inst->frame.hdr,
        HV9_PROTO_MODE, 0u);

    flipper_format_rewind(ff);
    uint8_t kd[8];
    for(int i = 0; i < 8; i++)
        kd[i] = (uint8_t)(inst->generic.data >> (56 - i * 8));
    flipper_format_update_hex(ff, "Key", kd, 8);

    fsk_build_upload(inst);
    inst->encoder.is_running = true;

    FURI_LOG_I(TAG, "[FSK] encoder ready ser=%07lX btn=%u cnt=%06lX",
               (unsigned long)inst->frame.serial,
               inst->active_button,
               (unsigned long)inst->frame.counter);
    return SubGhzProtocolStatusOk;
}

static SubGhzBlockGeneric* subghz_protocol_decoder_honda_v9_get_generic(void* context) {
    SubGhzProtocolDecoderHondaV9* instance = context;
    return &instance->generic;
}

const SubGhzProtocolDecoder subghz_protocol_honda_v9_decoder = {
    .alloc         = subghz_protocol_decoder_honda_v9_alloc,
    .free          = subghz_protocol_decoder_honda_v9_free,
    .feed          = subghz_protocol_decoder_honda_v9_feed,
    .reset         = subghz_protocol_decoder_honda_v9_reset,
    .get_hash_data = subghz_protocol_decoder_honda_v9_get_hash_data,
    .serialize     = subghz_protocol_decoder_honda_v9_serialize,
    .deserialize   = subghz_protocol_decoder_honda_v9_deserialize,
    .get_string    = subghz_protocol_decoder_honda_v9_get_string,
    .get_generic   = subghz_protocol_decoder_honda_v9_get_generic,
};

const SubGhzProtocolEncoder subghz_protocol_honda_v9_encoder = {
    .alloc       = subghz_protocol_encoder_honda_v9_alloc,
    .free        = subghz_protocol_encoder_honda_v9_free,
    .deserialize = subghz_protocol_encoder_honda_v9_deserialize,
    .stop        = subghz_protocol_encoder_honda_v9_stop,
    .yield       = subghz_protocol_encoder_honda_v9_yield,
};

const SubGhzProtocol subghz_protocol_honda_v9 = {
    .name    = SUBGHZ_PROTOCOL_HONDA_V9_NAME,
    .type    = SubGhzProtocolTypeDynamic,
    .flag    = SubGhzProtocolFlag_433 |
               SubGhzProtocolFlag_AM  |
               SubGhzProtocolFlag_Decodable |
               SubGhzProtocolFlag_Load |
               SubGhzProtocolFlag_Save |
               SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_honda_v9_decoder,
    .encoder = &subghz_protocol_honda_v9_encoder,
};
