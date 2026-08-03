#pragma once
#include <stdint.h>
#include <stdbool.h>

// Pure, hardware-free no-desync clone state + counter algorithm.
// No furi / furi_hal / FreeRTOS references (host-testable).
typedef struct {
    uint32_t serial;    // 28-bit fob serial
    uint8_t  btn;       // 4-bit button nibble
    uint64_t kdev;      // derived device key (final `man` fed to KeeLoq encrypt); 0 = absent
    uint8_t  learn_type;// KEELOQ_LEARNING_* id (for display / seed handling)
    uint16_t last_cnt;  // persisted last counter emitted (the +1 of the last pair)
    uint16_t jump;      // one-time forward lead applied until synced
    bool     synced;    // has the one-time jump been applied yet
} KeeloqClone;

// Compute the next no-desync frame pair.
// - If !synced: target = max(last_cnt, seen_cnt) + jump, then synced := true (one-time lead).
// - Else:       target = last_cnt + 1 (pair advances the counter by +2 per send).
// Writes target -> *out_a and (target+1) -> *out_b (both 16-bit modular),
// and updates c->last_cnt = *out_b. seen_cnt is a freshly-observed live-fob counter
// (pass 0 or last_cnt when re-opening without a re-capture).
void keeloq_clone_next_pair(KeeloqClone* c, uint16_t seen_cnt, uint16_t* out_a, uint16_t* out_b);
