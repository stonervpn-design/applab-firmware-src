#pragma once

#include "base.h"

#define SUBGHZ_PROTOCOL_RETEKESS_NAME "Retekess"

typedef struct SubGhzProtocolDecoderRetekess SubGhzProtocolDecoderRetekess;
typedef struct SubGhzProtocolEncoderRetekess SubGhzProtocolEncoderRetekess;

extern const SubGhzProtocolDecoder subghz_protocol_retekess_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_retekess_encoder;
extern const SubGhzProtocol subghz_protocol_retekess;

void* subghz_protocol_encoder_retekess_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_retekess_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_retekess_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_retekess_stop(void* context);
LevelDuration subghz_protocol_encoder_retekess_yield(void* context);

void* subghz_protocol_decoder_retekess_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_retekess_free(void* context);
void subghz_protocol_decoder_retekess_reset(void* context);
void subghz_protocol_decoder_retekess_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_retekess_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_retekess_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_retekess_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_retekess_get_string(void* context, FuriString* output);
