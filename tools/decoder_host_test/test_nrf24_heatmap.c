#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../applications/main/nrf24/helpers/nrf24_heatmap.h"

static void rgb(uint16_t c, int* r, int* g, int* b) {
    *r = (c >> 11) & 0x1F; *g = (c >> 5) & 0x3F; *b = c & 0x1F;
}

int main(void) {
    uint16_t lut[256];
    nrf24_heat_build_lut(lut);
    int r0,g0,b0, r255,g255,b255;
    rgb(lut[0], &r0,&g0,&b0);
    rgb(lut[255], &r255,&g255,&b255);
    /* cold end is blue-dominant, hot end is red-dominant */
    assert(b0 > r0);
    assert(r255 > b255);
    /* intensity mapping picks lut ends */
    assert(nrf24_heat_color(lut, 0)   == lut[0]);
    assert(nrf24_heat_color(lut, 100) == lut[255]);

    /* waterfall ring: newest-first ordering */
    uint8_t backing[3 * 4];
    Nrf24Waterfall wf;
    nrf24_wf_init(&wf, backing, 4, 3);
    uint8_t a[4] = {1,1,1,1}, b[4] = {2,2,2,2}, c[4] = {3,3,3,3};
    nrf24_wf_push(&wf, a);
    nrf24_wf_push(&wf, b);
    nrf24_wf_push(&wf, c);
    assert(nrf24_wf_row(&wf, 0)[0] == 3); /* newest */
    assert(nrf24_wf_row(&wf, 1)[0] == 2);
    assert(nrf24_wf_row(&wf, 2)[0] == 1);
    uint8_t d[4] = {4,4,4,4};
    nrf24_wf_push(&wf, d);                 /* evicts oldest (a) */
    assert(nrf24_wf_row(&wf, 0)[0] == 4);
    assert(nrf24_wf_row(&wf, 2)[0] == 2);

    printf("nrf24_heatmap: ALL PASS\n");
    return 0;
}
