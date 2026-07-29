#pragma once

#include "base.h"

#define SUBGHZ_PROTOCOL_CHEVROLET_V0_NAME "ChevroletV0"

typedef struct SubGhzProtocolDecoderChevroletV0 SubGhzProtocolDecoderChevroletV0;
typedef struct SubGhzProtocolEncoderChevroletV0 SubGhzProtocolEncoderChevroletV0;

extern const SubGhzProtocol subghz_protocol_chevrolet_v0;

/* Decoder */
void* subghz_protocol_decoder_chevrolet_v0_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_chevrolet_v0_free(void* context);
void subghz_protocol_decoder_chevrolet_v0_reset(void* context);
void subghz_protocol_decoder_chevrolet_v0_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_chevrolet_v0_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_chevrolet_v0_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_chevrolet_v0_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_chevrolet_v0_get_string(void* context, FuriString* output);

/* Encoder */
void* subghz_protocol_encoder_chevrolet_v0_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_chevrolet_v0_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_chevrolet_v0_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_chevrolet_v0_stop(void* context);
LevelDuration subghz_protocol_encoder_chevrolet_v0_yield(void* context);
