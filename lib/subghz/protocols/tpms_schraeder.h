#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_SCHRAEDER_NAME "Schraeder TPMS"

typedef struct TPMSProtocolDecoderTpmsSchraeder TPMSProtocolDecoderTpmsSchraeder;
typedef struct TPMSProtocolEncoderTpmsSchraeder TPMSProtocolEncoderTpmsSchraeder;

extern const SubGhzProtocolDecoder tpms_protocol_tpms_schraeder_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_tpms_schraeder_encoder;
extern const SubGhzProtocol tpms_protocol_tpms_schraeder;

void* tpms_protocol_decoder_tpms_schraeder_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_tpms_schraeder_free(void* context);
void tpms_protocol_decoder_tpms_schraeder_reset(void* context);
void tpms_protocol_decoder_tpms_schraeder_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_tpms_schraeder_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_tpms_schraeder_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_tpms_schraeder_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_tpms_schraeder_get_string(void* context, FuriString* output);

void* tpms_protocol_encoder_tpms_schraeder_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_tpms_schraeder_free(void* context);
void tpms_protocol_encoder_tpms_schraeder_stop(void* context);
LevelDuration tpms_protocol_encoder_tpms_schraeder_yield(void* context);

SubGhzProtocolStatus
    tpms_protocol_encoder_tpms_schraeder_deserialize(void* context, FlipperFormat* flipper_format);

/**
 * Re-pack the Schrader ("schraeder", FCC MRXGG4) 64-bit data word from edited
 * generic fields (id, pressure, temperature). Inverts the decoder analyze step:
 * writes the 0xf preamble nibble, zero flags, the 28-bit id, the raw pressure
 * (25 mbar/bit) and temperature (offset -50 C) bytes, then recomputes the
 * trailing CRC8 (poly 0x07, init 0xf0 over the six preceding bytes).
 *
 * @param g TPMSBlockGeneric whose id/pressure/temperature are read and whose
 *          data / data_count_bit fields get the freshly packed payload.
 */
void tpms_protocol_tpms_schraeder_pack(TPMSBlockGeneric* g);
