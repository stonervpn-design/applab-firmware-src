// Signal-ID fingerprint engine — pure classification, no radio / storage / UI.
//
// Turns a capture's measurable characteristics into a ranked, explained
// identification. The logic mirrors tools/signal_id/build_fingerprints.py
// (which is verified by execution); the fingerprint table is auto-mined from
// lib/subghz/protocols decoder constants via that same script.
//
// SAFETY: this module never selects a radio, never transmits. A wrong result is
// a mislabel, not an RF action.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    SIG_MOD_UNK = 0,
    SIG_MOD_AM,   // OOK / ASK
    SIG_MOD_FM,   // FSK
} SignalMod;

// Per-fingerprint flags.
#define SIG_F_ROLLING (1u << 0) // frame carries a rolling/crypto authenticator
#define SIG_F_TX      (1u << 1) // this firmware has a working encoder for it

// Suggested next action for the operator.
typedef enum {
    SIG_ACTION_CAPTURE = 0, // unknown / low confidence -> capture more / raw
    SIG_ACTION_ATTACK,      // rolling + TX-capable -> red-team surface
    SIG_ACTION_DEFEND,      // alarm/jammer/tracker family -> Guardian
    SIG_ACTION_EXPLAIN,     // known & benign -> hand to the copilot
} SignalAction;

typedef struct {
    const char* name;   // protocol name, e.g. "Kia V7"
    const char* make;   // make/model label, or NULL
    SignalMod mod;
    int32_t bits;       // nominal decoded bit count
    uint32_t te_short;  // us
    uint32_t te_long;   // us
    uint32_t band0;     // nominal centre frequencies (Hz); band1/band2 == 0 if unused
    uint32_t band1;
    uint32_t band2;
    uint32_t flags;     // SIG_F_*
} SignalFingerprint;

// What the caller observed about a capture. Set `name` to a decoded protocol
// name when a decoder already matched (authoritative); leave NULL for raw.
typedef struct {
    uint32_t freq;      // Hz
    SignalMod mod;
    int32_t bits;
    uint32_t te_short;  // us
    uint32_t te_long;   // us
    const char* name;   // decoded protocol name or NULL
} SignalObservation;

typedef struct {
    const char* name;
    const char* make;
    int32_t score;      // 0..100
    bool rolling;
    bool tx;
    bool confident;
    bool ambiguous;     // tied with same-timing rivals; decode to disambiguate
    SignalAction action;
} SignalMatch;

// Classify `obs` against the built-in fingerprint table. Writes up to `max_out`
// ranked matches into `out`; returns the count (>=1; a single UNKNOWN when
// nothing clears the floor). Deterministic, allocation-free.
size_t signal_fingerprint_classify(
    const SignalObservation* obs,
    SignalMatch* out,
    size_t max_out);
