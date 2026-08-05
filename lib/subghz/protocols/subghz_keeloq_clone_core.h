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
// live-fob counter (pass 0 / last_cnt when re-opening without a re-capture).
uint16_t keeloq_clone_next(KeeloqClone* c, uint16_t seen_cnt);
