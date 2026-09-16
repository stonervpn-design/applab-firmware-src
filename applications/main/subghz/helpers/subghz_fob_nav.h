#pragma once
#include <stdint.h>
// Fob tile navigation (pure, hardware-free). 2x2 grid index: 0=TL 1=TR 2=BL 3=BR.
enum { FOB_LEFT = 0, FOB_RIGHT, FOB_UP, FOB_DOWN, FOB_CYCLE, FOB_CYCLE_REV };
// N-tile navigation. n>=4 keeps the exact 2x2 spatial behavior (KeeLoq); n<4 steps
// linearly (LEFT/UP = prev, RIGHT/DOWN/CYCLE = next) modulo n. Result always < n.
uint8_t subghz_fob_nav_move_n(uint8_t idx, uint8_t key, uint8_t n);
// Back-compat 2x2 wrapper (== subghz_fob_nav_move_n(idx, key, 4)).
uint8_t subghz_fob_nav_move(uint8_t idx, uint8_t key);
