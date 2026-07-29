#pragma once

#include "base.h"

#define SUBGHZ_PROTOCOL_HONDA_V4_NAME "HondaV4"

typedef struct SubGhzProtocolDecoderHondaV4 SubGhzProtocolDecoderHondaV4;
typedef struct SubGhzProtocolEncoderHondaV4 SubGhzProtocolEncoderHondaV4;

extern const SubGhzProtocol subghz_protocol_honda_v4;

/* Decoder */
void* subghz_protocol_decoder_honda_v4_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_honda_v4_free(void* context);
void subghz_protocol_decoder_honda_v4_reset(void* context);
void subghz_protocol_decoder_honda_v4_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_honda_v4_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_honda_v4_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_honda_v4_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_honda_v4_get_string(void* context, FuriString* output);

/* Encoder */
void* subghz_protocol_encoder_honda_v4_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_honda_v4_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_honda_v4_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_honda_v4_stop(void* context);
LevelDuration subghz_protocol_encoder_honda_v4_yield(void* context);
