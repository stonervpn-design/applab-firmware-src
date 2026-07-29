#include "tracker_follow.h"
#include <string.h>

FollowState tracker_follow_state(uint32_t first_seen_ms, uint32_t last_seen_ms, uint32_t now_ms) {
    if((uint32_t)(now_ms - last_seen_ms) >= FOLLOW_STALE_MS) return FollowNew;
    uint32_t dwell = (uint32_t)(last_seen_ms - first_seen_ms);
    if(dwell >= FOLLOW_FOLLOWING_MS) return FollowFollowing;
    if(dwell >= FOLLOW_SUSPECT_MS) return FollowSuspect;
    if(dwell >= FOLLOW_PRESENT_MS) return FollowPresent;
    return FollowNew;
}

const char* tracker_follow_label(FollowState s) {
    switch(s) {
    case FollowFollowing: return "FOLLOW!";
    case FollowSuspect:   return "SUSPECT";
    case FollowPresent:   return "SEEN";
    default:              return "NEW";
    }
}

bool ble_adv_extract_local_name(const uint8_t* buf, uint8_t len, char* out, uint8_t out_sz) {
    if(!out || out_sz == 0) return false;
    uint8_t pos = 0;
    while(pos + 1 < len) {
        uint8_t fld_len = buf[pos];
        if(fld_len == 0) break;
        if(pos + fld_len >= len) break;            /* field would overrun buffer */
        uint8_t type = buf[pos + 1];
        if(type == 0x09 || type == 0x08) {         /* complete / shortened local name */
            uint8_t nlen = fld_len - 1;
            if(nlen > out_sz - 1) nlen = out_sz - 1;
            memcpy(out, &buf[pos + 2], nlen);
            out[nlen] = '\0';
            return nlen > 0;
        }
        pos += fld_len + 1;
    }
    return false;
}
