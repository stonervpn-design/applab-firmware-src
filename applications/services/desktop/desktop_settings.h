#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "helpers/asset_pack.h"

#define DISPLAY_BATTERY_BAR              0
#define DISPLAY_BATTERY_PERCENT          1
#define DISPLAY_BATTERY_INVERTED_PERCENT 2
#define DISPLAY_BATTERY_RETRO_3          3
#define DISPLAY_BATTERY_RETRO_5          4
#define DISPLAY_BATTERY_BAR_PERCENT      5

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FavoriteAppLeftShort,
    FavoriteAppLeftLong,
    FavoriteAppRightShort,
    FavoriteAppRightLong,
    FavoriteAppOkLong,

    FavoriteAppNumber,
} FavoriteAppShortcut;

typedef enum {
    DummyAppLeftShort,
    DummyAppLeftLong,
    DummyAppRightShort,
    DummyAppRightLong,
    DummyAppUpLong,
    DummyAppDownShort,
    DummyAppDownLong,
    DummyAppOkShort,
    DummyAppOkLong,

    DummyAppNumber,
} DummyAppShortcut;

typedef struct {
    char name_or_path[128];
} FavoriteApp;

// Anti-theft: consecutive wrong-PIN counter + wipe threshold. Persisted in
// this flash-backed saved-struct (NOT furi_hal_rtc) because on this ESP32
// port furi_hal_rtc's pin_fails/pin_value are plain in-RAM emulation state
// (components/furi_hal/furi_hal_rtc.c) that resets on every reboot, let
// alone a battery pull. DesktopSettings is loaded/saved via saved_struct on
// internal flash (/int/.desktop.settings) and survives full power-off.
#define SECURITY_WIPE_DEFAULT_FAILS_BEFORE_WIPE (10)

typedef struct {
    uint32_t auto_lock_delay_ms;
    uint8_t usb_inhibit_auto_lock;
    uint8_t displayBatteryPercentage;
    uint8_t dummy_mode;
    uint8_t display_clock;
    FavoriteApp favorite_apps[FavoriteAppNumber];
    FavoriteApp dummy_apps[DummyAppNumber];
    uint8_t pin_fail_count;
    uint8_t pin_fails_before_wipe;
    bool wipe_enabled;
    bool lock_on_boot;
    // Selected Asset Pack name, or "" for the built-in animation set.
    // Resolved to an Anims directory via asset_pack_anims_dir().
    char asset_pack[ASSET_PACK_NAME_LEN];
} DesktopSettings;

void desktop_settings_load(DesktopSettings* settings);
void desktop_settings_save(const DesktopSettings* settings);

#ifdef __cplusplus
}
#endif
