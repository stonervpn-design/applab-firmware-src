#include "gpio_driver.h"
#include <furi_hal.h>
#include <string.h>
#include <driver/ledc.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>

#define GPIO_MAX_NUM 49   // ESP32-S3 GPIO0..48

/* PWM uses ESP-IDF LEDC. The LCD backlight owns LEDC_TIMER_0 + LEDC_CHANNEL_0
 * and the RGB ring uses RMT (no LEDC). Manual-control PWM therefore uses a
 * single shared LEDC_TIMER_1 and channels LEDC_CHANNEL_1..LEDC_CHANNEL_7. */
#define GPIO_PWM_SPEED      LEDC_LOW_SPEED_MODE
#define GPIO_PWM_TIMER      LEDC_TIMER_1
#define GPIO_PWM_DUTY_RES   LEDC_TIMER_10_BIT   // 0..1023
#define GPIO_PWM_CH_MIN     LEDC_CHANNEL_1
#define GPIO_PWM_CH_MAX     LEDC_CHANNEL_7

static bool s_touched[GPIO_MAX_NUM];
/* GPIO -> LEDC channel allocation for active PWM pins. 0 = no channel held
 * (channel 0 is the backlight and is never handed out here, so 0 is a safe
 * "none" sentinel). */
static uint8_t s_pwm_ch[GPIO_MAX_NUM];
/* Which LEDC channels are currently in use (indexed by channel number). */
static bool s_ch_used[LEDC_CHANNEL_MAX];

/* ADC1 oneshot: one lazily-created unit + one calibration scheme shared across
 * channels (attenuation/bitwidth are identical for all channels here, so on the
 * S3 the curve-fitting scheme is channel-independent). */
static adc_oneshot_unit_handle_t s_adc_unit = NULL;
static adc_cali_handle_t s_adc_cali = NULL;
static bool s_adc_cali_ok = false;
#define GPIO_ADC_CH_MAX 10 // ADC1 exposes channels 0..9 on the S3

static GpioPin pin_of(uint8_t gpio) {
    GpioPin gp = {.port = NULL, .pin = gpio};
    return gp;
}

void gpio_driver_init(void) {
    memset(s_touched, 0, sizeof(s_touched));
    memset(s_pwm_ch, 0, sizeof(s_pwm_ch));
    memset(s_ch_used, 0, sizeof(s_ch_used));
    s_adc_unit = NULL;
    s_adc_cali = NULL;
    s_adc_cali_ok = false;
}

bool gpio_driver_set_output(uint8_t gpio, bool level) {
    if(gpio >= GPIO_MAX_NUM) return false;
    GpioPin gp = pin_of(gpio);
    furi_hal_gpio_init(&gp, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);
    furi_hal_gpio_write(&gp, level);
    s_touched[gpio] = true;
    return true;
}

bool gpio_driver_set_input(uint8_t gpio, GpioDriverPull pull) {
    if(gpio >= GPIO_MAX_NUM) return false;
    GpioPull p = (pull == GpioDriverPullUp) ? GpioPullUp :
                 (pull == GpioDriverPullDown) ? GpioPullDown : GpioPullNo;
    GpioPin gp = pin_of(gpio);
    furi_hal_gpio_init(&gp, GpioModeInput, p, GpioSpeedVeryHigh);
    s_touched[gpio] = true;
    return true;
}

bool gpio_driver_read_digital(uint8_t gpio) {
    if(gpio >= GPIO_MAX_NUM) return false;
    GpioPin gp = pin_of(gpio);
    return furi_hal_gpio_read(&gp);
}

bool gpio_driver_set_pwm(uint8_t gpio, uint32_t freq_hz, uint8_t duty_pct) {
    if(gpio >= GPIO_MAX_NUM) return false;
    if(duty_pct > 100) duty_pct = 100;

    /* Reuse this pin's channel if it already holds one, else grab a free
     * channel in the 1..7 range. */
    uint8_t ch = s_pwm_ch[gpio];
    if(ch == 0) {
        for(uint8_t c = GPIO_PWM_CH_MIN; c <= GPIO_PWM_CH_MAX; c++) {
            if(!s_ch_used[c]) {
                ch = c;
                break;
            }
        }
        if(ch == 0) return false; // all of channels 1..7 are in use
    }

    /* One shared LEDC_TIMER_1: (re)configuring it here changes the frequency
     * for ALL active PWM pins, since they all source this timer. Per-pin
     * frequency would need extra timers (TIMER_2/3) — intentionally not done
     * (YAGNI). */
    ledc_timer_config_t timer_conf = {
        .speed_mode = GPIO_PWM_SPEED,
        .duty_resolution = GPIO_PWM_DUTY_RES,
        .timer_num = GPIO_PWM_TIMER,
        .freq_hz = freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if(ledc_timer_config(&timer_conf) != ESP_OK) return false;

    uint32_t duty = (uint32_t)duty_pct * 1023 / 100;
    ledc_channel_config_t ch_conf = {
        .speed_mode = GPIO_PWM_SPEED,
        .channel = (ledc_channel_t)ch,
        .timer_sel = GPIO_PWM_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = gpio,
        .duty = duty,
        .hpoint = 0,
    };
    if(ledc_channel_config(&ch_conf) != ESP_OK) return false;

    ledc_set_duty(GPIO_PWM_SPEED, (ledc_channel_t)ch, duty);
    ledc_update_duty(GPIO_PWM_SPEED, (ledc_channel_t)ch);

    s_ch_used[ch] = true;
    s_pwm_ch[gpio] = ch;
    s_touched[gpio] = true;
    return true;
}

void gpio_driver_stop_pwm(uint8_t gpio) {
    if(gpio >= GPIO_MAX_NUM) return;
    uint8_t ch = s_pwm_ch[gpio];
    if(ch == 0) return; // pin holds no PWM channel
    ledc_stop(GPIO_PWM_SPEED, (ledc_channel_t)ch, 0);
    s_ch_used[ch] = false;
    s_pwm_ch[gpio] = 0;
}

bool gpio_driver_read_adc(int8_t adc_ch, uint32_t* out_mv) {
    if(adc_ch < 0 || adc_ch >= GPIO_ADC_CH_MAX || out_mv == NULL) return false;
    adc_channel_t ch = (adc_channel_t)adc_ch;

    /* Lazily create the single ADC1 oneshot unit. */
    if(s_adc_unit == NULL) {
        adc_oneshot_unit_init_cfg_t init_cfg = {
            .unit_id = ADC_UNIT_1,
        };
        if(adc_oneshot_new_unit(&init_cfg, &s_adc_unit) != ESP_OK) {
            s_adc_unit = NULL;
            return false;
        }
    }

    /* (Re)route the pad to ADC on EVERY read (~0..3.1 V full range). This is
     * deliberately not cached: if this pin (e.g. the ADV SAFE pins GPIO1/GPIO2,
     * which have both PWM and ADC caps) was driven as LEDC/GPIO in between,
     * ledc_stop() leaves the pad routed as a GPIO output. Reconfiguring here
     * re-claims the pad for ADC. adc_oneshot_config_channel is lightweight and
     * safe to call repeatedly, so doing it at the UI read tick is negligible. */
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if(adc_oneshot_config_channel(s_adc_unit, ch, &chan_cfg) != ESP_OK) return false;

    /* Lazily create the calibration scheme (curve fitting on the S3). If it is
     * unavailable, fall back to a linear raw->mV conversion below. */
    if(!s_adc_cali_ok) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id = ADC_UNIT_1,
            .chan = ch,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if(adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali) == ESP_OK) {
            s_adc_cali_ok = true;
        } else {
            s_adc_cali = NULL;
        }
#endif
    }

    int raw = 0;
    if(adc_oneshot_read(s_adc_unit, ch, &raw) != ESP_OK) return false;

    /* The oneshot driver owns the pad; still record the GPIO (S3: gpio = ch + 1)
     * as touched so release_all restores it to high-Z on exit. */
    uint8_t gpio = (uint8_t)adc_ch + 1;
    if(gpio < GPIO_MAX_NUM) s_touched[gpio] = true;

    int mv = 0;
    if(s_adc_cali_ok && adc_cali_raw_to_voltage(s_adc_cali, raw, &mv) == ESP_OK) {
        *out_mv = (uint32_t)mv;
    } else {
        *out_mv = (uint32_t)raw * 3300 / 4095;
    }
    return true;
}

void gpio_driver_release(uint8_t gpio) {
    if(gpio >= GPIO_MAX_NUM) return;
    GpioPin gp = pin_of(gpio);
    /* TASK 4/5: stop pwm / free adc here */
    if(s_pwm_ch[gpio]) gpio_driver_stop_pwm(gpio);
    furi_hal_gpio_init(&gp, GpioModeAnalog, GpioPullNo, GpioSpeedVeryHigh);
    s_touched[gpio] = false;
}

void gpio_driver_release_all(void) {
    for(uint8_t g = 0; g < GPIO_MAX_NUM; g++) if(s_touched[g]) gpio_driver_release(g);

    /* Tear down the ADC1 oneshot unit + calibration scheme if we created them. */
    if(s_adc_cali_ok) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_delete_scheme_curve_fitting(s_adc_cali);
#endif
        s_adc_cali = NULL;
        s_adc_cali_ok = false;
    }
    if(s_adc_unit != NULL) {
        adc_oneshot_del_unit(s_adc_unit);
        s_adc_unit = NULL;
    }
}
