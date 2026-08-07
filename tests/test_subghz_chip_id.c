// Host test for the pure encoder-chip classifier.
// Build: gcc -Wall -Werror tests/test_subghz_chip_id.c -o /tmp/tc && /tmp/tc
// subghz_chip_id.c is wrapped in #ifndef PUBLIC_BUILD, so do NOT define
// PUBLIC_BUILD when building this test (the impl must be visible).
#include "../applications/main/subghz/helpers/subghz_chip_id.c"
#include <assert.h>
#include <string.h>
#include <stdio.h>

static SubGhzChipId id(const char* n, int32_t b) {
    return subghz_chip_identify(n, b, 433920000u, SIG_MOD_AM);
}

int main(void) {
    // Each known fixed-code family maps to its chip + is marked fixed.
    assert(id("Princeton", 24).family == SubGhzChipPrinceton);
    assert(id("Princeton", 24).fixed);
    assert(strlen(id("Princeton", 24).chip_label) > 0);
    assert(id("CAME", 12).family == SubGhzChipCame);
    assert(id("Nice FLO", 12).family == SubGhzChipNice);
    assert(id("Holtek HT12", 12).family == SubGhzChipHoltek);
    assert(id("Ansonic", 12).family == SubGhzChipAnsonic);
    assert(id("Chamberlain", 8).family == SubGhzChipChamberlain);

    // Linear Delta must win over the plain-Linear branch.
    assert(id("Linear Delta", 8).family == SubGhzChipLinearDelta);
    assert(id("Linear", 10).family == SubGhzChipLinear);

    // Unrecognized / rolling names -> Unknown, not fixed.
    assert(id("KeeLoq", 64).family == SubGhzChipUnknown);
    assert(!id("KeeLoq", 64).fixed);
    assert(id(NULL, 0).family == SubGhzChipUnknown);
    assert(!id(NULL, 0).fixed);

    // Bit count passes through untouched.
    assert(id("CAME", 12).total_bits == 12);
    assert(id("Princeton", 24).total_bits == 24);

    printf("test_subghz_chip_id OK\n");
    return 0;
}
