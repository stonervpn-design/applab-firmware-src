#include "subghz_keeloq_clone_core.h"

uint16_t keeloq_clone_next(KeeloqClone* c, uint16_t seen_cnt) {
    if(!c->synced) {
        uint16_t base = c->last_cnt;
        if(seen_cnt > base) base = seen_cnt;      // catch up to a freshly-observed live counter
        c->last_cnt = (uint16_t)(base + c->jump); // one-time forward margin (default 0 = no lead)
        c->synced = true;
    }
    c->last_cnt = (uint16_t)(c->last_cnt + 1);    // +1 per press — exactly a real fob's step
    return c->last_cnt;
}
