#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_HONDA_NAME "Honda TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_honda_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_honda_encoder;
extern const SubGhzProtocol tpms_protocol_honda;

void* tpms_protocol_decoder_honda_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_honda_free(void* context);
void tpms_protocol_decoder_honda_reset(void* context);
void tpms_protocol_decoder_honda_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_honda_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_honda_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_honda_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_honda_get_string(void* context, FuriString* output);

void* tpms_protocol_encoder_honda_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_honda_free(void* context);
void tpms_protocol_encoder_honda_stop(void* context);
LevelDuration tpms_protocol_encoder_honda_yield(void* context);

SubGhzProtocolStatus
    tpms_protocol_encoder_honda_deserialize(void* context, FlipperFormat* flipper_format);

// Invert honda_analyze(): write pressure/temperature/id into the 8 wire bytes,
// recompute the CRC-8/SMBUS, store the bytes back in g->data and set
// g->data_count_bit = 64. Used to synthesize a (possibly 0-PSI) frame for TX.
void tpms_protocol_honda_pack(TPMSBlockGeneric* g);
