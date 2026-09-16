#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_TRW_FSK_NAME "TRW FSK TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_tpms_trw_fsk_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_tpms_trw_fsk_encoder;
extern const SubGhzProtocol tpms_protocol_tpms_trw_fsk;

void* tpms_protocol_decoder_tpms_trw_fsk_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_tpms_trw_fsk_free(void* context);
void tpms_protocol_decoder_tpms_trw_fsk_reset(void* context);
void tpms_protocol_decoder_tpms_trw_fsk_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_tpms_trw_fsk_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_tpms_trw_fsk_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_tpms_trw_fsk_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_tpms_trw_fsk_get_string(void* context, FuriString* output);

void* tpms_protocol_encoder_tpms_trw_fsk_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_tpms_trw_fsk_free(void* context);
void tpms_protocol_encoder_tpms_trw_fsk_stop(void* context);
LevelDuration tpms_protocol_encoder_tpms_trw_fsk_yield(void* context);

SubGhzProtocolStatus
    tpms_protocol_encoder_tpms_trw_fsk_deserialize(void* context, FlipperFormat* flipper_format);

// Invert tpms_trw_fsk_analyze(): write mode/id/pressure/temperature into the wire
// bytes, recompute the CRC-8/SMBUS, store the bytes back in g->data and set
// g->data_count_bit = 80. Used to synthesize a (possibly 0-PSI) frame for TX.
void tpms_protocol_tpms_trw_fsk_pack(TPMSBlockGeneric* g);
