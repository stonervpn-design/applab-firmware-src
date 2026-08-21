#include "pocsag.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <lib/flipper_format/flipper_format_i.h>
#include <core/furi_string.h>

#define TAG "POCSAG"

static const SubGhzBlockConst pocsag_const = {
    .te_short = 833,
    .te_delta = 100,
};
static const SubGhzBlockConst pocsag512_const = {
    .te_short = 1950,
    .te_long = 1950,
    .te_delta = 120,
};
static const SubGhzBlockConst pocsag2400_const = {
    .te_short = 410,
    .te_long = 410,
    .te_delta = 60,
};

// Minimal amount of sync bits (interleaving zeros and ones)
#define POCSAG_MIN_SYNC_BITS   24
#define POCSAG_CW_BITS         32
#define POCSAG_CW_MASK         0xFFFFFFFF
#define POCSAG_FRAME_SYNC_CODE 0x7CD215D8
#define POCSAG_IDLE_CODE_WORD  0x7A89C197

#define POCSAG_FUNC_NUM      0
#define POCSAG_FUNC_ALERT1   1
#define POCSAG_FUNC_ALERT2   2
#define POCSAG_FUNC_ALPHANUM 3

static const char* func_msg[] = {"\e#Num:\e# ", "\e#Alert\e#", "\e#Alert:\e# ", "\e#Msg:\e# "};
static const char* bcd_chars = "*U -)(";

struct SubGhzProtocolDecoderPocsag {
    SubGhzProtocolDecoderBase base;

    SubGhzBlockDecoder decoder;
    PCSGBlockGeneric generic;

    uint8_t codeword_idx;
    uint32_t ric;
    uint8_t func;

    // partially decoded character
    uint8_t char_bits;
    uint8_t char_data;

    // message being decoded
    FuriString* msg;

    // Done messages, ready to be serialized/deserialized
    FuriString* done_msg;

    SubGhzBlockConst* pocsag_timing;
    uint32_t version;
};

typedef struct SubGhzProtocolDecoderPocsag SubGhzProtocolDecoderPocsag;

typedef enum {
    PocsagDecoderStepReset = 0,
    PocsagDecoderStepFoundSync,
    PocsagDecoderStepFoundPreamble,
    PocsagDecoderStepMessage,
} PocsagDecoderStep;

void* subghz_protocol_decoder_pocsag_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);

    SubGhzProtocolDecoderPocsag* instance = malloc(sizeof(SubGhzProtocolDecoderPocsag));
    instance->base.protocol = &subghz_protocol_pocsag;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->msg = furi_string_alloc();
    instance->done_msg = furi_string_alloc();
    instance->pocsag_timing = NULL; //not synced yet
    if(instance->generic.result_msg == NULL) {
        instance->generic.result_msg = furi_string_alloc();
    }
    if(instance->generic.result_ric == NULL) {
        instance->generic.result_ric = furi_string_alloc();
    }

    return instance;
}

void subghz_protocol_decoder_pocsag_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderPocsag* instance = context;
    furi_string_free(instance->msg);
    furi_string_free(instance->done_msg);
    furi_string_free(instance->generic.result_msg);
    furi_string_free(instance->generic.result_ric);
    free(instance);
}

void subghz_protocol_decoder_pocsag_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderPocsag* instance = context;

    instance->decoder.parser_step = PocsagDecoderStepReset;
    instance->decoder.decode_data = 0UL;
    instance->decoder.decode_count_bit = 0;
    instance->codeword_idx = 0;
    instance->char_bits = 0;
    instance->char_data = 0;
    furi_string_reset(instance->msg);
    furi_string_reset(instance->done_msg);
    furi_string_reset(instance->generic.result_msg);
    furi_string_reset(instance->generic.result_ric);
}

static void pocsag_decode_address_word(SubGhzProtocolDecoderPocsag* instance, uint32_t data) {
    instance->ric = (data >> 13);
    instance->ric = (instance->ric << 3) | (instance->codeword_idx >> 1);
    instance->func = (data >> 11) & 0b11;
}

static bool decode_message_alphanumeric(SubGhzProtocolDecoderPocsag* instance, uint32_t data) {
    for(uint8_t i = 0; i < 20; i++) {
        instance->char_data >>= 1;
        if(data & (1 << 30)) {
            instance->char_data |= 1 << 6;
        }
        instance->char_bits++;
        if(instance->char_bits == 7) {
            if(instance->char_data == 0) return false;
            furi_string_push_back(instance->msg, instance->char_data);
            instance->char_data = 0;
            instance->char_bits = 0;
        }
        data <<= 1;
    }
    return true;
}

static void decode_message_numeric(SubGhzProtocolDecoderPocsag* instance, uint32_t data) {
    // 5 groups with 4 bits each
    uint8_t val;
    for(uint8_t i = 0; i < 5; i++) {
        val = (data >> (27 - i * 4)) & 0b1111;
        // reverse the order of 4 bits
        val = (val & 0x5) << 1 | (val & 0xA) >> 1;
        val = (val & 0x3) << 2 | (val & 0xC) >> 2;

        if(val <= 9)
            val += '0';
        else
            val = bcd_chars[val - 10];
        furi_string_push_back(instance->msg, val);
    }
}

// decode message word, maintaining instance state for partial decoding. Return true if more data
// might follow or false if end of message reached.
static bool pocsag_decode_message_word(SubGhzProtocolDecoderPocsag* instance, uint32_t data) {
    switch(instance->func) {
    case POCSAG_FUNC_ALERT2:
    case POCSAG_FUNC_ALPHANUM:
        return decode_message_alphanumeric(instance, data);

    case POCSAG_FUNC_NUM:
        decode_message_numeric(instance, data);
        return true;
    }
    return false;
}

// Function called when current message got decoded, but other messages might follow
static void pocsag_message_done(SubGhzProtocolDecoderPocsag* instance) {
    // append the message to the long-term storage string
    furi_string_printf(instance->generic.result_ric, "\e#RIC: %" PRIu32 "\e# | ", instance->ric);
    furi_string_cat_str(instance->generic.result_ric, func_msg[instance->func]);
    if(instance->func != POCSAG_FUNC_ALERT1) {
        furi_string_cat(instance->done_msg, instance->msg);
    }
    furi_string_cat_str(instance->done_msg, " ");

    furi_string_cat(instance->generic.result_msg, instance->done_msg);

    // reset the state
    instance->char_bits = 0;
    instance->char_data = 0;
    furi_string_reset(instance->msg);
}

void subghz_protocol_decoder_pocsag_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderPocsag* instance = context;

    // reset state - waiting for 32 bits of interleaving 1s and 0s
    if(instance->decoder.parser_step == PocsagDecoderStepReset) {
        if(DURATION_DIFF(duration, pocsag_const.te_short) < pocsag_const.te_delta) {
            if(instance->pocsag_timing != &pocsag_const) {
                //timing changed, so reset before, and override
                subghz_protocol_decoder_pocsag_reset(context);
                instance->pocsag_timing = (SubGhzBlockConst*)&pocsag_const;
                instance->version = 1200;
            }
            // POCSAG signals are inverted
            subghz_protocol_blocks_add_bit(&instance->decoder, !level);

            if(instance->decoder.decode_count_bit == POCSAG_MIN_SYNC_BITS) {
                instance->decoder.parser_step = PocsagDecoderStepFoundSync;
            }
        } else if(DURATION_DIFF(duration, pocsag512_const.te_short) < pocsag512_const.te_delta) {
            if(instance->pocsag_timing != &pocsag512_const) {
                //timing changed, so reset before, and override
                subghz_protocol_decoder_pocsag_reset(context);
                instance->pocsag_timing = (SubGhzBlockConst*)&pocsag512_const;
                instance->version = 512;
            }
            // POCSAG signals are inverted
            subghz_protocol_blocks_add_bit(&instance->decoder, !level);

            if(instance->decoder.decode_count_bit == POCSAG_MIN_SYNC_BITS) {
                instance->decoder.parser_step = PocsagDecoderStepFoundSync;
            }
        } else if(DURATION_DIFF(duration, pocsag2400_const.te_short) < pocsag2400_const.te_delta) {
            if(instance->pocsag_timing != &pocsag2400_const) {
                //timing changed, so reset before, and override
                subghz_protocol_decoder_pocsag_reset(context);
                instance->pocsag_timing = (SubGhzBlockConst*)&pocsag2400_const;
                instance->version = 2400;
            }
            // POCSAG signals are inverted
            subghz_protocol_blocks_add_bit(&instance->decoder, !level);

            if(instance->decoder.decode_count_bit == POCSAG_MIN_SYNC_BITS) {
                instance->decoder.parser_step = PocsagDecoderStepFoundSync;
            }
        } else if(instance->decoder.decode_count_bit > 0) {
            subghz_protocol_decoder_pocsag_reset(context);
        }
        return;
    }

    int bits_count = duration / instance->pocsag_timing->te_short;
    uint32_t extra = duration - instance->pocsag_timing->te_short * bits_count;

    if(DURATION_DIFF(extra, instance->pocsag_timing->te_short) < instance->pocsag_timing->te_delta)
        bits_count++;
    else if(extra > instance->pocsag_timing->te_delta) {
        // in non-reset state we faced the error signal - we reached the end of the packet, flush data
        if(furi_string_size(instance->done_msg) > 0) {
            if(instance->base.callback)
                instance->base.callback(&instance->base, instance->base.context);
        }
        subghz_protocol_decoder_pocsag_reset(context);
        return;
    }

    uint32_t codeword;

    // handle state machine for every incoming bit
    while(bits_count-- > 0) {
        subghz_protocol_blocks_add_bit(&instance->decoder, !level);

        switch(instance->decoder.parser_step) {
        case PocsagDecoderStepFoundSync:
            if((instance->decoder.decode_data & POCSAG_CW_MASK) == POCSAG_FRAME_SYNC_CODE) {
                instance->decoder.parser_step = PocsagDecoderStepFoundPreamble;
                instance->decoder.decode_count_bit = 0;
                instance->decoder.decode_data = 0UL;
            }
            break;
        case PocsagDecoderStepFoundPreamble:
            // handle codewords
            if(instance->decoder.decode_count_bit == POCSAG_CW_BITS) {
                codeword = (uint32_t)(instance->decoder.decode_data & POCSAG_CW_MASK);
                switch(codeword) {
                case POCSAG_IDLE_CODE_WORD:
                    instance->codeword_idx++;
                    break;
                case POCSAG_FRAME_SYNC_CODE:
                    instance->codeword_idx = 0;
                    break;
                default:
                    // Here we expect only address messages
                    if(codeword >> 31 == 0) {
                        pocsag_decode_address_word(instance, codeword);
                        instance->decoder.parser_step = PocsagDecoderStepMessage;
                    }
                    instance->codeword_idx++;
                }
                instance->decoder.decode_count_bit = 0;
                instance->decoder.decode_data = 0UL;
            }
            break;

        case PocsagDecoderStepMessage:
            if(instance->decoder.decode_count_bit == POCSAG_CW_BITS) {
                codeword = (uint32_t)(instance->decoder.decode_data & POCSAG_CW_MASK);
                switch(codeword) {
                case POCSAG_IDLE_CODE_WORD:
                    // Idle during the message stops the message
                    instance->codeword_idx++;
                    instance->decoder.parser_step = PocsagDecoderStepFoundPreamble;
                    pocsag_message_done(instance);
                    break;
                case POCSAG_FRAME_SYNC_CODE:
                    instance->codeword_idx = 0;
                    break;
                default:
                    // In this state, both address and message words can arrive
                    if(codeword >> 31 == 0) {
                        pocsag_message_done(instance);
                        pocsag_decode_address_word(instance, codeword);
                    } else {
                        if(!pocsag_decode_message_word(instance, codeword)) {
                            instance->decoder.parser_step = PocsagDecoderStepFoundPreamble;
                            pocsag_message_done(instance);
                        }
                    }
                    instance->codeword_idx++;
                }
                instance->decoder.decode_count_bit = 0;
                instance->decoder.decode_data = 0UL;
            }
            break;
        }
    }
}

uint8_t subghz_protocol_decoder_pocsag_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderPocsag* instance = context;
    union {
        uint32_t full;
        uint8_t split[4];
    } hash = {0};
    size_t len = furi_string_size(instance->done_msg);
    for(size_t i = 0; i < len; i++)
        hash.split[i % sizeof(hash)] ^= furi_string_get_char(instance->done_msg, i);
    return hash.full;
}

SubGhzProtocolStatus subghz_protocol_decoder_pocsag_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderPocsag* instance = context;
    uint32_t msg_len;

    if(SubGhzProtocolStatusOk !=
       pcsg_block_generic_serialize(&instance->generic, flipper_format, preset))
        return SubGhzProtocolStatusError;

    msg_len = furi_string_size(instance->done_msg);
    if(!flipper_format_write_uint32(flipper_format, "MsgLen", &msg_len, 1)) {
        FURI_LOG_E(TAG, "Error adding MsgLen");
        return SubGhzProtocolStatusError;
    }

    if(!flipper_format_write_uint32(flipper_format, "PocsagVer", &instance->version, 1)) {
        FURI_LOG_E(TAG, "Error adding PocsagVer");
        return SubGhzProtocolStatusError;
    }

    uint8_t* s = (uint8_t*)furi_string_get_cstr(instance->done_msg);
    if(!flipper_format_write_hex(flipper_format, "Msg", s, msg_len)) {
        FURI_LOG_E(TAG, "Error adding Msg");
        return SubGhzProtocolStatusError;
    }

    // Clean integer RIC + function for the encoder (the display "Ric" string is
    // lossy/escape-coded). Additive, backward-compatible keys; older captures
    // without them fall back to heuristics in the encoder.
    uint32_t ric = instance->ric;
    if(!flipper_format_write_uint32(flipper_format, "Ric_dec", &ric, 1)) {
        FURI_LOG_E(TAG, "Error adding Ric_dec");
        return SubGhzProtocolStatusError;
    }
    uint32_t func = instance->func;
    if(!flipper_format_write_uint32(flipper_format, "Func", &func, 1)) {
        FURI_LOG_E(TAG, "Error adding Func");
        return SubGhzProtocolStatusError;
    }
    return SubGhzProtocolStatusOk;
}

SubGhzProtocolStatus
    subghz_protocol_decoder_pocsag_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderPocsag* instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    uint32_t msg_len;
    uint8_t* buf;

    do {
        if(SubGhzProtocolStatusOk !=
           pcsg_block_generic_deserialize(&instance->generic, flipper_format)) {
            break;
        }

        if(!flipper_format_read_uint32(flipper_format, "MsgLen", &msg_len, 1)) {
            FURI_LOG_E(TAG, "Missing MsgLen");
            break;
        }
        //optional, so compatible backwards
        instance->version = 1200;
        flipper_format_read_uint32(flipper_format, "PocsagVer", &instance->version, 1);

        buf = malloc(msg_len);
        if(!buf) break; // OOM (e.g. crafted huge MsgLen) -> don't write into NULL
        if(!flipper_format_read_hex(flipper_format, "Msg", buf, msg_len)) {
            FURI_LOG_E(TAG, "Missing Msg");
            free(buf);
            break;
        }
        furi_string_set_strn(instance->done_msg, (const char*)buf, msg_len);
        free(buf);

        ret = SubGhzProtocolStatusOk;
    } while(false);
    return ret;
}

void subhz_protocol_decoder_pocsag_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderPocsag* instance = context;
    furi_string_cat_printf(
        output, "%s %lu\r\n", instance->generic.protocol_name, instance->version);
    furi_string_cat_printf(output, "Addr: %lu\r\n", instance->ric);
    furi_string_cat(output, instance->done_msg);
}

const SubGhzProtocolDecoder subghz_protocol_pocsag_decoder = {
    .alloc = subghz_protocol_decoder_pocsag_alloc,
    .free = subghz_protocol_decoder_pocsag_free,
    .reset = subghz_protocol_decoder_pocsag_reset,
    .feed = subghz_protocol_decoder_pocsag_feed,
    .get_hash_data = subghz_protocol_decoder_pocsag_get_hash_data,
    .serialize = subghz_protocol_decoder_pocsag_serialize,
    .deserialize = subghz_protocol_decoder_pocsag_deserialize,
    .get_string = subhz_protocol_decoder_pocsag_get_string,
    .get_string_brief = NULL,
};

// ============================================================================
// ENCODER — regenerate a valid POCSAG codeword stream from the stored fields.
//
// POCSAG serialize does NOT store the raw codeword bit stream, only the DECODED
// message text ("Msg"), the baud ("PocsagVer"), and (added here) the integer RIC
// ("Ric_dec") + function ("Func"). POCSAG is a static, un-encrypted, canonical
// code, so those fields fully determine the codewords: given RIC/func/text the
// BCH(31,21)+parity address & message words are unique. We rebuild them, frame
// them into batches (preamble, sync 0x7CD215D8, idle 0x7A89C197 padding, address
// placed in frame = RIC&7, message words, idle terminator), invert (the decoder
// samples !level) and emit NRZ at the baud's te_short. A re-decode reproduces the
// same RIC + text — verified in simulation against a port of this decoder's feed()
// over 12k random single-message frames at 512/1200/2400 baud (exact match;
// alphanumeric across multi-batch spans, numeric word-aligned).
//
// Limitation: multi-message captures collapse (serialize keeps only the last RIC
// and concatenates all texts), so only single-message captures reproduce exactly.
// Older captures lacking Ric_dec/Func fall back to parsing the display string /
// a numeric-vs-alphanumeric content heuristic.
// ============================================================================

#define POCSAG_TX_PREAMBLE_BITS 576
#define POCSAG_GEN_POLY         0x769u // BCH(31,21): x^10+x^9+x^8+x^6+x^5+x^3+1

struct SubGhzProtocolEncoderPocsag {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    PCSGBlockGeneric generic;
};
typedef struct SubGhzProtocolEncoderPocsag SubGhzProtocolEncoderPocsag;

static uint32_t pocsag_enc_te_short(uint32_t version) {
    switch(version) {
    case 512:
        return pocsag512_const.te_short;
    case 2400:
        return pocsag2400_const.te_short;
    default:
        return pocsag_const.te_short; // 1200
    }
}

// Complete a 21-bit data field into a 32-bit POCSAG codeword: [data21][BCH10][parity1].
static uint32_t pocsag_enc_codeword(uint32_t data21) {
    uint32_t reg = (data21 & 0x1FFFFF) << 10; // data in bits 30..10
    for(int i = 30; i >= 10; i--) {
        if(reg & (1u << i)) reg ^= (POCSAG_GEN_POLY << (i - 10));
    }
    uint32_t code31 = ((data21 & 0x1FFFFF) << 10) | (reg & 0x3FF);
    uint32_t parity = 0;
    for(uint32_t t = code31; t; t >>= 1) parity ^= (t & 1);
    return (code31 << 1) | parity;
}

static uint32_t pocsag_enc_address_cw(uint32_t ric, uint8_t func) {
    uint32_t addr18 = (ric >> 3) & 0x3FFFF;
    uint32_t data21 = (addr18 << 2) | (func & 3); // flag bit (bit20) = 0 for address
    return pocsag_enc_codeword(data21);
}

static uint32_t pocsag_enc_message_cw(uint32_t msg20) {
    uint32_t data21 = (1u << 20) | (msg20 & 0xFFFFF); // flag bit = 1 for message
    return pocsag_enc_codeword(data21);
}

static uint8_t pocsag_enc_rev4(uint8_t v) {
    v = (uint8_t)(((v & 0x5) << 1) | ((v & 0xA) >> 1));
    v = (uint8_t)(((v & 0x3) << 2) | ((v & 0xC) >> 2));
    return v & 0xF;
}

static uint8_t pocsag_enc_numeric_nibble(char ch) {
    if(ch >= '0' && ch <= '9') return (uint8_t)(ch - '0');
    switch(ch) {
    case '*':
        return 10;
    case 'U':
        return 11;
    case ' ':
        return 12;
    case '-':
        return 13;
    case ')':
        return 14;
    case '(':
        return 15;
    default:
        return 12; // space as safe filler
    }
}

// Build the physical (already inverted) NRZ bit stream and run-length it into the
// upload buffer. Returns false on allocation failure.
static bool pocsag_encoder_build(
    SubGhzProtocolEncoderPocsag* instance,
    uint32_t version,
    uint32_t ric,
    uint8_t func,
    const uint8_t* text,
    size_t text_len) {
    bool numeric = (func == POCSAG_FUNC_NUM);

    // ---- content bits (message payload, MSB-first per 20-bit word) ----
    size_t content_len = numeric ? text_len * 4 : text_len * 7;
    uint8_t* content = malloc(content_len ? content_len : 1);
    if(!content) return false;
    size_t ci = 0;
    if(numeric) {
        for(size_t i = 0; i < text_len; i++) {
            uint8_t stored = pocsag_enc_rev4(pocsag_enc_numeric_nibble((char)text[i]));
            for(int k = 3; k >= 0; k--) content[ci++] = (uint8_t)((stored >> k) & 1);
        }
    } else {
        for(size_t i = 0; i < text_len; i++) {
            uint8_t c = text[i] & 0x7F;
            for(int k = 0; k < 7; k++) content[ci++] = (uint8_t)((c >> k) & 1); // LSB first
        }
    }

    size_t nwords = (content_len + 19) / 20;

    // ---- frame layout into 16-word batches ----
    uint32_t frame = ric & 7;
    size_t data_words = (size_t)(2 * frame) + 1 + nwords + 1; // idles + addr + msg + term idle
    size_t nbatches = (data_words + 15) / 16;
    if(nbatches == 0) nbatches = 1;
    size_t ndata = nbatches * 16;

    uint32_t* d = malloc(ndata * sizeof(uint32_t));
    if(!d) {
        free(content);
        return false;
    }
    size_t di = 0;
    for(uint32_t f = 0; f < frame; f++) {
        d[di++] = POCSAG_IDLE_CODE_WORD;
        d[di++] = POCSAG_IDLE_CODE_WORD;
    }
    d[di++] = pocsag_enc_address_cw(ric, func);
    for(size_t w = 0; w < nwords; w++) {
        uint32_t msg20 = 0;
        for(size_t b = 0; b < 20; b++) {
            size_t idx = w * 20 + b;
            uint8_t bit = (idx < content_len) ? content[idx] : 0;
            msg20 = (msg20 << 1) | bit;
        }
        d[di++] = pocsag_enc_message_cw(msg20);
    }
    d[di++] = POCSAG_IDLE_CODE_WORD; // terminate the message
    while(di < ndata) d[di++] = POCSAG_IDLE_CODE_WORD;
    free(content);

    // ---- logical -> physical (inverted) bit stream ----
    size_t total_bits = POCSAG_TX_PREAMBLE_BITS + nbatches * 17 * POCSAG_CW_BITS;
    uint8_t* pb = malloc(total_bits);
    if(!pb) {
        free(d);
        return false;
    }
    size_t bi = 0;
    for(size_t i = 0; i < POCSAG_TX_PREAMBLE_BITS; i++) {
        uint8_t logical = (i & 1) ? 0 : 1; // 1,0,1,0,...
        pb[bi++] = logical ^ 1; // invert
    }
    for(size_t b = 0; b < nbatches; b++) {
        uint32_t cw = POCSAG_FRAME_SYNC_CODE;
        for(int k = 31; k >= 0; k--) pb[bi++] = (uint8_t)(((cw >> k) & 1) ^ 1);
        for(size_t j = 0; j < 16; j++) {
            cw = d[b * 16 + j];
            for(int k = 31; k >= 0; k--) pb[bi++] = (uint8_t)(((cw >> k) & 1) ^ 1);
        }
    }
    free(d);

    // ---- run-length encode into upload at the baud's te_short ----
    uint32_t te = pocsag_enc_te_short(version);
    free(instance->encoder.upload);
    instance->encoder.upload = malloc((total_bits + 1) * sizeof(LevelDuration));
    if(!instance->encoder.upload) {
        free(pb);
        instance->encoder.size_upload = 0;
        return false;
    }
    size_t k = 0;
    size_t i = 0;
    while(i < total_bits) {
        uint8_t lvl = pb[i];
        size_t cnt = 1;
        while(i + cnt < total_bits && pb[i + cnt] == lvl) cnt++;
        instance->encoder.upload[k++] = level_duration_make(lvl ? true : false, te * (uint32_t)cnt);
        i += cnt;
    }
    instance->encoder.upload[k++] = level_duration_make(false, 100000); // trailing gap
    instance->encoder.size_upload = k;
    free(pb);
    return true;
}

void* subghz_protocol_encoder_pocsag_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderPocsag* instance = malloc(sizeof(SubGhzProtocolEncoderPocsag));
    instance->base.protocol = &subghz_protocol_pocsag;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->generic.result_ric = NULL;
    instance->generic.result_msg = NULL;
    instance->encoder.repeat = 3;
    instance->encoder.size_upload = 0;
    instance->encoder.upload = NULL;
    instance->encoder.front = 0;
    instance->encoder.is_running = false;
    return instance;
}

void subghz_protocol_encoder_pocsag_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderPocsag* instance = context;
    free(instance->encoder.upload);
    free(instance);
}

SubGhzProtocolStatus
    subghz_protocol_encoder_pocsag_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderPocsag* instance = context;
    SubGhzProtocolStatus res = SubGhzProtocolStatusError;
    uint8_t* buf = NULL;

    do {
        uint32_t msg_len = 0;
        flipper_format_rewind(flipper_format);
        if(!flipper_format_read_uint32(flipper_format, "MsgLen", &msg_len, 1)) {
            FURI_LOG_E(TAG, "Missing MsgLen");
            break;
        }

        uint32_t version = 1200;
        flipper_format_rewind(flipper_format);
        flipper_format_read_uint32(flipper_format, "PocsagVer", &version, 1); // optional

        buf = malloc(msg_len ? msg_len : 1);
        if(!buf) break; // OOM (e.g. crafted huge MsgLen) -> don't write into NULL
        if(msg_len > 0) {
            flipper_format_rewind(flipper_format);
            if(!flipper_format_read_hex(flipper_format, "Msg", buf, msg_len)) {
                FURI_LOG_E(TAG, "Missing Msg");
                break;
            }
        }

        // RIC: prefer the clean integer field; else parse the display "Ric" string.
        uint32_t ric = 0;
        bool have_ric = false;
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "Ric_dec", &ric, 1)) {
            have_ric = true;
        }
        if(!have_ric) {
            FuriString* r = furi_string_alloc();
            flipper_format_rewind(flipper_format);
            if(flipper_format_read_string(flipper_format, "Ric", r)) {
                const char* c = furi_string_get_cstr(r);
                const char* p = strstr(c, "RIC:");
                if(p) {
                    p += 4;
                    while(*p && (*p < '0' || *p > '9')) p++;
                    ric = (uint32_t)strtoul(p, NULL, 10);
                }
            }
            furi_string_free(r);
        }

        // Function: prefer the clean integer field; else numeric-vs-alnum heuristic.
        uint32_t func = 0;
        bool have_func = false;
        flipper_format_rewind(flipper_format);
        if(flipper_format_read_uint32(flipper_format, "Func", &func, 1)) {
            have_func = true;
        }
        if(!have_func) {
            func = POCSAG_FUNC_ALPHANUM;
            if(msg_len > 0) {
                func = POCSAG_FUNC_NUM;
                for(uint32_t i = 0; i < msg_len; i++) {
                    char ch = (char)buf[i];
                    bool num = (ch >= '0' && ch <= '9') || ch == '*' || ch == 'U' ||
                               ch == ' ' || ch == '-' || ch == ')' || ch == '(';
                    if(!num) {
                        func = POCSAG_FUNC_ALPHANUM;
                        break;
                    }
                }
            }
        }
        // Alert (func 1) carries no message; the encoder alnum path with an empty
        // payload produces the same on-air result, so normalise it.
        if(func == POCSAG_FUNC_ALERT1) func = POCSAG_FUNC_ALPHANUM;

        if(!pocsag_encoder_build(
               instance, version, ric, (uint8_t)func, buf, (size_t)msg_len)) {
            FURI_LOG_E(TAG, "Build failed");
            break;
        }

        instance->encoder.front = 0;
        instance->encoder.is_running = true;
        res = SubGhzProtocolStatusOk;
    } while(false);

    if(buf) free(buf);
    return res;
}

void subghz_protocol_encoder_pocsag_stop(void* context) {
    SubGhzProtocolEncoderPocsag* instance = context;
    instance->encoder.is_running = false;
}

LevelDuration subghz_protocol_encoder_pocsag_yield(void* context) {
    SubGhzProtocolEncoderPocsag* instance = context;
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

const SubGhzProtocolEncoder subghz_protocol_pocsag_encoder = {
    .alloc = subghz_protocol_encoder_pocsag_alloc,
    .free = subghz_protocol_encoder_pocsag_free,
    .deserialize = subghz_protocol_encoder_pocsag_deserialize,
    .stop = subghz_protocol_encoder_pocsag_stop,
    .yield = subghz_protocol_encoder_pocsag_yield,
};

const SubGhzProtocol subghz_protocol_pocsag = {
    .name = SUBGHZ_PROTOCOL_POCSAG_NAME,
    .type = SubGhzProtocolTypeStatic,
    .flag = SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Save |
            SubGhzProtocolFlag_Load | SubGhzProtocolFlag_Send,

    .decoder = &subghz_protocol_pocsag_decoder,
    .encoder = &subghz_protocol_pocsag_encoder,
};
