#pragma once
#include <stdint.h>

void nrf24_heat_build_lut(uint16_t lut[256]);
uint16_t nrf24_heat_color(const uint16_t lut[256], uint8_t intensity_0_100);

typedef struct {
    uint8_t* rows;   /* height * width bytes */
    int width;
    int height;
    int head;        /* index of most-recently-written row */
} Nrf24Waterfall;

void nrf24_wf_init(Nrf24Waterfall* wf, uint8_t* backing, int width, int height);
void nrf24_wf_push(Nrf24Waterfall* wf, const uint8_t* row);
const uint8_t* nrf24_wf_row(const Nrf24Waterfall* wf, int age);
