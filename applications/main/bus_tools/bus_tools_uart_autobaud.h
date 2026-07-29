#ifndef BUS_TOOLS_UART_AUTOBAUD_H
#define BUS_TOOLS_UART_AUTOBAUD_H

#include <stdint.h>

/**
 * Estimate baud rate from timing deltas observed during serial reception.
 *
 * Typical use:
 * - Monitor bit transitions on the RX line
 * - Record the time deltas (in milliseconds) between consecutive transitions
 * - Pass the array and count to this function
 * - Returns the estimated standard baud rate, or 0 if uncertain
 *
 * Supported standard rates: 9600, 19200, 38400, 57600, 115200
 *
 * @param timing_deltas_ms Array of observed timing deltas in milliseconds
 * @param count Number of deltas in the array
 * @return Estimated baud rate (9600, 19200, 38400, 57600, 115200) or 0 if no match
 */
uint32_t bus_tools_uart_guess_baud(const uint16_t* timing_deltas_ms, uint8_t count);

#endif // BUS_TOOLS_UART_AUTOBAUD_H
