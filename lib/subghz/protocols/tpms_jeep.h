#pragma once

#include "base.h"
#include "tpms_generic.h"

#define TPMS_PROTOCOL_JEEP_NAME "Jeep TPMS"

extern const SubGhzProtocol tpms_protocol_jeep;

// Re-pack generic (Id + pressure/temperature/battery) into the 80-bit Jeep frame
// held in generic->data, recomputing the XOR checksum. Used by the TPMS 0-PSI
// spoof to rebuild a flat-tire frame. Mirrors tpms_protocol_citroen_pack.
void tpms_protocol_jeep_pack(TPMSBlockGeneric* g);
