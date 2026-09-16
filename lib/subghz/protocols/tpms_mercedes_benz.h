#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_MERCEDES_BENZ_NAME "Mercedes-Benz TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_tpms_mercedes_benz_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_tpms_mercedes_benz_encoder;
extern const SubGhzProtocol tpms_protocol_tpms_mercedes_benz;

void* tpms_protocol_decoder_tpms_mercedes_benz_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_tpms_mercedes_benz_free(void* context);
void tpms_protocol_decoder_tpms_mercedes_benz_reset(void* context);
void tpms_protocol_decoder_tpms_mercedes_benz_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_tpms_mercedes_benz_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_tpms_mercedes_benz_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_tpms_mercedes_benz_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_tpms_mercedes_benz_get_string(void* context, FuriString* output);

void* tpms_protocol_encoder_tpms_mercedes_benz_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_tpms_mercedes_benz_free(void* context);
void tpms_protocol_encoder_tpms_mercedes_benz_stop(void* context);
LevelDuration tpms_protocol_encoder_tpms_mercedes_benz_yield(void* context);
SubGhzProtocolStatus
    tpms_protocol_encoder_tpms_mercedes_benz_deserialize(void* context, FlipperFormat* flipper_format);

/** Pack friendly fields (id / pressure / temperature) into generic->data. Inverse
 * of the decoder's analyze(): writes the rtl_433 raw pressure (PSI*2.75) and
 * temperature (C+51), a stationary ST byte, and lets the encoder recompute the
 * CRC-8. Sets data + data_count_bit = 80. Used by the TPMS spoofer. */
void tpms_protocol_tpms_mercedes_benz_pack(TPMSBlockGeneric* g);
