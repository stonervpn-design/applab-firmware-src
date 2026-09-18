#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_ABARTH124_NAME "Abarth124 TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_abarth124_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_abarth124_encoder;
extern const SubGhzProtocol tpms_protocol_abarth124;

void* tpms_protocol_decoder_abarth124_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_abarth124_free(void* context);
void tpms_protocol_decoder_abarth124_reset(void* context);
void tpms_protocol_decoder_abarth124_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_abarth124_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_abarth124_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_abarth124_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_abarth124_get_string(void* context, FuriString* output);
