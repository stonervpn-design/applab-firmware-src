#include "power_settings.h"
#include "power_settings_filename.h"

#include <saved_struct.h>
#include <storage/storage.h>

#define TAG "PowerSettings"

#define POWER_SETTINGS_VER_1 (1) // Oldest version number
#define POWER_SETTINGS_VER_2 (2) // Previous version number
#define POWER_SETTINGS_VER   (3) // New version number

#define POWER_SETTINGS_PATH     INT_PATH(POWER_SETTINGS_FILE_NAME)
#define POWER_SETTINGS_MAGIC_V1 (0x19)
#define POWER_SETTINGS_MAGIC_V2 (0x21)
#define POWER_SETTINGS_MAGIC    (0x22)

// Defaults for fields added in POWER_SETTINGS_VER 3
#define POWER_SETTINGS_DEFAULT_IDLE_DIM_DELAY_MS    (30000)
#define POWER_SETTINGS_DEFAULT_LIGHT_SLEEP_DELAY_MS (120000)

typedef struct {
    uint32_t auto_poweroff_delay_ms;
} PowerSettingsV1;

typedef struct {
    uint32_t auto_poweroff_delay_ms;
    uint8_t charge_supress_percent;
} PowerSettingsV2;

// Initialize the fields introduced in POWER_SETTINGS_VER 3 to their defaults.
static void power_settings_init_new_fields(PowerSettings* settings) {
    settings->idle_dim_delay_ms = POWER_SETTINGS_DEFAULT_IDLE_DIM_DELAY_MS;
    settings->light_sleep_delay_ms = POWER_SETTINGS_DEFAULT_LIGHT_SLEEP_DELAY_MS;
}

void power_settings_load(PowerSettings* settings) {
    furi_assert(settings);

    bool success = false;

    do {
        uint8_t version;
        if(!saved_struct_get_metadata(POWER_SETTINGS_PATH, NULL, &version, NULL)) break;

        // if config actual version - load it directly
        if(version == POWER_SETTINGS_VER) {
            success = saved_struct_load(
                POWER_SETTINGS_PATH,
                settings,
                sizeof(PowerSettings),
                POWER_SETTINGS_MAGIC,
                POWER_SETTINGS_VER);

            // if config previous version (v2) - load it and default the new fields
        } else if(version == POWER_SETTINGS_VER_2) {
            PowerSettingsV2* settings_v2 = malloc(sizeof(PowerSettingsV2));

            success = saved_struct_load(
                POWER_SETTINGS_PATH,
                settings_v2,
                sizeof(PowerSettingsV2),
                POWER_SETTINGS_MAGIC_V2,
                POWER_SETTINGS_VER_2);
            // new settings initialization
            if(success) {
                settings->auto_poweroff_delay_ms = settings_v2->auto_poweroff_delay_ms;
                settings->charge_supress_percent = settings_v2->charge_supress_percent;
                power_settings_init_new_fields(settings);
            }

            free(settings_v2);

            // if config oldest version (v1) - load it and default the newer fields
        } else if(version == POWER_SETTINGS_VER_1) {
            PowerSettingsV1* settings_v1 = malloc(sizeof(PowerSettingsV1));

            success = saved_struct_load(
                POWER_SETTINGS_PATH,
                settings_v1,
                sizeof(PowerSettingsV1),
                POWER_SETTINGS_MAGIC_V1,
                POWER_SETTINGS_VER_1);
            // new settings initialization
            if(success) {
                settings->auto_poweroff_delay_ms = settings_v1->auto_poweroff_delay_ms;
                settings->charge_supress_percent = 0;
                power_settings_init_new_fields(settings);
            }

            free(settings_v1);
        }

    } while(false);

    if(!success) {
        FURI_LOG_W(TAG, "Failed to load file, using defaults");
        memset(settings, 0, sizeof(PowerSettings));
        power_settings_init_new_fields(settings);
        power_settings_save(settings);
    }
}

void power_settings_save(const PowerSettings* settings) {
    furi_assert(settings);

    const bool success = saved_struct_save(
        POWER_SETTINGS_PATH,
        settings,
        sizeof(PowerSettings),
        POWER_SETTINGS_MAGIC,
        POWER_SETTINGS_VER);

    if(!success) {
        FURI_LOG_E(TAG, "Failed to save file");
    }
}
