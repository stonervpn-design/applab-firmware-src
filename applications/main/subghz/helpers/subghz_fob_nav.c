#include "subghz_fob_nav.h"

uint8_t subghz_fob_nav_move_n(uint8_t idx, uint8_t key, uint8_t n) {
    if(n == 0) return 0;
    if(idx >= n) idx = 0;
    if(n >= 4) {
        // 2-column grid, ceil(n/2) rows (0=TL 1=TR 2=... down the two columns).
        // For n==4 this reduces to the legacy 2x2 spatial nav BYTE-IDENTICALLY
        // (2 rows: row^=1, 2 cols: col^=1, cycle (idx+1)&3); n=5..8 extend it to
        // 3-4 rows, clamping moves that would land past the (possibly short) last
        // row back onto a valid tile so the result is always < n.
        uint8_t rows = (uint8_t)((n + 1) / 2);
        uint8_t row = idx / 2, col = idx % 2;
        switch(key) {
        case FOB_LEFT:
        case FOB_RIGHT: {
            uint8_t t = (uint8_t)(row * 2 + (col ^ 1)); // toggle column
            if(t < n) idx = t; // else no horizontal neighbor (short row): stay put
            break;
        }
        case FOB_UP: {
            uint8_t r = (uint8_t)((row + rows - 1) % rows); // prev row, wrap
            uint8_t t = (uint8_t)(r * 2 + col);
            idx = (t < n) ? t : (uint8_t)(n - 1); // clamp into short last row
            break;
        }
        case FOB_DOWN: {
            uint8_t r = (uint8_t)((row + 1) % rows); // next row, wrap
            uint8_t t = (uint8_t)(r * 2 + col);
            idx = (t < n) ? t : (uint8_t)(n - 1); // clamp into short last row
            break;
        }
        case FOB_CYCLE: idx = (uint8_t)((idx + 1) % n); break;
        case FOB_CYCLE_REV: idx = (uint8_t)((idx + n - 1) % n); break;
        default: break;
        }
        return idx;
    }
    // Linear step for <4 tiles (VAG, 3 tiles): prev / next / cycle, modulo n.
    // idx is already < n here (normalized at the top), so no re-clamp is needed.
    switch(key) {
    case FOB_LEFT:
    case FOB_UP:    return (uint8_t)((idx + n - 1) % n);
    case FOB_RIGHT:
    case FOB_DOWN:
    case FOB_CYCLE: return (uint8_t)((idx + 1) % n);
    case FOB_CYCLE_REV: return (uint8_t)((idx + n - 1) % n);
    default:        return idx;
    }
}

uint8_t subghz_fob_nav_move(uint8_t idx, uint8_t key) {
    return subghz_fob_nav_move_n(idx, key, 4);
}
