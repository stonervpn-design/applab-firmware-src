#pragma once

#include "base.h"

#define SUBGHZ_PROTOCOL_DUCATO_V0_NAME "Ducato V0"

typedef struct SubGhzProtocolDecoderDucatoV0 SubGhzProtocolDecoderDucatoV0;
typedef struct SubGhzProtocolEncoderDucatoV0 SubGhzProtocolEncoderDucatoV0;

extern const SubGhzProtocolDecoder subghz_protocol_ducato_v0_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_ducato_v0_encoder;
extern const SubGhzProtocol subghz_protocol_ducato_v0;

void* subghz_protocol_encoder_ducato_v0_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_ducato_v0_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_ducato_v0_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_ducato_v0_stop(void* context);
LevelDuration subghz_protocol_encoder_ducato_v0_yield(void* context);

void* subghz_protocol_decoder_ducato_v0_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_ducato_v0_free(void* context);
void subghz_protocol_decoder_ducato_v0_reset(void* context);
void subghz_protocol_decoder_ducato_v0_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_ducato_v0_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_ducato_v0_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_ducato_v0_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_ducato_v0_get_string(void* context, FuriString* output);
