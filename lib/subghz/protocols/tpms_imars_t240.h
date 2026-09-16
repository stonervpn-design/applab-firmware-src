#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_IMARS_T240_NAME "iMars T240 TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_tpms_imars_t240_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_tpms_imars_t240_encoder;
extern const SubGhzProtocol tpms_protocol_tpms_imars_t240;

void* tpms_protocol_decoder_tpms_imars_t240_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_tpms_imars_t240_free(void* context);
void tpms_protocol_decoder_tpms_imars_t240_reset(void* context);
void tpms_protocol_decoder_tpms_imars_t240_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_tpms_imars_t240_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_tpms_imars_t240_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_tpms_imars_t240_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_tpms_imars_t240_get_string(void* context, FuriString* output);

void* tpms_protocol_encoder_tpms_imars_t240_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_tpms_imars_t240_free(void* context);
void tpms_protocol_encoder_tpms_imars_t240_stop(void* context);
LevelDuration tpms_protocol_encoder_tpms_imars_t240_yield(void* context);
SubGhzProtocolStatus
    tpms_protocol_encoder_tpms_imars_t240_deserialize(void* context, FlipperFormat* flipper_format);

/**
 * Pack friendly fields (id / pressure / temperature) into generic->data as the
 * 8 wire bytes B0..B7, enforcing every rtl_433 integrity invariant
 * (B7==B0, low-nibble(B1)==low-nibble(B0), (B3+B4)&0xff == 0x41). Inverse of
 * the decoder's analyze() step. Sets generic->data + data_count_bit = 64.
 */
void tpms_protocol_tpms_imars_t240_pack(TPMSBlockGeneric* generic);
