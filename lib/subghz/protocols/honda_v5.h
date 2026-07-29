#pragma once

#include "base.h"

#define SUBGHZ_PROTOCOL_HONDA_V5_NAME "HondaV5"

typedef struct SubGhzProtocolDecoderHondaV5 SubGhzProtocolDecoderHondaV5;
typedef struct SubGhzProtocolEncoderHondaV5 SubGhzProtocolEncoderHondaV5;

extern const SubGhzProtocol subghz_protocol_honda_v5;

/* Decoder */
void* subghz_protocol_decoder_honda_v5_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_honda_v5_free(void* context);
void subghz_protocol_decoder_honda_v5_reset(void* context);
void subghz_protocol_decoder_honda_v5_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_honda_v5_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_honda_v5_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_honda_v5_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_honda_v5_get_string(void* context, FuriString* output);

/* Encoder */
void* subghz_protocol_encoder_honda_v5_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_honda_v5_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_honda_v5_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_honda_v5_stop(void* context);
LevelDuration subghz_protocol_encoder_honda_v5_yield(void* context);
