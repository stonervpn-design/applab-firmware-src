#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_TOYOTA_NAME "Toyota TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_toyota_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_toyota_encoder;
extern const SubGhzProtocol tpms_protocol_toyota;

void* tpms_protocol_decoder_toyota_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_toyota_free(void* context);
void tpms_protocol_decoder_toyota_reset(void* context);
void tpms_protocol_decoder_toyota_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_toyota_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_toyota_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_toyota_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_toyota_get_string(void* context, FuriString* output);

void* tpms_protocol_encoder_toyota_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_toyota_free(void* context);
void tpms_protocol_encoder_toyota_stop(void* context);
LevelDuration tpms_protocol_encoder_toyota_yield(void* context);
SubGhzProtocolStatus
    tpms_protocol_encoder_toyota_deserialize(void* context, FlipperFormat* flipper_format);

/**
 * Invert analyze(): write pressure/temperature into the 9 wire bytes, recompute
 * the CRC-8 (poly 0x07, init 0x80) and the inverted-pressure integrity byte, then
 * store b[1..8] back into generic->data (b[0], the top ID byte, is carried by
 * generic->id). Sets generic->data_count_bit = 72.
 */
void tpms_protocol_toyota_pack(TPMSBlockGeneric* generic);
