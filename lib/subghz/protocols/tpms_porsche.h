#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_PORSCHE_NAME "Porsche TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_porsche_decoder;
extern const SubGhzProtocol tpms_protocol_porsche;

void* tpms_protocol_decoder_porsche_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_porsche_free(void* context);
void tpms_protocol_decoder_porsche_reset(void* context);
void tpms_protocol_decoder_porsche_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_porsche_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_porsche_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_porsche_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_porsche_get_string(void* context, FuriString* output);

// Re-encode a captured/edited (or 0-PSI) frame into transmittable wire bytes:
// inverts the decoder's byte layout + scaling and recomputes the CRC-16, storing
// the result back into g->data / g->data_count_bit.
void tpms_protocol_porsche_pack(TPMSBlockGeneric* g);
