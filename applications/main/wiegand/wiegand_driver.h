#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * Start capturing Wiegand pulses on D0/D1 GPIO pins.
 * Polls the pins and captures pulse sequences into an internal buffer.
 * @param gpio_d0 GPIO number for D0 line
 * @param gpio_d1 GPIO number for D1 line
 * @param timeout_ms Timeout in milliseconds (0 = no timeout)
 * @return true on success, false if already capturing or pin conflict
 */
bool wiegand_driver_start_capture(uint8_t gpio_d0, uint8_t gpio_d1, uint32_t timeout_ms);

/**
 * Read captured pulse bits.
 * Copies the captured bits into out_bits (1 bit per byte).
 * @param out_bits Output buffer for bits
 * @param max_bits Maximum bits to read
 * @return Number of bits captured (0 if still capturing), -1 on error
 */
int32_t wiegand_driver_read_capture(uint8_t* out_bits, uint8_t max_bits);

/**
 * Stop capture and release GPIO pins.
 */
void wiegand_driver_stop_capture(void);

/**
 * Replay a Wiegand pulse sequence on D0/D1 lines.
 * Drives the GPIO lines with standard Wiegand pulse timing (125µs per pulse).
 * @param gpio_d0 GPIO number for D0 line
 * @param gpio_d1 GPIO number for D1 line
 * @param pulse_bits Array of bits to send (1 bit per byte)
 * @param bit_count Number of bits to send
 * @return true on success, false on error
 */
bool wiegand_driver_replay(uint8_t gpio_d0, uint8_t gpio_d1, const uint8_t* pulse_bits, uint8_t bit_count);
