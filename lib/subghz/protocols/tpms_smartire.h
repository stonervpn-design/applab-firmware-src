#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_SMARTIRE_NAME "SmarTire TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_tpms_smartire_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_tpms_smartire_encoder;
extern const SubGhzProtocol tpms_protocol_tpms_smartire;

void* tpms_protocol_decoder_tpms_smartire_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_tpms_smartire_free(void* context);
void tpms_protocol_decoder_tpms_smartire_reset(void* context);
void tpms_protocol_decoder_tpms_smartire_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_tpms_smartire_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_tpms_smartire_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_tpms_smartire_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_tpms_smartire_get_string(void* context, FuriString* output);

// Re-encode a captured/edited (or 0-PSI) frame into transmittable wire bytes:
// inverts the decoder's byte layout + scaling and recomputes the CRC-7, storing
// the result back into g->data / g->data_count_bit. The encoder itself transmits
// BOTH a pressure and a temperature message (as the real sensor does), rebuilt
// from g->pressure / g->temperature.
void tpms_protocol_tpms_smartire_pack(TPMSBlockGeneric* g);
