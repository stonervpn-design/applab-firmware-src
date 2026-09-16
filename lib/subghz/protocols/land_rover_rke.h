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

// Land Rover RKE (KeeLoq hopping-code fob). The decoder surfaces
// Serial/Button/raw Hop and then attempts a manufacturer-KEY RECOVERY: it tries
// every resident keystore key (each under its own KeeLoq learning/derivation)
// and, only if one decrypts a hop that is self-consistent with the observed
// serial+button, recovers the effective key -> decodes the counter -> shows
// predicted next codes (durable clone). A lone frame that matches is a
// CANDIDATE; a 2nd capture that decrypts to an advanced counter under the same
// key is CONFIRMED. YIELD IS HONESTLY LOW: no genuine JLR OEM manufacturer key
// ships in this firmware, so a factory Land Rover fob will almost always report
// "not in keystore (replay-only)" — a match is realistic only for an
// aftermarket Pantera/Jaguar-brand KeeLoq remote whose key IS resident. When no
// key matches, the hop stays ENCRYPTED, no CRC/crypto is validated, and the
// next rolling code is NOT predictable. The encoder (ENABLE_EMULATE_FEATURE)
// remains a ONE-SHOT VERBATIM REPLAY of the retained bits — no prediction.
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

#ifdef ENABLE_EMULATE_FEATURE
void* subghz_protocol_encoder_land_rover_rke_alloc(SubGhzEnvironment* environment);
SubGhzProtocolStatus subghz_protocol_encoder_land_rover_rke_deserialize(
    void* context,
    FlipperFormat* flipper_format);
#endif
