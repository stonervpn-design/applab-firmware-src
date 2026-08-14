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

#include "../defines.h"

// Land Rover RKE (KeeLoq hopping-code fob). DECODE-ONLY: the 32-bit KeeLoq hop
// stays encrypted (no per-manufacturer key resident in this firmware) — this
// decoder surfaces Serial/Button/raw Hop only, never decrypts, never validates
// a CRC/crypto check. There is no encoder: TX would require the key.
#define LAND_ROVER_RKE_PROTOCOL_NAME "Land Rover RKE"

extern const SubGhzProtocol land_rover_rke_protocol;

void* subghz_protocol_decoder_land_rover_rke_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_land_rover_rke_free(void* context);
void subghz_protocol_decoder_land_rover_rke_reset(void* context);
void subghz_protocol_decoder_land_rover_rke_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_land_rover_rke_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_land_rover_rke_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_land_rover_rke_deserialize(
    void* context,
    FlipperFormat* flipper_format);
void subghz_protocol_decoder_land_rover_rke_get_string(void* context, FuriString* output);
