#include "subghz_keeloq_clone_core.h"

// Modular "is a ahead of b" over the 16-bit counter ring: true when the forward distance
// b->a is in (0, 0x8000). Handles the 0xFFFF->0x0000 wrap that a plain > compare misses.
static inline bool keeloq_clone_ahead(uint16_t a, uint16_t b) {
    uint16_t d = (uint16_t)(a - b);
    return d != 0 && d < 0x8000;
}

uint16_t keeloq_clone_next(KeeloqClone* c, uint16_t seen_cnt) {
    if(!c->synced) {
        uint16_t base = c->last_cnt;
        if(keeloq_clone_ahead(seen_cnt, base)) base = seen_cnt; // catch up to a live counter (modular)
        c->last_cnt = (uint16_t)(base + c->jump); // one-time forward margin (default 0 = no lead)
        c->synced = true;
    }
    c->last_cnt = (uint16_t)(c->last_cnt + 1);    // +1 per press — exactly a real fob's step
    return c->last_cnt;
}

bool keeloq_clone_resync_pair(
    KeeloqClone* c,
    uint16_t seen_cnt,
    uint16_t margin,
    uint16_t* out_first,
    uint16_t* out_second) {
    // Valid KeeLoq double-window resync leap: clear the single operating window (16) yet
    // stay out of the replay/behind zone (< 0x8000). Anything else can never resync.
    if(margin <= 16 || margin >= 0x8000) return false;
    uint16_t base = c->last_cnt;
    if(keeloq_clone_ahead(seen_cnt, base)) base = seen_cnt; // modular catch-up to a live counter
    uint16_t first = (uint16_t)(base + margin);
    uint16_t second = (uint16_t)(first + 1);
    if(out_first) *out_first = first;
    if(out_second) *out_second = second;
    c->last_cnt = second; // persist the leap; next ordinary press is second+1
    c->synced = true;
    return true;
}
