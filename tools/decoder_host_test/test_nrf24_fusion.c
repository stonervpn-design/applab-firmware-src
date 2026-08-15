#include <assert.h>
#include <stdio.h>
#include "../../applications/main/nrf24/helpers/nrf24_fusion_model.h"

int main(void) {
    /* fuse = max(occ, pct(wifi), pct(ble)); -128 = ignore */
    assert(nrf24_fuse_intensity(30, -128, -128) == 30);
    assert(nrf24_fuse_intensity(30, -35, -128) == 100); /* strong wifi wins */
    assert(nrf24_fuse_intensity(80, -95, -128) == 80);  /* weak wifi -> occ wins */
    assert(nrf24_fuse_intensity(0, -128, -65) == 50);   /* ble midpoint */

    /* classify: wifi > ble > hopper(occ>=thr) > none */
    assert(nrf24_classify(10, -40, -128, 40) == SRC_WIFI);
    assert(nrf24_classify(10, -128, -50, 40) == SRC_BLE);
    assert(nrf24_classify(70, -128, -128, 40) == SRC_HOPPER);
    assert(nrf24_classify(10, -128, -128, 40) == SRC_NONE);
    /* wifi beats ble when both present */
    assert(nrf24_classify(10, -40, -50, 40) == SRC_WIFI);

    printf("nrf24_fusion: ALL PASS\n");
    return 0;
}
