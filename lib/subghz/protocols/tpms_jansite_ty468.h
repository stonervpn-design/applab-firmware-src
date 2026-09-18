#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_JANSITE_TY468_NAME "Jansite TY468 TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_jansite_ty468_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_jansite_ty468_encoder;
extern const SubGhzProtocol tpms_protocol_jansite_ty468;

void* tpms_protocol_decoder_jansite_ty468_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_jansite_ty468_free(void* context);
void tpms_protocol_decoder_jansite_ty468_reset(void* context);
void tpms_protocol_decoder_jansite_ty468_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_jansite_ty468_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_jansite_ty468_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_jansite_ty468_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_jansite_ty468_get_string(void* context, FuriString* output);
