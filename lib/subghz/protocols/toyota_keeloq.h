#pragma once

#include <furi.h>
#include <lib/subghz/protocols/base.h>
#include <lib/subghz/types.h>
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h>
#include <flipper_format/flipper_format.h>
#include <lib/toolbox/level_duration.h>

#include "../defines.h"

// Toyota/Lexus KeeLoq-hopping RKE fob, two on-air variants selected by the
// width of the very first HIGH pulse (>310us -> Variant A, else Variant B):
//   Variant A (Corolla, 433.92): fixed-width PWM pairs.
//   Variant B (Tundra/Tacoma, 315): NRZ, one bit per pulse.
// DECODE-ONLY: the 32-bit KeeLoq hop stays encrypted (no per-manufacturer key
// resident in this firmware) — this decoder surfaces Serial/Button/raw Hop
// only, never decrypts, never validates a CRC/crypto check. There is no
// encoder: TX would require the key. NOTE: this is a distinct protocol from
// "Toyota V0" (subghz_protocol_toyota_v0 / toyota_v0.c), which is an
// unrelated OOK-Manchester 96-bit Estima decoder — do not confuse the two.
#define TOYOTA_KEELOQ_PROTOCOL_NAME "Toyota KeeLoq"

extern const SubGhzProtocol toyota_keeloq_protocol;

void* subghz_protocol_decoder_toyota_keeloq_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_toyota_keeloq_free(void* context);
void subghz_protocol_decoder_toyota_keeloq_reset(void* context);
void subghz_protocol_decoder_toyota_keeloq_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_toyota_keeloq_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_toyota_keeloq_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_toyota_keeloq_deserialize(
    void* context,
    FlipperFormat* flipper_format);
void subghz_protocol_decoder_toyota_keeloq_get_string(void* context, FuriString* output);
