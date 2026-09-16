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

// BMW OOK -- 433.92 MHz OOK remote telegram observed from a BMW (X5) key fob.
//
// Physical layer, MEASURED from a real capture and cross-checked with rtl_433's
// pulse analyser: On-Off-Keyed, Pulse-Position Modulated. Each data bit is one
// constant ~250 us carrier pulse followed by a gap whose LENGTH carries the bit:
//   short gap ~500 us  -> 0
//   long  gap ~1500 us -> 1
// A frame is 64 bits, transmitted MSB first, repeated several times per press
// with long (>20 ms) inter-frame gaps.
//
// What is NOT known: whether the 64-bit payload is FIXED or ROLLING, and the
// meaning of its sub-fields. A single press only yields identical repeats, so
// fixed-vs-rolling cannot be decided from one capture; the payload is high
// entropy (~42% ones, longest run 4), which is consistent with a rolling /
// encrypted code but does not prove it. It matches no decoder in this firmware
// nor any of rtl_433's ~200 device decoders.
//
// So this decoder is HONEST, like volvo_rke.c / land_rover_rke.c: it PPM-
// demodulates the burst into a tail-aligned window, surfaces the raw 64-bit
// code, and across two or more presses of the SAME fob in one RX session it
// computes a per-bit STABILITY MAP (bits constant across presses = ID/header;
// bits that change = counter / rolling block). It never claims a decrypted
// counter or a predicted next code. A capture can be replayed verbatim once
// (ENABLE_EMULATE_FEATURE); if the code rolls, the receiver rejects the repeat.
#define BMW_OOK_PROTOCOL_NAME "BMW OOK"

extern const SubGhzProtocol bmw_ook_protocol;

void* subghz_protocol_decoder_bmw_ook_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_bmw_ook_free(void* context);
void subghz_protocol_decoder_bmw_ook_reset(void* context);
void subghz_protocol_decoder_bmw_ook_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_bmw_ook_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_bmw_ook_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_bmw_ook_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_bmw_ook_get_string(void* context, FuriString* output);

#ifdef ENABLE_EMULATE_FEATURE
void* subghz_protocol_encoder_bmw_ook_alloc(SubGhzEnvironment* environment);
SubGhzProtocolStatus
    subghz_protocol_encoder_bmw_ook_deserialize(void* context, FlipperFormat* flipper_format);
#endif
