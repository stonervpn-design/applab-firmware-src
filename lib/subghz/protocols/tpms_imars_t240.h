#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_IMARS_T240_NAME "iMars T240 TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_imars_t240_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_imars_t240_encoder;
extern const SubGhzProtocol tpms_protocol_imars_t240;

void* tpms_protocol_decoder_imars_t240_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_imars_t240_free(void* context);
void tpms_protocol_decoder_imars_t240_reset(void* context);
void tpms_protocol_decoder_imars_t240_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_imars_t240_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_imars_t240_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_imars_t240_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_imars_t240_get_string(void* context, FuriString* output);
