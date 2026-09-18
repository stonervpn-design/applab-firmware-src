#pragma once

#include <furi.h>
#include <lib/subghz/protocols/base.h>
#include <lib/subghz/types.h>
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h>
#include <flipper_format/flipper_format.h>

#include "../defines.h"

// Porsche Cayenne key-fob variant. Ported from KaraZajac/KAT
// (src/protocols/porsche_cayenne.rs). The Cayenne shares the Touareg's
// decoder timing, 64-bit frame layout and 24-bit rotate cipher verbatim; the
// only KAT parameter that diverges is the encoder sync-preamble length
// (PC_SYNC_COUNT = 73 pairs vs the Touareg's shorter train).

#define PORSCHE_CYN_PROTOCOL_NAME "Porsche Cayenne"

typedef struct SubGhzProtocolDecoderPorscheCyn SubGhzProtocolDecoderPorscheCyn;

extern const SubGhzProtocol subghz_protocol_porsche_cayenne;

void* subghz_protocol_decoder_porsche_cyn_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_porsche_cyn_free(void* context);
void subghz_protocol_decoder_porsche_cyn_reset(void* context);
void subghz_protocol_decoder_porsche_cyn_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_porsche_cyn_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_porsche_cyn_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus subghz_protocol_decoder_porsche_cyn_deserialize(
    void* context,
    FlipperFormat* flipper_format);
void subghz_protocol_decoder_porsche_cyn_get_string(void* context, FuriString* output);

void* subghz_protocol_encoder_porsche_cyn_alloc(SubGhzEnvironment* environment);
SubGhzProtocolStatus subghz_protocol_encoder_porsche_cyn_deserialize(
    void* context,
    FlipperFormat* flipper_format);
