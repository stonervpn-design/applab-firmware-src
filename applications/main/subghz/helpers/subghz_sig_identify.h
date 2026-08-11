#pragma once
#include "signal_fingerprint.h" // SignalMod, SignalMatch, SignalObservation
#include "protoview_ring.h" // PvSample
#include <stdint.h>
#include <stddef.h>

// Raw-signal identification for the frequency analyzer (PRIVATE builds only — the
// call sites are #ifndef PUBLIC_BUILD, so these are unreferenced/GC'd in public).

// PURE (subghz_sig_match.c): estimate TE (protoview) + bit count from captured
// edges, build an observation with the given freq/mod, and classify it. Returns
// the top match; .name == "UNKNOWN" (or score 0) when nothing fits.
SignalMatch subghz_sig_identify_from_samples(
    uint32_t freq,
    SignalMod mod,
    const PvSample* samples,
    size_t n);

// PURE (subghz_sig_match.c): aggregate N per-frame matches into one verdict.
// K-of-N vote on the matched name among frames scoring >= min_score; returns the
// best-scoring frame of the winning name when it has >= min_votes, else UNKNOWN.
// Cross-frame corroboration cuts single-noisy-frame mislabels.
SignalMatch subghz_sig_aggregate(
    const SignalMatch* frames,
    size_t n,
    size_t min_votes,
    int32_t min_score);

// HW (subghz_sig_identify.c): load the preset matching mod, async-RX capture edges
// over a window, then subghz_sig_identify_from_samples(). Writes the match label
// (make if present, else name) into out (<= cap) and 0..100 score to *out_score.
// Returns true on a confident match. Runs in the analyzer worker (owns the radio).
//
// mod_confidence (0..100) is the mod-detect confidence for `mod`. A low-confidence
// guess is NOT trusted as a hard constraint: it neither forces the FSK demodulator
// nor suppresses the true protocol (see the .c for the exact gating).
bool subghz_sig_identify_run(
    uint32_t freq,
    SignalMod mod,
    uint8_t mod_confidence,
    char* out,
    size_t cap,
    uint8_t* out_score);
