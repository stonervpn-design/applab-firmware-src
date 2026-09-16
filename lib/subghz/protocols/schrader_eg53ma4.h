#pragma once

#include "base.h"
#include "tpms_generic.h"

#define TPMS_PROTOCOL_SCHRADER_EG53MA4_NAME "Schrader EG53MA4"

extern const SubGhzProtocol tpms_protocol_schrader_eg53ma4;

/**
 * Re-pack the Schrader EG53MA4 payload from edited generic fields
 * (id, pressure, temperature). Writes the 24-bit ID into b[4..6], the pressure
 * raw byte (b[7], 25 mbar/bit) and the temperature byte (b[8], degrees F), then
 * recomputes the byte-sum checksum (b[9]) so a modified / 0-PSI frame stays
 * valid. The result is written back to generic->data (b[2..9], 64 bits) with
 * generic->data_count_bit set to the 80-bit payload length.
 *
 * @param g TPMSBlockGeneric whose id/pressure/temperature are read and whose
 *          data/data_count_bit get the freshly packed payload written back.
 */
void tpms_protocol_schrader_eg53ma4_pack(TPMSBlockGeneric* g);
