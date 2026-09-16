#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_BMW_G3_NAME "BMW Gen2-3 TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_bmw_g3_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_bmw_g3_encoder;
extern const SubGhzProtocol tpms_protocol_bmw_g3;

void* tpms_protocol_decoder_bmw_g3_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_bmw_g3_free(void* context);
void tpms_protocol_decoder_bmw_g3_reset(void* context);
void tpms_protocol_decoder_bmw_g3_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_bmw_g3_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_bmw_g3_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_bmw_g3_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_bmw_g3_get_string(void* context, FuriString* output);

void* tpms_protocol_encoder_bmw_g3_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_bmw_g3_free(void* context);
void tpms_protocol_encoder_bmw_g3_stop(void* context);
LevelDuration tpms_protocol_encoder_bmw_g3_yield(void* context);
SubGhzProtocolStatus
    tpms_protocol_encoder_bmw_g3_deserialize(void* context, FlipperFormat* flipper_format);

/** Pack friendly fields (id / pressure / temperature) into generic->data + the
 * BMW Gen2 CRC-16 (poly 0x1021, init 0x0000). Inverse of bmw_g3_analyze. Rebuilds
 * the 10-byte Gen2 frame, preserving captured flag bytes, and recomputes the CRC
 * so a modified / 0-PSI frame stays valid. Sets data + data_count_bit = 80. */
void tpms_protocol_bmw_g3_pack(TPMSBlockGeneric* g);
