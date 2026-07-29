#pragma once

#include "base.h"

#define SUBGHZ_PROTOCOL_LEXUS_V0_NAME "Lexus V0"

typedef struct SubGhzProtocolDecoderLexusV0 SubGhzProtocolDecoderLexusV0;
typedef struct SubGhzProtocolEncoderLexusV0 SubGhzProtocolEncoderLexusV0;

extern const SubGhzProtocol subghz_protocol_lexus_v0;
extern const SubGhzProtocolEncoder subghz_protocol_lexus_v0_encoder;

/* Encoder (raw replay: Serial + 160-bit Rolling re-emitted verbatim as the
 * Manchester waveform the decoder accepts). */
void* subghz_protocol_encoder_lexus_v0_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_lexus_v0_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_lexus_v0_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_lexus_v0_stop(void* context);
LevelDuration subghz_protocol_encoder_lexus_v0_yield(void* context);

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
