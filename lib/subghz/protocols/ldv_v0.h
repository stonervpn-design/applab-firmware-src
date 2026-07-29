#pragma once

#include "base.h"

#define SUBGHZ_PROTOCOL_LDV_V0_NAME "LDV V0"

typedef struct SubGhzProtocolDecoderLdvV0 SubGhzProtocolDecoderLdvV0;
typedef struct SubGhzProtocolEncoderLdvV0 SubGhzProtocolEncoderLdvV0;

extern const SubGhzProtocolDecoder subghz_protocol_ldv_v0_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_ldv_v0_encoder;
extern const SubGhzProtocol subghz_protocol_ldv_v0;

void* subghz_protocol_encoder_ldv_v0_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_ldv_v0_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_ldv_v0_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_ldv_v0_stop(void* context);
LevelDuration subghz_protocol_encoder_ldv_v0_yield(void* context);

void* subghz_protocol_decoder_ldv_v0_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_ldv_v0_free(void* context);
void subghz_protocol_decoder_ldv_v0_reset(void* context);
void subghz_protocol_decoder_ldv_v0_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_ldv_v0_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_ldv_v0_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_ldv_v0_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_ldv_v0_get_string(void* context, FuriString* output);
