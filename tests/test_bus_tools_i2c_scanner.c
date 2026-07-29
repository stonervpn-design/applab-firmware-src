#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "../applications/main/bus_tools/bus_tools_i2c_scanner.h"

void test_i2c_common_devices() {
    printf("Test: I2C common device lookup...\n");

    // Test known devices
    const char* name_0x68 = bus_tools_i2c_device_name(0x68);
    assert(name_0x68 != NULL);
    printf("  0x68: %s\n", name_0x68);
    assert(strstr(name_0x68, "RTC") != NULL || strstr(name_0x68, "IMU") != NULL);

    const char* name_0x50 = bus_tools_i2c_device_name(0x50);
    assert(name_0x50 != NULL);
    printf("  0x50: %s\n", name_0x50);
    assert(strstr(name_0x50, "EEPROM") != NULL);

    const char* name_0x3C = bus_tools_i2c_device_name(0x3C);
    assert(name_0x3C != NULL);
    printf("  0x3C: %s\n", name_0x3C);
    assert(strstr(name_0x3C, "OLED") != NULL);

    // Test unknown device
    const char* name_unknown = bus_tools_i2c_device_name(0xFF);
    assert(name_unknown == NULL);
    printf("  0xFF (unknown): %s (NULL)\n", name_unknown == NULL ? "NULL" : name_unknown);

    printf("  PASS: device lookup\n\n");
}

void test_i2c_address_validation() {
    printf("Test: I2C address validation...\n");

    // Reserved addresses (should be invalid)
    assert(bus_tools_i2c_is_valid_address(0x00) == 0);
    assert(bus_tools_i2c_is_valid_address(0x07) == 0);
    assert(bus_tools_i2c_is_valid_address(0x78) == 0);
    assert(bus_tools_i2c_is_valid_address(0x7F) == 0);
    printf("  Reserved addresses (0x00-0x07, 0x78-0x7F): invalid\n");

    // Valid addresses
    assert(bus_tools_i2c_is_valid_address(0x08) == 1);
    assert(bus_tools_i2c_is_valid_address(0x50) == 1);
    assert(bus_tools_i2c_is_valid_address(0x77) == 1);
    printf("  Valid addresses (0x08-0x77): valid\n");

    printf("  PASS: address validation\n\n");
}

void test_i2c_scan() {
    printf("Test: I2C device scan...\n");

    I2cDevice devices[128];
    uint8_t count = bus_tools_i2c_scan(devices, 128);
    printf("  Scan found %u devices\n", count);
    assert(count > 0);
    assert(count <= 128);

    // Verify known devices are present
    int found_0x68 = 0, found_0x50 = 0, found_0x3C = 0;
    for(uint8_t i = 0; i < count; i++) {
        if(devices[i].address == 0x68) found_0x68 = 1;
        if(devices[i].address == 0x50) found_0x50 = 1;
        if(devices[i].address == 0x3C) found_0x3C = 1;

        // Verify all addresses are valid
        assert(bus_tools_i2c_is_valid_address(devices[i].address));
        assert(devices[i].name != NULL);
    }

    printf("  Found 0x68 (RTC/IMU): %s\n", found_0x68 ? "YES" : "NO");
    printf("  Found 0x50 (EEPROM): %s\n", found_0x50 ? "YES" : "NO");
    printf("  Found 0x3C (OLED): %s\n", found_0x3C ? "YES" : "NO");

    assert(found_0x68 == 1);
    assert(found_0x50 == 1);
    assert(found_0x3C == 1);

    printf("  PASS: device scan\n\n");
}

void test_i2c_scan_with_limit() {
    printf("Test: I2C scan with device limit...\n");

    I2cDevice devices[5];
    uint8_t count = bus_tools_i2c_scan(devices, 5);
    printf("  Requested 5 devices, got %u\n", count);
    assert(count == 5);

    printf("  PASS: device scan with limit\n\n");
}

int main() {
    printf("=== BUS TOOLS I2C SCANNER TEST SUITE ===\n\n");

    test_i2c_address_validation();
    test_i2c_common_devices();
    test_i2c_scan();
    test_i2c_scan_with_limit();

    printf("=== ALL BUS TOOLS I2C SCANNER TESTS PASSED ===\n");
    return 0;
}
