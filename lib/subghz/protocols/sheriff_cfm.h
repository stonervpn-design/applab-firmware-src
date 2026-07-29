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

// Ported from KAT (src/protocols/sheriff_cfm.rs). Sheriff CFM (ZX-750 / ZX-930)
// car alarm: KeeLoq hopping code with a per-model byte transform + PI-decoded key.
#define SHERIFF_CFM_PROTOCOL_NAME "Sheriff CFM"

typedef struct SubGhzProtocolDecoderSheriffCfm SubGhzProtocolDecoderSheriffCfm;
typedef struct SubGhzProtocolEncoderSheriffCfm SubGhzProtocolEncoderSheriffCfm;

extern const SubGhzProtocol sheriff_cfm_protocol;

void* subghz_protocol_decoder_sheriff_cfm_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_sheriff_cfm_free(void* context);
void subghz_protocol_decoder_sheriff_cfm_reset(void* context);
void subghz_protocol_decoder_sheriff_cfm_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_sheriff_cfm_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_sheriff_cfm_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_sheriff_cfm_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_sheriff_cfm_get_string(void* context, FuriString* output);

#ifdef ENABLE_EMULATE_FEATURE
void* subghz_protocol_encoder_sheriff_cfm_alloc(SubGhzEnvironment* environment);
SubGhzProtocolStatus
    subghz_protocol_encoder_sheriff_cfm_deserialize(void* context, FlipperFormat* flipper_format);
#endif
