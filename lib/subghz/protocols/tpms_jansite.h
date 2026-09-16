#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_JANSITE_NAME "Jansite TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_jansite_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_jansite_encoder;
extern const SubGhzProtocol tpms_protocol_jansite;

// Invert analyze(): write pressure/temperature/id into the frame bytes, recompute
// the CRC, and store the canonical payload back into g->data / g->data_count_bit.
void tpms_protocol_jansite_pack(TPMSBlockGeneric* g);
