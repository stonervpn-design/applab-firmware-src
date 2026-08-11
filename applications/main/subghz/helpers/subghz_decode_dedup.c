// applications/main/subghz/helpers/subghz_decode_dedup.c
#include "subghz_decode_dedup.h"
#include <string.h>

// Loose fixed-code decoders prone to grabbing signals that aren't theirs.
// Specific / rolling / checksum-validated decoders.
#ifndef PUBLIC_BUILD
static const char* const k_low[]  = {"Jeep V0", "Hitag2"};
static const char* const k_high[] = {"Hyundai TL", "KeeLoq", "Security+ 1.0", "Security+ 2.0"};
#else
// PUBLIC_BUILD: car decoders (Jeep V0 / Hitag2 / Hyundai TL) are absent — keep only
// the general fixed/rolling names so the dedup tiers still work for gate/garage codes.
// k_low placeholder never matches a real protocol name (in_list uses strcmp).
static const char* const k_low[]  = {""};
static const char* const k_high[] = {"KeeLoq", "Security+ 1.0", "Security+ 2.0"};
#endif

static bool in_list(const char* n, const char* const* list, size_t count) {
    if(!n) return false;
    for(size_t i = 0; i < count; i++)
        if(strcmp(n, list[i]) == 0) return true;
    return false;
}

void subghz_decode_dedup_reset(SubGhzDecodeDedup* d) {
    d->last_protocol = NULL;
    d->last_score = 0;
    d->last_tick = 0;
    d->last_history_idx = -1;
}

uint16_t subghz_decode_dedup_score(const char* protocol_name, uint16_t bit_count) {
    uint16_t tier = 2000; // medium (default)
    if(in_list(protocol_name, k_low, sizeof(k_low) / sizeof(k_low[0]))) tier = 1000;
    else if(in_list(protocol_name, k_high, sizeof(k_high) / sizeof(k_high[0]))) tier = 3000;
    uint16_t tie = bit_count < 999 ? bit_count : 999; // never crosses a tier
    return (uint16_t)(tier + tie);
}

SubGhzDedupAction subghz_decode_dedup_decide(
    SubGhzDecodeDedup* d, const char* protocol_name, uint16_t score, uint32_t now_tick) {
    if(d->last_history_idx >= 0 && (now_tick - d->last_tick) <= SUBGHZ_DEDUP_WINDOW_MS) {
        // Two DIFFERENT high-confidence protocols (score >= 3000 == the specific/rolling/
        // checksum-validated k_high tier) inside the burst window are almost certainly two
        // distinct transmissions, not one signal double-decoded — keep both. A loose
        // (lower-tier) decoder grabbing the same signal still collapses via the score check.
        if(protocol_name && d->last_protocol && strcmp(protocol_name, d->last_protocol) != 0 &&
           score >= 3000 && d->last_score >= 3000) {
            return SubGhzDedupAdd;
        }
        return (score > d->last_score) ? SubGhzDedupReplace : SubGhzDedupSuppress;
    }
    return SubGhzDedupAdd;
}

void subghz_decode_dedup_commit(
    SubGhzDecodeDedup* d, const char* protocol_name, uint16_t score,
    uint32_t now_tick, int32_t history_idx) {
    d->last_protocol = protocol_name;
    d->last_score = score;
    d->last_tick = now_tick;
    d->last_history_idx = history_idx;
}
