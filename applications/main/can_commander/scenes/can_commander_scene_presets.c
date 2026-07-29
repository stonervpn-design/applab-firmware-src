#include "../can_commander_app_i.h"
#include <stdio.h>

#define PRESET_SAVE_IDX 1000u

static void can_presets_build_submenu(CanCommanderApp* app);

static void can_presets_name_cb(void* context) {
    CanCommanderApp* app = context;
    scene_manager_handle_custom_event(app->scene_manager, CanCommanderEvtPresetNameEntered);
}

static void can_presets_submenu_cb(void* context, uint32_t index) {
    CanCommanderApp* app = context;
    if(index == PRESET_SAVE_IDX) {
        text_input_reset(app->text_input);
        text_input_set_header_text(app->text_input, "Preset name");
        app->preset_name_input[0] = '\0';
        text_input_set_result_callback(
            app->text_input,
            can_presets_name_cb,
            app,
            app->preset_name_input,
            sizeof(app->preset_name_input),
            true);
        view_dispatcher_switch_to_view(app->view_dispatcher, CanViewTextInput);
    } else if(index < app->preset_count) {
        // Load the preset into the shared frame buffer and jump to Send to fire it.
        can_frame_format(&app->presets[index], app->frame_input, sizeof(app->frame_input));
        scene_manager_next_scene(app->scene_manager, CanCommanderSceneSend);
    }
}

static void can_presets_build_submenu(CanCommanderApp* app) {
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "CAN Presets");
    submenu_add_item(
        app->submenu, "[+] Save current frame", PRESET_SAVE_IDX, can_presets_submenu_cb, app);
    for(uint8_t i = 0; i < app->preset_count; i++) {
        char label[40];
        char fmt[24];
        can_frame_format(&app->presets[i], fmt, sizeof(fmt));
        snprintf(label, sizeof(label), "%s  %s", app->preset_names[i], fmt);
        submenu_add_item(app->submenu, label, i, can_presets_submenu_cb, app);
    }
}

void can_commander_scene_presets_on_enter(void* context) {
    furi_assert(context);
    CanCommanderApp* app = context;
    can_presets_load(app);
    can_presets_build_submenu(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, CanViewSubmenu);
}

bool can_commander_scene_presets_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    CanCommanderApp* app = context;
    if(event.type == SceneManagerEventTypeCustom &&
       event.event == CanCommanderEvtPresetNameEntered) {
        CanFrame f;
        if(app->preset_name_input[0] && can_commander_parse_input(app->frame_input, &f)) {
            can_presets_append(app, app->preset_name_input, &f);
        }
        can_presets_build_submenu(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, CanViewSubmenu);
        return true;
    }
    return false;
}

void can_commander_scene_presets_on_exit(void* context) {
    furi_assert(context);
    CanCommanderApp* app = context;
    submenu_reset(app->submenu);
    text_input_reset(app->text_input);
}
