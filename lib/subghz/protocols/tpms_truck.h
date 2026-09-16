#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_TRUCK_NAME "Truck TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_truck_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_truck_encoder;
extern const SubGhzProtocol tpms_protocol_truck;

/** Invert analyze(): write pressure/temperature/battery + recomputed XOR checksum
 * into the wire bytes, store b[1..8] into g->data and set g->data_count_bit=72. */
void tpms_protocol_truck_pack(TPMSBlockGeneric* g);
