#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_SCHRADER_SMD3MA4_NAME "Schrader SMD3MA4 TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_schrader_smd3ma4_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_schrader_smd3ma4_encoder;
extern const SubGhzProtocol tpms_protocol_schrader_smd3ma4;

void* tpms_protocol_decoder_schrader_smd3ma4_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_schrader_smd3ma4_free(void* context);
void tpms_protocol_decoder_schrader_smd3ma4_reset(void* context);
void tpms_protocol_decoder_schrader_smd3ma4_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_schrader_smd3ma4_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_schrader_smd3ma4_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_schrader_smd3ma4_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_schrader_smd3ma4_get_string(void* context, FuriString* output);

void* tpms_protocol_encoder_schrader_smd3ma4_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_schrader_smd3ma4_free(void* context);
void tpms_protocol_encoder_schrader_smd3ma4_stop(void* context);
LevelDuration tpms_protocol_encoder_schrader_smd3ma4_yield(void* context);
SubGhzProtocolStatus
    tpms_protocol_encoder_schrader_smd3ma4_deserialize(void* context, FlipperFormat* flipper_format);

/** Pack friendly fields (id / pressure) into generic->data. Inverse of the
 * decoder's analyze step: writes the 24-bit ID, 8-bit pressure (rtl_433 raw
 * scaling), sets flags = 0x7 (normal driving) and the fixed leading 1-bit, and
 * recomputes the 2-bit parity checksum. Sets data + data_count_bit = 38.
 * NOTE: SMD3MA4 carries a 24-bit ID and NO temperature field. */
void tpms_protocol_schrader_smd3ma4_pack(TPMSBlockGeneric* generic);
