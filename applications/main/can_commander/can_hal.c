#include "can_hal.h"

#include <furi.h>
#include <driver/twai.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "CanHal"

static struct {
    bool running;
    CanHalStats stats;
} can_hal = {0};

static twai_mode_t can_hal_map_mode(CanMode mode) {
    switch(mode) {
    case CanModeNoAck:
        return TWAI_MODE_NO_ACK;
    case CanModeListenOnly:
        return TWAI_MODE_LISTEN_ONLY;
    case CanModeNormal:
    default:
        return TWAI_MODE_NORMAL;
    }
}

static bool can_hal_timing(uint32_t bitrate, twai_timing_config_t* out) {
    switch(bitrate) {
    case 125000: {
        twai_timing_config_t t = TWAI_TIMING_CONFIG_125KBITS();
        *out = t;
        return true;
    }
    case 250000: {
        twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS();
        *out = t;
        return true;
    }
    case 500000: {
        twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
        *out = t;
        return true;
    }
    case 1000000: {
        twai_timing_config_t t = TWAI_TIMING_CONFIG_1MBITS();
        *out = t;
        return true;
    }
    default:
        return false;
    }
}

bool can_hal_start(const CanHalConfig* cfg) {
    if(!cfg || can_hal.running) return false;

    twai_timing_config_t t_config;
    if(!can_hal_timing(cfg->bitrate, &t_config)) {
        FURI_LOG_W(TAG, "unsupported bitrate %lu", (unsigned long)cfg->bitrate);
        return false;
    }

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
        (gpio_num_t)cfg->tx_gpio, (gpio_num_t)cfg->rx_gpio, can_hal_map_mode(cfg->mode));
    g_config.tx_queue_len = 16;
    g_config.rx_queue_len = 32;

    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t err = twai_driver_install(&g_config, &t_config, &f_config);
    if(err != ESP_OK) {
        FURI_LOG_W(TAG, "driver_install failed: %d", (int)err);
        return false;
    }
    err = twai_start();
    if(err != ESP_OK) {
        FURI_LOG_W(TAG, "start failed: %d", (int)err);
        twai_driver_uninstall();
        return false;
    }

    memset(&can_hal.stats, 0, sizeof(can_hal.stats));
    can_hal.running = true;
    FURI_LOG_I(
        TAG,
        "started %lu bps mode %d tx %u rx %u",
        (unsigned long)cfg->bitrate,
        (int)cfg->mode,
        cfg->tx_gpio,
        cfg->rx_gpio);
    return true;
}

void can_hal_stop(void) {
    if(!can_hal.running) return;
    twai_stop();
    twai_driver_uninstall();
    can_hal.running = false;
    FURI_LOG_I(TAG, "stopped");
}

bool can_hal_is_running(void) {
    return can_hal.running;
}

bool can_hal_transmit(const CanFrame* f, uint32_t timeout_ms) {
    if(!can_hal.running || !f) return false;

    twai_message_t msg = {0};
    msg.identifier = f->id;
    msg.extd = f->extended ? 1 : 0;
    msg.rtr = f->rtr ? 1 : 0;
    msg.data_length_code = f->dlc;
    for(uint8_t i = 0; i < f->dlc && i < CAN_MAX_DATA; i++) msg.data[i] = f->data[i];

    esp_err_t err = twai_transmit(&msg, pdMS_TO_TICKS(timeout_ms));
    if(err == ESP_OK) {
        can_hal.stats.tx_ok++;
        return true;
    }
    can_hal.stats.tx_fail++;
    return false;
}

bool can_hal_receive(CanFrame* out, uint32_t timeout_ms) {
    if(!can_hal.running || !out) return false;

    twai_message_t msg;
    esp_err_t err = twai_receive(&msg, pdMS_TO_TICKS(timeout_ms));
    if(err != ESP_OK) return false;

    memset(out, 0, sizeof(*out));
    out->id = msg.identifier;
    out->extended = msg.extd ? true : false;
    out->rtr = msg.rtr ? true : false;
    out->dlc = msg.data_length_code > CAN_MAX_DATA ? CAN_MAX_DATA : msg.data_length_code;
    for(uint8_t i = 0; i < out->dlc; i++) out->data[i] = msg.data[i];
    can_hal.stats.rx_ok++;
    return true;
}

CanHalStats can_hal_stats(void) {
    if(can_hal.running) {
        twai_status_info_t st;
        if(twai_get_status_info(&st) == ESP_OK) {
            can_hal.stats.bus_err = st.bus_error_count;
            can_hal.stats.bus_off = (st.state == TWAI_STATE_BUS_OFF);
        }
    }
    return can_hal.stats;
}

void can_hal_recover(void) {
    if(!can_hal.running) return;
    twai_status_info_t st;
    if(twai_get_status_info(&st) == ESP_OK && st.state == TWAI_STATE_BUS_OFF) {
        twai_initiate_recovery();
    }
}
