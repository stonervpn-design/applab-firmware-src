#include "subghz_rollback.h"
#include <string.h>

uint32_t subghz_rollback_hex_after(const char* s, const char* marker) {
    const char* p = strstr(s, marker);
    if(!p) return 0;
    p += strlen(marker);
    while(*p == ' ') p++;
    if(p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    uint32_t value = 0; bool any = false;
    while(*p) {
        char c = *p; uint8_t d;
        if(c >= '0' && c <= '9') d = (uint8_t)(c - '0');
        else if(c >= 'a' && c <= 'f') d = (uint8_t)(c - 'a' + 10);
        else if(c >= 'A' && c <= 'F') d = (uint8_t)(c - 'A' + 10);
        else break;
        value = (value << 4) | d; any = true; p++;
    }
    return any ? value : 0;
}

bool subghz_rollback_manufacturer(const char* s, char* out, size_t out_size) {
    const char* p = strstr(s, "MF:");
    if(!p) return false;
    p += 3; size_t i = 0;
    while(*p && *p != '\r' && *p != '\n' && i + 1 < out_size) out[i++] = *p++;
    out[i] = '\0';
    return true;
}

void subghz_rollback_store_reset(RollbackStore* st) {
    memset(st, 0, sizeof(*st));
}

bool subghz_rollback_store_add(RollbackStore* st, const RollbackSlot* s) {
    if(st->count >= ROLLBACK_MAX_SLOTS) return false;
    for(uint8_t i = 0; i < st->count; i++)
        if(st->slot[i].serial == s->serial && st->slot[i].counter == s->counter)
            return false; // exact duplicate
    st->slot[st->count++] = *s;
    return true;
}

uint8_t subghz_rollback_store_count(const RollbackStore* st) { return st->count; }

bool subghz_rollback_ready(const RollbackStore* st, uint32_t* same_serial) {
    for(uint8_t i = 0; i < st->count; i++) {
        uint8_t group = 1;
        for(uint8_t j = i + 1; j < st->count; j++)
            if(st->slot[j].serial == st->slot[i].serial) group++;
        if(group >= 2) { if(same_serial) *same_serial = st->slot[i].serial; return true; }
    }
    return false;
}

uint8_t subghz_rollback_order_indices(
    const RollbackStore* st, uint32_t serial, RollbackOrder order,
    uint8_t* out_idx, uint8_t out_cap) {
    uint8_t n = 0;
    for(uint8_t i = 0; i < st->count && n < out_cap; i++)
        if(st->slot[i].serial == serial) out_idx[n++] = i;
    if(order != RollbackOrderAsCaptured) {
        for(uint8_t a = 0; a < n; a++)      // insertion sort by counter
            for(uint8_t b = (uint8_t)(a + 1); b < n; b++) {
                bool swap = (order == RollbackOrderAsc)
                    ? st->slot[out_idx[b]].counter < st->slot[out_idx[a]].counter
                    : st->slot[out_idx[b]].counter > st->slot[out_idx[a]].counter;
                if(swap) { uint8_t t = out_idx[a]; out_idx[a] = out_idx[b]; out_idx[b] = t; }
            }
    }
    return n;
}
