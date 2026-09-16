#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_SCHRADER_MOTO_NAME "Schrader Moto TPMS"

typedef struct TPMSProtocolDecoderSchraderMotorcycle TPMSProtocolDecoderSchraderMotorcycle;
typedef struct TPMSProtocolEncoderSchraderMotorcycle TPMSProtocolEncoderSchraderMotorcycle;

extern const SubGhzProtocolDecoder tpms_protocol_tpms_schrader_motorcycle_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_tpms_schrader_motorcycle_encoder;
extern const SubGhzProtocol tpms_protocol_tpms_schrader_motorcycle;

void* tpms_protocol_decoder_tpms_schrader_motorcycle_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_tpms_schrader_motorcycle_free(void* context);
void tpms_protocol_decoder_tpms_schrader_motorcycle_reset(void* context);
void tpms_protocol_decoder_tpms_schrader_motorcycle_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_tpms_schrader_motorcycle_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_tpms_schrader_motorcycle_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus tpms_protocol_decoder_tpms_schrader_motorcycle_deserialize(
    void* context,
    FlipperFormat* flipper_format);

void tpms_protocol_decoder_tpms_schrader_motorcycle_get_string(void* context, FuriString* output);

void* tpms_protocol_encoder_tpms_schrader_motorcycle_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_tpms_schrader_motorcycle_free(void* context);
void tpms_protocol_encoder_tpms_schrader_motorcycle_stop(void* context);
LevelDuration tpms_protocol_encoder_tpms_schrader_motorcycle_yield(void* context);

SubGhzProtocolStatus tpms_protocol_encoder_tpms_schrader_motorcycle_deserialize(
    void* context,
    FlipperFormat* flipper_format);

/**
 * Re-pack the Schrader Motorcycle 56-bit data word from edited generic fields
 * (id, pressure, temperature). Writes the fixed F field, the 24-bit ID, the
 * 10-bit pressure (kPa scale 2), the temperature (offset 50) and recomputes the
 * trailing CRC-8 (poly 0x07, init 0xE0). Inverse of the decoder's analyze step.
 *
 * @param g TPMSBlockGeneric whose id/pressure/temperature are read and whose
 *          data field gets the freshly packed 56-bit payload written back.
 */
void tpms_protocol_tpms_schrader_motorcycle_pack(TPMSBlockGeneric* g);
