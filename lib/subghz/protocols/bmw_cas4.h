#pragma once

#include <furi.h>
#include <lib/subghz/protocols/base.h>
#include <lib/subghz/types.h>
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h>
#include <flipper_format/flipper_format.h>

#include "../defines.h"

// Ported from KAT (src/protocols/bmw_cas4.rs). Fixed-code Manchester frame,
// validated by marker bytes 0x30 / 0xC5. Decode-only (no rolling fields).
#define BMW_CAS4_PROTOCOL_NAME "BMW CAS4"

typedef struct SubGhzProtocolDecoderBmwCas4 SubGhzProtocolDecoderBmwCas4;

extern const SubGhzProtocol bmw_cas4_protocol;

void* subghz_protocol_decoder_bmw_cas4_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_bmw_cas4_free(void* context);
void subghz_protocol_decoder_bmw_cas4_reset(void* context);
void subghz_protocol_decoder_bmw_cas4_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_bmw_cas4_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_bmw_cas4_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_bmw_cas4_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_bmw_cas4_get_string(void* context, FuriString* output);
