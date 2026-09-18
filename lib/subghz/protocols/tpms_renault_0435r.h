#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_RENAULT_0435R_NAME "Renault0435R TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_renault_0435r_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_renault_0435r_encoder;
extern const SubGhzProtocol tpms_protocol_renault_0435r;

void* tpms_protocol_decoder_renault_0435r_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_renault_0435r_free(void* context);
void tpms_protocol_decoder_renault_0435r_reset(void* context);
void tpms_protocol_decoder_renault_0435r_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_renault_0435r_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_renault_0435r_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_renault_0435r_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_renault_0435r_get_string(void* context, FuriString* output);
