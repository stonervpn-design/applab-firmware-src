#pragma once

// SX1262 LoRa driver for the M5Stack Cap LoRa-1262 on the Cardputer-ADV.
//
// Wiring (from docs.m5stack.com/en/cap/Cap_LoRa-1262):
//   SPI3 (shared with SD/CC1101): SCK=40, MOSI=14, MISO=39
//   CS/NSS=5, BUSY=6, DIO1/IRQ=4, RST=3   (CS is managed manually)
//   RF antenna switch is behind a PI4IOE5V6408 I2C expander (P0=high enables it),
//   on I2C_NUM_0 (SDA=8/SCL=9), same bus as the keyboard.
//
// NOT hardware-tested. Register/command sequences follow the SX126x datasheet;
// validate on the module. Every SPI transaction takes furi_hal_spi_bus_lock() so
// it coexists with the SD card and CC1101 on SPI3.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// If TX/RX never keys the antenna, try flipping this to 0x44.
#define SX1262_IO_EXPANDER_ADDR 0x43

typedef enum {
    Sx1262Bw125 = 0,
    Sx1262Bw250,
    Sx1262Bw500,
} Sx1262Bandwidth;

typedef struct {
    uint32_t frequency; // Hz, 868000000..923000000
    uint8_t spreading_factor; // 5..12
    Sx1262Bandwidth bandwidth;
    uint8_t coding_rate; // 5..8 (4/5..4/8)
    uint8_t sync_word; // 0x12 private / 0x34 public / 0x2b Meshtastic
    uint8_t power_dbm; // -9..22
    uint16_t preamble; // symbols (8 default, Meshtastic uses 16)
} Sx1262Config;

/** Meshtastic LongFast preset (EU 868 default freq; adjust for your region). */
void sx1262_meshtastic_config(Sx1262Config* cfg, uint32_t frequency);

/** Bring up SPI/GPIO/I2C, reset the chip, verify it responds. false on failure. */
bool sx1262_init(void);

/** Release the driver (does not deinit the shared SPI bus). */
void sx1262_deinit(void);

/** Apply a full LoRa configuration (freq, modem params, sync word, power). */
bool sx1262_configure(const Sx1262Config* cfg);

/** Transmit a LoRa packet (blocks until TX-done or timeout). */
bool sx1262_tx(const uint8_t* data, size_t len, uint32_t timeout_ms);

/** Put the radio into continuous RX. */
bool sx1262_start_rx(void);

/**
 * Poll for a received packet. Returns true and fills buf/len/rssi/snr if one is
 * available since the last call; false if nothing. Non-blocking.
 */
bool sx1262_rx_poll(uint8_t* buf, size_t buf_size, size_t* out_len, int16_t* rssi, int8_t* snr);

/** Default 868 MHz / SF7 / BW125 / CR4-5 / private sync / +14 dBm config. */
void sx1262_default_config(Sx1262Config* cfg);

/** Diagnostic: SX1262 GetDeviceErrors op-error word (bit5=XOSC_START, bit6=PLL_LOCK,
 * bit8=PA_RAMP, etc). Nonzero after config points at the failing subsystem. */
uint16_t sx1262_device_errors(void);

/** Diagnostic: SX1262 GetStatus byte (chip mode + command status). */
uint8_t sx1262_status(void);
