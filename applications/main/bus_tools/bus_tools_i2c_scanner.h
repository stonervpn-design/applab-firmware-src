#ifndef BUS_TOOLS_I2C_SCANNER_H
#define BUS_TOOLS_I2C_SCANNER_H

#include <stdint.h>

typedef struct {
    uint8_t address;     // 0x00-0x7F
    const char* name;    // Common device name or NULL
} I2cDevice;

/**
 * Scan a range (e.g., 0x00-0x7F) and populate results with known devices.
 * This is a pure core that returns a lookup table; actual I2C probing
 * is done by the firmware scene integration.
 * Returns count of devices found.
 */
uint8_t bus_tools_i2c_scan(I2cDevice* out_devices, uint8_t max_devices);

/**
 * Get the device name for a specific I2C address.
 * Returns the name if known, NULL otherwise.
 */
const char* bus_tools_i2c_device_name(uint8_t addr);

/**
 * Check if an address is valid for I2C (0x08-0x77).
 * Returns 1 if valid, 0 otherwise.
 */
int bus_tools_i2c_is_valid_address(uint8_t addr);

#endif // BUS_TOOLS_I2C_SCANNER_H
