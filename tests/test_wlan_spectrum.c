#include "../applications/main/wlan_app/wlan_spectrum.c"
#include <assert.h>
#include <stdio.h>

int main(void) {
    WlanSpectrum s;
    wlan_spectrum_reset(&s);
    assert(s.filled == 0);
    assert(wlan_spectrum_live_level(&s, 1) == 0);

    // idle channel → 0; busy strong channel → high
    wlan_spectrum_set_dwell(&s, 1, 0, -128);
    wlan_spectrum_set_dwell(&s, 6, 40, -50);   // busy + strong
    assert(wlan_spectrum_live_level(&s, 1) == 0);
    uint8_t l6 = wlan_spectrum_live_level(&s, 6);
    assert(l6 > 10 && l6 <= 15);

    wlan_spectrum_commit_sweep(&s);
    assert(s.filled == 1);
    assert(wlan_spectrum_cell(&s, 0, 1) == 0);
    assert(wlan_spectrum_cell(&s, 0, 6) == l6);
    // sweep accumulators cleared after commit
    assert(wlan_spectrum_live_level(&s, 6) == 0);

    // ring wraps: push > SPEC_ROWS sweeps, newest stays at row 0, filled caps
    for (int i = 0; i < SPEC_ROWS + 10; i++) {
        wlan_spectrum_set_dwell(&s, 3, (uint16_t)(i % 20), -70);
        wlan_spectrum_commit_sweep(&s);
    }
    assert(s.filled == SPEC_ROWS);
    // out-of-range channel/row are safe (return 0)
    assert(wlan_spectrum_cell(&s, 200, 3) == 0);
    assert(wlan_spectrum_live_level(&s, 99) == 0);

    printf("test_wlan_spectrum OK\n");
    return 0;
}
