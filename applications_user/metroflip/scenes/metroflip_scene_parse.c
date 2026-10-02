#include "../metroflip_i.h"
#include <furi.h>
#include "../metroflip_plugins.h"
#include "../metroflip_static_plugins.h"
#include "../api/metroflip/metroflip_api.h"
#define TAG "Metroflip:Scene:Parse"
#include <stdio.h>

void metroflip_scene_parse_on_enter(void* context) {
    Metroflip* app = context;

    FURI_LOG_I(
        TAG,
        "Parse scene entered - card_type: %s, data_loaded: %s",
        app->card_type ? app->card_type : "NULL",
        app->data_loaded ? "true" : "false");

    /* Mark plugin as not loaded yet */
    app->plugin_manager = NULL;
    app->resolver = NULL;

    if(!app->card_type || (app->card_type[0] == '\0') ||
       (strcmp(app->card_type, "unknown") == 0) ||
       (strcmp(app->card_type, "Unknown Card") == 0) ||
       (app->is_desfire && is_desfire_locked(app->card_type))) {
        FURI_LOG_I(TAG, "Bad card condition met - sending wrong card event");
        view_dispatcher_send_custom_event(app->view_dispatcher, MetroflipCustomEventWrongCard);
        return;
    }

    /* ATR sub-detection: T-Mobilitat vs T-Money vs unknown */
    if(strcmp(app->card_type, "atr") == 0) {
        if(app->hist_bytes_count == 0) {
            /* No historical bytes (e.g. phone-emulated cards). Re-loading the
               atr plugin would just re-read zero bytes and fire AtrComplete
               again, looping forever (issue #71 follow-up). */
            FURI_LOG_I(TAG, "ATR card without historical bytes - unknown");
            view_dispatcher_send_custom_event(
                app->view_dispatcher, MetroflipCustomEventWrongCard);
            return;
        }
        FURI_LOG_I(TAG, "Tag is either T-Mobilitat or T-Money");
        if(app->hist_bytes[0] == 0x2A && app->hist_bytes[1] == 0x26) {
            FURI_LOG_I(TAG, "Card is T-Mobilitat");
            app->card_type = "tmobilitat";
        } else if(app->hist_bytes[0] == 0x04 && app->hist_bytes[1] == 0x09) {
            FURI_LOG_I(TAG, "Card is T-Money");
            app->card_type = "tmoney";
        } else {
            view_dispatcher_send_custom_event(
                app->view_dispatcher, MetroflipCustomEventWrongCard);
            return;
        }
    }

    FURI_LOG_I(TAG, "Card is valid, loading plugin for: %s", app->card_type);

    // Show loading status on popup
    Popup* popup = app->popup;
    char status_msg[64];
    snprintf(status_msg, sizeof(status_msg), "Loading\n%s\nplugin...", app->card_type);
    popup_set_header(popup, status_msg, 68, 30, AlignLeft, AlignTop);

    // ESP32 port: no .fal loader — resolve the compiled-in plugin statically.
    const MetroflipPlugin* plugin = metroflip_get_static_plugin(app->card_type);
    if(!plugin) {
        FURI_LOG_E(TAG, "No static plugin for card: %s", app->card_type);
        /* Show error with Exit button */
        Widget* widget = app->widget;
        widget_reset(widget);
        FuriString* s = furi_string_alloc();
        furi_string_printf(
            s,
            "\e#Unsupported Card\n\n"
            "Card: %s\n"
            "No parser built in.",
            app->card_type);
        widget_add_text_scroll_element(widget, 0, 0, 128, 64, furi_string_get_cstr(s));
        widget_add_button_element(
            widget, GuiButtonTypeRight, "Exit", metroflip_exit_widget_callback, app);
        furi_string_free(s);
        view_dispatcher_switch_to_view(app->view_dispatcher, MetroflipViewWidget);
        return;
    }

    // Run the plugin's on_enter function
    app->static_plugin = plugin;
    plugin->plugin_on_enter(app);
}

bool metroflip_scene_parse_on_event(void* context, SceneManagerEvent event) {
    Metroflip* app = context;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == MetroflipCustomEventWrongCard) {
            FURI_LOG_I(TAG, "Wrong card event received - switching to unknown scene");
            scene_manager_next_scene(app->scene_manager, MetroflipSceneUnknown);
            return true;
        } else if(event.event == MetroflipCustomEventAtrComplete) {
            FURI_LOG_I(TAG, "ATR complete - re-entering parse scene");
            scene_manager_next_scene(app->scene_manager, MetroflipSceneParse);
            return true;
        } else if(event.event == MetroflipCustomEventTick) {
            /* Animate card view icon. Note: no view_get_model() check here -
               on a Locking model it would take the model mutex and never
               release it (deadlock). */
            if(app->card_view) {
                with_view_model(
                    app->card_view,
                    MetroflipCardViewModel * m,
                    {
                        if(m->anim[0]) {
                            m->anim_frame =
                                (m->anim_frame + 1) % METROFLIP_CARD_VIEW_ANIM_FRAMES;
                        }
                    },
                    true);
            }
            return true;
        } else if(event.event == MetroflipCustomEventSaveRequest) {
            scene_manager_next_scene(app->scene_manager, MetroflipSceneSave);
            return true;
        } else if(event.event == MetroflipCustomEventDeleteRequest) {
            scene_manager_next_scene(app->scene_manager, MetroflipSceneDelete);
            return true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        FURI_LOG_I(TAG, "Back event received - returning to start");
        scene_manager_search_and_switch_to_previous_scene(app->scene_manager, MetroflipSceneStart);
        return true;
    }

    // Delegate to the statically-resolved plugin if one is active
    if(app->static_plugin) {
        return app->static_plugin->plugin_on_event(app, event);
    }
    return false;
}

void metroflip_scene_parse_on_exit(void* context) {
    Metroflip* app = context;

    // Clean up the active plugin
    if(app->static_plugin) {
        app->static_plugin->plugin_on_exit(app);
        app->static_plugin = NULL;
    }

    // Reset the card view model (the view itself stays alive and registered
    // so the view dispatcher can safely deliver pending input release events).
    metroflip_card_view_free(app);

    app->is_desfire = false;
    app->data_loaded = false;
}
