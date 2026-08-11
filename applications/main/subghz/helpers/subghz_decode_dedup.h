// applications/main/subghz/helpers/subghz_decode_dedup.h
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SUBGHZ_DEDUP_WINDOW_MS 150u

typedef struct {
    const char* last_protocol;
    uint16_t    last_score;
    uint32_t    last_tick;
    int32_t     last_history_idx; // -1 = none committed yet
} SubGhzDecodeDedup;

typedef enum {
    SubGhzDedupAdd,       // add as a new history entry
    SubGhzDedupReplace,   // delete last_history_idx, then add the new one
    SubGhzDedupSuppress,  // skip: a better result already committed this burst
} SubGhzDedupAction;

void     subghz_decode_dedup_reset(SubGhzDecodeDedup* d);
uint16_t subghz_decode_dedup_score(const char* protocol_name, uint16_t bit_count);
SubGhzDedupAction subghz_decode_dedup_decide(
    SubGhzDecodeDedup* d, const char* protocol_name, uint16_t score, uint32_t now_tick);
void     subghz_decode_dedup_commit(
    SubGhzDecodeDedup* d, const char* protocol_name, uint16_t score,
    uint32_t now_tick, int32_t history_idx);
