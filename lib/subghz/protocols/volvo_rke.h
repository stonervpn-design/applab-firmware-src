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

// Volvo RKE (one-way rolling-code key fob, EU 433 MHz generation, ~2006-2013).
//
// Physical layer is DOCUMENTED (Per Olsson, "Investigation of technical and
// communicational problems with the remote key for Volvo cars", Chalmers Univ.
// of Technology MSc thesis EX012/2013): dual-channel 433.670 + 434.251 MHz,
// 2-GFSK, data Manchester-coded, 5.76 kbit/s; a wake-up burst (>11 8-bit WUPs)
// then 24-bit preamble + 24-bit SFD + 1-bit short-frame + 7-bit length + data-
// link header + payload (<=128 bit, containing an ID, an "SI" rolling counter,
// a button/command field, a 64-bit AES-encrypted block, and a checksum) + 2-bit
// end-of-message. The frame is sent on CH1 then repeated on CH2.
//
// What is NOT public: the exact bit OFFSETS of the payload sub-fields, the
// preamble/SFD bit PATTERNS, and the checksum algorithm all live in Volvo's
// unpublished internal documents (which the thesis cites but does not
// reproduce), and the 64-bit AES block is unbreakable without the key.
//
// So this decoder is HONEST about its ceiling: it FSK+Manchester-demodulates the
// burst and captures a tail-aligned window of the transmitted frame (the payload
// and footer sit at the END of the burst, so tail alignment is stable even as
// the leading WUP count varies), then across two or more presses of the SAME fob
// in one RX session it computes a per-bit STABILITY MAP (bits constant across
// presses = device fingerprint / ID / header; bits that change = the rolling
// counter + the AES block) and flags a candidate high-entropy 64-bit AES window.
// It never claims a decrypted counter or a predicted next code (that needs the
// unpublished offset table AND the AES key). The AES block is surfaced opaque,
// exactly as land_rover_rke.c surfaces its KeeLoq hop. A capture can be replayed
// verbatim once (ENABLE_EMULATE_FEATURE); a rolling receiver rejects a repeat.
#define VOLVO_RKE_PROTOCOL_NAME "Volvo RKE"

extern const SubGhzProtocol volvo_rke_protocol;

void* subghz_protocol_decoder_volvo_rke_alloc(SubGhzEnvironment* environment);
void subghz_protocol_decoder_volvo_rke_free(void* context);
void subghz_protocol_decoder_volvo_rke_reset(void* context);
void subghz_protocol_decoder_volvo_rke_feed(void* context, bool level, uint32_t duration);
uint8_t subghz_protocol_decoder_volvo_rke_get_hash_data(void* context);
SubGhzProtocolStatus subghz_protocol_decoder_volvo_rke_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset);
SubGhzProtocolStatus
    subghz_protocol_decoder_volvo_rke_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_decoder_volvo_rke_get_string(void* context, FuriString* output);

#ifdef ENABLE_EMULATE_FEATURE
void* subghz_protocol_encoder_volvo_rke_alloc(SubGhzEnvironment* environment);
SubGhzProtocolStatus
    subghz_protocol_encoder_volvo_rke_deserialize(void* context, FlipperFormat* flipper_format);
#endif
