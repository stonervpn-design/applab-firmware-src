#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "../applications/main/ble_spam/tracker_follow.h"

static void test_thresholds(void) {
    uint32_t t = 1000000;
    assert(tracker_follow_state(t, t + 0,      t + 0)      == FollowNew);
    assert(tracker_follow_state(t, t + 59999,  t + 59999)  == FollowNew);
    assert(tracker_follow_state(t, t + 60000,  t + 60000)  == FollowPresent);
    assert(tracker_follow_state(t, t + 179999, t + 179999) == FollowPresent);
    assert(tracker_follow_state(t, t + 180000, t + 180000) == FollowSuspect);
    assert(tracker_follow_state(t, t + 299999, t + 299999) == FollowSuspect);
    assert(tracker_follow_state(t, t + 300000, t + 300000) == FollowFollowing);
    printf("test_thresholds OK\n");
}

static void test_stale(void) {
    uint32_t t = 1000000;
    uint32_t last = t + 600000;               /* 10 min dwell */
    assert(tracker_follow_state(t, last, last + 30000) == FollowNew);        /* stale */
    assert(tracker_follow_state(t, last, last + 29999) == FollowFollowing);  /* active */
    printf("test_stale OK\n");
}

static void test_wrap(void) {
    uint32_t first = 0xFFFFFF00u;
    uint32_t last = first + 300000u;          /* wraps past 2^32 */
    assert(tracker_follow_state(first, last, last) == FollowFollowing);
    printf("test_wrap OK\n");
}

static void test_labels(void) {
    assert(strcmp(tracker_follow_label(FollowNew), "NEW") == 0);
    assert(strcmp(tracker_follow_label(FollowPresent), "SEEN") == 0);
    assert(strcmp(tracker_follow_label(FollowSuspect), "SUSPECT") == 0);
    assert(strcmp(tracker_follow_label(FollowFollowing), "FOLLOW!") == 0);
    printf("test_labels OK\n");
}

static void test_name(void) {
    char out[24];
    uint8_t a1[] = {0x06, 0x09, 'H','e','l','l','o'};       /* complete name */
    assert(ble_adv_extract_local_name(a1, sizeof(a1), out, sizeof(out)) && !strcmp(out, "Hello"));
    uint8_t a2[] = {0x04, 0x08, 'A','B','C'};               /* shortened name */
    assert(ble_adv_extract_local_name(a2, sizeof(a2), out, sizeof(out)) && !strcmp(out, "ABC"));
    uint8_t a3[] = {0x02, 0x01, 0x06, 0x03, 0xFF, 0x4C, 0x00}; /* flags + mfg, no name */
    assert(!ble_adv_extract_local_name(a3, sizeof(a3), out, sizeof(out)));
    uint8_t a4[] = {0x09, 0x09, 'A','B','C','D','E','F','G','H'}; /* name longer than out */
    char small[4];
    assert(ble_adv_extract_local_name(a4, sizeof(a4), small, sizeof(small)) && !strcmp(small, "ABC"));
    printf("test_name OK\n");
}

int main(void) {
    test_thresholds(); test_stale(); test_wrap(); test_labels(); test_name();
    printf("ALL TRACKER FOLLOW TESTS PASSED\n");
    return 0;
}
