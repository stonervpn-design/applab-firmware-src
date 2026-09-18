#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_KIA_NAME "Kia TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_kia_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_kia_encoder;
extern const SubGhzProtocol tpms_protocol_kia;

void* tpms_protocol_decoder_kia_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_kia_free(void* context);
void tpms_protocol_decoder_kia_reset(void* context);
void tpms_protocol_decoder_kia_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_kia_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_kia_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_kia_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_kia_get_string(void* context, FuriString* output);
