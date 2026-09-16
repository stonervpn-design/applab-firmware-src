#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_TYREGUARD400_NAME "TyreGuard400 TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_tpms_tyreguard400_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_tpms_tyreguard400_encoder;
extern const SubGhzProtocol tpms_protocol_tpms_tyreguard400;

void* tpms_protocol_decoder_tpms_tyreguard400_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_tpms_tyreguard400_free(void* context);
void tpms_protocol_decoder_tpms_tyreguard400_reset(void* context);
void tpms_protocol_decoder_tpms_tyreguard400_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_tpms_tyreguard400_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_tpms_tyreguard400_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_tpms_tyreguard400_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_tpms_tyreguard400_get_string(void* context, FuriString* output);

void* tpms_protocol_encoder_tpms_tyreguard400_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_tpms_tyreguard400_free(void* context);
void tpms_protocol_encoder_tpms_tyreguard400_stop(void* context);
LevelDuration tpms_protocol_encoder_tpms_tyreguard400_yield(void* context);
SubGhzProtocolStatus
    tpms_protocol_encoder_tpms_tyreguard400_deserialize(void* context, FlipperFormat* flipper_format);

/** Pack friendly fields (id / pressure / temperature) into generic->data. Inverse
 * of the decoder analyze(): writes the 28-bit ID, the 11-bit pressure (kPa) split
 * across b[7]/b[9], the temperature (C+40) into b[8], recomputes the CRC-8, and
 * stores b[3..10] into generic->data with data_count_bit = 88. Used by the TPMS
 * spoofer. */
void tpms_protocol_tpms_tyreguard400_pack(TPMSBlockGeneric* generic);
