/*
 * honda_v7.c — Honda OOK PWM Protocol
 * Source: Pandora firmware — Brand_Auto_Honda_TX @ 0x13F24
 *
 * OOK PWM, 11 bytes, 88 bits, LSB-first per byte on the air.
 * Wide timing tolerance (±100µs) for variability between remotes.
 */

#include "honda_v7.h"

#include <furi.h>
#include <furi_hal.h>
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h>
#include <lib/subghz/blocks/custom_btn_i.h>

#define TAG "HondaV7"

static inline uint8_t popcount8(uint8_t x) {
    x = x - ((x >> 1) & 0x55);
    x = (x & 0x33) + ((x >> 2) & 0x33);
    return (x + (x >> 4)) & 0x0F;
}

static inline uint8_t hv7_bit_rev8(uint8_t v) {
    v = (uint8_t)(((v & 0xF0u) >> 4) | ((v & 0x0Fu) << 4));
    v = (uint8_t)(((v & 0xCCu) >> 2) | ((v & 0x33u) << 2));
    v = (uint8_t)(((v & 0xAAu) >> 1) | ((v & 0x55u) << 1));
    return v;
}

static const char* hv7_btn_name(uint8_t btn) {
    switch(btn) {
    case HV7_BTN_PANIC:        return "Panic";
    case HV7_BTN_LOCK:         return "Lock";
    case HV7_BTN_TRUNK:        return "Trunk/Hatch";
    case HV7_BTN_REMOTE_START: return "Remote Start";
    case HV7_BTN_UNLOCK:       return "Unlock";
    case HV7_BTN_UNLOCK2:      return "Unlock x2";
    default:                   return "Unknown";
    }
}

static uint8_t hv7_btn_to_custom(uint8_t btn) {
    switch(btn) {
    case HV7_BTN_LOCK:         return 1;
    case HV7_BTN_UNLOCK:       return 2;
    case HV7_BTN_TRUNK:        return 3;
    case HV7_BTN_PANIC:        return 4;
    case HV7_BTN_REMOTE_START: return 5;
    default:                   return 1;
    }
}

static uint8_t hv7_custom_to_btn(uint8_t custom) {
    switch(custom) {
    case 1: return HV7_BTN_LOCK;
    case 2: return HV7_BTN_UNLOCK;
    case 3: return HV7_BTN_TRUNK;
    case 4: return HV7_BTN_PANIC;
    case 5: return HV7_BTN_REMOTE_START;
    default: return HV7_BTN_LOCK;
    }
}

static void hv7_register_btn(uint8_t btn) {
    uint8_t custom = hv7_btn_to_custom(btn);
    if(subghz_custom_btn_get_original() == 0)
        subghz_custom_btn_set_original(custom);
    subghz_custom_btn_set_max(HV7_BTN_CUSTOM_MAX);
}

static uint8_t hv7_get_active_btn(void) {
    uint8_t ac = subghz_custom_btn_get();
    if(ac == SUBGHZ_CUSTOM_BTN_OK)
        return hv7_custom_to_btn(subghz_custom_btn_get_original());
    return hv7_custom_to_btn(ac);
}

static uint64_t hv7_pack(
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

static void hv7_unpack(
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

const SubGhzProtocolDecoder subghz_protocol_honda_v7_decoder;
const SubGhzProtocolEncoder subghz_protocol_honda_v7_encoder;
const SubGhzProtocol        subghz_protocol_honda_v7;

static void ook_to_buf(const HondaV7Frame* f, uint8_t buf[HV7_FRAME_BYTES]) {
    memset(buf, 0, HV7_FRAME_BYTES);
    buf[0] = (uint8_t)((f->button << 4) | ((f->serial >> 24) & 0x0Fu));
    buf[1] = (uint8_t)((f->serial >> 16) & 0xFFu);
    buf[2] = (uint8_t)((f->serial >>  8) & 0xFFu);
    buf[3] = (uint8_t)( f->serial        & 0xFFu);
    buf[4] = (uint8_t)((f->counter >> 16) & 0xFFu);
    buf[5] = (uint8_t)((f->counter >>  8) & 0xFFu);
    buf[6] = (uint8_t)( f->counter        & 0xFFu);
    buf[7] = (uint8_t)(((f->mode & 0x0Fu) << 4) | (f->counter & 0x0Fu));
    buf[8] = f->checksum;
    buf[9]  = f->extra[0];
    buf[10] = f->extra[1];
}

static void ook_from_buf(const uint8_t buf[HV7_FRAME_BYTES], HondaV7Frame* f) {
    f->button  = (buf[0] >> 4) & 0x0Fu;
    f->serial  = ((uint32_t)(buf[0] & 0x0Fu) << 24) |
                 ((uint32_t)buf[1] << 16) |
                 ((uint32_t)buf[2] <<  8) |
                  (uint32_t)buf[3];
    f->counter = ((uint32_t)buf[4] << 16) |
                 ((uint32_t)buf[5] <<  8) |
                  (uint32_t)buf[6];
    f->mode     = (buf[7] >> 4) & 0x0Fu;
    f->checksum = buf[8];
    f->extra[0] = buf[9];
    f->extra[1] = buf[10];
}

static uint8_t ook_checksum(const uint8_t buf[HV7_FRAME_BYTES]) {
    uint8_t c = 0;
    for(uint8_t i = 0; i < 8u; i++) c ^= buf[i];
    return c;
}

/*
 * Counter — exact firmware logic @ 0xEFF8-0xF08E
 * Operates with bit_rev8 in the cascade byte (buf[3]).
 * Flip mode: 0x2 ↔ 0xC
 */
static void ook_counter_increment(HondaV7Frame* f) {
    uint8_t buf[HV7_FRAME_BYTES];
    ook_to_buf(f, buf);

    uint8_t src3 = buf[3];
    uint8_t src2 = buf[2];
    uint8_t tmp = hv7_bit_rev8((uint8_t)(hv7_bit_rev8(src3) + 1u));
    buf[3] = (tmp & 0xF0u) | (src3 & 0x0Fu);
    if((src3 >> 4) == 0x0Fu) {
        tmp = hv7_bit_rev8((uint8_t)(hv7_bit_rev8(src3) + 1u));
        buf[3] = (buf[3] & 0xF0u) | (tmp & 0x0Fu);
    }

    if(src3 == 0xFFu) {
        uint8_t hi_new = hv7_bit_rev8(
            (uint8_t)((hv7_bit_rev8(src2) >> 4) + 1u));
        buf[2] = (buf[2] & 0x0Fu) | (uint8_t)((hi_new & 0x0Fu) << 4);

        if((src2 >> 4) == 0x0Fu) {
            tmp = hv7_bit_rev8((uint8_t)(hv7_bit_rev8(src2) + 1u));
            buf[2] = (buf[2] & 0xF0u) | (tmp & 0x0Fu);
        }
    }

    f->mode = (f->mode == 0xCu) ? 0x2u : 0xCu;
    buf[7]  = (uint8_t)(((f->mode & 0x0Fu) << 4) | (buf[7] & 0x0Fu));

    f->counter = ((uint32_t)buf[4] << 16) |
                 ((uint32_t)buf[5] <<  8) |
                  (uint32_t)buf[6];

    f->checksum = ook_checksum(buf);
}

static uint8_t ook_classify(uint32_t dur) {
    if(dur + HV7_TE_DELTA >= HV7_TE_SHORT &&
       dur <= HV7_TE_SHORT + HV7_TE_DELTA) return 1u;
    if(dur + HV7_TE_DELTA >= HV7_TE_LONG &&
       dur <= HV7_TE_LONG  + HV7_TE_DELTA) return 2u;
    return 0u;
}

static void ook_decoder_reset_state(SubGhzProtocolDecoderHondaV7* inst) {
    inst->step           = HV7Step_Reset;
    inst->preamble_count = 0;
    inst->bit_count      = 0;
    inst->last_high_dur  = 0;
    memset(inst->data_buf, 0, sizeof(inst->data_buf));
}

static bool ook_validate_frame(SubGhzProtocolDecoderHondaV7* inst) {
    if(inst->bit_count < HV7_MIN_BITS) return false;

    uint8_t natural[HV7_FRAME_BYTES];
    uint8_t num_bytes = (inst->bit_count + 7u) / 8u;
    if(num_bytes > HV7_FRAME_BYTES) num_bytes = HV7_FRAME_BYTES;

    for(uint8_t i = 0; i < num_bytes; i++)
        natural[i] = hv7_bit_rev8(inst->data_buf[i]);

    FURI_LOG_D(TAG, "[OOK] bits=%u %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
               inst->bit_count,
               natural[0], natural[1], natural[2], natural[3], natural[4],
               natural[5], natural[6], natural[7], natural[8], natural[9], natural[10]);

    HondaV7Frame f;
    ook_from_buf(natural, &f);

    if(f.button == 0 || f.button > HV7_BTN_UNLOCK2)    return false;
    if(f.serial == 0 || f.serial == 0x0FFFFFFFu)        return false;

    uint8_t xchk = ook_checksum(natural);
    if(xchk != natural[8]) {
        if(popcount8(xchk ^ natural[8]) > 1) return false;
    }

    inst->frame       = f;
    inst->frame_valid = true;

    FURI_LOG_I(TAG, "[OOK] btn=%u ser=%07lX cnt=%06lX mode=%X",
               f.button,
               (unsigned long)f.serial,
               (unsigned long)f.counter,
               f.mode);
    return true;
}

static void ook_notify_frame(SubGhzProtocolDecoderHondaV7* inst) {
    inst->generic.data = hv7_pack(
        inst->frame.serial,
        inst->frame.button,
        (uint16_t)(inst->frame.counter & 0xFFFFu),
        inst->frame.mode,
        0xFu,
        HV7_PROTO_MODE,
        0u);
    inst->generic.data_count_bit = HV7_FRAME_BITS;
    inst->generic.serial = inst->frame.serial;
    inst->generic.btn    = inst->frame.button;
    inst->generic.cnt    = inst->frame.counter;
    hv7_register_btn(inst->frame.button);
    if(inst->base.callback)
        inst->base.callback(&inst->base, inst->base.context);
}

void* subghz_protocol_decoder_honda_v7_alloc(SubGhzEnvironment* env) {
    UNUSED(env);
    SubGhzProtocolDecoderHondaV7* inst =
        malloc(sizeof(SubGhzProtocolDecoderHondaV7));
    furi_check(inst);
    memset(inst, 0, sizeof(SubGhzProtocolDecoderHondaV7));
    inst->base.protocol         = &subghz_protocol_honda_v7;
    inst->generic.protocol_name = inst->base.protocol->name;
    inst->frame_valid           = false;
    FURI_LOG_I(TAG, "[OOK] decoder alloc");
    return inst;
}

void subghz_protocol_decoder_honda_v7_free(void* ctx) {
    furi_assert(ctx);
    free(ctx);
}

void subghz_protocol_decoder_honda_v7_reset(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV7* inst = ctx;
    ook_decoder_reset_state(inst);
}

/*
 * Feed state machine:
 *   Reset    → wait HIGH SHORT (preamble start)
 *   Preamble → count SHORT-SHORT cycles
 *   Data     → decode HIGH+LOW pairs
 *
 * Bit decode (LSB-first, accumulate in data_buf):
 *   HIGH SHORT + LOW SHORT → bit 0
 *   HIGH LONG  + LOW LONG  → bit 1
 *   Gap/invalid → try to decode frame
 */
void subghz_protocol_decoder_honda_v7_feed(
    void* ctx, bool level, uint32_t duration)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV7* inst = ctx;
    uint8_t c = ook_classify(duration);

    switch(inst->step) {

    case HV7Step_Reset:
        if(level && c == 1u) {
            inst->last_high_dur  = duration;
            inst->preamble_count = 0;
            inst->step           = HV7Step_Preamble;
        }
        break;

    case HV7Step_Preamble:
        if(!level) {
            if(ook_classify(inst->last_high_dur) == 1u && c == 1u) {
                inst->preamble_count++;
            } else if(c == 0u) {
                if(inst->preamble_count >= HV7_MIN_PREAMBLE) {
                    inst->bit_count = 0;
                    memset(inst->data_buf, 0, sizeof(inst->data_buf));
                    inst->step = HV7Step_Data;
                } else {
                    ook_decoder_reset_state(inst);
                }
            } else {

                if(inst->preamble_count >= HV7_MIN_PREAMBLE &&
                   ook_classify(inst->last_high_dur) == 2u && c == 2u) {
                    inst->bit_count = 0;
                    memset(inst->data_buf, 0, sizeof(inst->data_buf));
                    inst->data_buf[0] |= 0x01u;
                    inst->bit_count   = 1u;
                    inst->step        = HV7Step_Data;
                } else {
                    ook_decoder_reset_state(inst);
                }
            }
        } else {
            inst->last_high_dur = duration;
        }
        break;

    case HV7Step_Data:
        if(level) {
            inst->last_high_dur = duration;
        } else {
            uint8_t hc = ook_classify(inst->last_high_dur);

            if(hc == 1u && c == 1u) {
                if(inst->bit_count < (uint8_t)(HV7_FRAME_BITS))
                    inst->bit_count++;
            } else if(hc == 2u && c == 2u) {
                if(inst->bit_count < (uint8_t)(HV7_FRAME_BITS)) {
                    uint8_t byte_i = inst->bit_count / 8u;
                    uint8_t bit_i  = inst->bit_count % 8u;
                    if(byte_i < 16u)
                        inst->data_buf[byte_i] |= (uint8_t)(1u << bit_i);
                    inst->bit_count++;
                }
            } else {
                if(inst->bit_count >= HV7_MIN_BITS) {
                    if(ook_validate_frame(inst))
                        ook_notify_frame(inst);
                }
                ook_decoder_reset_state(inst);
                break;
            }

            if(inst->bit_count >= HV7_FRAME_BITS) {
                if(ook_validate_frame(inst))
                    ook_notify_frame(inst);
                ook_decoder_reset_state(inst);
            }
        }
        break;
    }

    inst->decoder.te_last = duration;
}

uint8_t subghz_protocol_decoder_honda_v7_get_hash_data(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV7* inst = ctx;
    return (uint8_t)(inst->generic.data ^
                    (inst->generic.data >>  8) ^
                    (inst->generic.data >> 16) ^
                    (inst->generic.data >> 24) ^
                    (inst->generic.data >> 32));
}

SubGhzProtocolStatus subghz_protocol_decoder_honda_v7_serialize(
    void* ctx, FlipperFormat* ff, SubGhzRadioPreset* preset)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV7* inst = ctx;
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
    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus subghz_protocol_decoder_honda_v7_deserialize(
    void* ctx, FlipperFormat* ff)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV7* inst = ctx;
    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize_check_count_bit(
            &inst->generic, ff, HV7_FRAME_BITS);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint32_t v = 0;
    if(!flipper_format_read_uint32(ff, "Serial", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.serial = v;

    if(!flipper_format_read_uint32(ff, "Counter", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.counter = v;

    if(!flipper_format_read_uint32(ff, "Button", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.button = (uint8_t)v;

    inst->frame.mode      = 0x2u;
    inst->frame.checksum  = 0u;
    inst->frame_valid     = true;
    inst->generic.serial  = inst->frame.serial;
    inst->generic.btn     = inst->frame.button;
    inst->generic.cnt     = inst->frame.counter;
    hv7_register_btn(inst->frame.button);
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_honda_v7_get_string(
    void* ctx, FuriString* out)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV7* inst = ctx;

    if(!inst->frame_valid && inst->generic.data != 0) {
        uint8_t pm, ki, fl, dm;
        uint16_t roll;
        hv7_unpack(inst->generic.data,
                   &inst->frame.serial, &inst->frame.button,
                   &roll, &dm, &ki, &pm, &fl);
        inst->frame.counter   = roll;
        inst->frame.mode      = dm;
        inst->frame_valid     = true;
    }

    furi_string_cat_printf(
        out,
        "%s %ubit\r\n"
        "Btn:%s (0x%X)\r\n"
        "Ser:%07lX\r\n"
        "Cnt:%06lX Mode:%X\r\n",
        SUBGHZ_PROTOCOL_HONDA_V7_NAME,
        inst->generic.data_count_bit,
        hv7_btn_name(inst->frame.button),
        inst->frame.button,
        (unsigned long)inst->frame.serial,
        (unsigned long)inst->frame.counter,
        inst->frame.mode);
}

static void ook_build_upload(SubGhzProtocolEncoderHondaV7* inst) {
    LevelDuration* buf = inst->encoder.upload;
    size_t idx = 0;

    buf[idx++] = level_duration_make(true,  HV7_GUARD_HIGH_US);
    buf[idx++] = level_duration_make(false, HV7_GUARD_LOW_US);

    for(uint16_t p = 0;
        p < HV7_PREAMBLE_CYCLES && idx < HV7_ENC_BUF_SIZE - 10u;
        p++) {
        buf[idx++] = level_duration_make(true,  HV7_TE_SHORT);
        buf[idx++] = level_duration_make(false, HV7_TE_SHORT);
    }

    uint8_t frame_buf[HV7_FRAME_BYTES];
    inst->frame.button = inst->active_button;
    ook_to_buf(&inst->frame, frame_buf);

    for(uint8_t byte_i = 0;
        byte_i < HV7_FRAME_BYTES && idx < HV7_ENC_BUF_SIZE - 4u;
        byte_i++) {
        uint8_t bval = frame_buf[byte_i];
        for(uint8_t bit_i = 0; bit_i < 8u; bit_i++) {
            bool bit = (bval >> bit_i) & 1u;
            if(bit) {
                buf[idx++] = level_duration_make(true,  HV7_TE_LONG);
                buf[idx++] = level_duration_make(false, HV7_TE_LONG);
            } else {
                buf[idx++] = level_duration_make(true,  HV7_TE_SHORT);
                buf[idx++] = level_duration_make(false, HV7_TE_SHORT);
            }
        }
    }

    buf[idx++] = level_duration_make(false, 4000u);

    inst->encoder.size_upload = idx;
    inst->encoder.front       = 0;
    FURI_LOG_D(TAG, "[OOK] upload %u symbols", (unsigned)idx);
}

void* subghz_protocol_encoder_honda_v7_alloc(SubGhzEnvironment* env) {
    UNUSED(env);
    SubGhzProtocolEncoderHondaV7* inst =
        malloc(sizeof(SubGhzProtocolEncoderHondaV7));
    furi_check(inst);
    memset(inst, 0, sizeof(SubGhzProtocolEncoderHondaV7));
    inst->base.protocol         = &subghz_protocol_honda_v7;
    inst->generic.protocol_name = inst->base.protocol->name;
    inst->encoder.repeat        = 3;
    inst->encoder.upload        =
        malloc(HV7_ENC_BUF_SIZE * sizeof(LevelDuration));
    furi_check(inst->encoder.upload);
    inst->encoder.is_running    = false;
    return inst;
}

void subghz_protocol_encoder_honda_v7_free(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolEncoderHondaV7* inst = ctx;
    free(inst->encoder.upload);
    free(inst);
}

void subghz_protocol_encoder_honda_v7_stop(void* ctx) {
    furi_assert(ctx);
    ((SubGhzProtocolEncoderHondaV7*)ctx)->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_honda_v7_yield(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolEncoderHondaV7* inst = ctx;
    if(!inst->encoder.is_running || inst->encoder.repeat == 0)
        return level_duration_reset();
    LevelDuration ret = inst->encoder.upload[inst->encoder.front];
    if(++inst->encoder.front >= inst->encoder.size_upload) {
        inst->encoder.repeat--;
        inst->encoder.front = 0;
    }
    return ret;
}

SubGhzProtocolStatus subghz_protocol_encoder_honda_v7_deserialize(
    void* ctx, FlipperFormat* ff)
{
    furi_assert(ctx);
    SubGhzProtocolEncoderHondaV7* inst = ctx;
    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize(&inst->generic, ff);
    if(ret != SubGhzProtocolStatusOk) return ret;

    uint32_t v = 0;
    if(!flipper_format_read_uint32(ff, "Serial", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.serial = v;

    if(!flipper_format_read_uint32(ff, "Counter", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.counter = v;

    if(!flipper_format_read_uint32(ff, "Button", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    inst->frame.button = (uint8_t)v;
    inst->frame.mode   = 0x2u;

    hv7_register_btn(inst->frame.button);
    inst->active_button = hv7_get_active_btn();

    inst->frame.counter = (inst->frame.counter +
        (uint32_t)furi_hal_subghz_get_rolling_counter_mult()) & 0x00FFFFFFu;
    ook_counter_increment(&inst->frame);
    inst->frame.button  = inst->active_button;

    inst->generic.cnt = inst->frame.counter;
    inst->generic.btn = inst->active_button;
    inst->generic.data = hv7_pack(
        inst->frame.serial, inst->active_button,
        (uint16_t)(inst->frame.counter & 0xFFFFu),
        inst->frame.mode, 0xFu, HV7_PROTO_MODE, 0u);

    flipper_format_rewind(ff);
    uint8_t kd[8];
    for(int i = 0; i < 8; i++)
        kd[i] = (uint8_t)(inst->generic.data >> (56 - i * 8));
    flipper_format_update_hex(ff, "Key", kd, 8);

    ook_build_upload(inst);
    inst->encoder.is_running = true;

    FURI_LOG_I(TAG, "[OOK] encoder ready ser=%07lX btn=%u cnt=%06lX",
               (unsigned long)inst->frame.serial,
               inst->active_button,
               (unsigned long)inst->frame.counter);
    return SubGhzProtocolStatusOk;
}

static SubGhzBlockGeneric* subghz_protocol_decoder_honda_v7_get_generic(void* context) {
    SubGhzProtocolDecoderHondaV7* instance = context;
    return &instance->generic;
}

const SubGhzProtocolDecoder subghz_protocol_honda_v7_decoder = {
    .alloc         = subghz_protocol_decoder_honda_v7_alloc,
    .free          = subghz_protocol_decoder_honda_v7_free,
    .feed          = subghz_protocol_decoder_honda_v7_feed,
    .reset         = subghz_protocol_decoder_honda_v7_reset,
    .get_hash_data = subghz_protocol_decoder_honda_v7_get_hash_data,
    .serialize     = subghz_protocol_decoder_honda_v7_serialize,
    .deserialize   = subghz_protocol_decoder_honda_v7_deserialize,
    .get_string    = subghz_protocol_decoder_honda_v7_get_string,
    .get_generic   = subghz_protocol_decoder_honda_v7_get_generic,
};

const SubGhzProtocolEncoder subghz_protocol_honda_v7_encoder = {
    .alloc       = subghz_protocol_encoder_honda_v7_alloc,
    .free        = subghz_protocol_encoder_honda_v7_free,
    .deserialize = subghz_protocol_encoder_honda_v7_deserialize,
    .stop        = subghz_protocol_encoder_honda_v7_stop,
    .yield       = subghz_protocol_encoder_honda_v7_yield,
};

const SubGhzProtocol subghz_protocol_honda_v7 = {
    .name    = SUBGHZ_PROTOCOL_HONDA_V7_NAME,
    .type    = SubGhzProtocolTypeDynamic,
    .flag    = SubGhzProtocolFlag_433 |
               SubGhzProtocolFlag_315 |
               SubGhzProtocolFlag_AM  |
               SubGhzProtocolFlag_Decodable |
               SubGhzProtocolFlag_Load |
               SubGhzProtocolFlag_Save |
               SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_honda_v7_decoder,
    .encoder = &subghz_protocol_honda_v7_encoder,
};
