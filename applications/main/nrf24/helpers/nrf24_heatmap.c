#include "nrf24_heatmap.h"
#include <string.h>

static uint16_t pack565(int r, int g, int b) { /* r,g,b in 0..255 */
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void nrf24_heat_build_lut(uint16_t lut[256]) {
    /* 4 segments: blue->cyan->green->yellow->red, 64 entries each */
    for(int i = 0; i < 256; i++) {
        int r, g, b;
        if(i < 64)      { r = 0;            g = i * 4;        b = 255; }       /* blue->cyan */
        else if(i < 128){ r = 0;            g = 255;          b = 255 - (i - 64) * 4; } /* cyan->green */
        else if(i < 192){ r = (i - 128) * 4; g = 255;         b = 0; }        /* green->yellow */
        else            { r = 255;          g = 255 - (i - 192) * 4; b = 0; } /* yellow->red */
        if(g > 255) g = 255;
        lut[i] = pack565(r, g, b);
    }
}

uint16_t nrf24_heat_color(const uint16_t lut[256], uint8_t intensity_0_100) {
    int idx = (intensity_0_100 * 255) / 100;
    if(idx < 0) idx = 0;
    if(idx > 255) idx = 255;
    return lut[idx];
}

void nrf24_wf_init(Nrf24Waterfall* wf, uint8_t* backing, int width, int height) {
    wf->rows = backing;
    wf->width = width;
    wf->height = height;
    wf->head = -1;
    memset(backing, 0, (size_t)width * height);
}

void nrf24_wf_push(Nrf24Waterfall* wf, const uint8_t* row) {
    wf->head = (wf->head + 1) % wf->height;
    memcpy(wf->rows + (size_t)wf->head * wf->width, row, wf->width);
}

const uint8_t* nrf24_wf_row(const Nrf24Waterfall* wf, int age) {
    int idx = ((wf->head - age) % wf->height + wf->height) % wf->height;
    return wf->rows + (size_t)idx * wf->width;
}
