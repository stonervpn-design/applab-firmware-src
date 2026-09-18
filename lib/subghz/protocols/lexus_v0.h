#pragma once

#include "base.h"

#define SUBGHZ_PROTOCOL_LEXUS_V0_NAME "Lexus V0"

typedef struct SubGhzProtocolDecoderLexusV0 SubGhzProtocolDecoderLexusV0;

extern const SubGhzProtocol subghz_protocol_lexus_v0;

/* Decoder (decode-only — rolling code is encrypted, no encoder/roundtrip) */
void* subghz_protocol_decoder_lexus_v0_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_lexus_v0_free(void* context);
void subghz_protocol_decoder_lexus_v0_reset(void* context);
void subghz_protocol_decoder_lexus_v0_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_lexus_v0_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_lexus_v0_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_lexus_v0_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_lexus_v0_get_string(void* context, FuriString* output);
