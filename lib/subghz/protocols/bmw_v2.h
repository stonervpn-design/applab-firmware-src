#pragma once

#include "base.h"

#define SUBGHZ_PROTOCOL_BMW_V2_NAME "BMW_v2"

typedef struct SubGhzProtocolDecoderBmwV2 SubGhzProtocolDecoderBmwV2;
typedef struct SubGhzProtocolEncoderBmwV2 SubGhzProtocolEncoderBmwV2;

extern const SubGhzProtocol subghz_protocol_bmw_v2;

/* Decoder */
void* subghz_protocol_decoder_bmw_v2_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_bmw_v2_free(void* context);
void subghz_protocol_decoder_bmw_v2_reset(void* context);
void subghz_protocol_decoder_bmw_v2_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_bmw_v2_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_bmw_v2_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_bmw_v2_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_bmw_v2_get_string(void* context, FuriString* output);

/* Encoder */
void* subghz_protocol_encoder_bmw_v2_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_bmw_v2_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_bmw_v2_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_bmw_v2_stop(void* context);
LevelDuration subghz_protocol_encoder_bmw_v2_yield(void* context);
