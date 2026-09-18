#pragma once

// Genie / Overhead Door Intellicode Sub-GHz protocol (decode + re-TX). The stock
// Flipper firmware ships no Genie support. Decode state machine + OOK waveform
// ported from Derek Jamison's Genie Recorder (MIT) via rsemnani/garagemate.
// Moves a raw 64-bit code (capture-and-replay); no next-code arithmetic.
#include "base.h"
#include "../blocks/const.h"
#include "../blocks/decoder.h"
#include "../blocks/encoder.h"
#include "../blocks/math.h"

#define GENIE_PROTOCOL_NAME "Genie"
#define GENIE_BIT_COUNT     64

extern const SubGhzProtocolDecoder genie_decoder;
extern const SubGhzProtocolEncoder genie_encoder;
extern const SubGhzProtocol subghz_protocol_genie;
