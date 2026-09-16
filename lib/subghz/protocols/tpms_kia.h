#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_KIA_NAME "Kia TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_tpms_kia_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_tpms_kia_encoder;
extern const SubGhzProtocol tpms_protocol_tpms_kia;

void* tpms_protocol_decoder_tpms_kia_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_tpms_kia_free(void* context);
void tpms_protocol_decoder_tpms_kia_reset(void* context);
void tpms_protocol_decoder_tpms_kia_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_tpms_kia_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_tpms_kia_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_tpms_kia_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_tpms_kia_get_string(void* context, FuriString* output);

void* tpms_protocol_encoder_tpms_kia_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_tpms_kia_free(void* context);
void tpms_protocol_encoder_tpms_kia_stop(void* context);
LevelDuration tpms_protocol_encoder_tpms_kia_yield(void* context);
SubGhzProtocolStatus
    tpms_protocol_encoder_tpms_kia_deserialize(void* context, FlipperFormat* flipper_format);

/** Pack friendly fields (id / pressure / temperature) into generic->data as the
 * Kia wire bytes b[0..7]. Inverse of the decoder's analyze(): writes the
 * nibble-packed pressure (PSI*5) / temperature (C+50) / 32-bit id, then brute-
 * forces the free "unknown2" byte so the CRC-8 (poly 0x07, init 0x76) has its
 * low 3 bits zero — which the on-wire 5-bit CRC field requires for the decoder's
 * exact rtl_433 check to pass. Sets data + data_count_bit = 69. */
void tpms_protocol_tpms_kia_pack(TPMSBlockGeneric* generic);
