// Host test for the pure K-of-N multi-frame aggregator.
// Build: gcc -Wall -Werror tests/test_subghz_sig_aggregate.c -o /tmp/ta && /tmp/ta
#include "../applications/main/subghz/helpers/subghz_sig_aggregate.c"
#include <assert.h>
#include <string.h>
#include <stdio.h>

static SignalMatch mk(const char* name, const char* make, int32_t score) {
    SignalMatch m = {0};
    m.name = name;
    m.make = make;
    m.score = score;
    return m;
}

int main(void) {
    // All 3 agree -> that name, reported via its best-scoring frame.
    SignalMatch a[3] = {mk("Kia V7", "Kia", 60), mk("Kia V7", "Kia", 75), mk("Kia V7", "Kia", 55)};
    SignalMatch r = subghz_sig_aggregate(a, 3, 2, 40);
    assert(strcmp(r.name, "Kia V7") == 0 && r.score == 75);

    // 2 of 3 agree -> winner (>= 2 votes), best of the agreeing pair.
    SignalMatch b[3] = {mk("Kia V7", "Kia", 60), mk("Ford V0", "Ford", 50), mk("Kia V7", "Kia", 65)};
    r = subghz_sig_aggregate(b, 3, 2, 40);
    assert(strcmp(r.name, "Kia V7") == 0 && r.score == 65);

    // All distinct -> no majority -> UNKNOWN.
    SignalMatch c[3] = {mk("Kia V7", "Kia", 60), mk("Ford V0", "Ford", 60), mk("Honda", "Honda", 60)};
    r = subghz_sig_aggregate(c, 3, 2, 40);
    assert(strcmp(r.name, "UNKNOWN") == 0);

    // Below the score floor -> not counted -> UNKNOWN.
    SignalMatch d[3] = {mk("Kia V7", "Kia", 30), mk("Kia V7", "Kia", 35), mk("Kia V7", "Kia", 20)};
    r = subghz_sig_aggregate(d, 3, 2, 40);
    assert(strcmp(r.name, "UNKNOWN") == 0);

    // 2 valid + 1 UNKNOWN -> winner from the valid pair.
    SignalMatch e[3] = {mk("Kia V7", "Kia", 60), mk("UNKNOWN", NULL, 0), mk("Kia V7", "Kia", 70)};
    r = subghz_sig_aggregate(e, 3, 2, 40);
    assert(strcmp(r.name, "Kia V7") == 0 && r.score == 70);

    // Empty / null -> UNKNOWN.
    r = subghz_sig_aggregate(NULL, 0, 2, 40);
    assert(strcmp(r.name, "UNKNOWN") == 0);

    printf("test_subghz_sig_aggregate OK\n");
    return 0;
}
