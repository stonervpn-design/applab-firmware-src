#pragma once

/*
 * honda_v8.h — Honda KeeLoq Inverted PWM Protocol for Flipper Zero
 *
 * Source: Pandora firmware — Crypto_Scramble_Finalize_Keeloq @ 0x9C00
 *
 * OOK PWM inverted, 66-bit KeeLoq, 5 repeats, 433 MHz
 *
 * Exact firmware timings:
 *   Preamble: 23 × HIGH 400µs + LOW 400µs
 *   Sync gap: LOW 4000µs
 *   Bit 0:   HIGH 400µs + LOW 400µs
 *   Bit 1:   HIGH 400µs + LOW ~50µs (PWM INVERTED)
 *   Trail:   LOW 15600µs
 *   Reps:    5 (with new RC in each one)
 *   Frame:   66-bit LSB-first over the air
 */

#include <lib/subghz/protocols/base.h>
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h>

#define SUBGHZ_PROTOCOL_HONDA_V8_NAME "HondaV8"

#define HV8_TE_BASE            400u
#define HV8_TE_DELTA           150u
#define HV8_TE_BIT1_LOW        50u
#define HV8_PREAMBLE_COUNT     23u
#define HV8_SYNC_GAP_US        4000u
#define HV8_TRAIL_US           15600u
#define HV8_SYNC_MIN_US        2500u
#define HV8_REPEAT             5u
#define HV8_FRAME_BITS         66u
#define HV8_ROLLING_WINDOW     128u
/* Buffer encoder:
 *   Per rep: 23×2 + 1 + 66×2 + 1 = 180 symbols
 *   5 reps: 900 + margin = 1200 */
#define HV8_ENC_BUF_SIZE       1200u

/* KeeLoq engine */
#define HV8_NLF                0x3A5C742EUL
#define HV8_ROUNDS             528

#define HV8_DEC_FUNC_MASK      0x0000000FUL
#define HV8_DEC_DISC_MASK      0x00000FF0UL
#define HV8_DEC_DISC_SHIFT     4
#define HV8_DEC_RC_MASK        0xFFFF0000UL
#define HV8_DEC_RC_SHIFT       16

#define HV8_BTN_PANIC         0x0u
#define HV8_BTN_LOCK          0x1u
#define HV8_BTN_TRUNK         0x2u
#define HV8_BTN_REMOTE_START  0x4u
#define HV8_BTN_UNLOCK        0x8u
#define HV8_BTN_UNLOCK2       0x9u
#define HV8_BTN_CUSTOM_MAX    5u

/* ══════════════════════════════════════════════════════════════════
 * MANUFACTURER KEY TABLE — 21 entries
 * Source: Pandora firmware @ 0x14D60
 * key_64 = (high32 << 32) | low32
 * ══════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t low;
    uint32_t high;
} HondaV8MfrKey;

#define HV8_MFR_KEY_COUNT 21

extern const HondaV8MfrKey honda_v8_mfr_keys[HV8_MFR_KEY_COUNT];

/* ══════════════════════════════════════════════════════════════════
 * GENERIC.DATA LAYOUT — 64 bits
 *
 * [63:36] 28-bit serial
 * [35:32] button 4 bits
 * [31:16] rolling 16 bits (RC)
 * [15:8]  discriminant 8 bits
 * [7:4]   key_idx 4 bits (table index)
 * [3:2]   proto_mode 2 bits (1 = KL)
 * [1:0]   status/flags 2 bits
 * ═════════════════════════════════════════════════════════════════════════ */

#define HV8_PROTO_MODE   1u

typedef struct {
    uint32_t encrypted;
    uint32_t serial;
    uint8_t  button;
    uint8_t  status;
    uint32_t decrypted;
    uint16_t rolling_code;
    uint8_t  discriminant;
    int8_t   key_idx;
} HondaV8Frame;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder        decoder;
    SubGhzBlockGeneric        generic;

    HondaV8Frame frame;
    bool         frame_valid;
    uint16_t     last_rolling;
    bool         has_last_rolling;
    FuriString*  result_str;
} SubGhzProtocolDecoderHondaV8;

typedef struct {
    SubGhzProtocolEncoderBase  base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric         generic;

    uint64_t key;
    uint32_t serial;
    uint16_t rolling_code;
    uint8_t  btn;
    uint8_t  status;
    uint8_t  active_button;
    bool     frame_bits[HV8_FRAME_BITS];
} SubGhzProtocolEncoderHondaV8;

extern const SubGhzProtocolDecoder subghz_protocol_honda_v8_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_honda_v8_encoder;
extern const SubGhzProtocol        subghz_protocol_honda_v8;

void*  subghz_protocol_decoder_honda_v8_alloc(SubGhzEnvironment* env);
void   subghz_protocol_decoder_honda_v8_free(void* ctx);
void   subghz_protocol_decoder_honda_v8_reset(void* ctx);
void   subghz_protocol_decoder_honda_v8_feed(void* ctx, bool level, uint32_t dur);
uint8_t subghz_protocol_decoder_honda_v8_get_hash_data(void* ctx);
SubGhzProtocolStatus subghz_protocol_decoder_honda_v8_serialize(
    void* ctx, FlipperFormat* ff, SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_honda_v8_deserialize(
    void* ctx, FlipperFormat* ff);
void   subghz_protocol_decoder_honda_v8_get_string(void* ctx, FuriString* out);

void*  subghz_protocol_encoder_honda_v8_alloc(SubGhzEnvironment* env);
void   subghz_protocol_encoder_honda_v8_free(void* ctx);
SubGhzProtocolStatus subghz_protocol_encoder_honda_v8_deserialize(
    void* ctx, FlipperFormat* ff);
void   subghz_protocol_encoder_honda_v8_stop(void* ctx);
LevelDuration subghz_protocol_encoder_honda_v8_yield(void* ctx);
