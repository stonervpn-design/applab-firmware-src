#pragma once
#include <gui/view.h>
#include "uart_monitor_fmt.h"

typedef struct {
    UartRing* ring; /* scene-owned; set once on enter */
    uint32_t rx_count;
    uint32_t baud;
} BusToolsUartViewModel;

View* bus_tools_uart_view_alloc(void);
void bus_tools_uart_view_free(View* v);
