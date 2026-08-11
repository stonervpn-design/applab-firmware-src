#pragma once
#include "fuzzer_mutate.h"
int fuzzer_frame_build(const FuzzFrame* f, FuzzMode mode, const char* protocol,
                       uint32_t te, uint8_t repeat, char* out, int cap);
