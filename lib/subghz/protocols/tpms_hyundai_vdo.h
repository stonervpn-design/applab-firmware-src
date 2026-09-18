#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_HYUNDAI_VDO_NAME "Hyundai/Kia TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_hyundai_vdo_decoder;
extern const SubGhzProtocol tpms_protocol_hyundai_vdo;

void* tpms_protocol_decoder_hyundai_vdo_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_hyundai_vdo_free(void* context);
void tpms_protocol_decoder_hyundai_vdo_reset(void* context);
void tpms_protocol_decoder_hyundai_vdo_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_hyundai_vdo_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_hyundai_vdo_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_hyundai_vdo_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_hyundai_vdo_get_string(void* context, FuriString* output);
