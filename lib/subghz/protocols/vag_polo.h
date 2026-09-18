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
#include <lib/toolbox/level_duration.h>
#include <lib/toolbox/manchester_decoder.h>

#include "../defines.h"

// VW Polo VAG variant: 434.42 MHz OOK Manchester, ~290us half-bit.
// Frame (after long all-1s preamble): 0x3F91 header + button byte + 9-byte
// rolling code (96 bits). Button high nibble 0x7=Unlock, 0xB=Lock. The rolling
// code is encrypted (no public VW key) so this decoder detects + displays the
// button and raw code; it does not decrypt/clone and is not sendable.
#define VAG_POLO_PROTOCOL_NAME "VW Polo"

extern const SubGhzProtocolDecoder subghz_protocol_vag_polo_decoder;
extern const SubGhzProtocolEncoder subghz_protocol_vag_polo_encoder;
extern const SubGhzProtocol vag_polo_protocol;

void* subghz_protocol_decoder_vag_polo_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_vag_polo_reset(void* context);
void subghz_protocol_decoder_vag_polo_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_vag_polo_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_vag_polo_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_vag_polo_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_vag_polo_get_string(void* context, FuriString* output);
