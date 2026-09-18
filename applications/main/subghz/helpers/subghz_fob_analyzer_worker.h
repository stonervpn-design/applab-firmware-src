#pragma once

#include <furi_hal.h>
#include "../subghz_i.h"

typedef struct SubGhzFobAnalyzerWorker SubGhzFobAnalyzerWorker;

typedef enum {
    FobModUnknown = 0,
    FobModOok, // amplitude-keyed (ASK/OOK) — structured on/off edges from the demod slicer
    FobModFsk, // frequency-keyed 2-FSK — steady carrier, few keyed edges; deviation unknown
    FobModFskNarrow, // 2-FSK, narrow deviation (~15–30 kHz) → FM238 capture preset
    FobModFskWide, // 2-FSK, wide deviation (~47 kHz) → FM476 capture preset
} FobMod;

// OOK line-coding hint. Advisory only: derived from the mark/space pulse-width
// histogram, which cannot always separate PWM from Manchester — treat as a
// best-guess to confirm by capturing RAW. FobEncUnknown for FSK.
typedef enum {
    FobEncUnknown = 0,
    FobEncPwm,
    FobEncPpm,
    FobEncManchester,
} FobEnc;

// One analysis result delivered to the view. When signal==false the fields
// other than frequency/rssi are not meaningful (still hunting).
typedef struct {
    uint32_t frequency; // Hz, locked carrier
    float rssi; // dBm, peak at lock
    bool signal; // true = locked, false = hunting/idle
    FobMod mod; // classified modulation
    FobEnc enc; // OOK sub-encoding hint (advisory; FobEncUnknown for FSK / unmeasured)
    uint32_t te_us; // shortest symbol element, microseconds (0 = not measured)
    uint32_t bitrate; // approx bps (0 = n/a)
    uint32_t fsk_dev_khz; // coarse FSK deviation estimate, kHz (0 = unknown / not FSK)
    float spread_db; // internal diagnostic (RSSI envelope swing; 0 on the edge path)
} FobAnalyzerResult;

typedef void (*SubGhzFobAnalyzerWorkerCallback)(void* context, const FobAnalyzerResult* result);

/** Allocate. @param context SubGhz* (for the frequency-list setting + trigger). */
SubGhzFobAnalyzerWorker* subghz_fob_analyzer_worker_alloc(void* context);

void subghz_fob_analyzer_worker_free(SubGhzFobAnalyzerWorker* instance);

void subghz_fob_analyzer_worker_set_callback(
    SubGhzFobAnalyzerWorker* instance,
    SubGhzFobAnalyzerWorkerCallback callback,
    void* context);

void subghz_fob_analyzer_worker_start(SubGhzFobAnalyzerWorker* instance);

void subghz_fob_analyzer_worker_stop(SubGhzFobAnalyzerWorker* instance);

bool subghz_fob_analyzer_worker_is_running(SubGhzFobAnalyzerWorker* instance);

void subghz_fob_analyzer_worker_set_trigger_level(SubGhzFobAnalyzerWorker* instance, float value);

float subghz_fob_analyzer_worker_get_trigger_level(SubGhzFobAnalyzerWorker* instance);
