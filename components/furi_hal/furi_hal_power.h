#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <core/common_defines.h>
#include <property.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FuriHalPowerICCharger,
    FuriHalPowerICFuelGauge,
} FuriHalPowerIC;

void furi_hal_power_init(void);
bool furi_hal_power_gauge_is_ok(void);
bool furi_hal_power_is_shutdown_requested(void);

/** Configure Dynamic Frequency Scaling (DFS): CPU scales between a min floor and
 * the board's max (240 MHz) to save battery when idle. Light sleep stays OFF.
 * Call once at boot after furi_hal_power_init(). No-op if CONFIG_PM_ENABLE is off. */
void furi_hal_power_pm_init(void);

/** Hold/release the CPU at max frequency for a timing-critical section (RF TX/RX,
 * etc.). Counted internally, so nested/independent callers are safe as long as each
 * acquire is matched by a release. Callers that toggle by state should guard with a
 * local bool so they acquire at most once. No-op if PM is disabled. */
void furi_hal_power_perf_lock_acquire(void);
void furi_hal_power_perf_lock_release(void);

/** Drop to / restore from the DFS min frequency. `idle=true` releases the always-on
 * boot hold so the CPU can scale down; `idle=false` re-takes it (back to max). Driven
 * by the desktop idle manager, and only in a confirmed-safe idle state (foreground
 * desktop, no app/RF, BT off, USB off); the always-on input hook calls it with false
 * so any keypress restores max immediately. Thread-safe. No-op if PM is disabled. */
void furi_hal_power_perf_idle_set(bool idle);

uint16_t furi_hal_power_insomnia_level(void);
void furi_hal_power_insomnia_enter(void);
void furi_hal_power_insomnia_exit(void);
bool furi_hal_power_sleep_available(void);
void furi_hal_power_sleep(void);

/** Seconds since boot from a clock that keeps advancing through light sleep
 * (esp_timer, unlike the FreeRTOS tick which freezes). Used by the idle manager
 * to time its idle window across sleep/wake cycles. */
uint32_t furi_hal_power_uptime_sec(void);

/** Enter ESP32 light sleep until a key / BOOT-button GPIO wake or USB connect.
 *
 * Blanks the backlight + status LED and puts the panel to sleep, then loops
 * light-sleep chunks (15 s watchdog) until a real wake event. Restores the
 * panel and backlight on return. No-op returning false while USB is present.
 *
 * @return  true if it slept and woke; false if inhibited (USB present).
 */
bool furi_hal_power_light_sleep(void);

/** Public USB-present query (wraps the internal supply-voltage check). Used by
 * the desktop idle manager to gate dim/light-sleep: idle behavior arms only
 * while USB is disconnected. */
bool furi_hal_power_is_usb_connected(void);

uint8_t furi_hal_power_get_pct(void);
uint8_t furi_hal_power_get_bat_health_pct(void);
bool furi_hal_power_is_charging(void);
bool furi_hal_power_is_charging_done(void);

void furi_hal_power_shutdown(void);
void furi_hal_power_off(void);
FURI_NORETURN void furi_hal_power_reset(void);

bool furi_hal_power_enable_otg(void);
void furi_hal_power_disable_otg(void);
bool furi_hal_power_check_otg_fault(void);
void furi_hal_power_check_otg_status(void);
bool furi_hal_power_is_otg_enabled(void);

float furi_hal_power_get_battery_charge_voltage_limit(void);
void furi_hal_power_set_battery_charge_voltage_limit(float voltage);

uint32_t furi_hal_power_get_battery_remaining_capacity(void);
uint32_t furi_hal_power_get_battery_full_capacity(void);
uint32_t furi_hal_power_get_battery_design_capacity(void);

float furi_hal_power_get_battery_voltage(FuriHalPowerIC ic);
float furi_hal_power_get_battery_current(FuriHalPowerIC ic);
float furi_hal_power_get_battery_temperature(FuriHalPowerIC ic);
float furi_hal_power_get_usb_voltage(void);

void furi_hal_power_enable_external_3_3v(void);
void furi_hal_power_disable_external_3_3v(void);
void furi_hal_power_suppress_charge_enter(void);
void furi_hal_power_suppress_charge_exit(void);

void furi_hal_power_info_get(PropertyValueCallback callback, char sep, void* context);
void furi_hal_power_debug_get(PropertyValueCallback callback, void* context);

#ifdef __cplusplus
}
#endif
