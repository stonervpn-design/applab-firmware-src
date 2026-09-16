#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_ELANTRA2012_NAME "Elantra 2012 TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_tpms_elantra2012_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_tpms_elantra2012_encoder;
extern const SubGhzProtocol tpms_protocol_tpms_elantra2012;

void* tpms_protocol_decoder_elantra2012_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_elantra2012_free(void* context);
void tpms_protocol_decoder_elantra2012_reset(void* context);
void tpms_protocol_decoder_elantra2012_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_elantra2012_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_elantra2012_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_elantra2012_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_elantra2012_get_string(void* context, FuriString* output);

void* tpms_protocol_encoder_elantra2012_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_elantra2012_free(void* context);
void tpms_protocol_encoder_elantra2012_stop(void* context);
LevelDuration tpms_protocol_encoder_elantra2012_yield(void* context);

SubGhzProtocolStatus
    tpms_protocol_encoder_elantra2012_deserialize(void* context, FlipperFormat* flipper_format);

// Invert elantra2012_analyze(): write pressure/temperature/id/battery into the 8
// wire bytes using rtl_433's raw scaling (b[0] = kPa - 60, b[1] = C + 50),
// recompute the CRC-8/SMBUS, store the bytes back in g->data and set
// g->data_count_bit = 64. Used to synthesize a (possibly floored 0-PSI) frame for TX.
void tpms_protocol_tpms_elantra2012_pack(TPMSBlockGeneric* g);
