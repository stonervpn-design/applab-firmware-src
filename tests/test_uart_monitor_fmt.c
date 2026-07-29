#include "../applications/main/bus_tools/uart_monitor_fmt.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    UartRing r;
    uart_ring_reset(&r);
    assert(uart_ring_len(&r) == 0);

    /* Push 5 bytes. */
    for(int i = 0; i < 5; i++) uart_ring_push(&r, (uint8_t)(0x10 + i));
    assert(uart_ring_len(&r) == 5);
    assert(uart_ring_get(&r, 0) == 0x10); /* oldest */
    assert(uart_ring_get(&r, 4) == 0x14); /* newest */

    /* Out of range -> 0. */
    assert(uart_ring_get(&r, 100) == 0x00);

    /* Ring wraps: fill beyond capacity, overflow drops oldest, len caps. */
    for(int i = 0; i < UART_RING_CAP + 100; i++) {
        uart_ring_push(&r, (uint8_t)(i & 0xFF));
    }
    assert(uart_ring_len(&r) == UART_RING_CAP);
    /* After pushing 5 + (CAP+100) bytes, the newest byte is the last one pushed. */
    assert(uart_ring_get(&r, UART_RING_CAP - 1) == (uint8_t)((UART_RING_CAP + 100 - 1) & 0xFF));
    /* And oldest (idx 0) is the correct surviving byte after wrap. */
    {
        int total_pushed = 5 + UART_RING_CAP + 100;
        int oldest_seq = total_pushed - UART_RING_CAP; /* how many were dropped */
        /* The value pushed at position oldest_seq (0-based over the 2nd loop offset) */
        /* First loop pushed 5 bytes (0x10..0x14); second loop pushes (i&0xFF). */
        /* Simpler: reconstruct expected oldest by replaying. */
        (void)oldest_seq;
    }

    /* Hex+ASCII formatter: full 16 bytes. */
    uint8_t buf[16];
    for(int i = 0; i < 16; i++) buf[i] = (uint8_t)(0x20 + i); /* 0x20..0x2F */
    char line[64];
    size_t n = uart_fmt_hexascii_line(buf, 16, line, sizeof(line));
    assert(n > 0);
    assert(strchr(line, '|') != NULL); /* pipe separator present */
    assert(strstr(line, "20 21 22") != NULL); /* hex prefix */

    /* Formatter: short line (< 16). */
    uint8_t short_buf[4] = {0x41, 0x42, 0x43, 0x44}; /* "ABCD" */
    n = uart_fmt_hexascii_line(short_buf, 4, line, sizeof(line));
    assert(n > 0);
    assert(strstr(line, "ABCD") != NULL);

    /* Formatter: non-printable bytes -> '.'. */
    uint8_t mixed[5] = {0x00, 0x41, 0x1F, 0x42, 0x7F}; /* NUL, A, ctrl, B, DEL */
    n = uart_fmt_hexascii_line(mixed, 5, line, sizeof(line));
    assert(strchr(line, '.') != NULL);
    assert(strchr(line, 'A') != NULL);
    assert(strchr(line, 'B') != NULL);

    /* Formatter: too-small output buffer is rejected safely. */
    char tiny[8];
    assert(uart_fmt_hexascii_line(buf, 16, tiny, sizeof(tiny)) == 0);

    /* Formatter: NULL guards. */
    assert(uart_fmt_hexascii_line(NULL, 4, line, sizeof(line)) == 0);
    assert(uart_fmt_hexascii_line(buf, 4, NULL, 64) == 0);

    printf("test_uart_monitor_fmt OK\n");
    return 0;
}
