#pragma once

#include "base.h"

#define SUBGHZ_PROTOCOL_MITSUBISHI_V1_NAME "Mitsubishi V1"

typedef struct SubGhzProtocolDecoderMitsubishiV1 SubGhzProtocolDecoderMitsubishiV1;
typedef struct SubGhzProtocolEncoderMitsubishiV1 SubGhzProtocolEncoderMitsubishiV1;

extern const SubGhzProtocolDecoder subghz_protocol_mitsubishi_v1_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_mitsubishi_v1_encoder;
extern const SubGhzProtocol subghz_protocol_mitsubishi_v1;

void* subghz_protocol_decoder_mitsubishi_v1_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_mitsubishi_v1_free(void* context);
void subghz_protocol_decoder_mitsubishi_v1_reset(void* context);
void subghz_protocol_decoder_mitsubishi_v1_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_mitsubishi_v1_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_mitsubishi_v1_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_mitsubishi_v1_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_mitsubishi_v1_get_string(void* context, FuriString* output);
