#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_GM_NAME "GM TPMS"

typedef struct TPMSProtocolDecoderGm TPMSProtocolDecoderGm;
typedef struct TPMSProtocolEncoderGm TPMSProtocolEncoderGm;

extern const SubGhzProtocolDecoder tpms_protocol_tpms_gm_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_tpms_gm_encoder;
extern const SubGhzProtocol tpms_protocol_tpms_gm;

void* tpms_protocol_decoder_tpms_gm_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_tpms_gm_free(void* context);
void tpms_protocol_decoder_tpms_gm_reset(void* context);
void tpms_protocol_decoder_tpms_gm_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_tpms_gm_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_tpms_gm_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_tpms_gm_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_tpms_gm_get_string(void* context, FuriString* output);

void* tpms_protocol_encoder_tpms_gm_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_tpms_gm_free(void* context);
void tpms_protocol_encoder_tpms_gm_stop(void* context);
LevelDuration tpms_protocol_encoder_tpms_gm_yield(void* context);
SubGhzProtocolStatus
    tpms_protocol_encoder_tpms_gm_deserialize(void* context, FlipperFormat* flipper_format);

/** Pack friendly fields (id / pressure / temperature / battery) into
 * generic->data. Inverse of gm_analyze(). generic->data holds the low 64 bits of
 * the 80-bit payload (frame bytes b[8..15]); the 16-bit flags field (b[6..7]) is
 * crafted from generic->battery_low on (re)encode and the modulo-256 checksum is
 * recomputed. Sets data + data_count_bit = 80. Used by the TPMS spoofer. */
void tpms_protocol_tpms_gm_pack(TPMSBlockGeneric* generic);
