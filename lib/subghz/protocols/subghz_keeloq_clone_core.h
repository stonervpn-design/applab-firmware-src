#pragma once
#include <stdint.h>
#include <stdbool.h>

// Pure, hardware-free desync-safe clone state + counter algorithm (host-testable).
// A clone shares the ORIGINAL fob's serial, so the receiver tracks ONE rolling-counter
// slot for both. To avoid stranding the original we advance that shared counter by
// exactly +1 per press (a genuine fob's step) and never leap ahead — so the original's
// worst-case catch-up is only as many presses as the clone was used, and it always
// recovers. (The old design leapt +jump on first use, which is what desynced the fob.)
// `jump` is an optional one-time forward margin (default 0 = never leap) to absorb a few
// real-fob presses made since capture, trading a little original catch-up for reliability.
typedef struct {
    uint32_t serial;    // 28-bit fob serial (shared with the original)
    uint8_t  btn;       // 4-bit button nibble
    uint64_t kdev;      // derived device key (final `man` fed to KeeLoq encrypt); 0 = absent
    uint8_t  learn_type;// KEELOQ_LEARNING_* id (for display / seed handling)
    uint16_t last_cnt;  // last counter transmitted (persisted); init to the captured counter
    uint16_t jump;      // one-time forward margin, default 0 (no lead)
    bool     synced;    // has the one-time margin been applied yet
} KeeloqClone;

// Next counter to transmit — a SINGLE frame, +1 from the last (spare-fob step).
// First call: catch up to max(last_cnt, seen_cnt), apply the one-time `jump` margin, +1.
// Subsequent calls: +1. Updates c->last_cnt and returns it. seen_cnt is a freshly-observed
// live-fob counter (pass 0 / last_cnt when re-opening without a re-capture). Catch-up is
// MODULAR (a seen_cnt that is 1..0x7FFF ahead of last_cnt wins, so it works across the
// 16-bit wrap; a behind/equal seen_cnt is ignored).
uint16_t keeloq_clone_next(KeeloqClone* c, uint16_t seen_cnt);

// FORCE-RESYNC ("Sync" button). Jump the shared receiver counter ahead by `margin` and
// return the TWO consecutive counters to transmit BACK-TO-BACK. A KeeLoq receiver whose
// stored counter has drifted forward (because the ORIGINAL fob was used) re-locks onto THIS
// remote on the pair's second frame via its double-operating-window resync — but ONLY if
// `margin` exceeds that drift, so pick a margin larger than the most presses the original
// could have made since the last sync. Persists state to the second counter
// (c->last_cnt = *out_second, c->synced = true) so the next ordinary press is second+1 and
// never desyncs the clone against itself. Returns false and leaves state UNCHANGED when
// `margin` is outside the valid resync window: it must clear the single window (> 16) and
// stay out of the replay/behind zone (< 0x8000). seen_cnt is an optional freshly-observed
// live counter (modular catch-up, as in keeloq_clone_next); pass 0 / last_cnt if none.
// Cost (inherent to a same-serial clone): after a Sync the ORIGINAL fob is `margin` behind
// and needs that many presses to catch back up — this is the deliberate "takeover" action.
bool keeloq_clone_resync_pair(
    KeeloqClone* c,
    uint16_t seen_cnt,
    uint16_t margin,
    uint16_t* out_first,
    uint16_t* out_second);
