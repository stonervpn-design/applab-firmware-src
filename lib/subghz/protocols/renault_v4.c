/**
 * Renault V4 — Short-burst keyfob decoder (Kangoo and similar).
 *
 * Signal structure per burst:
 *   [~1000µs HIGH sync] [~15 preamble 1-bits] [~10 data bits]
 *
 * Manchester encoding with TE_SHORT=125µs, TE_LONG=250µs.
 * Hundreds of identical bursts per button press.
 *
 * Observed data patterns (10 data bits after preamble):
 *   Lock:   0x9D-0x9E  (long press), 0x97-0x98 (short press)
 *   Unlock: 0x9A-0x9B  (long press), 0x95-0x96 (short press)
 */
#include "renault_v4.h"

#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/generic.h"
#include "../blocks/math.h"
#include "../blocks/custom_btn_i.h"

#define TAG "SubGhzProtocolRenaultV4"

static const SubGhzBlockConst subghz_protocol_renault_v4_const = {
    .te_short = RENAULT_V4_TE_SHORT,
    .te_long  = RENAULT_V4_TE_LONG,
    .te_delta = RENAULT_V4_TE_DELTA,
    .min_count_bit_for_found = RENAULT_V4_MIN_BITS,
};

/* ── frame data ──────────────────────────────────────────────────────── */

typedef struct SubGhzProtocolDecoderRenaultV4 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder        decoder;
    SubGhzBlockGeneric        generic;

    uint8_t  manchester_state;
    uint8_t  all_bits[128];
    uint16_t all_bit_count;
    bool     synced;

    /* Repeat-dedup: a held button emits hundreds of identical bursts; fire the
     * callback once per unique frame instead of once per burst. */
    uint8_t  last_frame[11];
    bool     last_frame_valid;
} SubGhzProtocolDecoderRenaultV4;

typedef struct SubGhzProtocolEncoderRenaultV4 {
    SubGhzProtocolEncoderBase   base;
    SubGhzProtocolBlockEncoder  encoder;
    SubGhzBlockGeneric          generic;
} SubGhzProtocolEncoderRenaultV4;

const SubGhzProtocolDecoder subghz_protocol_renault_v4_decoder;
const SubGhzProtocolEncoder subghz_protocol_renault_v4_encoder;
const SubGhzProtocol        subghz_protocol_renault_v4;

/* ── Manchester helpers ──────────────────────────────────────────────── */

/* Returns r1 code for Renault Manchester convention:
 *   SHORT HIGH=0, SHORT LOW=1, LONG HIGH=2, LONG LOW=3 */
static uint8_t _rv4_calc_r1(bool level, bool is_long) {
    if(!is_long) return level ? 0u : 1u;
    return level ? 2u : 3u;
}

/* Manchester state machine — same convention as Renault V0.
 * Returns true and sets *out when a full bit is decoded. */
static bool _rv4_manchester_decode(SubGhzProtocolDecoderRenaultV4* inst,
                                    uint8_t r1, uint8_t* out) {
    uint8_t s = inst->manchester_state;
    switch(r1) {
    case 0: /* SHORT HIGH */
        if(s == 0 || s == 2) { inst->manchester_state = 1; return false; }
        if(s == 1) { *out = 0; inst->manchester_state = 0; return true; }
        if(s == 3) { *out = 0; inst->manchester_state = 2; return true; }
        break;
    case 1: /* SHORT LOW */
        if(s == 0 || s == 2) { inst->manchester_state = 3; return false; }
        if(s == 1) { *out = 1; inst->manchester_state = 2; return true; }
        if(s == 3) { *out = 1; inst->manchester_state = 0; return true; }
        break;
    case 2: /* LONG HIGH */
        if(s == 0 || s == 2) { *out = 0; inst->manchester_state = 0; return true; }
        if(s == 1 || s == 3) { inst->manchester_state = 1; return false; }
        break;
    case 3: /* LONG LOW */
        if(s == 0 || s == 2) { *out = 1; inst->manchester_state = 2; return true; }
        if(s == 1 || s == 3) { inst->manchester_state = 3; return false; }
        break;
    }
    return false;
}

/* ── process a complete burst ────────────────────────────────────────── */

static void _rv4_dump_bits(const char* tag, const uint8_t* bits, int n) {
    char buf[260];
    int len = 0;
    for(int i = 0; i < n && len < (int)sizeof(buf) - 2; i++) {
        buf[len++] = bits[i] ? '1' : '0';
        if(((i + 1) % 8) == 0 && len < (int)sizeof(buf) - 1) buf[len++] = ' ';
    }
    buf[len] = 0;
    FURI_LOG_I(TAG, "%s (n=%d): %s", tag, n, buf);
}

static void _rv4_process_frame(SubGhzProtocolDecoderRenaultV4* inst) {
    FURI_LOG_I(TAG, "=== process_frame: total_bits=%u ===", inst->all_bit_count);
    _rv4_dump_bits("RAW bits", inst->all_bits, inst->all_bit_count);

    /* Try both polarities: preamble is all-1s or all-0s */
    for(int inv = 0; inv < 2; inv++) {
        uint8_t preamble_val = inv ? 0u : 1u;

        int preamble = 0;
        while(preamble < inst->all_bit_count &&
              inst->all_bits[preamble] == preamble_val)
            preamble++;

        FURI_LOG_I(TAG, "  try inv=%d (preamble_val=%u): preamble_len=%d",
                   inv, preamble_val, preamble);

        if(preamble < 10) continue;

        int data_start = preamble + 1; /* skip sync bit (0 after 1-preamble) */
        int data_len = inst->all_bit_count - data_start;
        FURI_LOG_I(TAG, "  data_start=%d data_len=%d (MIN=%u FRAME=%u)",
                   data_start, data_len,
                   (unsigned)RENAULT_V4_MIN_BITS, (unsigned)RENAULT_V4_FRAME_BITS);
        /* Strict length gate: a genuine frame is exactly RENAULT_V4_FRAME_BITS
         * (88) data bits. Requiring the burst to land in [88, 88+4] rejects a
         * foreign signal (e.g. a Fiat V1 burst read through this Manchester SM)
         * that assembles a different bit count instead of silently truncating it
         * to 88 and letting the XOR coincidence through. */
        if(data_len < (int)RENAULT_V4_FRAME_BITS ||
           data_len > (int)RENAULT_V4_FRAME_BITS + 4) continue;

        /* Pack data bits into byte array — no inversion needed,
         * the preamble polarity just indicates Manchester convention */
        uint8_t frame[12] = {0};
        int use_bits = data_len > (int)RENAULT_V4_FRAME_BITS ? (int)RENAULT_V4_FRAME_BITS : data_len;
        for(int i = 0; i < use_bits; i++) {
            uint8_t bit = inst->all_bits[data_start + i];
            if(bit) frame[i / 8] |= (uint8_t)(0x80u >> (i % 8));
        }
        _rv4_dump_bits("  DATA bits", inst->all_bits + data_start, use_bits);
        FURI_LOG_I(TAG, "  TRAIL bits: %d discarded (%d after frame)",
                   data_len - use_bits,
                   data_len - use_bits);

        /* Frame: [serial 32b][btn 8b][rolling 48b] = 88 bits
         * XOR of all 11 bytes must equal 0xFF */
        uint8_t xor_check = 0;
        for(int i = 0; i < 11 && i < (use_bits + 7) / 8; i++)
            xor_check ^= frame[i];
        FURI_LOG_I(TAG, "  bytes: %02X %02X %02X %02X | %02X | %02X %02X %02X %02X %02X %02X  XOR=%02X",
                   frame[0], frame[1], frame[2], frame[3], frame[4],
                   frame[5], frame[6], frame[7], frame[8], frame[9], frame[10],
                   xor_check);
        if(xor_check != 0xFF) continue;

        /* Button-nibble whitelist: only valid xHorse buttons pass.
         * Fake-matches from XOR-only validation often have illegal nibbles like 0xF. */
        uint8_t _btn_check = (frame[4] >> 4) & 0x0Fu;
        if(_btn_check != 0xEu && _btn_check != 0xDu &&
           _btn_check != 0xBu && _btn_check != 0x7u) {
            FURI_LOG_I(TAG, "  rejected: invalid btn_nibble=0x%X (not E/D/B/7)", _btn_check);
            continue;
        }

        /* Structural invariant: frame bits 36-37 (the 2-bit "pad" field, i.e.
         * bits 2-3 of frame[4]) are constant 0b01 on genuine xHorse fobs —
         * verified constant across 300 Nissan Almera samples (see the field map
         * below and tools/xhorse_rv4/bit_correlation.py). A foreign burst that
         * merely satisfied the 8-bit XOR and the 4-value button whitelist will
         * almost never also carry pad==01, so this closes the residual
         * false-positive gap (a Fiat V1 burst decodes here with pad==0b10).
         * Relax this if a genuine non-xHorse Renault V4 fob shows pad!=01. */
        uint8_t _pad_check = (frame[4] >> 2) & 0x03u;
        if(_pad_check != 0x01u) {
            FURI_LOG_I(TAG, "  rejected: pad=0b%u%u (not 0b01)",
                       (_pad_check >> 1) & 1u, _pad_check & 1u);
            continue;
        }

        /* Bit-level field extraction (Nissan xHorse-style layout, corrected):
         *   bits  0..31 : Serial (= frame[0..3])
         *   bits 32..35 : Btn nibble (upper nibble of frame[4])
         *   bits 36..37 : pad/reserved (2 bits)
         *   bits 38..45 : 8-bit visible counter (increments by 1 per button press)
         *   bits 46..47 : 2-bit session nonce (constant within a session, differs
         *                 between sessions — previously misinterpreted as enc40 top)
         *   bits 48..85 : 38-bit encrypted payload
         *   bits 86..87 : trailing 2 bits
         *
         * Width correction (previously declared 40-bit enc):
         * Walsh correlation analysis over 300 Nissan Almera samples showed
         * enc40 bits 38, 39 are identical with P=1.000 to a per-session constant
         * — they are NOT cipher output. The real cipher output is 38 bits.
         * See tools/xhorse_rv4/bit_correlation.py. */
        uint8_t  btn_nibble = (frame[4] >> 4) & 0x0Fu;
        uint8_t  pad2       = (frame[4] >> 2) & 0x03u;
        uint8_t  counter8   = (uint8_t)(((frame[4] & 0x03u) << 6) | ((frame[5] >> 2) & 0x3Fu));
        uint8_t  nonce2     = (uint8_t)(frame[5] & 0x03u);
        uint64_t enc38 = 0;
        for(int bi = 48; bi < 86; bi++) {
            enc38 = (enc38 << 1) | ((frame[bi >> 3] >> (7 - (bi & 7))) & 1u);
        }
        uint8_t tail2 = (uint8_t)(frame[10] & 0x03u);
        FURI_LOG_I(TAG, "  fields: btn=0x%X pad=0x%X cnt=0x%02X nonce=0x%X enc38=0x%010llX tail=0x%X",
                   btn_nibble, pad2, counter8, nonce2,
                   (unsigned long long)enc38, tail2);

        uint32_t serial = ((uint32_t)frame[0] << 24) | ((uint32_t)frame[1] << 16) |
                          ((uint32_t)frame[2] << 8)  |  (uint32_t)frame[3];
        uint8_t  btn    = frame[4];
        uint64_t rolling = 0;
        for(int i = 5; i < 11; i++)
            rolling = (rolling << 8) | frame[i];

        /* Pack into generic.data (64 bit): serial(32) + btn(8) + rolling_high(24) */
        inst->generic.data = ((uint64_t)serial << 32) | ((uint64_t)btn << 24) |
                             ((rolling >> 24) & 0xFFFFFFu);
        inst->generic.data_2 = rolling & 0xFFFFFFu;
        inst->generic.data_count_bit = (uint8_t)use_bits;
        inst->generic.serial = serial;
        inst->generic.btn    = btn_nibble;
        inst->generic.cnt    = counter8;
        inst->generic.crc_check = true;
        inst->generic.extra_check = true;

        FURI_LOG_I(TAG, "DECODED ser=%08lX btn=%02X roll=%02X%02X%02X%02X%02X%02X xor=%02X",
            (unsigned long)serial, btn,
            frame[5], frame[6], frame[7], frame[8], frame[9], frame[10], xor_check);

        /* Repeat-dedup: the burst train repeats the same frame hundreds of
         * times; fire once per unique frame so one press yields one row. */
        if(inst->last_frame_valid && memcmp(inst->last_frame, frame, 11) == 0) {
            return;
        }
        memcpy(inst->last_frame, frame, 11);
        inst->last_frame_valid = true;

        if(inst->base.callback)
            inst->base.callback(&inst->base, inst->base.context);
        return;
    }
}

/* ── protocol tables ─────────────────────────────────────────────────── */

#ifdef SUBGHZ_DECODER_TEST
static SubGhzBlockGeneric* subghz_protocol_decoder_renault_v4_get_generic(void* context) {
    SubGhzProtocolDecoderRenaultV4* inst = context;
    return &inst->generic;
}
#endif

const SubGhzProtocolDecoder subghz_protocol_renault_v4_decoder = {
    .alloc         = subghz_protocol_decoder_renault_v4_alloc,
    .free          = subghz_protocol_decoder_renault_v4_free,
    .feed          = subghz_protocol_decoder_renault_v4_feed,
    .reset         = subghz_protocol_decoder_renault_v4_reset,
    .get_hash_data = subghz_protocol_decoder_renault_v4_get_hash_data,
    .serialize     = subghz_protocol_decoder_renault_v4_serialize,
    .deserialize   = subghz_protocol_decoder_renault_v4_deserialize,
    .get_string    = subghz_protocol_decoder_renault_v4_get_string,
#ifdef SUBGHZ_DECODER_TEST
    .get_generic   = subghz_protocol_decoder_renault_v4_get_generic,
#endif
};

const SubGhzProtocolEncoder subghz_protocol_renault_v4_encoder = {
    .alloc       = subghz_protocol_encoder_renault_v4_alloc,
    .free        = subghz_protocol_encoder_renault_v4_free,
    .deserialize = subghz_protocol_encoder_renault_v4_deserialize,
    .stop        = subghz_protocol_encoder_renault_v4_stop,
    .yield       = subghz_protocol_encoder_renault_v4_yield,
};

const SubGhzProtocol subghz_protocol_renault_v4 = {
    .name    = SUBGHZ_PROTOCOL_RENAULT_V4_NAME,
    .type    = SubGhzProtocolTypeStatic,
    .flag    = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM |
               SubGhzProtocolFlag_Decodable |
               SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_renault_v4_decoder,
    .encoder = &subghz_protocol_renault_v4_encoder,
};

/* ── decoder implementation ──────────────────────────────────────────── */

void* subghz_protocol_decoder_renault_v4_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderRenaultV4* inst = malloc(sizeof(SubGhzProtocolDecoderRenaultV4));
    furi_check(inst);
    memset(inst, 0, sizeof(SubGhzProtocolDecoderRenaultV4));
    inst->base.protocol         = &subghz_protocol_renault_v4;
    inst->generic.protocol_name = inst->base.protocol->name;
    return inst;
}

void subghz_protocol_decoder_renault_v4_free(void* context) {
    furi_assert(context);
    free(context);
}

void subghz_protocol_decoder_renault_v4_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderRenaultV4* inst = context;
    inst->synced        = false;
    inst->all_bit_count = 0;
    inst->manchester_state = 0;
}

void subghz_protocol_decoder_renault_v4_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderRenaultV4* inst = context;

    uint32_t te_s = RENAULT_V4_TE_SHORT;
    uint32_t te_l = RENAULT_V4_TE_LONG;
    uint32_t te_d = RENAULT_V4_TE_DELTA;

    if(!inst->synced) {
        /* Wait for sync: HIGH pulse >= SYNC_MIN */
        if(level && duration >= RENAULT_V4_SYNC_MIN) {
            inst->synced = true;
            inst->all_bit_count = 0;
            inst->manchester_state = 1;
        }
        return;
    }

    /* Accumulate Manchester bits */
    bool is_short = DURATION_DIFF(duration, te_s) < te_d;
    bool is_long  = DURATION_DIFF(duration, te_l) < te_d;

    if(is_short || is_long) {
        uint8_t r1 = _rv4_calc_r1(level, is_long);
        uint8_t out_bit;
        if(_rv4_manchester_decode(inst, r1, &out_bit)) {
            if(inst->all_bit_count < sizeof(inst->all_bits)) {
                inst->all_bits[inst->all_bit_count++] = out_bit;
            }
        }
    } else {
        /* Invalid duration — end of burst */
        _rv4_process_frame(inst);
        /* Check if this is a new sync */
        if(level && duration >= RENAULT_V4_SYNC_MIN) {
            inst->all_bit_count = 0;
            inst->manchester_state = 1;
        } else {
            inst->synced = false;
        }
    }
}

uint8_t subghz_protocol_decoder_renault_v4_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderRenaultV4* inst = context;
    return (uint8_t)(inst->generic.data ^ (inst->generic.data >> 8));
}

SubGhzProtocolStatus subghz_protocol_decoder_renault_v4_serialize(
    void* context, FlipperFormat* flipper_format, SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderRenaultV4* inst = context;
    SubGhzProtocolStatus res = subghz_block_generic_serialize(&inst->generic, flipper_format, preset);
    if(res != SubGhzProtocolStatusOk) return res;

    /* Save data_2 (lower 24 bits of the 88-bit frame) as "Key2" so round-trip
     * can reconstruct the complete frame. */
    uint32_t k2 = (uint32_t)(inst->generic.data_2 & 0xFFFFFFu);
    if(!flipper_format_rewind(flipper_format)) return SubGhzProtocolStatusErrorParserOthers;
    if(!flipper_format_insert_or_update_uint32(flipper_format, "Key2", &k2, 1))
        return SubGhzProtocolStatusErrorParserOthers;
    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus subghz_protocol_decoder_renault_v4_deserialize(
    void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderRenaultV4* inst = context;
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &inst->generic, flipper_format,
        subghz_protocol_renault_v4_const.min_count_bit_for_found);
    if(ret != SubGhzProtocolStatusOk) return ret;
    uint32_t k2 = 0;
    if(flipper_format_rewind(flipper_format) &&
       flipper_format_read_uint32(flipper_format, "Key2", &k2, 1)) {
        inst->generic.data_2 = k2 & 0xFFFFFFu;
    }
    return SubGhzProtocolStatusOk;
}

void subghz_protocol_decoder_renault_v4_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderRenaultV4* inst = context;

    /* Bit-level field layout of the 88-bit frame:
     *   bits  0..31 : Serial
     *   bits 32..35 : Btn (4-bit one-hot: E=Lock, D=Unlock, B=Trunk, 7=Panic)
     *   bits 36..37 : Pad (observed constant "01" on xHorse fobs)
     *   bits 38..45 : 8-bit visible counter
     *   bits 46..47 : 2-bit session nonce
     *   bits 48..85 : 38-bit encrypted payload
     *   bits 86..87 : Tail (2 bits) */
    uint64_t d  = inst->generic.data;
    uint32_t d2 = (uint32_t)inst->generic.data_2;
    uint8_t  btn_byte = (uint8_t)((d >> 24) & 0xFFu);

    uint8_t  btn4     = (btn_byte >> 4) & 0x0Fu;
    uint8_t  pad2     = (btn_byte >> 2) & 0x03u;
    uint8_t  counter8 = (uint8_t)(((btn_byte & 0x03u) << 6) | ((d >> 18) & 0x3Fu));
    uint8_t  nonce2   = (uint8_t)((d >> 16) & 0x03u);
    /* enc38 = frame bits 48..85 = data[15:0] (16 bits) concat data_2[23:2] (22 bits) */
    uint64_t enc38    = ((uint64_t)(d & 0xFFFFu) << 22) | ((d2 >> 2) & 0x3FFFFFu);
    uint8_t  tail2    = (uint8_t)(d2 & 0x03u);

    const char* btn_name;
    switch(btn4) {
    case 0xE: btn_name = "Lock";    break;
    case 0xD: btn_name = "Unlock";  break;
    case 0xB: btn_name = "Trunk";   break;
    case 0x7: btn_name = "Panic";   break;
    default:  btn_name = "Unknown"; break;
    }

    /* Expose counter metadata to the GUI / roundtrip test harness. */
    subghz_block_generic_global.cnt_length_bit = 8;

    furi_string_cat_printf(
        output,
        "%s %ubit\r\n"
        "Sn:%08lX  Btn:%X [%s]  Cnt:%02X  Nonce:%X\r\n"
        "Enc:%010llX  Pad:%X Tail:%X  CRC:OK\r\n",
        inst->generic.protocol_name,
        inst->generic.data_count_bit,
        (unsigned long)inst->generic.serial,
        btn4, btn_name, counter8, nonce2,
        (unsigned long long)enc38,
        pad2, tail2);
}

/* ── encoder (minimal — static replay) ───────────────────────────────── */

#define RV4_ENC_BUF_SIZE 256u

void* subghz_protocol_encoder_renault_v4_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderRenaultV4* inst = malloc(sizeof(SubGhzProtocolEncoderRenaultV4));
    furi_check(inst);
    memset(inst, 0, sizeof(SubGhzProtocolEncoderRenaultV4));
    inst->base.protocol         = &subghz_protocol_renault_v4;
    inst->generic.protocol_name = inst->base.protocol->name;
    inst->encoder.repeat        = 50; /* many short bursts like the original */
    inst->encoder.size_upload   = 0;
    inst->encoder.upload        = malloc(RV4_ENC_BUF_SIZE * sizeof(LevelDuration));
    furi_check(inst->encoder.upload);
    inst->encoder.is_running = false;
    inst->encoder.front      = 0;
    return inst;
}

void subghz_protocol_encoder_renault_v4_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderRenaultV4* inst = context;
    free(inst->encoder.upload);
    free(inst);
}

void subghz_protocol_encoder_renault_v4_stop(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderRenaultV4* inst = context;
    inst->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_renault_v4_yield(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderRenaultV4* inst = context;
    if(inst->encoder.repeat == 0 || !inst->encoder.is_running) {
        inst->encoder.is_running = false;
        return level_duration_reset();
    }
    LevelDuration ret = inst->encoder.upload[inst->encoder.front];
    if(++inst->encoder.front >= inst->encoder.size_upload) {
        inst->encoder.repeat--;
        inst->encoder.front = 0;
    }
    return ret;
}

static void _rv4_build_upload(SubGhzProtocolEncoderRenaultV4* inst) {
    LevelDuration* buf = inst->encoder.upload;
    size_t idx = 0;
    uint32_t te_s = RENAULT_V4_TE_SHORT;

    /* Reconstruct the full 88-bit frame from generic.data (64b) + data_2 (24b).
     * Layout:
     *   data[63:32] = serial (bytes 0..3)
     *   data[31:24] = frame byte 4 (btn + pad + counter high)
     *   data[23:0]  = frame bytes 5..7 (rolling high)
     *   data_2[23:0] = frame bytes 8..10 (rolling low + XOR byte)
     */
    uint8_t frame[11] = {0};
    uint64_t d  = inst->generic.data;
    uint64_t d2 = inst->generic.data_2;
    frame[0] = (uint8_t)((d >> 56) & 0xFFu);
    frame[1] = (uint8_t)((d >> 48) & 0xFFu);
    frame[2] = (uint8_t)((d >> 40) & 0xFFu);
    frame[3] = (uint8_t)((d >> 32) & 0xFFu);
    frame[4] = (uint8_t)((d >> 24) & 0xFFu);
    frame[5] = (uint8_t)((d >> 16) & 0xFFu);
    frame[6] = (uint8_t)((d >>  8) & 0xFFu);
    frame[7] = (uint8_t)( d        & 0xFFu);
    frame[8] = (uint8_t)((d2 >> 16) & 0xFFu);
    frame[9] = (uint8_t)((d2 >>  8) & 0xFFu);
    frame[10] = (uint8_t)(d2        & 0xFFu);

    /* Sync pulse */
    buf[idx++] = level_duration_make(true, 1000);

    /* Preamble: 15 Manchester 1-bits (LOW-HIGH pairs) */
    for(int p = 0; p < 15; p++) {
        buf[idx++] = level_duration_make(false, te_s);
        buf[idx++] = level_duration_make(true,  te_s);
    }

    /* Sync bit: 1× Manchester 0 (HIGH-LOW) — separates preamble from data.
     * The decoder counts 15 ones then skips 1 bit before reading data. */
    buf[idx++] = level_duration_make(true,  te_s);
    buf[idx++] = level_duration_make(false, te_s);

    /* Data bits — all 88 bits from frame, MSB-first per byte.
     * Manchester: 1 = LOW→HIGH, 0 = HIGH→LOW */
    /* Clamp to the fixed frame size: a malicious/corrupt .sub can carry Bit>88
     * (generic deserialize does not bound it), which would read past frame[11]
     * and overrun the upload buffer (then furi_check-abort the device). */
    int nbits = inst->generic.data_count_bit;
    if(nbits <= 0 || nbits > (int)RENAULT_V4_FRAME_BITS) nbits = (int)RENAULT_V4_FRAME_BITS;
    for(int b = 0; b < nbits; b++) {
        uint8_t bit = (frame[b >> 3] >> (7 - (b & 7))) & 1u;
        if(bit) {
            buf[idx++] = level_duration_make(false, te_s);
            buf[idx++] = level_duration_make(true,  te_s);
        } else {
            buf[idx++] = level_duration_make(true,  te_s);
            buf[idx++] = level_duration_make(false, te_s);
        }
        furi_check(idx < RV4_ENC_BUF_SIZE);
    }

    /* Gap between bursts */
    buf[idx++] = level_duration_make(false, 21000);

    inst->encoder.size_upload = idx;
    inst->encoder.front       = 0;
}

SubGhzProtocolStatus subghz_protocol_encoder_renault_v4_deserialize(
    void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderRenaultV4* inst = context;
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize(&inst->generic, flipper_format);
    if(ret != SubGhzProtocolStatusOk) return ret;

    /* Restore lower 24 bits of the frame from Key2. */
    uint32_t k2 = 0;
    if(flipper_format_rewind(flipper_format) &&
       flipper_format_read_uint32(flipper_format, "Key2", &k2, 1)) {
        inst->generic.data_2 = k2 & 0xFFFFFFu;
    }

    /* Counter advance (roundtrip test mode). Also updates the counter bits
     * inside generic.data so the next encode emits the new counter, and fixes
     * the XOR-check byte (frame[10], lowest byte of data_2).
     *
     * Note: generic.cnt is NOT auto-populated by subghz_block_generic_deserialize,
     * so we extract the current counter directly from data bits 18..25 instead. */
    int32_t mult = furi_hal_subghz_get_rolling_counter_mult();
    if(mult != (int32_t)(-0x7FFFFFFF)) {
        uint8_t cur_cnt = (uint8_t)((inst->generic.data >> 18) & 0xFFu);
        uint32_t new_cnt = ((uint32_t)cur_cnt + (uint32_t)mult) & 0xFFu;
        inst->generic.cnt = new_cnt;

        /* Counter occupies bits 38..45 of the 88-bit frame:
         *   bits 38-39 = low 2 bits of frame[4]  → generic.data bits 25-24
         *   bits 40-45 = high 6 bits of frame[5] → generic.data bits 23-18
         * => counter spans generic.data bits 25..18 (8 contiguous bits).
         * Bits 46-47 of frame are a separate 2-bit session nonce (NOT counter). */
        uint64_t d = inst->generic.data;
        d &= ~((uint64_t)0xFFu << 18);
        d |=  ((uint64_t)new_cnt & 0xFFu) << 18;
        inst->generic.data = d;

        /* Recompute XOR-check: XOR of all 11 bytes must equal 0xFF.
         * Adjust the LAST byte (frame[10] = low byte of data_2). */
        uint8_t frame[11];
        uint64_t d2 = inst->generic.data_2;
        frame[0] = (uint8_t)((d  >> 56) & 0xFFu);
        frame[1] = (uint8_t)((d  >> 48) & 0xFFu);
        frame[2] = (uint8_t)((d  >> 40) & 0xFFu);
        frame[3] = (uint8_t)((d  >> 32) & 0xFFu);
        frame[4] = (uint8_t)((d  >> 24) & 0xFFu);
        frame[5] = (uint8_t)((d  >> 16) & 0xFFu);
        frame[6] = (uint8_t)((d  >>  8) & 0xFFu);
        frame[7] = (uint8_t)( d         & 0xFFu);
        frame[8] = (uint8_t)((d2 >> 16) & 0xFFu);
        frame[9] = (uint8_t)((d2 >>  8) & 0xFFu);
        uint8_t x = 0;
        for(int i = 0; i < 10; i++) x ^= frame[i];
        uint8_t fixed_last = (uint8_t)(x ^ 0xFFu);
        inst->generic.data_2 = (d2 & ~(uint64_t)0xFFu) | fixed_last;
    }

    _rv4_build_upload(inst);
    inst->encoder.is_running = true;
    return SubGhzProtocolStatusOk;
}
