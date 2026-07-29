#pragma once

#include <furi.h>
#include <lib/subghz/protocols/base.h>
#include <lib/subghz/types.h>
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h>
#include <flipper_format/flipper_format.h>

#include "../defines.h"

// Ported from KAT (src/protocols/mazda_siemens.rs). 64-bit payload behind an
// XOR de-obfuscation + additive checksum. Pair-based (level-agnostic) decode.
#define MAZDA_SIEMENS_PROTOCOL_NAME "MazdaSiemens"

typedef struct SubGhzProtocolDecoderMazdaSiemens SubGhzProtocolDecoderMazdaSiemens;
typedef struct SubGhzProtocolEncoderMazdaSiemens SubGhzProtocolEncoderMazdaSiemens;

extern const SubGhzProtocol mazda_siemens_protocol;

void* subghz_protocol_decoder_mazda_siemens_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_mazda_siemens_free(void* context);
void subghz_protocol_decoder_mazda_siemens_reset(void* context);
void subghz_protocol_decoder_mazda_siemens_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_mazda_siemens_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_mazda_siemens_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_mazda_siemens_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_mazda_siemens_get_string(void* context, FuriString* output);

#ifdef ENABLE_EMULATE_FEATURE
void* subghz_protocol_encoder_mazda_siemens_alloc(SubGhzEnvironment* environment);
SubGhzProtocolStatus
    subghz_protocol_encoder_mazda_siemens_deserialize(void* context, FlipperFormat* flipper_format);
#endif
