#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_PMV107J_NAME "Toyota PMV-107J"

extern const SubGhzProtocolDecoder tpms_protocol_pmv107j_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_pmv107j_encoder;
extern const SubGhzProtocol tpms_protocol_pmv107j;

void* tpms_protocol_decoder_pmv107j_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_pmv107j_free(void* context);
void tpms_protocol_decoder_pmv107j_reset(void* context);
void tpms_protocol_decoder_pmv107j_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_pmv107j_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_pmv107j_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_pmv107j_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_pmv107j_get_string(void* context, FuriString* output);

void* tpms_protocol_encoder_pmv107j_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_pmv107j_free(void* context);
void tpms_protocol_encoder_pmv107j_stop(void* context);
LevelDuration tpms_protocol_encoder_pmv107j_yield(void* context);
SubGhzProtocolStatus
    tpms_protocol_encoder_pmv107j_deserialize(void* context, FlipperFormat* flipper_format);

/** Pack friendly fields (id / pressure / temperature) into generic->data + the
 * Toyota CRC-8 (poly 0x13). Sets b[6] = ~b[5] to satisfy the decoder's integrity
 * check; the encoder recovers the 2-bit b[0] from the CRC. Inverse of
 * pmv107j_analyze. Used by the TPMS spoofer. Sets data + data_count_bit = 66. */
void tpms_protocol_pmv107j_pack(TPMSBlockGeneric* generic);
