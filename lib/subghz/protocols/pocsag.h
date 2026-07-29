#pragma once

#include <lib/subghz/protocols/base.h>

#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include "pcsg_generic.h"
#include <lib/subghz/blocks/math.h>

#define SUBGHZ_PROTOCOL_POCSAG_NAME "POCSAG"

extern const SubGhzProtocol subghz_protocol_pocsag;

/* Encoder */
void* subghz_protocol_encoder_pocsag_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_pocsag_free(void* context);
SubGhzProtocolStatus
    subghz_protocol_encoder_pocsag_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_pocsag_stop(void* context);
LevelDuration subghz_protocol_encoder_pocsag_yield(void* context);
