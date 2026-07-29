#pragma once

#include "base.h"

#define SUBGHZ_PROTOCOL_MAZDA_V1_NAME "Mazda V1"

typedef struct SubGhzProtocolDecoderMazdaV1 SubGhzProtocolDecoderMazdaV1;
typedef struct SubGhzProtocolEncoderMazdaV1 SubGhzProtocolEncoderMazdaV1;

extern const SubGhzProtocol subghz_protocol_mazda_v1;

/* Decoder */
void* subghz_protocol_decoder_mazda_v1_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_mazda_v1_free(void* context);
void subghz_protocol_decoder_mazda_v1_reset(void* context);
void subghz_protocol_decoder_mazda_v1_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_mazda_v1_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_mazda_v1_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_mazda_v1_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_mazda_v1_get_string(void* context, FuriString* output);

/* Encoder */
void* subghz_protocol_encoder_mazda_v1_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_mazda_v1_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_mazda_v1_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_mazda_v1_stop(void* context);
LevelDuration subghz_protocol_encoder_mazda_v1_yield(void* context);
