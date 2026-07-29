#include "../applications/main/subghz/helpers/protoview_decode.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define S 350u
#define L 1050u

static size_t push_pwm_bit(PvSample* a, size_t k, char bit) {
    if (bit == '1') {
        a[k].level = 1;
        a[k].dur = L;
        a[k + 1].level = 0;
        a[k + 1].dur = S;
    } else {
        a[k].level = 1;
        a[k].dur = S;
        a[k + 1].level = 0;
        a[k + 1].dur = L;
    }
    return k + 2;
}

int main(void) {
    const char* want = "1011";
    PvSample a[64];
    size_t n = 0;

    for (const char* p = want; *p; ++p) {
        n = push_pwm_bit(a, n, *p);
    }

    // Test 1: Classifier finds correct thresholds
    PvThresholds th;
    assert(pv_classify_symbols(a, n, &th));
    printf("Classifier: t_short=%u, t_long=%u, t_sync=%u\n", th.t_short, th.t_long, th.t_sync);
    assert(th.t_short > 250 && th.t_short < 500);
    assert(th.t_long > 900 && th.t_long < 1200);

    // Test 2: PWM decoder
    char bits[64];
    int nb = pv_decode_pwm(a, n, &th, bits, sizeof(bits));
    printf("Decoded: %s (got %d bits, want %d bits)\n", bits, nb, (int)strlen(want));
    assert(nb == 4 && !strcmp(bits, want));

    // Test 3: Round-trip: bits -> raw -> same durations
    PvRawSymbol raw[64];
    size_t nr = pv_bits_to_raw(want, S, raw, 64);
    printf("Round-trip: got %zu raw symbols, expected %zu\n", nr, n);
    assert(nr == n);

    // Verify first two symbols (bit '1': long high, short low)
    printf("raw[0]: level=%u dur=%u (expect level=1 dur=%u)\n", raw[0].level, raw[0].dur, L);
    printf("raw[1]: level=%u dur=%u (expect level=0 dur=%u)\n", raw[1].level, raw[1].dur, S);
    assert(raw[0].level == 1 && raw[0].dur == L);
    assert(raw[1].level == 0 && raw[1].dur == S);

    printf("test_protoview_decode OK\n");
    return 0;
}
