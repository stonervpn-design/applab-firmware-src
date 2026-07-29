#pragma once

#include "base.h"

#define SUBGHZ_PROTOCOL_AUDI_V3_NAME "Audi V3"

typedef struct SubGhzProtocolDecoderAudiV3 SubGhzProtocolDecoderAudiV3;
typedef struct SubGhzProtocolEncoderAudiV3 SubGhzProtocolEncoderAudiV3;

extern const SubGhzProtocol subghz_protocol_audi_v3;
extern const SubGhzProtocolEncoder subghz_protocol_audi_v3_encoder;

/* Encoder (raw replay of the captured frame: Button + Hop + Counter + Chk are
 * re-emitted verbatim as the Manchester waveform the decoder accepts). */
void* subghz_protocol_encoder_audi_v3_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_audi_v3_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_audi_v3_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_audi_v3_stop(void* context);
LevelDuration subghz_protocol_encoder_audi_v3_yield(void* context);

/* Decoder (decode-only — rolling code is encrypted, no encoder/roundtrip) */
void* subghz_protocol_decoder_audi_v3_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_audi_v3_free(void* context);
void subghz_protocol_decoder_audi_v3_reset(void* context);
void subghz_protocol_decoder_audi_v3_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_audi_v3_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_audi_v3_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_audi_v3_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_audi_v3_get_string(void* context, FuriString* output);
