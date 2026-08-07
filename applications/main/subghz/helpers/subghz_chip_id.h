#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "signal_fingerprint.h" // SignalMod

// Encoder-chip identity derived from a DECODED fixed-code protocol. Pure: no
// radio, storage, or UI. PRIVATE builds only (the implementation in
// subghz_chip_id.c is compiled out under PUBLIC_BUILD; callers must guard the
// include + use with #ifndef PUBLIC_BUILD).

typedef enum {
    SubGhzChipUnknown = 0,
    SubGhzChipPrinceton,   // PT2262/PT2260/EV1527/SMC5326 class, 24-bit PWM
    SubGhzChipCame,        // CAME 12-bit
    SubGhzChipNice,        // NICE FLO 12-bit
    SubGhzChipHoltek,      // HT12/HT6 12-bit
    SubGhzChipAnsonic,     // 12-bit
    SubGhzChipChamberlain, // 7/8/9-bit
    SubGhzChipLinear,      // 10-bit
    SubGhzChipLinearDelta, // 8-bit
} SubGhzChipFamily;

typedef struct {
    const char* chip_label; // human label WITHOUT bit count (caller appends bits)
    SubGhzChipFamily family;
    int32_t total_bits;
    bool fixed;             // true = fixed-code (attackable via framed brute)
} SubGhzChipId;

// Maps a decoded fixed-code protocol name to an encoder-chip identity. Returns
// family=SubGhzChipUnknown for unrecognized names. freq_hz/mod are reserved for
// future finer discrimination.
SubGhzChipId subghz_chip_identify(
    const char* proto_name,
    int32_t bits,
    uint32_t freq_hz,
    SignalMod mod);
