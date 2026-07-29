#include "bus_tools_uart_view.h"
#include <gui/elements.h>
#include <furi.h>
#include <stdio.h>

/* Narrow 128px screen: 4 bytes/row keeps "XX XX XX XX |....|" within width. */
#define BT_UART_BYTES_PER_ROW 4
#define BT_UART_ROWS 3

static void bus_tools_uart_view_draw(Canvas* canvas, void* model) {
    BusToolsUartViewModel* m = model;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "UART Monitor");
    canvas_set_font(canvas, FontSecondary);

    char hdr[32];
    snprintf(
        hdr, sizeof(hdr), "%lu 8N1  %lu B", (unsigned long)m->baud, (unsigned long)m->rx_count);
    canvas_draw_str(canvas, 2, 21, hdr);
    canvas_draw_line(canvas, 0, 24, 127, 24);

    if(!m->ring || uart_ring_len(m->ring) == 0) {
        canvas_draw_str(canvas, 2, 40, "RX=G2 TX=G1  waiting...");
        return;
    }

    const uint16_t window = BT_UART_ROWS * BT_UART_BYTES_PER_ROW;
    uint16_t total = uart_ring_len(m->ring);
    uint16_t start = (total > window) ? (uint16_t)(total - window) : 0;

    int y = 34;
    for(uint16_t row = start; row < total && y < 64; row += BT_UART_BYTES_PER_ROW) {
        uint8_t buf[BT_UART_BYTES_PER_ROW];
        uint8_t n = 0;
        for(uint16_t i = row; i < row + BT_UART_BYTES_PER_ROW && i < total; i++) {
            buf[n++] = uart_ring_get(m->ring, i);
        }
        char line[48];
        uart_fmt_hexascii_line(buf, n, line, sizeof(line));
        canvas_draw_str(canvas, 2, y, line);
        y += 10;
    }
}

static bool bus_tools_uart_view_input(InputEvent* event, void* ctx) {
    UNUSED(event);
    UNUSED(ctx);
    return false; /* scene manager owns Back navigation */
}

View* bus_tools_uart_view_alloc(void) {
    View* v = view_alloc();
    view_allocate_model(v, ViewModelTypeLocking, sizeof(BusToolsUartViewModel));
    view_set_draw_callback(v, bus_tools_uart_view_draw);
    view_set_input_callback(v, bus_tools_uart_view_input);
    return v;
}

void bus_tools_uart_view_free(View* v) {
    view_free(v);
}
