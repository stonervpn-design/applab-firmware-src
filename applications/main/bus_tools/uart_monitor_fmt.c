#include "uart_monitor_fmt.h"

void uart_ring_reset(UartRing* r) {
    r->head = 0;
    r->len = 0;
}

void uart_ring_push(UartRing* r, uint8_t byte) {
    r->data[r->head] = byte;
    r->head = (uint16_t)((r->head + 1) % UART_RING_CAP);
    if(r->len < UART_RING_CAP) {
        r->len++;
    }
    /* else: full — head has wrapped over and overwritten the oldest byte. */
}

uint16_t uart_ring_len(const UartRing* r) {
    return r->len;
}

uint8_t uart_ring_get(const UartRing* r, uint16_t idx) {
    if(idx >= r->len) return 0x00;
    /* Full: oldest sits at head (next write slot). Not full: oldest at 0. */
    uint16_t start = (r->len == UART_RING_CAP) ? r->head : 0;
    uint16_t pos = (uint16_t)((start + idx) % UART_RING_CAP);
    return r->data[pos];
}

size_t uart_fmt_hexascii_line(const uint8_t* bytes, uint8_t n, char* out, size_t out_sz) {
    if(!bytes || !out || out_sz < 40) return 0;
    if(n > 16) n = 16;

    static const char hexd[] = "0123456789ABCDEF";
    size_t wr = 0;

    /* Hex part: "XX XX XX ..." — bounds-checked at every write so a short
     * out_sz truncates cleanly instead of overrunning the buffer. */
    for(uint8_t i = 0; i < n; i++) {
        if(i > 0) {
            if(wr + 1 >= out_sz) goto done;
            out[wr++] = ' ';
        }
        if(wr + 2 >= out_sz) goto done;
        out[wr++] = hexd[(bytes[i] >> 4) & 0x0F];
        out[wr++] = hexd[bytes[i] & 0x0F];
    }

    /* Separator " |" */
    if(wr + 2 >= out_sz) goto done;
    out[wr++] = ' ';
    out[wr++] = '|';

    /* ASCII part */
    for(uint8_t i = 0; i < n; i++) {
        if(wr + 1 >= out_sz) goto done;
        uint8_t c = bytes[i];
        out[wr++] = (c >= 0x20 && c <= 0x7E) ? (char)c : '.';
    }

    if(wr + 1 < out_sz) out[wr++] = '|';

done:
    out[wr] = '\0';
    return wr;
}
