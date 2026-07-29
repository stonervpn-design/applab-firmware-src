#pragma once

#include <m-array.h>

struct SubGhzKeystore {
    SubGhzKeyArray_t data;
    const char* mfname;
    uint8_t kl_type;
    bool low_heap_truncated; /* one-shot log guard for heap-bounded keystore load */
};
