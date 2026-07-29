#include "gpio_manual_edit.h"
#include "../gpio_app_i.h"
#include "../gpio_pin_catalog.h"
#include "../gpio_driver.h"

#include <furi.h>
#include <gui/modules/variable_item_list.h>

/* Mode option order maps 1:1 onto GpioPinMode {Off,Output,Input,Pwm,Adc}. The
 * PWM option is only offered for pins whose caps include GpioCapPwm, and ADC
 * only for pins with GpioCapAdc (which, in this catalog, always also have PWM,
 * keeping the option indices contiguous with the enum). */
static const char* const mode_names[] = {"Off", "Output", "Input", "PWM", "ADC"};
static const char* const level_names[] = {"LOW", "HIGH"};
static const char* const pull_names[] = {"None", "Up", "Down"};

/* PWM frequency steps (Hz) offered in the editor. */
static const uint32_t pwm_freq_values[] = {1, 10, 100, 1000, 5000, 10000};
static const char* const pwm_freq_names[] = {"1", "10", "100", "1000", "5000", "10000"};
#define PWM_FREQ_COUNT (sizeof(pwm_freq_values) / sizeof(pwm_freq_values[0]))
#define PWM_FREQ_DEFAULT_IDX 3 // 1000 Hz
#define PWM_DUTY_STEP 5
#define PWM_DUTY_COUNT 21 // 0..100 in 5% steps

struct GpioManualEdit {
    VariableItemList* vil;
    GpioApp* app;
    size_t index; // catalog index currently edited
    VariableItem* mode_item;
    VariableItem* level_item;
    VariableItem* pull_item;
    VariableItem* read_item;
    VariableItem* freq_item; // PWM only (NULL if pin lacks GpioCapPwm)
    VariableItem* duty_item; // PWM only
    VariableItem* pwm_status_item; // PWM only (shows OK / No free ch)
    VariableItem* voltage_item; // ADC only (NULL if pin lacks GpioCapAdc)
    bool pwm_no_channel; // last set_pwm failed to allocate a channel
};

/* Refresh the read-only ADC voltage row from a fresh sample (X.XX V), or '-'
 * when not in ADC mode / read failed. */
static void gpio_manual_edit_update_voltage(GpioManualEdit* instance) {
    if(!instance->voltage_item) return;
    const GpioCatalogEntry* e =
        gpio_catalog_get(instance->index, instance->app->show_advanced);
    GpioPinState* st = &instance->app->pin_state[instance->index];
    uint32_t mv = 0;
    if(e && st->mode == GpioPinModeAdc && gpio_driver_read_adc(e->adc_ch, &mv)) {
        char b[12];
        snprintf(b, sizeof(b), "%lu.%02lu V", (unsigned long)(mv / 1000),
                 (unsigned long)((mv % 1000) / 10));
        variable_item_set_current_value_text(instance->voltage_item, b);
    } else {
        variable_item_set_current_value_text(instance->voltage_item, "-");
    }
}

/* Reflect the PWM channel-allocation result on the status row. */
static void gpio_manual_edit_update_pwm_status(GpioManualEdit* instance) {
    if(!instance->pwm_status_item) return;
    GpioPinState* st = &instance->app->pin_state[instance->index];
    if(st->mode != GpioPinModePwm) {
        variable_item_set_current_value_text(instance->pwm_status_item, "-");
    } else {
        variable_item_set_current_value_text(
            instance->pwm_status_item, instance->pwm_no_channel ? "No free ch" : "OK");
    }
}

static void gpio_manual_edit_apply(GpioManualEdit* instance) {
    GpioApp* app = instance->app;
    const GpioCatalogEntry* e = gpio_catalog_get(instance->index, app->show_advanced);
    if(!e) return;
    GpioPinState* st = &app->pin_state[instance->index];
    instance->pwm_no_channel = false;
    if(st->mode == GpioPinModeOutput) {
        gpio_driver_stop_pwm(e->gpio); // release any PWM channel we were holding
        gpio_driver_set_output(e->gpio, st->out_level);
    } else if(st->mode == GpioPinModeInput) {
        gpio_driver_stop_pwm(e->gpio);
        gpio_driver_set_input(e->gpio, st->pull);
    } else if(st->mode == GpioPinModePwm) {
        instance->pwm_no_channel = !gpio_driver_set_pwm(e->gpio, st->pwm_freq, st->pwm_duty);
    } else if(st->mode == GpioPinModeAdc) {
        /* ADC drives nothing; just release any PWM channel we held. The oneshot
         * driver claims the pad lazily on the first read (in the tick). */
        gpio_driver_stop_pwm(e->gpio);
    } else {
        gpio_driver_release(e->gpio); // Off — release() stops PWM internally
    }
    gpio_manual_edit_update_pwm_status(instance);
    gpio_manual_edit_update_voltage(instance);
}

static void gpio_manual_edit_mode_cb(VariableItem* item) {
    GpioManualEdit* instance = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, mode_names[idx]);
    GpioPinState* st = &instance->app->pin_state[instance->index];
    st->mode = (GpioPinMode)idx; // Off=0, Output=1, Input=2, Pwm=3
    gpio_manual_edit_apply(instance);
}

static void gpio_manual_edit_freq_cb(VariableItem* item) {
    GpioManualEdit* instance = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= PWM_FREQ_COUNT) idx = PWM_FREQ_COUNT - 1;
    variable_item_set_current_value_text(item, pwm_freq_names[idx]);
    GpioPinState* st = &instance->app->pin_state[instance->index];
    st->pwm_freq = pwm_freq_values[idx];
    if(st->mode == GpioPinModePwm) {
        const GpioCatalogEntry* e =
            gpio_catalog_get(instance->index, instance->app->show_advanced);
        if(e) {
            instance->pwm_no_channel = !gpio_driver_set_pwm(e->gpio, st->pwm_freq, st->pwm_duty);
            gpio_manual_edit_update_pwm_status(instance);
        }
    }
}

static void gpio_manual_edit_duty_cb(VariableItem* item) {
    GpioManualEdit* instance = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= PWM_DUTY_COUNT) idx = PWM_DUTY_COUNT - 1;
    uint8_t duty = idx * PWM_DUTY_STEP;
    char b[8];
    snprintf(b, sizeof(b), "%u%%", (unsigned)duty);
    variable_item_set_current_value_text(item, b);
    GpioPinState* st = &instance->app->pin_state[instance->index];
    st->pwm_duty = duty;
    if(st->mode == GpioPinModePwm) {
        const GpioCatalogEntry* e =
            gpio_catalog_get(instance->index, instance->app->show_advanced);
        if(e) {
            instance->pwm_no_channel = !gpio_driver_set_pwm(e->gpio, st->pwm_freq, st->pwm_duty);
            gpio_manual_edit_update_pwm_status(instance);
        }
    }
}

static void gpio_manual_edit_level_cb(VariableItem* item) {
    GpioManualEdit* instance = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, level_names[idx]);
    GpioPinState* st = &instance->app->pin_state[instance->index];
    st->out_level = (idx != 0);
    if(st->mode == GpioPinModeOutput) {
        const GpioCatalogEntry* e = gpio_catalog_get(instance->index, instance->app->show_advanced);
        if(e) gpio_driver_set_output(e->gpio, st->out_level);
    }
}

static void gpio_manual_edit_pull_cb(VariableItem* item) {
    GpioManualEdit* instance = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, pull_names[idx]);
    GpioPinState* st = &instance->app->pin_state[instance->index];
    st->pull = (GpioDriverPull)idx; // None=0, Up=1, Down=2
    if(st->mode == GpioPinModeInput) {
        const GpioCatalogEntry* e = gpio_catalog_get(instance->index, instance->app->show_advanced);
        if(e) gpio_driver_set_input(e->gpio, st->pull);
    }
}

GpioManualEdit* gpio_manual_edit_alloc(void) {
    GpioManualEdit* instance = malloc(sizeof(GpioManualEdit));
    instance->vil = variable_item_list_alloc();
    instance->app = NULL;
    instance->index = 0;
    instance->mode_item = NULL;
    instance->level_item = NULL;
    instance->pull_item = NULL;
    instance->read_item = NULL;
    instance->freq_item = NULL;
    instance->duty_item = NULL;
    instance->pwm_status_item = NULL;
    instance->voltage_item = NULL;
    instance->pwm_no_channel = false;
    return instance;
}

void gpio_manual_edit_free(GpioManualEdit* instance) {
    furi_assert(instance);
    variable_item_list_free(instance->vil);
    free(instance);
}

View* gpio_manual_edit_get_view(GpioManualEdit* instance) {
    furi_assert(instance);
    return variable_item_list_get_view(instance->vil);
}

void gpio_manual_edit_set_context(GpioManualEdit* instance, GpioApp* app) {
    furi_assert(instance);
    instance->app = app;
    instance->index = app->edit_index;

    VariableItemList* vil = instance->vil;
    variable_item_list_reset(vil);

    const GpioCatalogEntry* e = gpio_catalog_get(instance->index, app->show_advanced);
    GpioPinState* st = &app->pin_state[instance->index];
    VariableItem* item;

    instance->freq_item = NULL;
    instance->duty_item = NULL;
    instance->pwm_status_item = NULL;
    instance->voltage_item = NULL;
    instance->pwm_no_channel = false;

    bool has_pwm = e && (e->caps & GpioCapPwm);
    bool has_adc = e && (e->caps & GpioCapAdc);

    /* Warning shown as the first (inert) line, if the pin has one. */
    if(e && e->warn) {
        item = variable_item_list_add(vil, e->warn, 1, NULL, instance);
        variable_item_set_current_value_text(item, "!");
    }

    /* Mode: Off / Output / Input, plus PWM when the pin is PWM-capable.
       TASK 5: add ADC mode option when GpioCapAdc present. */
    /* ADC extends the cycle to index 4; it is only offered together with PWM so
       the option indices stay contiguous with GpioPinMode. */
    bool has_adc_mode = has_pwm && has_adc;
    uint8_t mode_max = has_adc_mode ? GpioPinModeAdc :
                       has_pwm      ? GpioPinModePwm :
                                      GpioPinModeInput;
    uint8_t mode_count = has_adc_mode ? 5 : has_pwm ? 4 : 3;
    uint8_t mode_idx = (st->mode <= mode_max) ? (uint8_t)st->mode : 0;
    item = variable_item_list_add(vil, "Mode", mode_count, gpio_manual_edit_mode_cb, instance);
    variable_item_set_current_value_index(item, mode_idx);
    variable_item_set_current_value_text(item, mode_names[mode_idx]);
    instance->mode_item = item;

    /* Level (applies in Output mode). */
    item = variable_item_list_add(vil, "Level", 2, gpio_manual_edit_level_cb, instance);
    variable_item_set_current_value_index(item, st->out_level ? 1 : 0);
    variable_item_set_current_value_text(item, level_names[st->out_level ? 1 : 0]);
    instance->level_item = item;

    /* Pull (applies in Input mode). */
    item = variable_item_list_add(vil, "Pull", 3, gpio_manual_edit_pull_cb, instance);
    variable_item_set_current_value_index(item, (uint8_t)st->pull);
    variable_item_set_current_value_text(item, pull_names[(uint8_t)st->pull]);
    instance->pull_item = item;

    /* Read (read-only live value). */
    item = variable_item_list_add(vil, "Read", 1, NULL, instance);
    {
        bool v = e ? gpio_driver_read_digital(e->gpio) : false;
        variable_item_set_current_value_text(item, v ? "1" : "0");
    }
    instance->read_item = item;

    /* PWM rows: only present for PWM-capable pins. Like Level/Pull, these are
       always shown but only take effect while Mode == PWM. */
    if(has_pwm) {
        /* Freq (stepped list). */
        uint8_t fidx = PWM_FREQ_DEFAULT_IDX;
        for(uint8_t k = 0; k < PWM_FREQ_COUNT; k++) {
            if(pwm_freq_values[k] == st->pwm_freq) {
                fidx = k;
                break;
            }
        }
        item = variable_item_list_add(vil, "Freq", PWM_FREQ_COUNT, gpio_manual_edit_freq_cb, instance);
        variable_item_set_current_value_index(item, fidx);
        variable_item_set_current_value_text(item, pwm_freq_names[fidx]);
        instance->freq_item = item;

        /* Duty (0..100% in 5% steps). */
        uint8_t didx = st->pwm_duty / PWM_DUTY_STEP;
        if(didx >= PWM_DUTY_COUNT) didx = PWM_DUTY_COUNT - 1;
        item = variable_item_list_add(vil, "Duty", PWM_DUTY_COUNT, gpio_manual_edit_duty_cb, instance);
        variable_item_set_current_value_index(item, didx);
        {
            char b[8];
            snprintf(b, sizeof(b), "%u%%", (unsigned)(didx * PWM_DUTY_STEP));
            variable_item_set_current_value_text(item, b);
        }
        instance->duty_item = item;

        /* PWM status (inert): OK / No free ch while in PWM mode, else '-'. */
        item = variable_item_list_add(vil, "PWM", 1, NULL, instance);
        instance->pwm_status_item = item;
        gpio_manual_edit_update_pwm_status(instance);
    }

    /* Voltage (inert, read-only): live ADC reading while Mode == ADC, else '-'. */
    if(has_adc_mode) {
        item = variable_item_list_add(vil, "Voltage", 1, NULL, instance);
        instance->voltage_item = item;
        gpio_manual_edit_update_voltage(instance);
    }
}

void gpio_manual_edit_tick(GpioManualEdit* instance) {
    furi_assert(instance);
    if(!instance->app || !instance->read_item) return;
    const GpioCatalogEntry* e = gpio_catalog_get(instance->index, instance->app->show_advanced);
    if(!e) return;
    bool v = gpio_driver_read_digital(e->gpio);
    variable_item_set_current_value_text(instance->read_item, v ? "1" : "0");
    gpio_manual_edit_update_voltage(instance);
}
