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

// Hyundai TL-generation RKE (2016-2021 Tucson/Elantra/etc), US 433.92 MHz 2-FSK.
// Reverse-engineered from live captures (round-trip verified, see tucson_codec.py):
//   pulse-distance line code -- fixed ~73us mark, then
//     gap ~100us  = bit 0
//     gap ~150us  = bit 1
//     gap ~330us  = block/frame separator
//   long alternating 0101... run at the head = clock-training preamble.
// Rolling code: this module RECOGNIZES + reads the payload bits (no field
// crypto -- serial/counter are encrypted). Replay/TX is done via the RAW .sub.
#define HYUNDAI_TL_PROTOCOL_NAME "Hyundai TL"

typedef struct SubGhzProtocolDecoderHyundaiTl SubGhzProtocolDecoderHyundaiTl;

extern const SubGhzProtocol hyundai_tl_protocol;

void* subghz_protocol_decoder_hyundai_tl_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_hyundai_tl_free(void* context);
void subghz_protocol_decoder_hyundai_tl_reset(void* context);
void subghz_protocol_decoder_hyundai_tl_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_hyundai_tl_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_hyundai_tl_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_hyundai_tl_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_hyundai_tl_get_string(void* context, FuriString* output);
