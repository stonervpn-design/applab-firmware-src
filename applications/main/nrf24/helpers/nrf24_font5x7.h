#pragma once
#include <stdint.h>

const uint8_t* nrf24_font5x7_glyph(char c);
void nrf24_font5x7_draw(uint16_t* buf, int buf_w, int buf_h, int x, int y,
                        const char* s, uint16_t color);
