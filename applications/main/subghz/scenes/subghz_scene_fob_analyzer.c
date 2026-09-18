#include "../subghz_i.h" // IWYU pragma: keep
#include "../views/subghz_fob_analyzer.h"

void subghz_scene_fob_analyzer_callback(SubGhzCustomEvent event, void* context) {
    furi_assert(context);
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, event);
}

void subghz_scene_fob_analyzer_on_enter(void* context) {
    SubGhz* subghz = context;
    subghz_fob_analyzer_set_callback(
        subghz->subghz_fob_analyzer, subghz_scene_fob_analyzer_callback, subghz);
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdFobAnalyzer);
}

bool subghz_scene_fob_analyzer_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubGhzCustomEventSceneAnalyzerLock) {
            notification_message(subghz->notifications, &sequence_display_backlight_on);
            return true;
        }
    }
    return false;
}

void subghz_scene_fob_analyzer_on_exit(void* context) {
    SubGhz* subghz = context;
    notification_message(subghz->notifications, &sequence_reset_rgb);
}
