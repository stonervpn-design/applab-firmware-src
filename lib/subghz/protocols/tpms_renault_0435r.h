#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_RENAULT_0435R_NAME "Renault 0435R TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_renault_0435r_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_renault_0435r_encoder;
extern const SubGhzProtocol tpms_protocol_tpms_renault_0435r;

void* tpms_protocol_decoder_renault_0435r_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_renault_0435r_free(void* context);
void tpms_protocol_decoder_renault_0435r_reset(void* context);
void tpms_protocol_decoder_renault_0435r_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_renault_0435r_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_renault_0435r_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_renault_0435r_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_renault_0435r_get_string(void* context, FuriString* output);

void* tpms_protocol_encoder_renault_0435r_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_renault_0435r_free(void* context);
void tpms_protocol_encoder_renault_0435r_stop(void* context);
LevelDuration tpms_protocol_encoder_renault_0435r_yield(void* context);
SubGhzProtocolStatus
    tpms_protocol_encoder_renault_0435r_deserialize(void* context, FlipperFormat* flipper_format);

/** Pack friendly fields (id / pressure / temperature) into generic->data. The
 * encoder recomputes the XOR checksum byte b[7] in build_frame. Inverse of
 * renault_0435r_analyze. Used by the TPMS spoofer. Sets data + data_count_bit
 * = 72. Renault sensors carry a 24-bit ID, so only the low 24 bits of g->id are
 * representable (top byte is dropped, matching rtl_433). */
void tpms_protocol_tpms_renault_0435r_pack(TPMSBlockGeneric* g);
