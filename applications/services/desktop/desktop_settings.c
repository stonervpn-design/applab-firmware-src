#include "desktop_settings.h"
#include "desktop_settings_filename.h"

#include <saved_struct.h>
#include <storage/storage.h>

#define TAG "DesktopSettings"

#define DESKTOP_SETTINGS_VER_14 (14)
#define DESKTOP_SETTINGS_VER_17 (17)
#define DESKTOP_SETTINGS_VER_18 (18)
#define DESKTOP_SETTINGS_VER    (19)

#define DESKTOP_SETTINGS_PATH  INT_PATH(DESKTOP_SETTINGS_FILE_NAME)
#define DESKTOP_SETTINGS_MAGIC (0x17)

typedef struct {
    uint32_t auto_lock_delay_ms;
    uint8_t displayBatteryPercentage;
    uint8_t dummy_mode;
    uint8_t display_clock;
    FavoriteApp favorite_apps[FavoriteAppNumber];
    FavoriteApp dummy_apps[DummyAppNumber];
} DesktopSettingsV14;

// DesktopSettings as it existed at version 17, i.e. before Task 2 added the
// anti-theft wipe fields (pin_fail_count / pin_fails_before_wipe /
// wipe_enabled). Needed so pre-existing v17 settings files on already-flashed
// devices still load instead of being discarded.
typedef struct {
    uint32_t auto_lock_delay_ms;
    uint8_t usb_inhibit_auto_lock;
    uint8_t displayBatteryPercentage;
    uint8_t dummy_mode;
    uint8_t display_clock;
    FavoriteApp favorite_apps[FavoriteAppNumber];
    FavoriteApp dummy_apps[DummyAppNumber];
} DesktopSettingsV17;

// DesktopSettings as it existed at version 18, i.e. after Task 2 added the
// anti-theft wipe fields but before Task 3 added lock_on_boot. Needed so
// pre-existing v18 settings files on already-flashed devices still load
// instead of being discarded.
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
} DesktopSettingsV18;

// Actual size of DesktopSettings v13
//static_assert(sizeof(DesktopSettingsV13) == 1234);

void desktop_settings_load(DesktopSettings* settings) {
    furi_assert(settings);

    bool success = false;

    do {
        uint8_t version;
        if(!saved_struct_get_metadata(DESKTOP_SETTINGS_PATH, NULL, &version, NULL)) break;

        if(version == DESKTOP_SETTINGS_VER) {
            success = saved_struct_load(
                DESKTOP_SETTINGS_PATH,
                settings,
                sizeof(DesktopSettings),
                DESKTOP_SETTINGS_MAGIC,
                DESKTOP_SETTINGS_VER);

        } else if(version == DESKTOP_SETTINGS_VER_18) {
            DesktopSettingsV18* settings_v18 = malloc(sizeof(DesktopSettingsV18));

            success = saved_struct_load(
                DESKTOP_SETTINGS_PATH,
                settings_v18,
                sizeof(DesktopSettingsV18),
                DESKTOP_SETTINGS_MAGIC,
                DESKTOP_SETTINGS_VER_18);

            if(success) {
                settings->auto_lock_delay_ms = settings_v18->auto_lock_delay_ms;
                settings->usb_inhibit_auto_lock = settings_v18->usb_inhibit_auto_lock;
                settings->displayBatteryPercentage = settings_v18->displayBatteryPercentage;
                settings->dummy_mode = settings_v18->dummy_mode;
                settings->display_clock = settings_v18->display_clock;
                memcpy(
                    settings->favorite_apps,
                    settings_v18->favorite_apps,
                    sizeof(settings->favorite_apps));
                memcpy(
                    settings->dummy_apps, settings_v18->dummy_apps, sizeof(settings->dummy_apps));
                settings->pin_fail_count = settings_v18->pin_fail_count;
                settings->pin_fails_before_wipe = settings_v18->pin_fails_before_wipe;
                settings->wipe_enabled = settings_v18->wipe_enabled;
                // New in v19: lock-on-boot. Default it since v18 files
                // predate this feature. Default true so upgraded devices
                // that already had a PIN set keep locking on boot (matches
                // the pre-Task-3 unconditional behavior).
                settings->lock_on_boot = true;
            }

            free(settings_v18);

        } else if(version == DESKTOP_SETTINGS_VER_17) {
            DesktopSettingsV17* settings_v17 = malloc(sizeof(DesktopSettingsV17));

            success = saved_struct_load(
                DESKTOP_SETTINGS_PATH,
                settings_v17,
                sizeof(DesktopSettingsV17),
                DESKTOP_SETTINGS_MAGIC,
                DESKTOP_SETTINGS_VER_17);

            if(success) {
                settings->auto_lock_delay_ms = settings_v17->auto_lock_delay_ms;
                settings->usb_inhibit_auto_lock = settings_v17->usb_inhibit_auto_lock;
                settings->displayBatteryPercentage = settings_v17->displayBatteryPercentage;
                settings->dummy_mode = settings_v17->dummy_mode;
                settings->display_clock = settings_v17->display_clock;
                memcpy(
                    settings->favorite_apps,
                    settings_v17->favorite_apps,
                    sizeof(settings->favorite_apps));
                memcpy(
                    settings->dummy_apps, settings_v17->dummy_apps, sizeof(settings->dummy_apps));
                // New in v18: anti-theft wipe fields. Default them since v17
                // files predate this feature.
                settings->pin_fail_count = 0;
                settings->pin_fails_before_wipe = SECURITY_WIPE_DEFAULT_FAILS_BEFORE_WIPE;
                settings->wipe_enabled = true;
                // New in v19: lock-on-boot. Default it since v17 files
                // predate this feature.
                settings->lock_on_boot = true;
            }

            free(settings_v17);

        } else if(version == DESKTOP_SETTINGS_VER_14) {
            DesktopSettingsV14* settings_v14 = malloc(sizeof(DesktopSettingsV14));

            success = saved_struct_load(
                DESKTOP_SETTINGS_PATH,
                settings_v14,
                sizeof(DesktopSettingsV14),
                DESKTOP_SETTINGS_MAGIC,
                DESKTOP_SETTINGS_VER_14);

            if(success) {
                settings->auto_lock_delay_ms = settings_v14->auto_lock_delay_ms;
                settings->usb_inhibit_auto_lock = 0;
                settings->displayBatteryPercentage = settings_v14->displayBatteryPercentage;
                settings->dummy_mode = settings_v14->dummy_mode;
                settings->display_clock = settings_v14->display_clock;
                memcpy(
                    settings->favorite_apps,
                    settings_v14->favorite_apps,
                    sizeof(settings->favorite_apps));
                memcpy(
                    settings->dummy_apps, settings_v14->dummy_apps, sizeof(settings->dummy_apps));
                // New in v18: anti-theft wipe fields. Default them since v14
                // files predate this feature.
                settings->pin_fail_count = 0;
                settings->pin_fails_before_wipe = SECURITY_WIPE_DEFAULT_FAILS_BEFORE_WIPE;
                settings->wipe_enabled = true;
                // New in v19: lock-on-boot. Default it since v14 files
                // predate this feature.
                settings->lock_on_boot = true;
            }

            free(settings_v14);
        }

    } while(false);

    if(!success) {
        FURI_LOG_W(TAG, "Failed to load file, using defaults");
        memset(settings, 0, sizeof(DesktopSettings));
        // Default the status-bar battery to the numeric percent inside the icon (100/60/..)
        // rather than the plain fill bar. Changeable in Settings > Desktop > Battery view.
        settings->displayBatteryPercentage = DISPLAY_BATTERY_PERCENT;
        // Anti-theft defaults for a fresh settings file: wipe armed at 10
        // consecutive wrong PINs.
        settings->pin_fail_count = 0;
        settings->pin_fails_before_wipe = SECURITY_WIPE_DEFAULT_FAILS_BEFORE_WIPE;
        settings->wipe_enabled = true;
        // Lock-on-boot defaults to enabled: if a PIN is set, the device
        // should come up locked.
        settings->lock_on_boot = true;
        desktop_settings_save(settings);
    }
}

void desktop_settings_save(const DesktopSettings* settings) {
    furi_assert(settings);

    const bool success = saved_struct_save(
        DESKTOP_SETTINGS_PATH,
        settings,
        sizeof(DesktopSettings),
        DESKTOP_SETTINGS_MAGIC,
        DESKTOP_SETTINGS_VER);

    if(!success) {
        FURI_LOG_E(TAG, "Failed to save file");
    }
}
