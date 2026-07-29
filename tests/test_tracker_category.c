#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "../applications/main/ble_spam/tracker_category.h"

static void test_everything(void) {
    assert(tracker_category_matches(TrackerCatEverything, TrackerKindAirTag));
    assert(tracker_category_matches(TrackerCatEverything, TrackerKindMetaGlasses));
    assert(tracker_category_matches(TrackerCatEverything, TrackerKindDroneRID));
    assert(!tracker_category_matches(TrackerCatEverything, TrackerKindUnknown));
    printf("test_everything OK\n");
}
static void test_apple(void) {
    assert(tracker_category_matches(TrackerCatApple, TrackerKindAirTag));
    assert(tracker_category_matches(TrackerCatApple, TrackerKindApplePP));
    assert(tracker_category_matches(TrackerCatApple, TrackerKindAppleNearby));
    assert(!tracker_category_matches(TrackerCatApple, TrackerKindSmartTag));
    assert(!tracker_category_matches(TrackerCatApple, TrackerKindMetaGlasses));
    printf("test_apple OK\n");
}
static void test_singles(void) {
    assert(tracker_category_matches(TrackerCatSamsung, TrackerKindSmartTag));
    assert(!tracker_category_matches(TrackerCatSamsung, TrackerKindTile));
    assert(tracker_category_matches(TrackerCatTile, TrackerKindTile));
    assert(tracker_category_matches(TrackerCatMeta, TrackerKindMetaGlasses));
    assert(!tracker_category_matches(TrackerCatMeta, TrackerKindAppleNearby));
    assert(tracker_category_matches(TrackerCatDrone, TrackerKindDroneRID));
    assert(!tracker_category_matches(TrackerCatDrone, TrackerKindTile));
    printf("test_singles OK\n");
}
static void test_sweep(void) {
    assert(tracker_category_matches(TrackerCatBugSweep, TrackerKindUnknown));
    assert(!tracker_category_matches(TrackerCatBugSweep, TrackerKindAirTag));
    assert(tracker_category_uses_sweep(TrackerCatBugSweep));
    assert(!tracker_category_uses_sweep(TrackerCatEverything));
    assert(!tracker_category_uses_sweep(TrackerCatMeta));
    printf("test_sweep OK\n");
}
static void test_labels(void) {
    assert(strcmp(tracker_category_label(TrackerCatEverything), "Everything") == 0);
    assert(strcmp(tracker_category_label(TrackerCatApple), "Apple") == 0);
    assert(strcmp(tracker_category_label(TrackerCatMeta), "Meta Glasses") == 0);
    assert(strcmp(tracker_category_label(TrackerCatBugSweep), "Bug Sweep") == 0);
    printf("test_labels OK\n");
}
int main(void) {
    test_everything(); test_apple(); test_singles(); test_sweep(); test_labels();
    printf("ALL TRACKER CATEGORY TESTS PASSED\n");
    return 0;
}
