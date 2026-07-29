#pragma once

/*
 * honda_v7.h — Honda OOK PWM Protocol for Flipper Zero
 *
 * Source: Pandora firmware — Brand_Auto_Honda_TX @ 0x13F24
 *
 * OOK PWM, 11 bytes, 88 bits, LSB-first, 315/433 MHz
 *
 * Exact firmware timings:
 *   Preamble: 312 × HIGH 250µs + LOW 250µs
 *   Bit 0:   HIGH 250µs + LOW 250µs
 *   Bit 1:   HIGH 480µs + LOW 480µs
 *   Guard:   HIGH 100µs + LOW 740µs before preamble
 *   Frame:   11 bytes, 88 bits, LSB-first per byte
 */

#include <lib/subghz/protocols/base.h>
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h>

#define SUBGHZ_PROTOCOL_HONDA_V7_NAME "HondaV7"

#define HV7_TE_SHORT          250u
#define HV7_TE_LONG           480u
#define HV7_TE_DELTA          100u
#define HV7_GUARD_HIGH_US     100u
#define HV7_GUARD_LOW_US      740u
#define HV7_PREAMBLE_CYCLES   312u
#define HV7_FRAME_BYTES       11u
#define HV7_FRAME_BITS        88u
#define HV7_MIN_BITS          64u
#define HV7_MIN_PREAMBLE      20u
/* Buffer encoder:
 *   2 (guard) + 312×2 (preamble) + 88×2 (data) + 1 (tail) = 803 → 900 */
#define HV7_ENC_BUF_SIZE      900u

#define HV7_BTN_PANIC         0x0u
#define HV7_BTN_LOCK          0x1u
#define HV7_BTN_TRUNK         0x2u
#define HV7_BTN_REMOTE_START  0x4u
#define HV7_BTN_UNLOCK        0x8u
#define HV7_BTN_UNLOCK2       0x9u
#define HV7_BTN_CUSTOM_MAX    5u

/* ══════════════════════════════════════════════════════════════════
 * GENERIC.DATA LAYOUT — 64 bits
 *
 * [63:36] 28-bit serial
 * [35:32] button 4 bits
 * [31:16] rolling 16 bits (counter[15:0])
 * [15:8]  mode 8 bits
 * [7:4]   key_idx 4 bits (0xF for OOK)
 * [3:2]   proto_mode 2 bits (0 = OOK)
 * [1:0]   status/flags 2 bits
 * ═════════════════════════════════════════════════════════════════════════ */

#define HV7_PROTO_MODE   0u

typedef struct {
    uint8_t  button;
    uint32_t serial;
    uint32_t counter;
    uint8_t  mode;
    uint8_t  checksum;
    uint8_t  extra[2];
} HondaV7Frame;

typedef enum {
    HV7Step_Reset = 0,
    HV7Step_Preamble,
    HV7Step_Data,
} HV7DecoderStep;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder        decoder;
    SubGhzBlockGeneric        generic;

    HV7DecoderStep step;
    uint16_t       preamble_count;
    uint8_t        data_buf[16];
    uint8_t        bit_count;
    uint32_t       last_high_dur;
    bool           frame_valid;
    HondaV7Frame   frame;
} SubGhzProtocolDecoderHondaV7;

typedef struct {
    SubGhzProtocolEncoderBase  base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric         generic;

    HondaV7Frame frame;
    uint8_t      active_button;
} SubGhzProtocolEncoderHondaV7;

extern const SubGhzProtocolDecoder subghz_protocol_honda_v7_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_honda_v7_encoder;
extern const SubGhzProtocol        subghz_protocol_honda_v7;

void*  subghz_protocol_decoder_honda_v7_alloc(SubGhzEnvironment* env);
void   subghz_protocol_decoder_honda_v7_free(void* ctx);
void   subghz_protocol_decoder_honda_v7_reset(void* ctx);
void   subghz_protocol_decoder_honda_v7_feed(void* ctx, bool level, uint32_t dur);
uint8_t subghz_protocol_decoder_honda_v7_get_hash_data(void* ctx);
SubGhzProtocolStatus subghz_protocol_decoder_honda_v7_serialize(
    void* ctx, FlipperFormat* ff, SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_honda_v7_deserialize(
    void* ctx, FlipperFormat* ff);
void   subghz_protocol_decoder_honda_v7_get_string(void* ctx, FuriString* out);

void*  subghz_protocol_encoder_honda_v7_alloc(SubGhzEnvironment* env);
void   subghz_protocol_encoder_honda_v7_free(void* ctx);
SubGhzProtocolStatus subghz_protocol_encoder_honda_v7_deserialize(
    void* ctx, FlipperFormat* ff);
void   subghz_protocol_encoder_honda_v7_stop(void* ctx);
LevelDuration subghz_protocol_encoder_honda_v7_yield(void* ctx);
