#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_SEFIS_M3_NAME "Sefis M3 TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_tpms_sefis_m3_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_tpms_sefis_m3_encoder;
extern const SubGhzProtocol tpms_protocol_tpms_sefis_m3;

void* tpms_protocol_decoder_tpms_sefis_m3_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_tpms_sefis_m3_free(void* context);
void tpms_protocol_decoder_tpms_sefis_m3_reset(void* context);
void tpms_protocol_decoder_tpms_sefis_m3_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_tpms_sefis_m3_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_tpms_sefis_m3_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_tpms_sefis_m3_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_tpms_sefis_m3_get_string(void* context, FuriString* output);

void* tpms_protocol_encoder_tpms_sefis_m3_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_tpms_sefis_m3_free(void* context);
void tpms_protocol_encoder_tpms_sefis_m3_stop(void* context);
LevelDuration tpms_protocol_encoder_tpms_sefis_m3_yield(void* context);
SubGhzProtocolStatus
    tpms_protocol_encoder_tpms_sefis_m3_deserialize(void* context, FlipperFormat* flipper_format);

/** Pack friendly fields (id / pressure[bar] / temperature[C]) into generic->data.
 * Inverse of the decoder's analyze(): writes the pressure page/code (rtl_433
 * 0x0e00/102.4 scaling), the temperature nibble, folds the 32-bit id into the
 * four opaque bytes (B0/B1/B3/B6) and recomputes the CRC-16. Sets data + id +
 * data_count_bit = 72. Used by the TPMS spoofer / round-trip harness. */
void tpms_protocol_tpms_sefis_m3_pack(TPMSBlockGeneric* generic);
