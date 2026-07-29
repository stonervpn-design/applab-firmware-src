#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    GpioDriverPullNone,
    GpioDriverPullUp,
    GpioDriverPullDown,
} GpioDriverPull;

void gpio_driver_init(void);
bool gpio_driver_set_output(uint8_t gpio, bool level);
bool gpio_driver_set_input(uint8_t gpio, GpioDriverPull pull);
bool gpio_driver_read_digital(uint8_t gpio);
/* PWM via ESP-IDF LEDC (shared LEDC_TIMER_1, channels 1..7).
 * gpio_driver_set_pwm returns false if all PWM channels (1..7) are in use. */
bool gpio_driver_set_pwm(uint8_t gpio, uint32_t freq_hz, uint8_t duty_pct);
void gpio_driver_stop_pwm(uint8_t gpio);
/* Reads ADC1 on the given channel; writes millivolts to *out_mv. false if unavailable. */
bool gpio_driver_read_adc(int8_t adc_ch, uint32_t* out_mv);
void gpio_driver_release(uint8_t gpio);
void gpio_driver_release_all(void);

#ifdef __cplusplus
}
#endif
