#pragma once

#include "base.h"

#define SUBGHZ_PROTOCOL_RENAULT_V4_NAME "Renault V4"

#define RENAULT_V4_TE_SHORT   125u
#define RENAULT_V4_TE_LONG    250u
#define RENAULT_V4_TE_DELTA    70u
#define RENAULT_V4_SYNC_MIN   800u

#define RENAULT_V4_MIN_BITS    80u
#define RENAULT_V4_FRAME_BITS  88u

extern const SubGhzProtocolDecoder subghz_protocol_renault_v4_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_renault_v4_encoder;
extern const SubGhzProtocol subghz_protocol_renault_v4;

void* subghz_protocol_decoder_renault_v4_alloc(SubGhzEnvironment* environment);
void  subghz_protocol_decoder_renault_v4_free(void* context);
void  subghz_protocol_decoder_renault_v4_reset(void* context);
void  subghz_protocol_decoder_renault_v4_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_renault_v4_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_renault_v4_serialize(
    void* context, FlipperFormat* flipper_format, SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_renault_v4_deserialize(
    void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_renault_v4_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_renault_v4_alloc(SubGhzEnvironment* environment);
void  subghz_protocol_encoder_renault_v4_free(void* context);
void  subghz_protocol_encoder_renault_v4_stop(void* context);
LevelDuration subghz_protocol_encoder_renault_v4_yield(void* context);
SubGhzProtocolStatus subghz_protocol_encoder_renault_v4_deserialize(
    void* context, FlipperFormat* flipper_format);
