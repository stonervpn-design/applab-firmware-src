// Host test for the pure panadapter math.
// Build: gcc -Wall -Werror tests/test_subghz_spectrum.c \
//   applications/main/subghz/helpers/subghz_spectrum_util.c -o /tmp/ts && /tmp/ts
#include "../applications/main/subghz/helpers/subghz_spectrum_util.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    // bin_freq
    assert(spectrum_bin_freq(433000000u, 13600u, 0) == 433000000u);
    assert(spectrum_bin_freq(433000000u, 13600u, 10) == 433136000u);
    assert(spectrum_bin_freq(315000000u, 20000u, 5) == 315100000u);

    // rssi_q: clamp + endpoints
    assert(spectrum_rssi_q(-97.0f, 26) == 0);
    assert(spectrum_rssi_q(-120.0f, 26) == 0); // below floor
    assert(spectrum_rssi_q(-60.0f, 26) == 26);
    assert(spectrum_rssi_q(-40.0f, 26) == 26); // above ceil

    // midpoint (-78.5) -> ~half of 26 = 13
    uint8_t mid = spectrum_rssi_q(-78.5f, 26);
    assert(mid >= 12 && mid <= 14);

    // monotonic
    assert(spectrum_rssi_q(-90.0f, 26) < spectrum_rssi_q(-70.0f, 26));

    printf("test_subghz_spectrum OK\n");
    return 0;
}
