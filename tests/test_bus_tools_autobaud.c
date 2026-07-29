#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include "../applications/main/bus_tools/bus_tools_uart_autobaud.h"

void test_baud_9600() {
    printf("Test: UART autobaud detection at 9600...\n");

    // At 9600 baud, one bit ≈ 104 µs = 0.104 ms
    // Simulating a sync pattern (e.g., '\r' repeated) with bit transitions ~104 µs apart
    uint16_t deltas[10] = {104, 104, 104, 104, 104, 104, 104, 104, 104, 104};
    uint32_t baud = bus_tools_uart_guess_baud(deltas, 10);
    printf("  Input: 104 µs deltas (10 samples)\n");
    printf("  Result: %u baud\n", baud);
    assert(baud == 9600);
    printf("  PASS: 9600 baud\n\n");
}

void test_baud_19200() {
    printf("Test: UART autobaud detection at 19200...\n");

    // At 19200 baud, one bit ≈ 52 µs = 0.052 ms
    uint16_t deltas[10] = {52, 52, 52, 52, 52, 52, 52, 52, 52, 52};
    uint32_t baud = bus_tools_uart_guess_baud(deltas, 10);
    printf("  Input: 52 µs deltas (10 samples)\n");
    printf("  Result: %u baud\n", baud);
    assert(baud == 19200);
    printf("  PASS: 19200 baud\n\n");
}

void test_baud_38400() {
    printf("Test: UART autobaud detection at 38400...\n");

    // At 38400 baud, one bit ≈ 26 µs = 0.026 ms
    uint16_t deltas[10] = {26, 26, 26, 26, 26, 26, 26, 26, 26, 26};
    uint32_t baud = bus_tools_uart_guess_baud(deltas, 10);
    printf("  Input: 26 µs deltas (10 samples)\n");
    printf("  Result: %u baud\n", baud);
    assert(baud == 38400);
    printf("  PASS: 38400 baud\n\n");
}

void test_baud_57600() {
    printf("Test: UART autobaud detection at 57600...\n");

    // At 57600 baud, one bit ≈ 17.36 µs
    // Using 17 µs as a representative value
    uint16_t deltas[10] = {17, 17, 17, 17, 17, 17, 17, 17, 17, 17};
    uint32_t baud = bus_tools_uart_guess_baud(deltas, 10);
    printf("  Input: 17 µs deltas (10 samples)\n");
    printf("  Result: %u baud\n", baud);
    assert(baud == 57600);
    printf("  PASS: 57600 baud\n\n");
}

void test_baud_115200() {
    printf("Test: UART autobaud detection at 115200...\n");

    // At 115200 baud, one bit ≈ 8.68 µs
    // Using 8 µs as a representative value
    uint16_t deltas[10] = {8, 8, 8, 8, 8, 8, 8, 8, 8, 8};
    uint32_t baud = bus_tools_uart_guess_baud(deltas, 10);
    printf("  Input: 8 µs deltas (10 samples)\n");
    printf("  Result: %u baud\n", baud);
    assert(baud == 115200);
    printf("  PASS: 115200 baud\n\n");
}

void test_baud_with_noise() {
    printf("Test: UART autobaud with timing noise...\n");

    // Simulate 9600 baud with ±10% timing variation
    uint16_t deltas[10] = {94, 106, 102, 98, 110, 100, 96, 105, 99, 103};
    uint32_t baud = bus_tools_uart_guess_baud(deltas, 10);
    printf("  Input: 9600±10%% deltas with noise\n");
    printf("  Result: %u baud\n", baud);
    assert(baud == 9600);  // Should still detect 9600
    printf("  PASS: noise resilience\n\n");
}

void test_baud_edge_cases() {
    printf("Test: UART autobaud edge cases...\n");

    // Empty input
    uint32_t baud = bus_tools_uart_guess_baud(NULL, 0);
    assert(baud == 0);
    printf("  NULL input: returns 0\n");

    // Single delta (insufficient)
    uint16_t single[1] = {104};
    baud = bus_tools_uart_guess_baud(single, 1);
    assert(baud == 0);
    printf("  Single delta: returns 0\n");

    printf("  PASS: edge cases\n\n");
}

int main() {
    printf("=== BUS TOOLS UART AUTOBAUD TEST SUITE ===\n\n");

    test_baud_9600();
    test_baud_19200();
    test_baud_38400();
    test_baud_57600();
    test_baud_115200();
    test_baud_with_noise();
    test_baud_edge_cases();

    printf("=== ALL BUS TOOLS AUTO-BAUD TESTS PASSED ===\n");
    return 0;
}
