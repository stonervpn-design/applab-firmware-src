#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_EEZRV_NAME "EezRV TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_tpms_eezrv_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_tpms_eezrv_encoder;
extern const SubGhzProtocol tpms_protocol_tpms_eezrv;

void* tpms_protocol_decoder_tpms_eezrv_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_tpms_eezrv_free(void* context);
void tpms_protocol_decoder_tpms_eezrv_reset(void* context);
void tpms_protocol_decoder_tpms_eezrv_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_tpms_eezrv_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_tpms_eezrv_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_tpms_eezrv_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_tpms_eezrv_get_string(void* context, FuriString* output);

void* tpms_protocol_encoder_tpms_eezrv_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_tpms_eezrv_free(void* context);
void tpms_protocol_encoder_tpms_eezrv_stop(void* context);
LevelDuration tpms_protocol_encoder_tpms_eezrv_yield(void* context);
SubGhzProtocolStatus
    tpms_protocol_encoder_tpms_eezrv_deserialize(void* context, FlipperFormat* flipper_format);

/** Pack friendly fields (id / pressure / temperature / battery_low) into
 * generic->data as the EezTire/EezRV 64-bit payload (CC + 7 data bytes).
 * Recomputes the sum-with-overflow checksum. Inverse of eezrv_analyze. Sets
 * data + data_count_bit = 64. The wire ID field is only 24 bits, so only the
 * low 24 bits of generic->id are used. */
void tpms_protocol_tpms_eezrv_pack(TPMSBlockGeneric* generic);
