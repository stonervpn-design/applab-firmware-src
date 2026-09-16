#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_AVE_NAME "AVE TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_tpms_ave_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_tpms_ave_encoder;
extern const SubGhzProtocol tpms_protocol_tpms_ave;

void* tpms_protocol_decoder_tpms_ave_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_tpms_ave_free(void* context);
void tpms_protocol_decoder_tpms_ave_reset(void* context);
void tpms_protocol_decoder_tpms_ave_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_tpms_ave_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_tpms_ave_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_tpms_ave_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_tpms_ave_get_string(void* context, FuriString* output);

void* tpms_protocol_encoder_tpms_ave_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_tpms_ave_free(void* context);
void tpms_protocol_encoder_tpms_ave_stop(void* context);
LevelDuration tpms_protocol_encoder_tpms_ave_yield(void* context);
SubGhzProtocolStatus
    tpms_protocol_encoder_tpms_ave_deserialize(void* context, FlipperFormat* flipper_format);

/** Pack friendly fields (id / pressure / temperature / battery) into
 * generic->data + the AVE CRC-8 (poly 0x31, init 0xff). Inverse of
 * tpms_ave_analyze: writes the 8 wire bytes (id, pressure raw at mode-1 scaling,
 * temperature raw = C+50, mode/battery/flags byte) and recomputes the CRC.
 * Sets data + data_count_bit = 64. Used by the TPMS spoofer / 0-PSI packer. */
void tpms_protocol_tpms_ave_pack(TPMSBlockGeneric* generic);
