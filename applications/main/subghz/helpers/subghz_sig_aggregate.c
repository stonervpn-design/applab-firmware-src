// Pure K-of-N aggregator for multi-frame signal-ID. Declared in
// subghz_sig_identify.h; kept in its own file so it depends only on SignalMatch
// (+ string.h) and host-tests without the protoview/radio pipeline.
#include "signal_fingerprint.h" // SignalMatch
#include <string.h>
#include <stddef.h>
#include <stdint.h>

#define SIG_AGG_MAX 8 // max distinct candidate names tallied when aggregating

// K-of-N vote across per-frame matches. Only frames with a real name clearing
// min_score vote; the winning name (>= min_votes) is returned via its
// best-scoring frame (keeps the make/action from the cleanest read). No winner
// -> UNKNOWN. Corroboration is what stops one dirty frame from mislabelling.
SignalMatch subghz_sig_aggregate(
    const SignalMatch* frames,
    size_t n,
    size_t min_votes,
    int32_t min_score) {
    SignalMatch unknown = {0};
    unknown.name = "UNKNOWN";
    if(!frames || n == 0) return unknown;

    struct {
        const char* name;
        size_t count;
        int32_t best_score;
        size_t best_idx;
    } tally[SIG_AGG_MAX];
    size_t tn = 0;

    for(size_t i = 0; i < n; i++) {
        const SignalMatch* f = &frames[i];
        if(!f->name || strcmp(f->name, "UNKNOWN") == 0 || f->score < min_score) continue;

        size_t j = 0;
        for(; j < tn; j++) {
            if(tally[j].name && strcmp(tally[j].name, f->name) == 0) break;
        }
        if(j == tn) {
            if(tn >= SIG_AGG_MAX) continue; // table full -> ignore extra rare names
            tally[tn].name = f->name;
            tally[tn].count = 0;
            tally[tn].best_score = -1;
            tally[tn].best_idx = i;
            tn++;
        }
        tally[j].count++;
        if(f->score > tally[j].best_score) {
            tally[j].best_score = f->score;
            tally[j].best_idx = i;
        }
    }

    // Winner = most votes; ties broken by the higher best score.
    size_t best = tn; // sentinel
    for(size_t j = 0; j < tn; j++) {
        if(best == tn || tally[j].count > tally[best].count ||
           (tally[j].count == tally[best].count &&
            tally[j].best_score > tally[best].best_score)) {
            best = j;
        }
    }
    if(best == tn || tally[best].count < min_votes) return unknown;
    return frames[tally[best].best_idx];
}
