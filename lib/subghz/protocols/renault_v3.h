#pragma once

#include "base.h"

#define SUBGHZ_PROTOCOL_RENAULT_V3_NAME "Renault V3"

typedef struct SubGhzProtocolDecoderRenaultV3 SubGhzProtocolDecoderRenaultV3;
typedef struct SubGhzProtocolEncoderRenaultV3 SubGhzProtocolEncoderRenaultV3;

extern const SubGhzProtocol subghz_protocol_renault_v3;

/* Decoder */
void* subghz_protocol_decoder_renault_v3_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_renault_v3_free(void* context);
void subghz_protocol_decoder_renault_v3_reset(void* context);
void subghz_protocol_decoder_renault_v3_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_renault_v3_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_renault_v3_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_renault_v3_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_renault_v3_get_string(void* context, FuriString* output);

/* Encoder */
void* subghz_protocol_encoder_renault_v3_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_renault_v3_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_renault_v3_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_renault_v3_stop(void* context);
LevelDuration subghz_protocol_encoder_renault_v3_yield(void* context);
