#include <assert.h>
#include <stdio.h>
#include "../../applications/main/nrf24/helpers/nrf24_chanmap.h"

int main(void) {
    assert(nrf24_ch_to_mhz(0) == 2400);
    assert(nrf24_ch_to_mhz(125) == 2525);

    /* Wi-Fi centers: ch1=2412->12, ch6=2437->37, ch11=2462->62, ch13=2472->72, ch14=2484->84 */
    assert(nrf24_wifi_ch_to_nrf(1) == 12);
    assert(nrf24_wifi_ch_to_nrf(6) == 37);
    assert(nrf24_wifi_ch_to_nrf(11) == 62);
    assert(nrf24_wifi_ch_to_nrf(13) == 72);
    assert(nrf24_wifi_ch_to_nrf(14) == 84);
    assert(nrf24_wifi_ch_to_nrf(0) == -1);
    assert(nrf24_wifi_ch_to_nrf(15) == -1);

    /* Wi-Fi span = center +-11 clamped */
    uint8_t lo, hi;
    nrf24_wifi_span(6, &lo, &hi);   assert(lo == 26 && hi == 48);
    nrf24_wifi_span(1, &lo, &hi);   assert(lo == 1  && hi == 23);   /* 12-11=1 */
    nrf24_wifi_span(99, &lo, &hi);  assert(lo > hi);                /* invalid -> empty */

    /* BLE adv */
    assert(nrf24_ble_adv_to_nrf(37) == 2);
    assert(nrf24_ble_adv_to_nrf(38) == 26);
    assert(nrf24_ble_adv_to_nrf(39) == 80);
    assert(nrf24_ble_adv_to_nrf(0) == -1);

    /* dBm -> percent */
    assert(nrf24_dbm_to_pct(-100) == 0);
    assert(nrf24_dbm_to_pct(-95) == 0);
    assert(nrf24_dbm_to_pct(-35) == 100);
    assert(nrf24_dbm_to_pct(-20) == 100);
    assert(nrf24_dbm_to_pct(-65) == 50);   /* midpoint */

    /* byte swap */
    assert(nrf24_rgb565_swap(0x1234) == 0x3412);

    printf("nrf24_chanmap: ALL PASS\n");
    return 0;
}
