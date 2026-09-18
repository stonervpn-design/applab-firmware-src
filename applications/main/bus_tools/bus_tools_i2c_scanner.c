#include "bus_tools_i2c_scanner.h"
#include <string.h>
#include <driver/i2c.h>

/* The external/Grove I2C bus (= BOARD_NFC_I2C_PORT). The legacy driver is already
 * installed + pin-configured here by furi_hal_nfc_init() at boot, so we reuse it. */
#define I2C_SCAN_PORT I2C_NUM_1

// Known I2C devices: address and common name
static const struct {
    uint8_t addr;
    const char* name;
} I2C_DEVICES[] = {
    {0x08, "Generic Sensor"},
    {0x18, "ADXL345 Accelerometer"},
    {0x19, "ADXL345 Alt"},
    {0x20, "MCP23017 I/O Expander"},
    {0x27, "PCF8574 I/O Expander"},
    {0x28, "BMP280 Barometer"},
    {0x29, "TSL2561 Light Sensor"},
    {0x38, "APDS9960 Proximity"},
    {0x3C, "OLED SSD1306 128x64"},
    {0x3D, "OLED SSD1306 128x32"},
    {0x48, "ADS1115 ADC"},
    {0x50, "EEPROM 24C32"},
    {0x51, "EEPROM 24C32 Alt"},
    {0x52, "EEPROM 24C32 Alt2"},
    {0x53, "EEPROM 24C32 Alt3"},
    {0x54, "EEPROM 24C32 Alt4"},
    {0x55, "EEPROM 24C32 Alt5"},
    {0x56, "EEPROM 24C32 Alt6"},
    {0x57, "EEPROM 24C32 Alt7"},
    {0x60, "MCP4725 DAC"},
    {0x68, "RTC DS3231 / MPU6050 IMU"},
    {0x69, "MPU6050 IMU Alt"},
    {0x70, "TCA9548 Multiplexer"},
    {0x72, "LCD I2C Driver"},
    {0x76, "BME280 Enviro Sensor"},
    {0x77, "BMP085 Barometer"},
};
#define I2C_DEVICE_COUNT (sizeof(I2C_DEVICES) / sizeof(I2C_DEVICES[0]))

const char* bus_tools_i2c_device_name(uint8_t addr) {
    for(size_t i = 0; i < I2C_DEVICE_COUNT; i++) {
        if(I2C_DEVICES[i].addr == addr) {
            return I2C_DEVICES[i].name;
        }
    }
    return NULL;
}

int bus_tools_i2c_is_valid_address(uint8_t addr) {
    // Valid I2C 7-bit addresses are 0x08-0x77
    // (0x00-0x07 and 0x78-0x7F are reserved)
    return (addr >= 0x08 && addr <= 0x77);
}

uint8_t bus_tools_i2c_scan(I2cDevice* out_devices, uint8_t max_devices) {
    if(!out_devices || max_devices == 0) {
        return 0;
    }

    uint8_t count = 0;
    /* Real bus probe: for each valid 7-bit address, issue START + addr(write) +
     * STOP and check for an ACK. ESP_OK == a device acknowledged. Name is looked
     * up from the table (NULL -> the scene shows "Unknown"). If the driver isn't
     * installed (NFC init failed) cmd_begin returns an error and we find nothing. */
    for(uint8_t addr = 0x08; addr <= 0x77 && count < max_devices; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (uint8_t)((addr << 1) | I2C_MASTER_WRITE), true);
        i2c_master_stop(cmd);
        esp_err_t err = i2c_master_cmd_begin(I2C_SCAN_PORT, cmd, pdMS_TO_TICKS(10));
        i2c_cmd_link_delete(cmd);
        if(err == ESP_OK) {
            out_devices[count].address = addr;
            out_devices[count].name = bus_tools_i2c_device_name(addr);
            count++;
        }
    }

    return count;
}
