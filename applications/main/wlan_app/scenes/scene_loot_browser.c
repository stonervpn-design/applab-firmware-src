#include "../wlan_app.h"
#include <storage/storage.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define LOOT_ROOT "/ext/loot"
#define LOOT_MAX_SESSIONS 32

// Session names are "YYYYMMDD_HHMMSS" (15 chars) → 32 is ample.
static char s_sessions[LOOT_MAX_SESSIONS][32];
static uint8_t s_session_count;

// Menu indices 0..s_session_count-1 map to sessions. The rest are sentinels
// (all > LOOT_MAX_SESSIONS, so they never collide with a session index).
enum {
    LootIdxWipeAll = 900,
    LootIdxEmpty = 901,
    LootIdxWipeYes = 902,
    LootIdxWipeNo = 903,
};

static void loot_menu_cb(void* context, uint32_t index) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static int loot_cmp_desc(const void* a, const void* b) {
    return strncmp((const char*)b, (const char*)a, 32); // reverse = newest first
}

static void loot_scan_sessions(void) {
    s_session_count = 0;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* dir = storage_file_alloc(storage);
    if(storage_dir_open(dir, LOOT_ROOT)) {
        FileInfo info;
        char name[64];
        while(storage_dir_read(dir, &info, name, sizeof(name)) &&
              s_session_count < LOOT_MAX_SESSIONS) {
            if(file_info_is_dir(&info)) {
                strncpy(s_sessions[s_session_count], name, 31);
                s_sessions[s_session_count][31] = '\0';
                s_session_count++;
            }
        }
    }
    storage_dir_close(dir);
    storage_file_free(dir);
    furi_record_close(RECORD_STORAGE);
    if(s_session_count > 1) {
        qsort(s_sessions, s_session_count, 32, loot_cmp_desc);
    }
}

// Reset the widget (in case we came back from the confirm dialog), scan the
// loot dir and (re)build the session submenu, then show it.
static void loot_browser_populate(WlanApp* app) {
    widget_reset(app->widget);
    submenu_reset(app->submenu);
    submenu_set_header_centered(app->submenu, "Loot");
    loot_scan_sessions();
    for(uint8_t i = 0; i < s_session_count; i++) {
        submenu_add_item(app->submenu, s_sessions[i], i, loot_menu_cb, app);
    }
    if(s_session_count) {
        submenu_add_separator(app->submenu);
        submenu_add_item(app->submenu, "Wipe all loot", LootIdxWipeAll, loot_menu_cb, app);
    } else {
        submenu_add_item(app->submenu, "(no loot yet)", LootIdxEmpty, loot_menu_cb, app);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSubmenu);
}

static void loot_wipe_yes_cb(GuiButtonType result, InputType type, void* context) {
    WlanApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeRight) {
        view_dispatcher_send_custom_event(app->view_dispatcher, LootIdxWipeYes);
    }
}

static void loot_wipe_no_cb(GuiButtonType result, InputType type, void* context) {
    WlanApp* app = context;
    if(type == InputTypeShort && result == GuiButtonTypeLeft) {
        view_dispatcher_send_custom_event(app->view_dispatcher, LootIdxWipeNo);
    }
}

static void loot_show_wipe_confirm(WlanApp* app) {
    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 12, AlignCenter, AlignBottom, FontPrimary, "Wipe ALL loot?");
    widget_add_string_element(
        app->widget, 64, 30, AlignCenter, AlignBottom, FontSecondary, "Deletes every session");
    widget_add_button_element(app->widget, GuiButtonTypeLeft, "No", loot_wipe_no_cb, app);
    widget_add_button_element(app->widget, GuiButtonTypeRight, "Yes", loot_wipe_yes_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewWidget);
}

void wlan_app_scene_loot_browser_on_enter(void* context) {
    WlanApp* app = context;
    loot_browser_populate(app);
}

bool wlan_app_scene_loot_browser_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == LootIdxWipeAll) {
        loot_show_wipe_confirm(app);
        return true;
    }
    if(event.event == LootIdxWipeYes) {
        Storage* storage = furi_record_open(RECORD_STORAGE);
        storage_simply_remove_recursive(storage, LOOT_ROOT);
        furi_record_close(RECORD_STORAGE);
        loot_browser_populate(app);
        return true;
    }
    if(event.event == LootIdxWipeNo) {
        loot_browser_populate(app);
        return true;
    }
    if(event.event == LootIdxEmpty) {
        return true; // non-actionable placeholder
    }
    if(event.event < s_session_count) {
        strncpy(app->loot_session, s_sessions[event.event], sizeof(app->loot_session) - 1);
        app->loot_session[sizeof(app->loot_session) - 1] = '\0';
        scene_manager_next_scene(app->scene_manager, WlanAppSceneLootEntries);
        return true;
    }
    return false;
}

void wlan_app_scene_loot_browser_on_exit(void* context) {
    WlanApp* app = context;
    submenu_reset(app->submenu);
    widget_reset(app->widget);
}
