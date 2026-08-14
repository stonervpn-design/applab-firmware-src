#include <assert.h>
#include <stdio.h>
#include "../../applications/main/nrf24/helpers/nrf24_spectrum_calc.h"

int main(void) {
    /* max-hold keeps the peak */
    assert(nrf24_maxhold_update(10, 40) == 40);
    assert(nrf24_maxhold_update(40, 10) == 40);
    /* channel -> x on a 128px canvas of 80 channels */
    assert(nrf24_channel_to_x(0, 128, 80) == 0);
    assert(nrf24_channel_to_x(40, 128, 80) == 64);
    /* markers */
    assert(nrf24_channel_is_ble_marker(2) && nrf24_channel_is_ble_marker(26) && nrf24_channel_is_ble_marker(80));
    assert(!nrf24_channel_is_ble_marker(3));
    assert(nrf24_channel_is_wifi_marker(12) && nrf24_channel_is_wifi_marker(37) && nrf24_channel_is_wifi_marker(62));
    assert(!nrf24_channel_is_wifi_marker(13));
    /* MHz + dwell cycle */
    assert(nrf24_channel_to_mhz(0) == 2400);
    assert(nrf24_channel_to_mhz(80) == 2480);
    assert(nrf24_dwell_presets[0] == 130 && nrf24_dwell_presets[4] == 2000);
    assert(nrf24_dwell_next(4) == 0 && nrf24_dwell_next(0) == 1);
    printf("nrf24_spectrum_calc: ALL PASS\n");
    return 0;
}
