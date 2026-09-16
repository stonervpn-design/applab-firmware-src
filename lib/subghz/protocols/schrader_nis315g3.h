#pragma once

#include "base.h"
#include "tpms_generic.h"

#define TPMS_PROTOCOL_SCHRADER_NIS315G3_NAME "Schrader NIS315G3 TPMS"

extern const SubGhzProtocol tpms_protocol_schrader_nis315g3;
extern const SubGhzProtocolDecoder tpms_protocol_schrader_nis315g3_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_schrader_nis315g3_encoder;

/**
 * Re-pack the Schrader MRXNIS315G3 (3039) payload from edited generic fields.
 *
 * The wire word is 38 bits: 1 fixed bit + 3 flag bits + 24-bit ID + 8-bit
 * pressure + 2-bit checksum, laid out MSB-first as
 *   1FFFSSSS SSSSSSSS SSSSSSSS SSSSPPPP PPPPXX
 *
 * This writes the fixed leading 1, the flags (default 0x7 = normal driving),
 * the 24-bit ID from generic->id, the pressure raw byte derived from
 * generic->pressure (rtl_433 scale: PSI = raw * 0.25) and then chooses the
 * 2-bit checksum so the sum of all 2-bit groups modulo 4 equals 1 (exactly
 * rtl_433's parity check). The 38-bit result is written back to generic->data
 * with generic->data_count_bit = 38.
 *
 * NOTE: this protocol carries NO temperature and NO battery flag, so
 * generic->temperature is ignored by the pack (there is nowhere to store it).
 *
 * @param g TPMSBlockGeneric whose id/pressure are read and whose
 *          data/data_count_bit get the freshly packed payload written back.
 */
void tpms_protocol_schrader_nis315g3_pack(TPMSBlockGeneric* g);
