// Host unit test for the pure RSSI OOK/FSK classifier.
// Build: gcc -Wall -Werror tests/test_subghz_mod_classify.c -o /tmp/tmod && /tmp/tmod
// Includes the real source (no furi_hal), mirroring tests/test_hitag2_cipher.c.
#include "../applications/main/subghz/helpers/subghz_mod_classify.c"
#include <assert.h>
#include <stdio.h>

int main(void) {
    // OOK: bimodal, carrier keyed on/off -> big swing between floor and peak.
    int8_t ook[200];
    for(int i = 0; i < 200; i++) ook[i] = (i % 2) ? -40 : -100;
    SubGhzModDetect a = subghz_mod_classify(ook, 200);
    assert(a.mod == SIG_MOD_AM);
    assert(a.confidence > 0);

    // FSK: steady near peak (carrier always on), tiny swing, high level.
    int8_t fsk[200];
    for(int i = 0; i < 200; i++) fsk[i] = (int8_t)(-45 + (i % 3 - 1)); // -46..-44
    SubGhzModDetect b = subghz_mod_classify(fsk, 200);
    assert(b.mod == SIG_MOD_FM);

    // Noise only: steady but low level -> no real signal present.
    int8_t noise[200];
    for(int i = 0; i < 200; i++) noise[i] = (int8_t)(-100 + (i % 2)); // -100..-99
    SubGhzModDetect c = subghz_mod_classify(noise, 200);
    assert(c.mod == SIG_MOD_UNK);

    // Too few samples -> UNK.
    int8_t few[8] = {-40, -100, -40, -100, -40, -100, -40, -100};
    assert(subghz_mod_classify(few, 8).mod == SIG_MOD_UNK);

    printf("test_subghz_mod_classify OK\n");
    return 0;
}
