#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_TRW_OOK_NAME "TRW OOK TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_tpms_trw_ook_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_tpms_trw_ook_encoder;
extern const SubGhzProtocol tpms_protocol_tpms_trw_ook;

void* tpms_protocol_decoder_tpms_trw_ook_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_tpms_trw_ook_free(void* context);
void tpms_protocol_decoder_tpms_trw_ook_reset(void* context);
void tpms_protocol_decoder_tpms_trw_ook_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_tpms_trw_ook_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_tpms_trw_ook_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_tpms_trw_ook_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_tpms_trw_ook_get_string(void* context, FuriString* output);

void* tpms_protocol_encoder_tpms_trw_ook_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_tpms_trw_ook_free(void* context);
void tpms_protocol_encoder_tpms_trw_ook_stop(void* context);
LevelDuration tpms_protocol_encoder_tpms_trw_ook_yield(void* context);
SubGhzProtocolStatus
    tpms_protocol_encoder_tpms_trw_ook_deserialize(void* context, FlipperFormat* flipper_format);

// Invert analyze(): write pressure/temperature into the frame bytes, recompute the
// CRC-8/SMBUS, store the low 64 bits back into g->data and set g->data_count_bit = 88.
void tpms_protocol_tpms_trw_ook_pack(TPMSBlockGeneric* g);
