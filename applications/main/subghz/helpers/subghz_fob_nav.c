#include "subghz_fob_nav.h"
uint8_t subghz_fob_nav_move(uint8_t idx, uint8_t key) {
    if(idx > 3) idx = 0;
    uint8_t row = idx / 2, col = idx % 2;
    switch(key) {
    case FOB_LEFT:
    case FOB_RIGHT: col ^= 1; break;         // 2 cols: toggle
    case FOB_UP:
    case FOB_DOWN:  row ^= 1; break;          // 2 rows: toggle
    case FOB_CYCLE: return (uint8_t)((idx + 1) & 0x3);
    default: break;
    }
    return (uint8_t)(row * 2 + col);
}
