#include <assert.h>
#include <stdio.h>
#include "../../applications/main/nrf24/helpers/nrf24_font5x7.h"

int main(void) {
    /* space is blank */
    const uint8_t* sp = nrf24_font5x7_glyph(' ');
    assert(sp[0]==0 && sp[1]==0 && sp[2]==0 && sp[3]==0 && sp[4]==0);
    /* unknown char falls back to space */
    const uint8_t* q = nrf24_font5x7_glyph('~');
    assert(q[0]==0 && q[4]==0);
    /* a supported digit has at least one lit column */
    const uint8_t* z = nrf24_font5x7_glyph('0');
    int lit = 0; for(int i=0;i<5;i++) lit |= z[i];
    assert(lit != 0);

    /* draw clips safely and writes into buffer */
    uint16_t buf[8*8]; for(int i=0;i<64;i++) buf[i]=0;
    nrf24_font5x7_draw(buf, 8, 8, 0, 0, "0", 0xFFFF);
    int any = 0; for(int i=0;i<64;i++) if(buf[i]) any=1;
    assert(any == 1);
    /* off-screen draw does not crash / write */
    nrf24_font5x7_draw(buf, 8, 8, 100, 100, "0", 0xFFFF);

    printf("nrf24_font5x7: ALL PASS\n");
    return 0;
}
