#pragma once
#include "can_frame.h"
#include <stdbool.h>
#include <stdint.h>

// Thin wrapper over the ESP32-S3 TWAI (CAN) controller. The driver is installed
// on start and uninstalled on stop so the Grove pins are freed for the NFC I2C
// bus when CAN is not in use.

typedef enum {
    CanModeNormal, // ACK required from another node on the bus
    CanModeNoAck, // self-test: transmit without needing an ACK
    CanModeListenOnly, // passive: never transmits or ACKs
} CanMode;

typedef struct {
    uint32_t bitrate; // 125000 / 250000 / 500000 / 1000000
    CanMode mode;
    uint8_t tx_gpio; // default Grove GPIO2
    uint8_t rx_gpio; // default Grove GPIO1
} CanHalConfig;

typedef struct {
    uint32_t tx_ok;
    uint32_t rx_ok;
    uint32_t tx_fail;
    uint32_t bus_err;
    bool bus_off;
} CanHalStats;

// Install + start the TWAI driver. Returns false if already running or on error.
bool can_hal_start(const CanHalConfig* cfg);
// Stop + uninstall (frees the pins). Safe to call when not running.
void can_hal_stop(void);
bool can_hal_is_running(void);

// Transmit one frame. Returns false on timeout/error (also in ListenOnly mode).
bool can_hal_transmit(const CanFrame* f, uint32_t timeout_ms);
// Receive one frame. Returns false on timeout.
bool can_hal_receive(CanFrame* out, uint32_t timeout_ms);

CanHalStats can_hal_stats(void);
// Attempt bus-off recovery (call after bus_off is seen).
void can_hal_recover(void);
