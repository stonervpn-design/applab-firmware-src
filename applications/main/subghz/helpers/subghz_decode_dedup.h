// applications/main/subghz/helpers/subghz_decode_dedup.h
#pragma once
#include <stdint.h>
#include <stdbool.h>

// Burst window for the score-tier fallback (used only when a decode has no hashable
// payload, i.e. a decoder without get_string).
#define SUBGHZ_DEDUP_WINDOW_MS 150u
// Guard window for payload-identity suppression: an identical frame recurring inside
// this window is one press bursting its resends; past it, an identical fixed code is a
// deliberate re-press and is kept.
#define SUBGHZ_DEDUP_SAME_PAYLOAD_WINDOW_MS 2000u

typedef struct {
    const char* last_protocol;
    uint16_t    last_score;
    uint32_t    last_tick;
    int32_t     last_history_idx; // -1 = none committed yet
    uint32_t    last_hash;        // payload-identity hash of the last accepted item
    bool        last_hash_valid;  // false when the last item had no hashable payload
} SubGhzDecodeDedup;

typedef enum {
    SubGhzDedupAdd,       // add as a new history entry
    SubGhzDedupReplace,   // delete last_history_idx, then add the new one
    SubGhzDedupSuppress,  // skip: same protocol + same payload as the last item
} SubGhzDedupAction;

void     subghz_decode_dedup_reset(SubGhzDecodeDedup* d);
uint16_t subghz_decode_dedup_score(const char* protocol_name, uint16_t bit_count);
// FNV-1a 32-bit hash of a decoded-payload string (from get_string) — the payload identity.
uint32_t subghz_decode_dedup_hash(const char* s);
SubGhzDedupAction subghz_decode_dedup_decide(
    SubGhzDecodeDedup* d, const char* protocol_name, uint16_t score,
    uint32_t payload_hash, bool hash_valid, uint32_t now_tick);
void     subghz_decode_dedup_commit(
    SubGhzDecodeDedup* d, const char* protocol_name, uint16_t score,
    uint32_t payload_hash, bool hash_valid, uint32_t now_tick, int32_t history_idx);
