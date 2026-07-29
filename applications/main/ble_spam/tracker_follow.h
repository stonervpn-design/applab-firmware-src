#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    FollowNew,
    FollowPresent,
    FollowSuspect,
    FollowFollowing,
} FollowState;

#define FOLLOW_STALE_MS      30000u
#define FOLLOW_PRESENT_MS    60000u
#define FOLLOW_SUSPECT_MS    180000u
#define FOLLOW_FOLLOWING_MS  300000u

/* Escalate by continuous-presence dwell (last_seen - first_seen), but only while
 * ACTIVE (seen within FOLLOW_STALE_MS). A stale device returns FollowNew. All
 * args are furi_get_tick() milliseconds; arithmetic is uint32 modular (wrap-safe). */
FollowState tracker_follow_state(uint32_t first_seen_ms, uint32_t last_seen_ms, uint32_t now_ms);

/* "NEW" / "SEEN" / "SUSPECT" / "FOLLOW!" */
const char* tracker_follow_label(FollowState s);

/* Extract a BLE local name (AD type 0x09 complete or 0x08 shortened) from an
 * advertisement TLV buffer into out (NUL-terminated, truncated to out_sz-1).
 * Returns true only if a non-empty name was found. Pure — no esp/furi deps. */
bool ble_adv_extract_local_name(const uint8_t* buf, uint8_t len, char* out, uint8_t out_sz);
