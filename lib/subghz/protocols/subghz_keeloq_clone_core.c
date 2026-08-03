#include "subghz_keeloq_clone_core.h"

void keeloq_clone_next_pair(KeeloqClone* c, uint16_t seen_cnt, uint16_t* out_a, uint16_t* out_b) {
    uint16_t base = c->last_cnt;
    if(seen_cnt > base) base = seen_cnt; // catch up to a fresh live capture that is ahead

    uint16_t target;
    if(!c->synced) {
        target = (uint16_t)(base + c->jump); // one-time forward lead (modular)
        c->synced = true;
    } else {
        target = (uint16_t)(base + 1); // pair advances counter by +2 per send
    }

    *out_a = target;
    *out_b = (uint16_t)(target + 1);
    c->last_cnt = *out_b;
}
