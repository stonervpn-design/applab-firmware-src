#pragma once

#include "base.h"

#define SUBGHZ_PROTOCOL_HONDA_V6_NAME "HondaV6"

typedef struct SubGhzProtocolDecoderHondaV6 SubGhzProtocolDecoderHondaV6;
typedef struct SubGhzProtocolEncoderHondaV6 SubGhzProtocolEncoderHondaV6;

extern const SubGhzProtocol subghz_protocol_honda_v6;

/* Decoder */
void* subghz_protocol_decoder_honda_v6_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_honda_v6_free(void* context);
void subghz_protocol_decoder_honda_v6_reset(void* context);
void subghz_protocol_decoder_honda_v6_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_honda_v6_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_honda_v6_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_honda_v6_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_honda_v6_get_string(void* context, FuriString* output);

/* Encoder */
void* subghz_protocol_encoder_honda_v6_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_honda_v6_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_honda_v6_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_honda_v6_stop(void* context);
LevelDuration subghz_protocol_encoder_honda_v6_yield(void* context);
