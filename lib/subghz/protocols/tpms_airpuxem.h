#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_AIRPUXEM_NAME "Airpux EM TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_tpms_airpuxem_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_tpms_airpuxem_encoder;
extern const SubGhzProtocol tpms_protocol_tpms_airpuxem;

void* tpms_protocol_decoder_tpms_airpuxem_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_tpms_airpuxem_free(void* context);
void tpms_protocol_decoder_tpms_airpuxem_reset(void* context);
void tpms_protocol_decoder_tpms_airpuxem_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_tpms_airpuxem_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_tpms_airpuxem_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_tpms_airpuxem_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_tpms_airpuxem_get_string(void* context, FuriString* output);

void* tpms_protocol_encoder_tpms_airpuxem_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_tpms_airpuxem_free(void* context);
void tpms_protocol_encoder_tpms_airpuxem_stop(void* context);
LevelDuration tpms_protocol_encoder_tpms_airpuxem_yield(void* context);
SubGhzProtocolStatus
    tpms_protocol_encoder_tpms_airpuxem_deserialize(void* context, FlipperFormat* flipper_format);

/** Pack friendly fields (id / pressure / temperature / battery) into
 * generic->data. Inverse of airpuxem_analyze(): writes the 8 CRC'd payload bytes
 * (ID + flags/position + pressure MSBs + pressure LSB + temperature + battery)
 * into generic->data and sets data_count_bit = 64. The 4-bit constant header
 * (0x5), the CRC byte and the trailing spare byte are (re)built by the encoder.
 * Used by the TPMS spoofer / 0-PSI packer. */
void tpms_protocol_tpms_airpuxem_pack(TPMSBlockGeneric* g);
