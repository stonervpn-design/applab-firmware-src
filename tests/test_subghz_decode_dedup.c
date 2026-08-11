// tests/test_subghz_decode_dedup.c
#include "../applications/main/subghz/helpers/subghz_decode_dedup.c"
#include <assert.h>
#include <stdio.h>

int main(void) {
    SubGhzDecodeDedup d; subghz_decode_dedup_reset(&d);
    assert(d.last_history_idx == -1);

    // tier ordering
    assert(subghz_decode_dedup_score("Hyundai TL", 0) >
           subghz_decode_dedup_score("CAME", 999));
    assert(subghz_decode_dedup_score("CAME", 999) >
           subghz_decode_dedup_score("Jeep V0", 999));

    // 1) Hyundai then jeep in-window -> suppress
    uint16_t sh = subghz_decode_dedup_score("Hyundai TL", 40);
    assert(subghz_decode_dedup_decide(&d, "Hyundai TL", sh, 1000) == SubGhzDedupAdd);
    subghz_decode_dedup_commit(&d, "Hyundai TL", sh, 1000, 0);
    uint16_t sj = subghz_decode_dedup_score("Jeep V0", 24);
    assert(subghz_decode_dedup_decide(&d, "Jeep V0", sj, 1050) == SubGhzDedupSuppress);

    // 2) jeep then Hyundai in-window -> replace
    subghz_decode_dedup_reset(&d);
    assert(subghz_decode_dedup_decide(&d, "Jeep V0", sj, 2000) == SubGhzDedupAdd);
    subghz_decode_dedup_commit(&d, "Jeep V0", sj, 2000, 5);
    assert(subghz_decode_dedup_decide(&d, "Hyundai TL", sh, 2040) == SubGhzDedupReplace);

    // 3) two low in-window -> suppress
    subghz_decode_dedup_reset(&d);
    assert(subghz_decode_dedup_decide(&d, "Jeep V0", sj, 100) == SubGhzDedupAdd);
    subghz_decode_dedup_commit(&d, "Jeep V0", sj, 100, 1);
    uint16_t shit = subghz_decode_dedup_score("Hitag2", 24);
    assert(subghz_decode_dedup_decide(&d, "Hitag2", shit, 120) == SubGhzDedupSuppress);

    // 4) repeat outside window -> add
    subghz_decode_dedup_reset(&d);
    assert(subghz_decode_dedup_decide(&d, "Hyundai TL", sh, 500) == SubGhzDedupAdd);
    subghz_decode_dedup_commit(&d, "Hyundai TL", sh, 500, 2);
    assert(subghz_decode_dedup_decide(&d, "Hyundai TL", sh, 500 + SUBGHZ_DEDUP_WINDOW_MS + 1)
           == SubGhzDedupAdd);

    printf("ALL C DEDUP TESTS PASSED\n");
    return 0;
}
