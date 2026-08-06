#include "subghz_mod_detect.h"

// Starting thresholds — require one on-hardware tuning pass (see the plan).
#define SMD_FLOOR_PCTL 15 // low percentile = window floor
#define SMD_PEAK_PCTL 90 // high percentile = window peak
#define SMD_SWING_MIN_DB 12 // >= this floor->peak swing => OOK (bimodal)
#define SMD_FM_SWING_MAX_DB 8 // <= this swing => steady carrier (FSK)
#define SMD_MIN_PEAK_DBM (-90) // peak below this => no real signal => UNK
#define SMD_MIN_SAMPLES 16

static int8_t smd_percentile(const uint16_t hist[256], size_t total, uint8_t pctl) {
    size_t target = (total * pctl) / 100;
    size_t cum = 0;
    for(int v = 0; v < 256; v++) {
        cum += hist[v];
        if(cum >= target) return (int8_t)(v - 128);
    }
    return 0;
}

SubGhzModDetect subghz_mod_classify(const int8_t* s, size_t n) {
    SubGhzModDetect r = {SIG_MOD_UNK, 0, 0, 0};
    if(!s || n < SMD_MIN_SAMPLES) return r;

    uint16_t hist[256] = {0};
    for(size_t i = 0; i < n; i++) hist[(uint8_t)((int)s[i] + 128)]++;

    int8_t floor = smd_percentile(hist, n, SMD_FLOOR_PCTL);
    int8_t peak = smd_percentile(hist, n, SMD_PEAK_PCTL);
    r.floor_dbm = floor;
    r.peak_dbm = peak;
    int swing = (int)peak - (int)floor;

    if((int)peak < SMD_MIN_PEAK_DBM) return r; // no real signal present -> UNK

    if(swing >= SMD_SWING_MIN_DB) {
        r.mod = SIG_MOD_AM; // OOK: carrier keyed on/off
        int c = 45 + (swing - SMD_SWING_MIN_DB) * 3;
        r.confidence = (uint8_t)(c > 100 ? 100 : c);
    } else if(swing <= SMD_FM_SWING_MAX_DB) {
        r.mod = SIG_MOD_FM; // FSK: carrier steady, only frequency shifts
        int c = 45 + (SMD_FM_SWING_MAX_DB - swing) * 5 + ((int)peak - SMD_MIN_PEAK_DBM);
        r.confidence = (uint8_t)(c > 100 ? 100 : (c < 0 ? 0 : c));
    } else {
        r.mod = SIG_MOD_UNK; // ambiguous middle
        r.confidence = 20;
    }
    return r;
}
