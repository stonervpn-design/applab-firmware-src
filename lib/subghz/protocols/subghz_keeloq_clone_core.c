#include "subghz_keeloq_clone_core.h"

void keeloq_clone_next_pair(KeeloqClone* c, uint16_t seen_cnt, uint16_t* out_a, uint16_t* out_b) {
    uint16_t target;
    if(!c->synced) {
        uint16_t base = c->last_cnt;
        if(seen_cnt > base) base = seen_cnt; // one-time catch-up before the jump lead
        target = (uint16_t)(base + c->jump);
        c->synced = true;
    } else {
        target = (uint16_t)(c->last_cnt + 1); // +2 pair advance; seen_cnt ignored once synced
    }
    *out_a = target;
    *out_b = (uint16_t)(target + 1);
    c->last_cnt = *out_b;
}
