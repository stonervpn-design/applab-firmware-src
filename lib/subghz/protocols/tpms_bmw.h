#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_BMW_NAME "BMW TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_bmw_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_bmw_encoder;
extern const SubGhzProtocol tpms_protocol_bmw;

void* tpms_protocol_decoder_bmw_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_bmw_free(void* context);
void tpms_protocol_decoder_bmw_reset(void* context);
void tpms_protocol_decoder_bmw_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_bmw_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_bmw_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_bmw_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_bmw_get_string(void* context, FuriString* output);

// Invert analyze(): write pressure/temperature into the frame bytes, recompute the
// CRC, store the low 64 bits back into g->data and set g->data_count_bit = 88.
void tpms_protocol_bmw_pack(TPMSBlockGeneric* g);
