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
    d->last_hash = 0;
    d->last_hash_valid = false;
}

uint32_t subghz_decode_dedup_hash(const char* s) {
    // FNV-1a 32-bit. Only used to compare a decode against the immediately-previous
    // one, so collision resistance is a non-issue; identical payload text -> identical hash.
    uint32_t h = 2166136261u;
    if(s) {
        for(; *s; s++) {
            h ^= (uint8_t)*s;
            h *= 16777619u;
        }
    }
    return h;
}

uint16_t subghz_decode_dedup_score(const char* protocol_name, uint16_t bit_count) {
    uint16_t tier = 2000; // medium (default)
    if(in_list(protocol_name, k_low, sizeof(k_low) / sizeof(k_low[0]))) tier = 1000;
    else if(in_list(protocol_name, k_high, sizeof(k_high) / sizeof(k_high[0]))) tier = 3000;
    uint16_t tie = bit_count < 999 ? bit_count : 999; // never crosses a tier
    return (uint16_t)(tier + tie);
}

SubGhzDedupAction subghz_decode_dedup_decide(
    SubGhzDecodeDedup* d, const char* protocol_name, uint16_t score,
    uint32_t payload_hash, bool hash_valid, uint32_t now_tick) {
    if(d->last_history_idx < 0) return SubGhzDedupAdd; // nothing to compare against

    bool same_protocol = protocol_name && d->last_protocol &&
                         strcmp(protocol_name, d->last_protocol) == 0;

    // PRIMARY key: payload identity. A new item is a duplicate only when it is the same
    // protocol AND the same decoded payload as the immediately-previous accepted item.
    // A rolling code advances its counter every press, so its payload hash differs —
    // that is a genuinely different press and is always kept.
    if(hash_valid && d->last_hash_valid) {
        if(same_protocol && payload_hash == d->last_hash) {
            // Identical frame. A fob resends one frame several times per press: inside the
            // guard window that is the same press repeating -> suppress. Past the window a
            // repeat of an identical fixed code is a deliberate new press -> keep it.
            return ((now_tick - d->last_tick) <= SUBGHZ_DEDUP_SAME_PAYLOAD_WINDOW_MS)
                       ? SubGhzDedupSuppress
                       : SubGhzDedupAdd;
        }
        // Payload differs -> a new / rolling press. Keep both regardless of the window.
        return SubGhzDedupAdd;
    }

    // SECONDARY fallback: payload identity unavailable (a decoder without get_string).
    // Fall back to the original score-tier heuristic within the short burst window so a
    // loose low-tier re-decode of one signal still collapses into the better result.
    if((now_tick - d->last_tick) <= SUBGHZ_DEDUP_WINDOW_MS) {
        // Two DIFFERENT high-confidence protocols (score >= 3000 == the specific/rolling/
        // checksum-validated k_high tier) inside the burst window are almost certainly two
        // distinct transmissions, not one signal double-decoded — keep both.
        if(!same_protocol && score >= 3000 && d->last_score >= 3000) {
            return SubGhzDedupAdd;
        }
        return (score > d->last_score) ? SubGhzDedupReplace : SubGhzDedupSuppress;
    }
    return SubGhzDedupAdd;
}

void subghz_decode_dedup_commit(
    SubGhzDecodeDedup* d, const char* protocol_name, uint16_t score,
    uint32_t payload_hash, bool hash_valid, uint32_t now_tick, int32_t history_idx) {
    d->last_protocol = protocol_name;
    d->last_score = score;
    d->last_tick = now_tick;
    d->last_history_idx = history_idx;
    d->last_hash = payload_hash;
    d->last_hash_valid = hash_valid;
}
