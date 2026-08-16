#include <applications.h>
#include <lib/toolbox/value_index.h>

#include "../desktop_settings_app.h"
#include "desktop_settings_scene.h"
#include "desktop_settings_scene_i.h"
#include <power/power_service/power.h>

typedef enum {
    DesktopSettingsPinSetup = 0,
    DesktopSettingsLockOnBoot,
    DesktopSettingsWipeEnabled,
    DesktopSettingsPinFailsBeforeWipe,
    DesktopSettingsAutoLockDelay,
    DesktopSettingsAutoPowerOff,
    DesktopSettingsBatteryDisplay,
    DesktopSettingsClockDisplay,
    DesktopSettingsChangeName,
    DesktopSettingsHappyMode,
    DesktopSettingsFavoriteLeftShort,
    DesktopSettingsFavoriteLeftLong,
    DesktopSettingsFavoriteRightShort,
    DesktopSettingsFavoriteRightLong,
    DesktopSettingsFavoriteOkLong,
    DesktopSettingsDummyLeft,
    DesktopSettingsDummyLeftLong,
    DesktopSettingsDummyRight,
    DesktopSettingsDummyRightLong,
    DesktopSettingsDummyUpLong,
    DesktopSettingsDummyDown,
    DesktopSettingsDummyDownLong,
    DesktopSettingsDummyOk,
    DesktopSettingsDummyOkLong,
} DesktopSettingsEntry;

#define AUTO_LOCK_DELAY_COUNT 9
const char* const auto_lock_delay_text[AUTO_LOCK_DELAY_COUNT] = {
    "OFF",
    "10s",
    "15s",
    "30s",
    "60s",
    "90s",
    "2min",
    "5min",
    "10min",
};

const uint32_t auto_lock_delay_value[AUTO_LOCK_DELAY_COUNT] =
    {0, 10000, 15000, 30000, 60000, 90000, 120000, 300000, 600000};

#define USB_INHIBIT_AUTO_LOCK_DELAY_COUNT 2

const char* const usb_inhibit_auto_lock_delay_text[USB_INHIBIT_AUTO_LOCK_DELAY_COUNT] = {
    "OFF",
    "ON",
};

const uint32_t usb_inhibit_auto_lock_delay_value[USB_INHIBIT_AUTO_LOCK_DELAY_COUNT] = {0, 1};

#define CLOCK_ENABLE_COUNT 2
const char* const clock_enable_text[CLOCK_ENABLE_COUNT] = {
    "OFF",
    "ON",
};

const uint32_t clock_enable_value[CLOCK_ENABLE_COUNT] = {0, 1};

#define BATTERY_VIEW_COUNT 6

const char* const battery_view_count_text[BATTERY_VIEW_COUNT] =
    {"Bar", "%", "Inv. %", "Retro 3", "Retro 5", "Bar %"};

const uint32_t displayBatteryPercentage_value[BATTERY_VIEW_COUNT] = {
    DISPLAY_BATTERY_BAR,
    DISPLAY_BATTERY_PERCENT,
    DISPLAY_BATTERY_INVERTED_PERCENT,
    DISPLAY_BATTERY_RETRO_3,
    DISPLAY_BATTERY_RETRO_5,
    DISPLAY_BATTERY_BAR_PERCENT};

#define LOCK_ON_BOOT_COUNT 2
const char* const lock_on_boot_text[LOCK_ON_BOOT_COUNT] = {
    "OFF",
    "ON",
};

const uint32_t lock_on_boot_value[LOCK_ON_BOOT_COUNT] = {0, 1};

#define WIPE_ENABLED_COUNT 2
const char* const wipe_enabled_text[WIPE_ENABLED_COUNT] = {
    "OFF",
    "ON",
};

const uint32_t wipe_enabled_value[WIPE_ENABLED_COUNT] = {0, 1};

#define PIN_FAILS_BEFORE_WIPE_COUNT 5
const char* const pin_fails_before_wipe_text[PIN_FAILS_BEFORE_WIPE_COUNT] = {
    "Off",
    "3",
    "5",
    "10",
    "15",
};

const uint32_t pin_fails_before_wipe_value[PIN_FAILS_BEFORE_WIPE_COUNT] = {0, 3, 5, 10, 15};

// Asset Pack selector. Unlike the compile-time text/value tables above, the
// pack list is discovered at runtime from the SD card (asset_pack_enumerate),
// so it can't be a `const` array. These are file-scope statics rather than
// DesktopSettingsApp fields because their lifetime only needs to span
// on_enter -> the VariableItem "changed" callback -> on_exit of THIS scene
// (repopulated fresh every time the scene is entered), matching how the
// VariableItemList itself is torn down/rebuilt per scene visit.
#define ASSET_PACK_SETTINGS_MAX_PACKS 16
static char asset_pack_settings_names[ASSET_PACK_SETTINGS_MAX_PACKS][ASSET_PACK_NAME_LEN];
static uint8_t asset_pack_settings_count = 0;

static void desktop_settings_scene_start_var_list_enter_callback(void* context, uint32_t index) {
    DesktopSettingsApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static void desktop_settings_scene_start_battery_view_changed(VariableItem* item) {
    DesktopSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, battery_view_count_text[index]);
    app->settings.displayBatteryPercentage = index;
}

static void desktop_settings_scene_start_clock_enable_changed(VariableItem* item) {
    DesktopSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, clock_enable_text[index]);
    app->settings.display_clock = index;
}

static void desktop_settings_scene_start_lock_on_boot_changed(VariableItem* item) {
    DesktopSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, lock_on_boot_text[index]);
    app->settings.lock_on_boot = lock_on_boot_value[index];
}

static void desktop_settings_scene_start_wipe_enabled_changed(VariableItem* item) {
    DesktopSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, wipe_enabled_text[index]);
    app->settings.wipe_enabled = wipe_enabled_value[index];

    // Coherence with "PIN Fails to Wipe": re-arming wipe with no threshold
    // configured (e.g. it was left at 0 after being set to "Off" via that
    // item) must not silently arm a 0-fails-to-wipe trap.
    if(app->settings.wipe_enabled && app->settings.pin_fails_before_wipe == 0) {
        app->settings.pin_fails_before_wipe = SECURITY_WIPE_DEFAULT_FAILS_BEFORE_WIPE;
    }
}

static void desktop_settings_scene_start_pin_fails_before_wipe_changed(VariableItem* item) {
    DesktopSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, pin_fails_before_wipe_text[index]);

    if(pin_fails_before_wipe_value[index] == 0) {
        // "Off": disable wipe-on-fail outright. Leave the stored threshold
        // untouched so re-enabling (either here or via "Wipe on PIN Fail")
        // recalls the last chosen value.
        app->settings.wipe_enabled = false;
    } else {
        app->settings.pin_fails_before_wipe = pin_fails_before_wipe_value[index];
        app->settings.wipe_enabled = true;
    }
}

static void desktop_settings_scene_start_auto_lock_delay_changed(VariableItem* item) {
    DesktopSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, auto_lock_delay_text[index]);
    app->settings.auto_lock_delay_ms = auto_lock_delay_value[index];
}

static void desktop_settings_scene_start_usb_inhibit_auto_lock_delay_changed(VariableItem* item) {
    DesktopSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, usb_inhibit_auto_lock_delay_text[index]);
    app->settings.usb_inhibit_auto_lock = usb_inhibit_auto_lock_delay_value[index];
}

// index 0 == "Default" (built-in animations, settings.asset_pack == "").
// index 1..asset_pack_settings_count map to asset_pack_settings_names[index-1].
// Applies on the NEXT desktop (re)load, not live (Phase 1 scope, see task-3
// brief step 3) — the animation loader only reads settings.asset_pack at
// load time (Task 2).
static void desktop_settings_scene_start_asset_pack_changed(VariableItem* item) {
    DesktopSettingsApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    if(index == 0) {
        variable_item_set_current_value_text(item, "Default");
        app->settings.asset_pack[0] = '\0';
        return;
    }

    const char* name = asset_pack_settings_names[index - 1];
    // Defense-in-depth: asset_pack_enumerate() only ever returns names that
    // already passed this same check, but never store an unvalidated name
    // (task-3 brief step 2).
    if(asset_pack_name_is_valid(name)) {
        variable_item_set_current_value_text(item, name);
        strlcpy(app->settings.asset_pack, name, ASSET_PACK_NAME_LEN);
    } else {
        // Shouldn't happen; fall back to Default rather than storing a name
        // that failed validation.
        variable_item_set_current_value_index(item, 0);
        variable_item_set_current_value_text(item, "Default");
        app->settings.asset_pack[0] = '\0';
    }
}

void desktop_settings_scene_start_on_enter(void* context) {
    DesktopSettingsApp* app = context;
    VariableItemList* variable_item_list = app->variable_item_list;

    VariableItem* item;
    uint8_t value_index;

    variable_item_list_add(variable_item_list, "PIN Setup", 1, NULL, NULL);

    item = variable_item_list_add(
        variable_item_list,
        "Lock on Boot",
        LOCK_ON_BOOT_COUNT,
        desktop_settings_scene_start_lock_on_boot_changed,
        app);

    value_index =
        value_index_uint32(app->settings.lock_on_boot, lock_on_boot_value, LOCK_ON_BOOT_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, lock_on_boot_text[value_index]);

    item = variable_item_list_add(
        variable_item_list,
        "Wipe on PIN Fail",
        WIPE_ENABLED_COUNT,
        desktop_settings_scene_start_wipe_enabled_changed,
        app);

    value_index =
        value_index_uint32(app->settings.wipe_enabled, wipe_enabled_value, WIPE_ENABLED_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, wipe_enabled_text[value_index]);

    item = variable_item_list_add(
        variable_item_list,
        "PIN Fails to Wipe",
        PIN_FAILS_BEFORE_WIPE_COUNT,
        desktop_settings_scene_start_pin_fails_before_wipe_changed,
        app);

    if(app->settings.wipe_enabled) {
        value_index = value_index_uint32(
            app->settings.pin_fails_before_wipe,
            pin_fails_before_wipe_value,
            PIN_FAILS_BEFORE_WIPE_COUNT);
        if(value_index == 0) {
            // Enabled but the stored threshold isn't one of the offered
            // values (or is 0): fall back to the shipped default rather
            // than displaying "Off" while wipe is actually armed.
            value_index = value_index_uint32(
                SECURITY_WIPE_DEFAULT_FAILS_BEFORE_WIPE,
                pin_fails_before_wipe_value,
                PIN_FAILS_BEFORE_WIPE_COUNT);
        }
    } else {
        value_index = 0; // Off
    }
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, pin_fails_before_wipe_text[value_index]);

    item = variable_item_list_add(
        variable_item_list,
        "Auto Lock Time",
        AUTO_LOCK_DELAY_COUNT,
        desktop_settings_scene_start_auto_lock_delay_changed,
        app);

    value_index = value_index_uint32(
        app->settings.auto_lock_delay_ms, auto_lock_delay_value, AUTO_LOCK_DELAY_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, auto_lock_delay_text[value_index]);

    // USB connection Inhibit autolock OFF|ON|with opened RPC session
    item = variable_item_list_add(
        variable_item_list,
        "Auto Lock disarm by active USB session",
        USB_INHIBIT_AUTO_LOCK_DELAY_COUNT,
        desktop_settings_scene_start_usb_inhibit_auto_lock_delay_changed,
        app);

    value_index = value_index_uint32(
        app->settings.usb_inhibit_auto_lock,
        usb_inhibit_auto_lock_delay_value,
        USB_INHIBIT_AUTO_LOCK_DELAY_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, usb_inhibit_auto_lock_delay_text[value_index]);

    item = variable_item_list_add(
        variable_item_list,
        "Battery View",
        BATTERY_VIEW_COUNT,
        desktop_settings_scene_start_battery_view_changed,
        app);

    value_index = value_index_uint32(
        app->settings.displayBatteryPercentage,
        displayBatteryPercentage_value,
        BATTERY_VIEW_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, battery_view_count_text[value_index]);

    item = variable_item_list_add(
        variable_item_list,
        "Show Clock",
        CLOCK_ENABLE_COUNT,
        desktop_settings_scene_start_clock_enable_changed,
        app);

    value_index =
        value_index_uint32(app->settings.display_clock, clock_enable_value, CLOCK_ENABLE_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, clock_enable_text[value_index]);

    variable_item_list_add(variable_item_list, "Change Flipper Name", 0, NULL, app);

    variable_item_list_add(variable_item_list, "Happy Mode", 1, NULL, NULL);

    variable_item_list_add(variable_item_list, "Favorite App - Left Short", 1, NULL, NULL);
    variable_item_list_add(variable_item_list, "Favorite App - Left Long", 1, NULL, NULL);
    variable_item_list_add(variable_item_list, "Favorite App - Right Short", 1, NULL, NULL);
    variable_item_list_add(variable_item_list, "Favorite App - Right Long", 1, NULL, NULL);
    variable_item_list_add(variable_item_list, "Favorite App - Ok Long", 1, NULL, NULL);

    variable_item_list_add(variable_item_list, "DummyMode - Left", 1, NULL, NULL);
    variable_item_list_add(variable_item_list, "DummyMode - Left Long", 1, NULL, NULL);
    variable_item_list_add(variable_item_list, "DummyMode - Right", 1, NULL, NULL);
    variable_item_list_add(variable_item_list, "DummyMode - Right Long", 1, NULL, NULL);
    variable_item_list_add(variable_item_list, "DummyMode - Up Long", 1, NULL, NULL);
    variable_item_list_add(variable_item_list, "DummyMode - Down", 1, NULL, NULL);
    variable_item_list_add(variable_item_list, "DummyMode - Down Long", 1, NULL, NULL);
    variable_item_list_add(variable_item_list, "DummyMode - Ok", 1, NULL, NULL);
    variable_item_list_add(variable_item_list, "DummyMode - Ok Long", 1, NULL, NULL);

    // Asset Pack selector. Appended LAST (rather than grouped near the other
    // display settings above) so it doesn't shift the list index of any item
    // earlier in this function -- desktop_settings_scene_start_on_event's
    // switch matches on the item's position in this list (see
    // DesktopSettingsEntry / DesktopSettingsChangeName et al above), so
    // inserting a new item anywhere but the end would silently desync those
    // case labels from the items they're meant to handle.
    asset_pack_settings_count =
        asset_pack_enumerate(asset_pack_settings_names, ASSET_PACK_SETTINGS_MAX_PACKS);

    item = variable_item_list_add(
        variable_item_list,
        "Asset Pack",
        1 + asset_pack_settings_count,
        desktop_settings_scene_start_asset_pack_changed,
        app);

    // index 0 ("Default") unless the stored pack name matches one currently
    // enumerated from the SD card. If the stored pack is missing (SD swapped,
    // pack folder deleted/renamed, etc.) this falls back to displaying
    // "Default" WITHOUT touching app->settings.asset_pack -- the stored name
    // is left alone unless the user actively picks a different value below,
    // so it isn't silently overwritten (e.g. if the SD is later restored).
    value_index = 0;
    if(app->settings.asset_pack[0] != '\0') {
        for(uint8_t i = 0; i < asset_pack_settings_count; i++) {
            if(strncmp(
                   app->settings.asset_pack,
                   asset_pack_settings_names[i],
                   ASSET_PACK_NAME_LEN) == 0) {
                value_index = i + 1;
                break;
            }
        }
    }
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(
        item, value_index == 0 ? "Default" : asset_pack_settings_names[value_index - 1]);

    variable_item_list_set_enter_callback(
        variable_item_list, desktop_settings_scene_start_var_list_enter_callback, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, DesktopSettingsAppViewVarItemList);
}

bool desktop_settings_scene_start_on_event(void* context, SceneManagerEvent event) {
    DesktopSettingsApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case DesktopSettingsPinSetup:
            scene_manager_next_scene(app->scene_manager, DesktopSettingsAppScenePinMenu);
            break;

            // case DesktopSettingsAutoLockDelay:
            // case DesktopSettingsBatteryDisplay:
            // case DesktopSettingsClockDisplay:
            // Proces in default

        case DesktopSettingsChangeName:
            scene_manager_next_scene(app->scene_manager, DesktopSettingsAppSceneChangeName);
            break;

        case DesktopSettingsFavoriteLeftShort:
            scene_manager_set_scene_state(
                app->scene_manager,
                DesktopSettingsAppSceneFavorite,
                SCENE_STATE_SET_FAVORITE_APP | FavoriteAppLeftShort);
            scene_manager_next_scene(app->scene_manager, DesktopSettingsAppSceneFavorite);
            break;
        case DesktopSettingsFavoriteLeftLong:
            scene_manager_set_scene_state(
                app->scene_manager,
                DesktopSettingsAppSceneFavorite,
                SCENE_STATE_SET_FAVORITE_APP | FavoriteAppLeftLong);
            scene_manager_next_scene(app->scene_manager, DesktopSettingsAppSceneFavorite);
            break;
        case DesktopSettingsFavoriteRightShort:
            scene_manager_set_scene_state(
                app->scene_manager,
                DesktopSettingsAppSceneFavorite,
                SCENE_STATE_SET_FAVORITE_APP | FavoriteAppRightShort);
            scene_manager_next_scene(app->scene_manager, DesktopSettingsAppSceneFavorite);
            break;
        case DesktopSettingsFavoriteRightLong:
            scene_manager_set_scene_state(
                app->scene_manager,
                DesktopSettingsAppSceneFavorite,
                SCENE_STATE_SET_FAVORITE_APP | FavoriteAppRightLong);
            scene_manager_next_scene(app->scene_manager, DesktopSettingsAppSceneFavorite);
            break;
        case DesktopSettingsFavoriteOkLong:
            scene_manager_set_scene_state(
                app->scene_manager,
                DesktopSettingsAppSceneFavorite,
                SCENE_STATE_SET_FAVORITE_APP | FavoriteAppOkLong);
            scene_manager_next_scene(app->scene_manager, DesktopSettingsAppSceneFavorite);
            break;

        case DesktopSettingsDummyLeft:
            scene_manager_set_scene_state(
                app->scene_manager,
                DesktopSettingsAppSceneFavorite,
                SCENE_STATE_SET_DUMMY_APP | DummyAppLeftShort);
            scene_manager_next_scene(app->scene_manager, DesktopSettingsAppSceneFavorite);
            break;
        case DesktopSettingsDummyLeftLong:
            scene_manager_set_scene_state(
                app->scene_manager,
                DesktopSettingsAppSceneFavorite,
                SCENE_STATE_SET_DUMMY_APP | DummyAppLeftLong);
            scene_manager_next_scene(app->scene_manager, DesktopSettingsAppSceneFavorite);
            break;
        case DesktopSettingsDummyRight:
            scene_manager_set_scene_state(
                app->scene_manager,
                DesktopSettingsAppSceneFavorite,
                SCENE_STATE_SET_DUMMY_APP | DummyAppRightShort);
            scene_manager_next_scene(app->scene_manager, DesktopSettingsAppSceneFavorite);
            break;
        case DesktopSettingsDummyRightLong:
            scene_manager_set_scene_state(
                app->scene_manager,
                DesktopSettingsAppSceneFavorite,
                SCENE_STATE_SET_DUMMY_APP | DummyAppRightLong);
            scene_manager_next_scene(app->scene_manager, DesktopSettingsAppSceneFavorite);
            break;
        case DesktopSettingsDummyUpLong:
            scene_manager_set_scene_state(
                app->scene_manager,
                DesktopSettingsAppSceneFavorite,
                SCENE_STATE_SET_DUMMY_APP | DummyAppUpLong);
            scene_manager_next_scene(app->scene_manager, DesktopSettingsAppSceneFavorite);
            break;
        case DesktopSettingsDummyDown:
            scene_manager_set_scene_state(
                app->scene_manager,
                DesktopSettingsAppSceneFavorite,
                SCENE_STATE_SET_DUMMY_APP | DummyAppDownShort);
            scene_manager_next_scene(app->scene_manager, DesktopSettingsAppSceneFavorite);
            break;
        case DesktopSettingsDummyDownLong:
            scene_manager_set_scene_state(
                app->scene_manager,
                DesktopSettingsAppSceneFavorite,
                SCENE_STATE_SET_DUMMY_APP | DummyAppDownLong);
            scene_manager_next_scene(app->scene_manager, DesktopSettingsAppSceneFavorite);
            break;
        case DesktopSettingsDummyOk:
            scene_manager_set_scene_state(
                app->scene_manager,
                DesktopSettingsAppSceneFavorite,
                SCENE_STATE_SET_DUMMY_APP | DummyAppOkShort);
            scene_manager_next_scene(app->scene_manager, DesktopSettingsAppSceneFavorite);
            break;
        case DesktopSettingsDummyOkLong:
            scene_manager_set_scene_state(
                app->scene_manager,
                DesktopSettingsAppSceneFavorite,
                SCENE_STATE_SET_DUMMY_APP | DummyAppOkLong);
            scene_manager_next_scene(app->scene_manager, DesktopSettingsAppSceneFavorite);
            break;

        case DesktopSettingsHappyMode:
            scene_manager_next_scene(app->scene_manager, DesktopSettingsAppSceneHappyMode);
            break;

        default:
            break;
        }
        consumed = true;
    }
    return consumed;
}

void desktop_settings_scene_start_on_exit(void* context) {
    DesktopSettingsApp* app = context;
    variable_item_list_reset(app->variable_item_list);
    desktop_settings_save(&app->settings);

    // Trigger UI update in case we changed battery layout
    Power* power = furi_record_open(RECORD_POWER);
    power_trigger_ui_update(power);
    furi_record_close(RECORD_POWER);
}
