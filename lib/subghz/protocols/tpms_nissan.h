#pragma once

#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "tpms_generic.h"
#include "../blocks/math.h"

#define TPMS_PROTOCOL_NISSAN_NAME "Nissan TPMS"

extern const SubGhzProtocolDecoder tpms_protocol_nissan_decoder;
extern const SubGhzProtocolEncoder tpms_protocol_nissan_encoder;
extern const SubGhzProtocol tpms_protocol_nissan;

void* tpms_protocol_decoder_nissan_alloc(SubGhzEnvironment* environment);
void tpms_protocol_decoder_nissan_free(void* context);
void tpms_protocol_decoder_nissan_reset(void* context);
void tpms_protocol_decoder_nissan_feed(void* context, bool level, uint32_t duration);
uint8_t tpms_protocol_decoder_nissan_get_hash_data(void* context);

SubGhzProtocolStatus tpms_protocol_decoder_nissan_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);

SubGhzProtocolStatus
    tpms_protocol_decoder_nissan_deserialize(void* context, FlipperFormat* flipper_format);

void tpms_protocol_decoder_nissan_get_string(void* context, FuriString* output);

// Encoder entry points (mirrors tpms_citroen).
void* tpms_protocol_encoder_nissan_alloc(SubGhzEnvironment* environment);
void tpms_protocol_encoder_nissan_free(void* context);
void tpms_protocol_encoder_nissan_stop(void* context);
SubGhzProtocolStatus
    tpms_protocol_encoder_nissan_deserialize(void* context, FlipperFormat* flipper_format);
LevelDuration tpms_protocol_encoder_nissan_yield(void* context);

// Invert analyze(): write pressure/id (temperature is NOT transmitted by this
// sensor) into the 37-bit wire frame, recompute the 2-bit sum2N checksum, and
// store the packed bytes back into g->data with g->data_count_bit = 37.
void tpms_protocol_nissan_pack(TPMSBlockGeneric* g);
