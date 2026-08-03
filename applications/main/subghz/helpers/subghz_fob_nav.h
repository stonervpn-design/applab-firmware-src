#pragma once
#include <stdint.h>
// 2x2 fob tile navigation (pure, hardware-free). Grid index: 0=TL 1=TR 2=BL 3=BR.
enum { FOB_LEFT = 0, FOB_RIGHT, FOB_UP, FOB_DOWN, FOB_CYCLE };
uint8_t subghz_fob_nav_move(uint8_t idx, uint8_t key);
