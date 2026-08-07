#pragma once
#include <stdbool.h>

// Live capture-quality assessment for the Sub-GHz receiver (PRIVATE builds only —
// call sites are #ifndef PUBLIC_BUILD, so this is unreferenced/GC'd in public).

typedef enum {
    SubGhzQualityIdle, // no signal above the RSSI trigger
    SubGhzQualityWeak, // above trigger but thin margin
    SubGhzQualityNoDecode, // strong signal but nothing is decoding (usually wrong preset)
    SubGhzQualityGood, // strong signal + decodes are landing
} SubGhzQualityLevel;

typedef struct {
    SubGhzQualityLevel level;
    const char* hint; // short user hint, "" when none
} SubGhzCaptureQuality;

// PURE: score from live RSSI vs the receiver's RSSI trigger and whether a decode
// landed recently. No hardware, no globals — host-testable.
SubGhzCaptureQuality subghz_capture_quality_eval(
    float rssi_dbm,
    float threshold_dbm,
    bool decoded_recently);
