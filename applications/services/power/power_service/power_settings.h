#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t auto_poweroff_delay_ms;
    uint8_t charge_supress_percent;
    uint32_t idle_dim_delay_ms; /* 0 = off */
    uint32_t light_sleep_delay_ms; /* 0 = off */
} PowerSettings;

#ifdef __cplusplus
extern "C" {
#endif

void power_settings_load(PowerSettings* settings);
void power_settings_save(const PowerSettings* settings);

#ifdef __cplusplus
}
#endif
