// Host test for the pure capture-quality scorer.
// Build: gcc -Wall -Werror tests/test_subghz_capture_quality.c -o /tmp/tq && /tmp/tq
#include "../applications/main/subghz/helpers/subghz_capture_quality.c"
#include <assert.h>
#include <stdio.h>

int main(void) {
    // Below the trigger -> Idle (even if a decode was recent).
    assert(subghz_capture_quality_eval(-100.f, -90.f, false).level == SubGhzQualityIdle);
    assert(subghz_capture_quality_eval(-100.f, -90.f, true).level == SubGhzQualityIdle);

    // Above trigger + decodes landing -> Good.
    assert(subghz_capture_quality_eval(-50.f, -90.f, true).level == SubGhzQualityGood);

    // Just above trigger (margin 4 < 6), no decode -> Weak, with a hint.
    SubGhzCaptureQuality w = subghz_capture_quality_eval(-86.f, -90.f, false);
    assert(w.level == SubGhzQualityWeak);
    assert(w.hint[0] != '\0');

    // Strong signal but nothing decoding -> No decode, with a hint.
    SubGhzCaptureQuality nd = subghz_capture_quality_eval(-50.f, -90.f, false);
    assert(nd.level == SubGhzQualityNoDecode);
    assert(nd.hint[0] != '\0');

    printf("test_subghz_capture_quality OK\n");
    return 0;
}
