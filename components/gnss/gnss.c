#include "gnss.h"

#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <driver/uart.h>
#include <esp_log.h>

#define TAG          "gnss"
#define GNSS_UART    UART_NUM_1
#define GNSS_RX_BUF  1024
#define GNSS_LINE_MAX     128

static volatile bool s_run = false;
static volatile bool s_alive = false;
static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_lock = NULL;
static GnssFix s_fix;
static volatile uint32_t s_lines = 0;

static void gnss_task(void* arg) {
    (void)arg;
    char line[GNSS_LINE_MAX];
    size_t len = 0;
    uint8_t byte;

    while(s_run) {
        int n = uart_read_bytes(GNSS_UART, &byte, 1, pdMS_TO_TICKS(200));
        if(n <= 0) continue;
        if(byte == '\n' || byte == '\r') {
            if(len > 0) {
                line[len] = 0;
                s_lines++;
                GnssFix tmp;
                // parse against a private copy so a partial line can't expose garbage
                if(s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
                tmp = s_fix;
                if(s_lock) xSemaphoreGive(s_lock);
                if(nmea_parse_line(&tmp, line)) {
                    if(s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
                    s_fix = tmp;
                    if(s_lock) xSemaphoreGive(s_lock);
                }
                len = 0;
            }
        } else if(len < GNSS_LINE_MAX - 1) {
            line[len++] = (char)byte;
        } else {
            len = 0; // overrun — drop the oversized line
        }
    }

    uart_driver_delete(GNSS_UART);
    s_alive = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

bool gnss_start(void) {
    if(s_run || s_alive) return true;

    if(!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if(!s_lock) return false;
    }
    nmea_fix_init(&s_fix);
    s_lines = 0;

    const uart_config_t cfg = {
        .baud_rate = GNSS_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if(uart_driver_install(GNSS_UART, GNSS_RX_BUF, 0, 0, NULL, 0) != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed");
        return false;
    }
    uart_param_config(GNSS_UART, &cfg);
    // host-RX = GPIO15, host-TX = GPIO13 (no RTS/CTS)
    uart_set_pin(GNSS_UART, GNSS_UART_TX_PIN, GNSS_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    s_run = true;
    s_alive = true;
    if(xTaskCreate(gnss_task, "gnss", 4096, NULL, tskIDLE_PRIORITY + 4, &s_task) != pdPASS) {
        s_run = false;
        s_alive = false;
        uart_driver_delete(GNSS_UART);
        return false;
    }
    ESP_LOGI(TAG, "started (RX=%d TX=%d @ %d)", GNSS_UART_RX_PIN, GNSS_UART_TX_PIN, GNSS_UART_BAUD);
    return true;
}

void gnss_stop(void) {
    s_run = false; // gnss_task tears down UART + clears s_alive
}

bool gnss_is_running(void) {
    return s_run;
}

bool gnss_get_fix(GnssFix* out) {
    if(!out) return false;
    if(s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_fix;
    if(s_lock) xSemaphoreGive(s_lock);
    return out->valid;
}

uint32_t gnss_line_count(void) {
    return s_lines;
}
