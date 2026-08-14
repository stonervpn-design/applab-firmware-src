#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "../../applications/main/nrf24/helpers/nrf24_export.h"

int main(void) {
    uint8_t lv[3] = {0, 125, 10};
    char buf[128];
    int n = nrf24_export_format_csv(lv, 3, buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "0,2400,0\n"));
    assert(strstr(buf, "1,2401,125\n"));
    assert(strstr(buf, "2,2402,10\n"));
    /* overflow guarded */
    assert(nrf24_export_format_csv(lv, 3, buf, 4) == -1);
    /* settings validation */
    uint8_t m; uint16_t d;
    assert(nrf24_settings_validate(9, 500, &m, &d) && m == 9 && d == 500);
    assert(!nrf24_settings_validate(10, 500, &m, &d));   /* mode out of range (0..9) */
    assert(!nrf24_settings_validate(0, 99999, &m, &d));  /* dwell out of range */
    printf("nrf24_export: ALL PASS\n");
    return 0;
}
