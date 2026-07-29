#include "bus_tools_uart_autobaud.h"

uint32_t bus_tools_uart_guess_baud(const uint16_t* timing_deltas_ms, uint8_t count) {
    if(!timing_deltas_ms || count < 2) {
        return 0;
    }

    // Calculate average delta (input is in 100µs units for practical measurements)
    uint32_t sum = 0;
    for(uint8_t i = 0; i < count; i++) {
        sum += timing_deltas_ms[i];
    }
    uint32_t avg_us = sum / count;  // Average timing delta in same units as input

    /*
     * Baud rate vs bit duration:
     * 9600 baud   = 1/9600 s/bit ≈ 104 µs/bit
     * 19200 baud  = 1/19200 s/bit ≈ 52 µs/bit
     * 38400 baud  = 1/38400 s/bit ≈ 26 µs/bit
     * 57600 baud  = 1/57600 s/bit ≈ 17.36 µs/bit
     * 115200 baud = 1/115200 s/bit ≈ 8.68 µs/bit
     *
     * Heuristic: use threshold midpoints between standard rates
     */

    // Thresholds with tolerance for observed variation
    if(avg_us > 70) {
        return 9600;   // > 70 µs suggests 9600
    }
    if(avg_us > 40) {
        return 19200;  // 40-70 µs suggests 19200
    }
    if(avg_us > 22) {
        return 38400;  // 22-40 µs suggests 38400
    }
    if(avg_us > 13) {
        return 57600;  // 13-22 µs suggests 57600
    }
    // < 13 µs suggests 115200
    return 115200;
}
