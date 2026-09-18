#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_TRW_NAME "TRW TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_trw_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_trw_encoder;
extern const SubGhzProtocol tpms_protocol_trw;

void* tpms_protocol_decoder_trw_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_trw_free(void* context);
void tpms_protocol_decoder_trw_reset(void* context);
void tpms_protocol_decoder_trw_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_trw_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_trw_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_trw_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_trw_get_string(void* context, FuriString* output);
