/*
 * honda_v10.c — Honda V2H4 Aftermarket OEM FSK Protocol (decoder only)
 *
 * Native 2FSK Manchester at 104µs half-bit, MSB-first, 96-bit frame.
 * Structure reverse-engineered from HondaV2H4_OEM_300lck.sub and _300ulk.sub:
 * first 3 bytes = device fingerprint, byte 3 high nibble = button,
 * bytes 4..6 = counter, bytes 8..11 = cipher/CRC tail.
 *
 * Encoder not yet implemented — cipher algorithm over bytes 8..11
 * has not been reverse engineered.
 */

#include "honda_v10.h"

#include <furi.h>
#include <furi_hal.h>
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h>
#include <lib/subghz/blocks/custom_btn_i.h>

#define TAG "HondaV10"

static const char* hv10_btn_name(uint8_t btn) {
    switch(btn) {
    case HV10_BTN_LOCK:   return "Lock";
    case HV10_BTN_UNLOCK: return "Unlock";
    default:              return "Unknown";
    }
}

static bool hv10_btn_is_known(uint8_t btn) {
    return btn == HV10_BTN_LOCK || btn == HV10_BTN_UNLOCK;
}

/* The raw counter bytes in the frame are bitwise inverted
 * (fresh EEPROM 0xFF..FF = count 0). We store the real counter in
 * f->counter and invert on the wire. */
static uint64_t hv10_pack(const HondaV10Frame* f) {
    /* 64-bit generic.data layout:
     *   [63:40] 24-bit serial
     *   [39:36] button
     *   [35:32] flags
     *   [31:8]  24-bit counter (real value, NOT inverted)
     *   [7:0]   status
     */
    return ((uint64_t)(f->serial  & 0x00FFFFFFu) << 40) |
           ((uint64_t)(f->button  & 0x0Fu)        << 36) |
           ((uint64_t)(f->flags   & 0x0Fu)        << 32) |
           ((uint64_t)(f->counter & 0x00FFFFFFu) <<  8) |
            (uint64_t)(f->status  & 0xFFu);
}

static void hv10_unpack(uint64_t raw, HondaV10Frame* f) {
    f->serial  = (uint32_t)((raw >> 40) & 0x00FFFFFFu);
    f->button  = (uint8_t) ((raw >> 36) & 0x0Fu);
    f->flags   = (uint8_t) ((raw >> 32) & 0x0Fu);
    f->counter = (uint32_t)((raw >>  8) & 0x00FFFFFFu);
    f->status  = (uint8_t) ( raw        & 0xFFu);
}

const SubGhzProtocolDecoder subghz_protocol_honda_v10_decoder;
const SubGhzProtocol        subghz_protocol_honda_v10;

static inline uint8_t fsk_classify(uint32_t dur) {
    if(dur + HV10_TE_DELTA >= HV10_TE_SHORT &&
       dur <= HV10_TE_SHORT + HV10_TE_DELTA) return 1u;
    if(dur + HV10_TE_DELTA >= HV10_TE_LONG &&
       dur <= HV10_TE_LONG  + HV10_TE_DELTA) return 2u;
    return 0u;
}

static void fsk_from_buf(
    const uint8_t buf[HV10_FRAME_BYTES],
    HondaV10Frame* f)
{
    f->serial  = ((uint32_t)buf[0] << 16) |
                 ((uint32_t)buf[1] <<  8) |
                  (uint32_t)buf[2];
    f->button  = (buf[3] >> 4) & 0x0Fu;
    f->flags   =  buf[3]       & 0x0Fu;
    /* Counter is stored bitwise-inverted in the frame (likely EEPROM
     * life optimisation — fresh 0xFFFFFF maps to real counter 0). */
    f->counter = 0x00FFFFFFu ^ (((uint32_t)buf[4] << 16) |
                                ((uint32_t)buf[5] <<  8) |
                                 (uint32_t)buf[6]);
    f->status  = buf[7];
    f->tail    = ((uint32_t)buf[8]  << 24) |
                 ((uint32_t)buf[9]  << 16) |
                 ((uint32_t)buf[10] <<  8) |
                  (uint32_t)buf[11];
}

/* Same Manchester decoder as HondaV9: MSB-first, pairs of half-bits.
 * (0,1) → bit 1, (1,0) → bit 0 (invert swaps).
 *
 * Searches for an alternating preamble beginning at start_ofs, returns
 * in *consumed the first half-bit index past this frame so the caller
 * can continue scanning for further repetitions in the same burst. */
static bool fsk_manchester_decode(
    const uint8_t* hb, uint16_t hb_count, uint16_t start_ofs,
    uint8_t* decoded, uint8_t* out_bits, uint16_t* consumed,
    uint8_t max_bits, bool invert)
{
    uint8_t bit_count = 0;
    *consumed = start_ofs;
    memset(decoded, 0, (max_bits + 7u) / 8u);

    if(start_ofs + 1 >= hb_count) { *out_bits = 0; return false; }

    /* Locate a run of alternating half-bits of minimum length. */
    uint16_t alt_start = start_ofs;
    while(alt_start + 1 < hb_count) {
        /* Scan alternation run starting here. */
        uint16_t alt_end = alt_start + 1;
        while(alt_end < hb_count && hb[alt_end] != hb[alt_end-1]) alt_end++;
        uint16_t run_len = alt_end - alt_start;
        if(run_len >= HV10_MIN_PREAMBLE && alt_end < hb_count) {
            /* Preamble ends at alt_end. Walk past any repeated half-bits
             * (sync gap between preamble and payload). */
            uint16_t i = alt_end;
            while(i + 1 < hb_count && hb[i] == hb[i+1]) i++;

            if(max_bits > (HV10_FRAME_BYTES + 1) * 8u) {
                max_bits = (HV10_FRAME_BYTES + 1) * 8u;
            }

            bit_count = 0;
            memset(decoded, 0, (max_bits + 7u) / 8u);

            while(i + 1 < hb_count && bit_count < max_bits) {
                uint8_t h0 = hb[i];
                uint8_t h1 = hb[i+1];
                if(h0 == h1) break;

                uint8_t bit_val;
                if(!invert) bit_val = (h0 == 0u && h1 == 1u) ? 1u : 0u;
                else        bit_val = (h0 == 1u && h1 == 0u) ? 1u : 0u;

                uint8_t byte_idx = bit_count / 8u;
                uint8_t bit_idx  = 7u - (bit_count % 8u);
                if(bit_val)
                    decoded[byte_idx] |= (uint8_t)(1u << bit_idx);

                bit_count++;
                i += 2;
            }

            *out_bits = bit_count;
            *consumed = i;
            return bit_count >= HV10_MIN_BITS;
        }
        /* This run was too short — advance past it and keep scanning. */
        alt_start = alt_end;
    }
    *out_bits = 0;
    return false;
}

static bool fsk_try_decode_polarity_at(
    SubGhzProtocolDecoderHondaV10* inst, bool invert,
    uint16_t start_ofs, uint16_t* next_ofs)
{
    uint8_t decoded[HV10_FRAME_BYTES + 1] = {0};
    uint8_t bit_count = 0;
    uint16_t consumed = start_ofs;

    if(!fsk_manchester_decode(
        inst->half_bits, inst->hb_count, start_ofs,
        decoded, &bit_count, &consumed,
        HV10_FRAME_BITS, invert)) {
        *next_ofs = inst->hb_count;
        return false;
    }

    *next_ofs = consumed;

    if(bit_count < HV10_MIN_BITS) return false;

    HondaV10Frame f;
    fsk_from_buf(decoded, &f);

    if(!hv10_btn_is_known(f.button))              return false;
    if(f.serial == 0u || f.serial == 0x00FFFFFFu) return false;
    /* Observed constant across all V2H4 captures. Drops random-noise matches. */
    if(f.flags != 0x7u)                           return false;

    inst->frame       = f;
    inst->frame_valid = true;
    FURI_LOG_D(TAG,
        "[V10] frame pol=%s btn=%s ser=%06lX cnt=%06lX typ=%c st=%02X",
        invert ? "INV" : "NOR",
        hv10_btn_name(f.button),
        (unsigned long)f.serial,
        (unsigned long)f.counter,
        (f.status & 0x80u) ? 'A' : 'B',
        f.status);
    return true;
}

/* Iterates through hb[] decoding every repetition within the burst.
 * V2H4 emits four frames per press (AABB pattern) seamlessly in one
 * burst, so the caller must continue past each decoded frame to catch
 * the rest. Emits the protocol callback once per accepted frame. */
static uint16_t fsk_try_decode(SubGhzProtocolDecoderHondaV10* inst) {
    if(inst->hb_count < (HV10_MIN_PREAMBLE + 16u)) return 0;

    uint16_t emitted = 0;
    uint16_t ofs = 0;
    while(ofs + HV10_MIN_PREAMBLE + 16u <= inst->hb_count) {
        uint16_t next_nor = inst->hb_count;
        uint16_t next_inv = inst->hb_count;
        bool ok_nor = fsk_try_decode_polarity_at(inst, false, ofs, &next_nor);
        bool ok_inv = !ok_nor && fsk_try_decode_polarity_at(inst, true, ofs, &next_inv);

        if(ok_nor || ok_inv) {
            inst->generic.data = hv10_pack(&inst->frame);
            inst->generic.data_count_bit = HV10_FRAME_BITS;
            inst->generic.serial = inst->frame.serial;
            inst->generic.btn    = inst->frame.button;
            inst->generic.cnt    = inst->frame.counter;
            if(inst->base.callback)
                inst->base.callback(&inst->base, inst->base.context);
            emitted++;
            uint16_t next = ok_nor ? next_nor : next_inv;
            /* Guarantee forward progress even on pathological decoder state. */
            ofs = (next > ofs) ? next : ofs + 1;
            continue;
        }
        /* Nothing decodable here — skip past the next alternation run. */
        uint16_t skip = ofs + 1;
        while(skip < inst->hb_count &&
              inst->half_bits[skip] != inst->half_bits[skip-1]) skip++;
        ofs = (skip > ofs) ? skip : ofs + 1;
    }
    return emitted;
}

void* subghz_protocol_decoder_honda_v10_alloc(SubGhzEnvironment* env) {
    UNUSED(env);
    SubGhzProtocolDecoderHondaV10* inst =
        malloc(sizeof(SubGhzProtocolDecoderHondaV10));
    furi_check(inst);
    memset(inst, 0, sizeof(SubGhzProtocolDecoderHondaV10));
    inst->base.protocol         = &subghz_protocol_honda_v10;
    inst->generic.protocol_name = inst->base.protocol->name;
    inst->frame_valid           = false;
    FURI_LOG_I(TAG, "[V10] decoder alloc");
    return inst;
}

void subghz_protocol_decoder_honda_v10_free(void* ctx) {
    furi_assert(ctx);
    free(ctx);
}

void subghz_protocol_decoder_honda_v10_reset(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV10* inst = ctx;
    inst->decoder.parser_step = 0;
    inst->decoder.te_last     = 0;
    inst->hb_count            = 0;
}

void subghz_protocol_decoder_honda_v10_feed(
    void* ctx, bool level, uint32_t duration)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV10* inst = ctx;
    uint8_t lvl = level ? 1u : 0u;
    uint8_t dc  = fsk_classify(duration);

    if(dc == 1u) {
        if(inst->hb_count < HV10_HALF_BIT_BUF)
            inst->half_bits[inst->hb_count++] = lvl;
    } else if(dc == 2u) {
        if(inst->hb_count + 2u <= HV10_HALF_BIT_BUF) {
            inst->half_bits[inst->hb_count++] = lvl;
            inst->half_bits[inst->hb_count++] = lvl;
        }
    } else {
        /* Real guard gap (≥1000µs) — frame boundary. */
        if(inst->hb_count >= (HV10_MIN_PREAMBLE + 16u)) {
            fsk_try_decode(inst);
        }
        inst->hb_count = 0;
    }

    inst->decoder.te_last = duration;
}

uint8_t subghz_protocol_decoder_honda_v10_get_hash_data(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV10* inst = ctx;
    return (uint8_t)(inst->generic.data ^
                    (inst->generic.data >>  8) ^
                    (inst->generic.data >> 16) ^
                    (inst->generic.data >> 24) ^
                    (inst->generic.data >> 32));
}

SubGhzProtocolStatus subghz_protocol_decoder_honda_v10_serialize(
    void* ctx, FlipperFormat* ff, SubGhzRadioPreset* preset)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV10* inst = ctx;
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
    v = inst->frame.flags;
    if(!flipper_format_write_uint32(ff, "Flags", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    v = inst->frame.status;
    if(!flipper_format_write_uint32(ff, "Status", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    v = inst->frame.tail;
    if(!flipper_format_write_uint32(ff, "Tail", &v, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus subghz_protocol_decoder_honda_v10_deserialize(
    void* ctx, FlipperFormat* ff)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV10* inst = ctx;
    SubGhzProtocolStatus ret =
        subghz_block_generic_deserialize_check_count_bit(
            &inst->generic, ff, HV10_FRAME_BITS);
    if(ret != SubGhzProtocolStatusOk) return ret;

    hv10_unpack(inst->generic.data, &inst->frame);
    // hv10_unpack restores only the 64-bit data; the 32-bit cipher Tail is stored
    // in its own field and must be read back or it reloads as 0.
    uint32_t hv10_tail = 0;
    flipper_format_rewind(ff);
    if(flipper_format_read_uint32(ff, "Tail", &hv10_tail, 1)) inst->frame.tail = hv10_tail;
    inst->frame_valid        = true;
    inst->generic.serial     = inst->frame.serial;
    inst->generic.btn        = inst->frame.button;
    inst->generic.cnt        = inst->frame.counter;
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_honda_v10_get_string(
    void* ctx, FuriString* out)
{
    furi_assert(ctx);
    SubGhzProtocolDecoderHondaV10* inst = ctx;

    if(!inst->frame_valid && inst->generic.data != 0) {
        hv10_unpack(inst->generic.data, &inst->frame);
        inst->frame_valid = true;
    }

    furi_string_cat_printf(
        out,
        "%s %ubit\r\n"
        "Btn:%s (0x%X) Flags:%X\r\n"
        "Ser:%06lX\r\n"
        "Cnt:%06lX (%lu) Typ:%c St:%02X\r\n"
        "Tail:%08lX\r\n",
        SUBGHZ_PROTOCOL_HONDA_V10_NAME,
        inst->generic.data_count_bit,
        hv10_btn_name(inst->frame.button),
        inst->frame.button,
        inst->frame.flags,
        (unsigned long)inst->frame.serial,
        (unsigned long)inst->frame.counter,
        (unsigned long)inst->frame.counter,
        (inst->frame.status & 0x80u) ? 'A' : 'B',
        inst->frame.status,
        (unsigned long)inst->frame.tail);
}

static SubGhzBlockGeneric* subghz_protocol_decoder_honda_v10_get_generic(void* context) {
    SubGhzProtocolDecoderHondaV10* instance = context;
    return &instance->generic;
}

// ============================================================================
// ENCODER
//
// Rebuilds the 12-byte frame (counter bitwise-inverted on the wire), expands it
// to native 2FSK Manchester half-bit cells (bit1 -> {0,1}, bit0 -> {1,0}), then
// renders equal-adjacent cells into SHORT/LONG pulses. A short alternating
// preamble ends on the value of the first payload half-bit so it merges into the
// decoder's sync gap and the first Manchester pair lands correctly aligned.
// Verified to round-trip fsk_try_decode() bit-for-bit.
// ============================================================================

#define HV10_ENC_PREAMBLE_CELLS 24
#define HV10_ENC_UPLOAD_SIZE    (HV10_ENC_PREAMBLE_CELLS + HV10_FRAME_BITS * 2 + 2)

static void hv10_frame_to_buf(const HondaV10Frame* f, uint8_t buf[HV10_FRAME_BYTES]) {
    buf[0] = (uint8_t)(f->serial >> 16);
    buf[1] = (uint8_t)(f->serial >> 8);
    buf[2] = (uint8_t)(f->serial);
    buf[3] = (uint8_t)(((f->button & 0x0Fu) << 4) | (f->flags & 0x0Fu));
    uint32_t inv = 0x00FFFFFFu ^ (f->counter & 0x00FFFFFFu);
    buf[4] = (uint8_t)(inv >> 16);
    buf[5] = (uint8_t)(inv >> 8);
    buf[6] = (uint8_t)(inv);
    buf[7] = f->status;
    buf[8]  = (uint8_t)(f->tail >> 24);
    buf[9]  = (uint8_t)(f->tail >> 16);
    buf[10] = (uint8_t)(f->tail >> 8);
    buf[11] = (uint8_t)(f->tail);
}

static void subghz_protocol_encoder_honda_v10_get_upload(SubGhzProtocolEncoderHondaV10* inst) {
    uint8_t buf[HV10_FRAME_BYTES];
    hv10_frame_to_buf(&inst->frame, buf);

    // Build the half-bit cell stream.
    uint8_t cells[HV10_ENC_PREAMBLE_CELLS + HV10_FRAME_BITS * 2];
    int n = 0;

    // c0 = first payload half-bit = first frame bit? 1 -> cell 0, 0 -> cell 1.
    uint8_t b0 = (buf[0] >> 7) & 1u;
    uint8_t c0 = b0 ? 0u : 1u;

    const int L = HV10_ENC_PREAMBLE_CELLS;
    for(int i = 0; i < L; i++)
        cells[n++] = (((L - 1 - i) % 2) == 0) ? c0 : (uint8_t)(1u - c0);

    for(int by = 0; by < HV10_FRAME_BYTES; by++) {
        for(int k = 0; k < 8; k++) {
            uint8_t bit = (buf[by] >> (7 - k)) & 1u;
            if(bit) {
                cells[n++] = 0u;
                cells[n++] = 1u;
            } else {
                cells[n++] = 1u;
                cells[n++] = 0u;
            }
        }
    }

    // Render equal-adjacent cells (max run 2) into pulses; level = cell value.
    size_t idx = 0;
    const size_t enc_cap = HV10_ENC_UPLOAD_SIZE;
    int i = 0;
    while(i < n) {
        int j = i + 1;
        if(j < n && cells[j] == cells[i]) j++; // run of at most 2
        int rl = j - i;
        if(idx < enc_cap)
            inst->encoder.upload[idx++] =
                level_duration_make(cells[i] != 0, (uint32_t)(rl == 2 ? HV10_TE_LONG : HV10_TE_SHORT));
        i = j;
    }

    // Guard gap (>= HV10_GUARD_TIME_US) flushes the decoder.
    if(idx < enc_cap) inst->encoder.upload[idx++] = level_duration_make(false, (uint32_t)5000);
    inst->encoder.size_upload = idx;
}

void* subghz_protocol_encoder_honda_v10_alloc(SubGhzEnvironment* env) {
    UNUSED(env);
    SubGhzProtocolEncoderHondaV10* inst = malloc(sizeof(SubGhzProtocolEncoderHondaV10));
    furi_check(inst);
    memset(inst, 0, sizeof(SubGhzProtocolEncoderHondaV10));
    inst->base.protocol = &subghz_protocol_honda_v10;
    inst->generic.protocol_name = inst->base.protocol->name;
    inst->encoder.repeat = 4; // real fob emits ~4 reps per press
    inst->encoder.size_upload = HV10_ENC_UPLOAD_SIZE;
    inst->encoder.upload = malloc(inst->encoder.size_upload * sizeof(LevelDuration));
    inst->encoder.is_running = false;
    return inst;
}

void subghz_protocol_encoder_honda_v10_free(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolEncoderHondaV10* inst = ctx;
    free(inst->encoder.upload);
    free(inst);
}

SubGhzProtocolStatus subghz_protocol_encoder_honda_v10_deserialize(void* ctx, FlipperFormat* ff) {
    furi_assert(ctx);
    SubGhzProtocolEncoderHondaV10* inst = ctx;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    do {
        if(subghz_block_generic_deserialize_check_count_bit(
               &inst->generic, ff, HV10_FRAME_BITS) != SubGhzProtocolStatusOk) {
            break;
        }
        hv10_unpack(inst->generic.data, &inst->frame);
        uint32_t tail = 0;
        flipper_format_rewind(ff);
        if(flipper_format_read_uint32(ff, "Tail", &tail, 1)) inst->frame.tail = tail;

        flipper_format_read_uint32(ff, "Repeat", (uint32_t*)&inst->encoder.repeat, 1);

        subghz_protocol_encoder_honda_v10_get_upload(inst);
        inst->encoder.front = 0;
        inst->encoder.is_running = true;
        res = SubGhzProtocolStatusOk;
    } while(false);
    return res;
}

void subghz_protocol_encoder_honda_v10_stop(void* ctx) {
    furi_assert(ctx);
    SubGhzProtocolEncoderHondaV10* inst = ctx;
    inst->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_honda_v10_yield(void* ctx) {
    SubGhzProtocolEncoderHondaV10* inst = ctx;
    if(inst->encoder.repeat == 0 || !inst->encoder.is_running) {
        inst->encoder.is_running = false;
        return level_duration_reset();
    }
    LevelDuration ret = inst->encoder.upload[inst->encoder.front];
    if(++inst->encoder.front == inst->encoder.size_upload) {
        if(!subghz_block_generic_global.endless_tx) inst->encoder.repeat--;
        inst->encoder.front = 0;
    }
    return ret;
}

const SubGhzProtocolEncoder subghz_protocol_honda_v10_encoder = {
    .alloc       = subghz_protocol_encoder_honda_v10_alloc,
    .free        = subghz_protocol_encoder_honda_v10_free,
    .deserialize = subghz_protocol_encoder_honda_v10_deserialize,
    .stop        = subghz_protocol_encoder_honda_v10_stop,
    .yield       = subghz_protocol_encoder_honda_v10_yield,
};

const SubGhzProtocolDecoder subghz_protocol_honda_v10_decoder = {
    .alloc         = subghz_protocol_decoder_honda_v10_alloc,
    .free          = subghz_protocol_decoder_honda_v10_free,
    .feed          = subghz_protocol_decoder_honda_v10_feed,
    .reset         = subghz_protocol_decoder_honda_v10_reset,
    .get_hash_data = subghz_protocol_decoder_honda_v10_get_hash_data,
    .serialize     = subghz_protocol_decoder_honda_v10_serialize,
    .deserialize   = subghz_protocol_decoder_honda_v10_deserialize,
    .get_string    = subghz_protocol_decoder_honda_v10_get_string,
    .get_generic   = subghz_protocol_decoder_honda_v10_get_generic,
};

const SubGhzProtocol subghz_protocol_honda_v10 = {
    .name    = SUBGHZ_PROTOCOL_HONDA_V10_NAME,
    .type    = SubGhzProtocolTypeDynamic,
    .flag    = SubGhzProtocolFlag_433 |
               SubGhzProtocolFlag_FM  |
               SubGhzProtocolFlag_Decodable |
               SubGhzProtocolFlag_Load |
               SubGhzProtocolFlag_Save |
               SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_honda_v10_decoder,
    .encoder = &subghz_protocol_honda_v10_encoder,
};
