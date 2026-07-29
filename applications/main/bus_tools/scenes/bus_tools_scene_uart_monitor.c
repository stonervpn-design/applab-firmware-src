#include "../bus_tools_app_i.h"
#include "../uart_monitor_fmt.h"
#include "../bus_tools_uart_view.h"
#include <driver/uart.h>

/* Grove/HY2.0 port pins (the only user-wireable pins on the Cardputer-ADV).
 * UART1 is otherwise only used by the GNSS wardriver, which never runs
 * concurrently with Bus Tools. Passive RX monitor; the TX line is configured
 * but unused by this scene. */
#define BT_UART_MON_PORT UART_NUM_1
#define BT_UART_MON_RX_PIN 2 /* G2 / Grove SDA */
#define BT_UART_MON_TX_PIN 1 /* G1 / Grove SCL */
#define BT_UART_MON_RXBUF 2048

static bool s_uart_open;
static uint32_t s_rx_total;

void bus_tools_scene_uart_monitor_on_enter(void* context) {
    furi_assert(context);
    BusToolsApp* app = context;
    uart_ring_reset(&app->uart_ring);
    s_rx_total = 0;
    s_uart_open = false;

    const uart_config_t cfg = {
        .baud_rate = (int)app->uart_baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    /* If a stale install lingers (e.g. GNSS), delete then re-install cleanly. */
    if(uart_driver_install(BT_UART_MON_PORT, BT_UART_MON_RXBUF, 0, 0, NULL, 0) != ESP_OK) {
        uart_driver_delete(BT_UART_MON_PORT);
        if(uart_driver_install(BT_UART_MON_PORT, BT_UART_MON_RXBUF, 0, 0, NULL, 0) != ESP_OK) {
            goto view;
        }
    }
    uart_param_config(BT_UART_MON_PORT, &cfg);
    uart_set_pin(
        BT_UART_MON_PORT, BT_UART_MON_TX_PIN, BT_UART_MON_RX_PIN, UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE);
    s_uart_open = true;

view:
    with_view_model(
        app->uart_view,
        BusToolsUartViewModel * m,
        {
            m->ring = &app->uart_ring;
            m->rx_count = 0;
            m->baud = app->uart_baud;
        },
        true);
    view_dispatcher_switch_to_view(app->view_dispatcher, BusToolsViewUartMonitor);
}

bool bus_tools_scene_uart_monitor_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    BusToolsApp* app = context;
    if(event.type != SceneManagerEventTypeTick || !s_uart_open) return false;

    uint8_t buf[128];
    int n = uart_read_bytes(BT_UART_MON_PORT, buf, sizeof(buf), 0); /* non-blocking */
    if(n > 0) {
        for(int i = 0; i < n; i++) uart_ring_push(&app->uart_ring, buf[i]);
        s_rx_total += (uint32_t)n;
    }
    with_view_model(
        app->uart_view, BusToolsUartViewModel * m, { m->rx_count = s_rx_total; }, true);
    return false;
}

void bus_tools_scene_uart_monitor_on_exit(void* context) {
    UNUSED(context);
    if(s_uart_open) {
        uart_driver_delete(BT_UART_MON_PORT);
        s_uart_open = false;
    }
}
