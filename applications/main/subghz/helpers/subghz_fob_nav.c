#include "subghz_fob_nav.h"

uint8_t subghz_fob_nav_move_n(uint8_t idx, uint8_t key, uint8_t n) {
    if(n == 0) return 0;
    if(n >= 4) {
        // 2x2 spatial grid (KeeLoq, 4 tiles) — legacy behavior preserved exactly.
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
    // Linear step for <4 tiles (VAG, 3 tiles): prev / next / cycle, modulo n.
    if(idx >= n) idx %= n;
    switch(key) {
    case FOB_LEFT:
    case FOB_UP:    return (uint8_t)((idx + n - 1) % n);
    case FOB_RIGHT:
    case FOB_DOWN:
    case FOB_CYCLE: return (uint8_t)((idx + 1) % n);
    default:        return idx;
    }
}

uint8_t subghz_fob_nav_move(uint8_t idx, uint8_t key) {
    return subghz_fob_nav_move_n(idx, key, 4);
}
