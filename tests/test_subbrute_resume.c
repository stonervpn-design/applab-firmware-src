// Host test for the pure resumable-brute validator.
// Build: gcc -Wall -Werror tests/test_subbrute_resume.c -o /tmp/tr && /tmp/tr
#include "../applications/main/subghz/helpers/subbrute_resume.c"
#include <assert.h>
#include <stdio.h>

static SubBruteResume mk(uint32_t schema, uint32_t attack) {
    SubBruteResume r = {0};
    r.schema_version = schema;
    r.attack = attack;
    r.current_step = 12345; // a mid-sweep position
    r.extra_repeats = 2;
    return r;
}

int main(void) {
    // Valid: schema 1, first fixed-code attack (index 0 = CAME12bit303).
    SubBruteResume ok0 = mk(SUBBRUTE_RESUME_SCHEMA, 0);
    assert(subbrute_resume_is_valid(&ok0));

    // Valid: last normal attack before DeBruijn (index 42).
    SubBruteResume ok42 = mk(SUBBRUTE_RESUME_SCHEMA, 42);
    assert(subbrute_resume_is_valid(&ok42));

    // Wrong schema version -> rejected (stale file after a schema bump).
    SubBruteResume bad_schema = mk(SUBBRUTE_RESUME_SCHEMA + 1u, 0);
    assert(!subbrute_resume_is_valid(&bad_schema));

    // Out of range: == total, and well beyond.
    SubBruteResume at_total = mk(SUBBRUTE_RESUME_SCHEMA, SUBBRUTE_RESUME_ATTACK_TOTAL);
    assert(!subbrute_resume_is_valid(&at_total));
    SubBruteResume way_over = mk(SUBBRUTE_RESUME_SCHEMA, 99);
    assert(!subbrute_resume_is_valid(&way_over));

    // DeBruijn (43): single continuous burst, not resumable.
    SubBruteResume debruijn = mk(SUBBRUTE_RESUME_SCHEMA, SUBBRUTE_RESUME_ATTACK_DEBRUIJN);
    assert(!subbrute_resume_is_valid(&debruijn));

    // LoadFile (44): deferred to phase 2.
    SubBruteResume loadfile = mk(SUBBRUTE_RESUME_SCHEMA, SUBBRUTE_RESUME_ATTACK_LOADFILE);
    assert(!subbrute_resume_is_valid(&loadfile));

    // NULL.
    assert(!subbrute_resume_is_valid(NULL));

    printf("test_subbrute_resume OK\n");
    return 0;
}
