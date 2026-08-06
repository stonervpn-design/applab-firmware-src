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

// HW (subghz_sig_identify.c): load the preset matching mod, async-RX capture edges
// over a window, then subghz_sig_identify_from_samples(). Writes the match label
// (make if present, else name) into out (<= cap) and 0..100 score to *out_score.
// Returns true on a confident match. Runs in the analyzer worker (owns the radio).
bool subghz_sig_identify_run(
    uint32_t freq,
    SignalMod mod,
    char* out,
    size_t cap,
    uint8_t* out_score);
