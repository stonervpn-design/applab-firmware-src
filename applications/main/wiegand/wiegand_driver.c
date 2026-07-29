#include "wiegand_driver.h"
#include <furi.h>
#include <furi_hal_gpio.h>
#include <string.h>

#define WIEGAND_MAX_BITS 34
#define WIEGAND_PULSE_WIDTH_US 125
#define WIEGAND_BIT_TIMEOUT_MS 100

typedef struct {
    uint8_t bits[WIEGAND_MAX_BITS];
    uint8_t count;
    bool capturing;
    uint8_t gpio_d0;
    uint8_t gpio_d1;
} WiegandCaptureState;

static WiegandCaptureState g_capture_state = {0};

bool wiegand_driver_start_capture(uint8_t gpio_d0, uint8_t gpio_d1, uint32_t timeout_ms) {
    UNUSED(timeout_ms);

    if (g_capture_state.capturing) {
        return false;
    }

    // Configure GPIO pins as inputs
    const GpioPin pin_d0 = {.port = NULL, .pin = gpio_d0};
    const GpioPin pin_d1 = {.port = NULL, .pin = gpio_d1};

    furi_hal_gpio_init(&pin_d0, GpioModeInput, GpioPullUp, GpioSpeedVeryHigh);
    furi_hal_gpio_init(&pin_d1, GpioModeInput, GpioPullUp, GpioSpeedVeryHigh);

    memset(g_capture_state.bits, 0, sizeof(g_capture_state.bits));
    g_capture_state.count = 0;
    g_capture_state.gpio_d0 = gpio_d0;
    g_capture_state.gpio_d1 = gpio_d1;
    g_capture_state.capturing = true;

    return true;
}

int32_t wiegand_driver_read_capture(uint8_t* out_bits, uint8_t max_bits) {
    if (!out_bits) {
        return -1;
    }

    if (!g_capture_state.capturing) {
        // Done capturing; copy the bits
        if (g_capture_state.count > max_bits) {
            return -1;
        }
        memcpy(out_bits, g_capture_state.bits, g_capture_state.count);
        return g_capture_state.count;
    }

    // Still capturing; poll for new bits with timeout
    const GpioPin pin_d0 = {.port = NULL, .pin = g_capture_state.gpio_d0};
    const GpioPin pin_d1 = {.port = NULL, .pin = g_capture_state.gpio_d1};

    uint32_t last_pulse_time = furi_get_tick();

    for (int attempt = 0; attempt < 1000 && g_capture_state.capturing; attempt++) {
        uint32_t now = furi_get_tick();

        // Timeout: stop capturing if no pulse for WIEGAND_BIT_TIMEOUT_MS
        if ((now - last_pulse_time) > WIEGAND_BIT_TIMEOUT_MS) {
            g_capture_state.capturing = false;
            break;
        }

        // Check for pulse on D0 or D1 (LOW pulse = 1 bit)
        bool d0_low = !furi_hal_gpio_read(&pin_d0);
        bool d1_low = !furi_hal_gpio_read(&pin_d1);

        if (d0_low || d1_low) {
            // Record which line pulsed
            uint8_t bit = d1_low ? 1 : 0;

            if (g_capture_state.count < WIEGAND_MAX_BITS) {
                g_capture_state.bits[g_capture_state.count++] = bit;
                last_pulse_time = now;
            } else {
                // Buffer full
                g_capture_state.capturing = false;
                break;
            }

            // Wait for the pulse to end
            furi_delay_us(WIEGAND_PULSE_WIDTH_US + 50);
        } else {
            furi_delay_us(10);
        }
    }

    // Return current captured count
    if (g_capture_state.count > max_bits) {
        return -1;
    }
    memcpy(out_bits, g_capture_state.bits, g_capture_state.count);
    return g_capture_state.count;
}

void wiegand_driver_stop_capture(void) {
    g_capture_state.capturing = false;
    g_capture_state.count = 0;
}

bool wiegand_driver_replay(uint8_t gpio_d0, uint8_t gpio_d1, const uint8_t* pulse_bits, uint8_t bit_count) {
    if (!pulse_bits || bit_count == 0 || bit_count > WIEGAND_MAX_BITS) {
        return false;
    }

    const GpioPin pin_d0 = {.port = NULL, .pin = gpio_d0};
    const GpioPin pin_d1 = {.port = NULL, .pin = gpio_d1};

    // Set pins as outputs
    furi_hal_gpio_init(&pin_d0, GpioModeOutputPushPull, GpioPullUp, GpioSpeedVeryHigh);
    furi_hal_gpio_init(&pin_d1, GpioModeOutputPushPull, GpioPullUp, GpioSpeedVeryHigh);

    // Replay each bit: 0 = D0 pulse, 1 = D1 pulse
    for (uint8_t i = 0; i < bit_count; i++) {
        if (pulse_bits[i] == 0) {
            // D0 pulse: D0 LOW for WIEGAND_PULSE_WIDTH_US
            furi_hal_gpio_write(&pin_d0, false);
            furi_delay_us(WIEGAND_PULSE_WIDTH_US);
            furi_hal_gpio_write(&pin_d0, true);
        } else {
            // D1 pulse: D1 LOW for WIEGAND_PULSE_WIDTH_US
            furi_hal_gpio_write(&pin_d1, false);
            furi_delay_us(WIEGAND_PULSE_WIDTH_US);
            furi_hal_gpio_write(&pin_d1, true);
        }
        // Inter-pulse spacing (typical: 200µs between pulses)
        furi_delay_us(200);
    }

    return true;
}
