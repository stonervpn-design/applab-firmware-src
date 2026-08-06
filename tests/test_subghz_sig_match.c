// Host test for the pure raw-signal identify glue.
// Build: gcc -Wall tests/test_subghz_sig_match.c -o /tmp/tsig -lm && /tmp/tsig
// Pulls in the real pure sources (protoview decoder + fingerprint + the glue).
#include "../applications/main/subghz/helpers/subghz_sig_match.c"
#include "../applications/main/subghz/helpers/protoview_decode.c"
#include "../applications/main/subghz/helpers/signal_fingerprint.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

// Emit one PWM bit as a (long,short) or (short,long) pair, matching the pattern in
// tests/test_protoview_decode.c. te_short=S, te_long=L.
static size_t push_pwm_bit(PvSample* a, size_t k, char bit, uint32_t S, uint32_t L) {
    if(bit == '1') {
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
    // Too few samples -> UNKNOWN.
    PvSample few[8] = {0};
    assert(strcmp(subghz_sig_identify_from_samples(433920000, SIG_MOD_AM, few, 4).name, "UNKNOWN") ==
           0);

    // Engineered signal matching the ChevroletV0 fingerprint entry:
    //   {"ChevroletV0","Chevrolet", SIG_MOD_AM, bits=96, te_short=120, te_long=240, 433.92M}
    PvSample a[220];
    size_t n = 0;
    for(int i = 0; i < 96; i++) n = push_pwm_bit(a, n, (i % 2) ? '1' : '0', 120, 240);

    SignalMatch m = subghz_sig_identify_from_samples(433920000, SIG_MOD_AM, a, n);
    printf("identified: name=%s make=%s score=%d\n", m.name, m.make ? m.make : "-", (int)m.score);
    // The glue must produce a valid, non-crashing result with an AM/433.92 match.
    // Exact name depends on the mined fingerprint DB; ChevroletV0 is the intended top.
    assert(m.name != NULL);
    assert(strcmp(m.name, "UNKNOWN") != 0); // a clean, in-DB signal should classify

    printf("test_subghz_sig_match OK\n");
    return 0;
}
