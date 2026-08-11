// Signal-ID fingerprint engine — see signal_fingerprint.h.
// Logic mirrors tools/signal_id/build_fingerprints.py (verified by execution).
#include "signal_fingerprint.h"

#include <string.h>

#define BAND_TOL_HZ  3000000u
#define TIE_DELTA    4          // within this many points of top == tied
#define FLOOR_SCORE  40         // below this top score -> UNKNOWN
#define CONF_SCORE   55         // at/above this (and unambiguous) -> confident

#ifdef PUBLIC_BUILD
// Public builds ship NO automotive fingerprint data (owner guardrail: no car
// traces on public — the mined table is full of vehicle make/model names). One
// inert placeholder keeps the classifier allocation-safe: all bands are 0 so it
// never matches, and it carries no vehicle name/string. classify() then always
// returns UNKNOWN, which is the correct public behaviour.
static const SignalFingerprint k_signal_fingerprints[] = {
    {.name = "", .make = NULL},
};
#else
// The mined table: static const SignalFingerprint k_signal_fingerprints[].
#include "signal_fingerprint_db.inc"
#endif
#define FP_COUNT (sizeof(k_signal_fingerprints) / sizeof(k_signal_fingerprints[0]))

static bool in_any_band(uint32_t freq, const SignalFingerprint* e) {
    const uint32_t bands[3] = {e->band0, e->band1, e->band2};
    for(int i = 0; i < 3; i++) {
        if(bands[i] == 0u) continue;
        uint32_t d = (freq > bands[i]) ? (freq - bands[i]) : (bands[i] - freq);
        if(d <= BAND_TOL_HZ) return true;
    }
    return false;
}

// 0..100 scaled ratio closeness (>=0.92 -> full, <=0.60 -> zero).
static int32_t ratio_score_x100(uint32_t obs, uint32_t ref) {
    if(obs == 0u || ref == 0u) return 0;
    // r = min/max in fixed point (x1000)
    uint32_t lo = obs < ref ? obs : ref;
    uint32_t hi = obs < ref ? ref : obs;
    uint32_t r = (uint32_t)((uint64_t)lo * 1000u / hi); // 0..1000
    if(r >= 920u) return 100;
    if(r <= 600u) return 0;
    return (int32_t)((r - 600u) * 100u / (920u - 600u));
}

static int32_t bits_score_x100(int32_t obs, int32_t ref) {
    if(ref <= 0 || obs <= 0) return 50;
    int32_t d = obs > ref ? obs - ref : ref - obs;
    if(d == 0) return 100;
    if(d <= 2) return 80;
    if(d <= 6) return 50;
    if(d <= 12) return 20;
    return 0;
}

static SignalAction action_for(const SignalFingerprint* e, bool confident) {
    if(!confident) return SIG_ACTION_CAPTURE;
    if(e->make && strstr(e->make, "alarm")) return SIG_ACTION_DEFEND;
    if((e->flags & SIG_F_ROLLING) && (e->flags & SIG_F_TX)) return SIG_ACTION_ATTACK;
    return SIG_ACTION_EXPLAIN;
}

static void fill_from(SignalMatch* m, const SignalFingerprint* e, int32_t score,
                      bool confident, bool ambiguous) {
    m->name = e->name;
    m->make = e->make;
    m->score = score;
    m->rolling = (e->flags & SIG_F_ROLLING) != 0;
    m->tx = (e->flags & SIG_F_TX) != 0;
    m->confident = confident;
    m->ambiguous = ambiguous;
    m->action = action_for(e, confident);
}

size_t signal_fingerprint_classify(
    const SignalObservation* obs,
    SignalMatch* out,
    size_t max_out) {
    if(!obs || !out || max_out == 0) return 0;

    // Authoritative path: a decoder already matched -> ground truth.
    if(obs->name) {
        for(size_t i = 0; i < FP_COUNT; i++) {
            if(strcmp(obs->name, k_signal_fingerprints[i].name) == 0) {
                fill_from(&out[0], &k_signal_fingerprints[i], 100, true, false);
                return 1;
            }
        }
    }

    // Score every candidate that passes the hard filters.
    int32_t scores[FP_COUNT];
    int32_t top = -1;
    for(size_t i = 0; i < FP_COUNT; i++) {
        const SignalFingerprint* e = &k_signal_fingerprints[i];
        if(obs->mod != SIG_MOD_UNK && e->mod != SIG_MOD_UNK && obs->mod != e->mod) {
            scores[i] = -1;
            continue;
        }
        if(!in_any_band(obs->freq, e)) {
            scores[i] = -1;
            continue;
        }
        int32_t s = (45 * ratio_score_x100(obs->te_short, e->te_short) +
                     25 * ratio_score_x100(obs->te_long, e->te_long) +
                     30 * bits_score_x100(obs->bits, e->bits)) / 100;
        scores[i] = s;
        if(s > top) top = s;
    }

    if(top < FLOOR_SCORE) {
        out[0].name = "UNKNOWN";
        out[0].make = NULL;
        out[0].score = top < 0 ? 0 : top;
        out[0].rolling = false;
        out[0].tx = false;
        out[0].confident = false;
        out[0].ambiguous = false;
        out[0].action = SIG_ACTION_CAPTURE;
        return 1;
    }

    // Count how many are tied near the top (== ambiguous when >1).
    size_t tied = 0;
    for(size_t i = 0; i < FP_COUNT; i++) {
        if(scores[i] >= 0 && (top - scores[i]) <= TIE_DELTA) tied++;
    }
    bool ambiguous = tied > 1;

    // Selection sort the top `max_out` by score (small N, allocation-free).
    size_t n = 0;
    bool used[FP_COUNT];
    memset(used, 0, sizeof(used));
    while(n < max_out) {
        int32_t best = -1;
        size_t best_i = 0;
        for(size_t i = 0; i < FP_COUNT; i++) {
            if(used[i] || scores[i] < 0) continue;
            if(scores[i] > best) {
                best = scores[i];
                best_i = i;
            }
        }
        if(best < 0) break;
        // When ambiguous, only surface the tied set; otherwise fill normally.
        if(ambiguous && (top - best) > TIE_DELTA) break;
        used[best_i] = true;
        bool confident = (best >= CONF_SCORE) && !ambiguous;
        fill_from(&out[n], &k_signal_fingerprints[best_i], best, confident, ambiguous);
        n++;
    }
    return n;
}
