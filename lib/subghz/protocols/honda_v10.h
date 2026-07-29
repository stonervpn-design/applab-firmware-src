#pragma once

/*
 * honda_v10.h — Honda V2H4 Aftermarket OEM FSK Protocol for Flipper Zero
 *
 * Observed in captures named HondaV2H4_OEM_*.sub. 2FSK Manchester,
 * captured native (not OOK-compressed) at CC1101 preset
 * FuriHalSubGhzPreset2FSKDev476Async. Half-bit ≈ 104µs.
 *
 * Burst behaviour: every button press emits FOUR repetitions seamlessly
 * back-to-back inside one burst (pattern AABB): two frames with status
 * bit7=1 (type A) followed by two with bit7=0 (type B). Tail differs
 * between A and B but is fully deterministic from (counter, button,
 * status), so the same press is identifiable by the counter alone.
 *
 * Frame layout (12 bytes / 96 bits, 2 trailing padding bits → 98b bursts):
 *   bytes 0..2  : 24-bit fingerprint/serial (constant per device)
 *   byte  3 hi  : button (0xF=Lock, 0xE=Unlock — others unknown)
 *   byte  3 lo  : flags (observed 0x7)
 *   bytes 4..6  : 24-bit rolling counter, stored BITWISE INVERTED
 *                 (fresh EEPROM 0xFFFFFF → real count 0). f->counter is
 *                 the real (de-inverted) value.
 *   byte  7     : status — bit7,bit4 = burst frame-index (A/B selector),
 *                 bits 3,5,6 always 0, bits 0..2 and bit1 carry cipher
 *                 input, not fully decoded.
 *   bytes 8..11 : 32-bit cipher tail. Bit 28 is a constant framing bit.
 *                 Full cipher algorithm is proprietary (similar family
 *                 to HondaV9 S-box rolling scheme, not reversed).
 */

#include <lib/subghz/protocols/base.h>
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h>

#define SUBGHZ_PROTOCOL_HONDA_V10_NAME "HondaV10"

#define HV10_TE_SHORT          104u
#define HV10_TE_LONG           208u
#define HV10_TE_DELTA          35u
#define HV10_GUARD_TIME_US     1000u
#define HV10_PREAMBLE_CYCLES   55u
#define HV10_MIN_PREAMBLE      20u
#define HV10_FRAME_BYTES       12u
#define HV10_FRAME_BITS        96u
#define HV10_MIN_BITS          88u        /* accept captures with minor truncation */
#define HV10_HALF_BIT_BUF      2048u

/* Button nibble codes (observed high-nibble of byte 3) */
#define HV10_BTN_LOCK          0xFu
#define HV10_BTN_UNLOCK        0xEu

typedef struct {
    uint32_t serial;       /* 24 bit, bytes 0..2 */
    uint8_t  button;       /* 4 bit, byte 3 high nibble */
    uint8_t  flags;        /* 4 bit, byte 3 low nibble */
    uint32_t counter;      /* 24 bit, bytes 4..6 */
    uint8_t  status;       /* byte 7 */
    uint32_t tail;         /* 32 bit, bytes 8..11 */
} HondaV10Frame;

typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder        decoder;
    SubGhzBlockGeneric        generic;

    uint8_t  half_bits[HV10_HALF_BIT_BUF];
    uint16_t hb_count;
    bool     frame_valid;
    HondaV10Frame frame;
} SubGhzProtocolDecoderHondaV10;

typedef struct {
    SubGhzProtocolEncoderBase  base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric         generic;
    HondaV10Frame              frame;
} SubGhzProtocolEncoderHondaV10;

extern const SubGhzProtocolDecoder subghz_protocol_honda_v10_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_honda_v10_encoder;
extern const SubGhzProtocol        subghz_protocol_honda_v10;

/* Encoder (raw replay: re-emits the captured 96-bit frame — serial, button,
 * counter, status and 32-bit cipher tail — as the native 2FSK Manchester
 * waveform the decoder accepts. The cipher tail is replayed verbatim, so a
 * counter-incremented frame is NOT produced: this round-trips the decoder but
 * is not a live rolling-code attack.) */
void*  subghz_protocol_encoder_honda_v10_alloc(SubGhzEnvironment* env);
void   subghz_protocol_encoder_honda_v10_free(void* ctx);
SubGhzProtocolStatus subghz_protocol_encoder_honda_v10_deserialize(
    void* ctx, FlipperFormat* ff);
void   subghz_protocol_encoder_honda_v10_stop(void* ctx);
LevelDuration subghz_protocol_encoder_honda_v10_yield(void* ctx);

void*  subghz_protocol_decoder_honda_v10_alloc(SubGhzEnvironment* env);
void   subghz_protocol_decoder_honda_v10_free(void* ctx);
void   subghz_protocol_decoder_honda_v10_reset(void* ctx);
void   subghz_protocol_decoder_honda_v10_feed(void* ctx, bool level, uint32_t dur);
uint8_t subghz_protocol_decoder_honda_v10_get_hash_data(void* ctx);
SubGhzProtocolStatus subghz_protocol_decoder_honda_v10_serialize(
    void* ctx, FlipperFormat* ff, SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_honda_v10_deserialize(
    void* ctx, FlipperFormat* ff);
void   subghz_protocol_decoder_honda_v10_get_string(void* ctx, FuriString* out);
