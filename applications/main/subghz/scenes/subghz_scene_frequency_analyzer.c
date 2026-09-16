#include "../subghz_i.h" // IWYU pragma: keep
#include "../views/subghz_frequency_analyzer.h"
#include <string.h>

#define TAG "SubGhzSceneFrequencyAnalyzer"
#define SMD_CONF_AUTOSELECT 50 // below this confidence, keep the user's preset

static const NotificationSequence sequence_saved = {
    &message_blink_stop,
    &message_blue_0,
    &message_green_255,
    &message_red_0,
    &message_vibro_on,
    &message_delay_100,
    &message_vibro_off,
    NULL,
};

void subghz_scene_frequency_analyzer_callback(SubGhzCustomEvent event, void* context) {
    furi_assert(context);
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, event);
}

#ifndef PUBLIC_BUILD
// Auto-select the demodulator preset matching the analyzer's detected modulation,
// so the Receiver opens in the right mode. No-op on low confidence / unknown.
// PRIVATE builds only (the analyzer never detects a modulation in public).
static void subghz_fa_apply_preset_for_mod(SubGhz* subghz) {
    uint8_t conf = 0;
    SignalMod mod = subghz_frequency_analyzer_get_mod(subghz->subghz_frequency_analyzer, &conf);
    if(mod == SIG_MOD_UNK || conf < SMD_CONF_AUTOSELECT) return;
    const char* want = (mod == SIG_MOD_AM) ? "AM650" : "FM476";
    SubGhzSetting* setting = subghz_txrx_get_setting(subghz->txrx);
    for(size_t i = 0; i < subghz_setting_get_preset_count(setting); i++) {
        if(!strcmp(subghz_setting_get_preset_name(setting, i), want)) {
            subghz->last_settings->preset_index = i;
            subghz_last_settings_save(subghz->last_settings);
            break;
        }
    }
}
#endif

void subghz_scene_frequency_analyzer_on_enter(void* context) {
    SubGhz* subghz = context;
    // Lazy-load the analyzer view+worker on entry (freed on exit) so its internal RAM
    // stays out of the SubGhz launch path. Mirrors the jammer/playlist lazy pattern.
    if(!subghz->subghz_frequency_analyzer) {
        subghz->subghz_frequency_analyzer = subghz_frequency_analyzer_alloc(subghz->txrx);
        view_dispatcher_add_view(
            subghz->view_dispatcher,
            SubGhzViewIdFrequencyAnalyzer,
            subghz_frequency_analyzer_get_view(subghz->subghz_frequency_analyzer));
    }
    subghz_frequency_analyzer_set_callback(
        subghz->subghz_frequency_analyzer, subghz_scene_frequency_analyzer_callback, subghz);
    subghz_frequency_analyzer_feedback_level(
        subghz->subghz_frequency_analyzer,
        subghz->last_settings->frequency_analyzer_feedback_level,
        true);
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdFrequencyAnalyzer);
}

bool subghz_scene_frequency_analyzer_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubGhzCustomEventSceneAnalyzerLock) {
            notification_message(subghz->notifications, &sequence_set_green_255);
            switch(subghz_frequency_analyzer_feedback_level(
                subghz->subghz_frequency_analyzer,
                SubGHzFrequencyAnalyzerFeedbackLevelAll,
                false)) {
            case SubGHzFrequencyAnalyzerFeedbackLevelAll:
                notification_message(subghz->notifications, &sequence_success);
                break;
            case SubGHzFrequencyAnalyzerFeedbackLevelVibro:
                notification_message(subghz->notifications, &sequence_single_vibro);
                break;
            case SubGHzFrequencyAnalyzerFeedbackLevelMute:
                break;
            }
            notification_message(subghz->notifications, &sequence_display_backlight_on);
            return true;
        } else if(event.event == SubGhzCustomEventSceneAnalyzerUnlock) {
            notification_message(subghz->notifications, &sequence_reset_rgb);
            return true;
        } else if(event.event == SubGhzCustomEventViewFreqAnalOkShort) {
            notification_message(subghz->notifications, &sequence_saved);
            uint32_t frequency =
                subghz_frequency_analyzer_get_frequency_to_save(subghz->subghz_frequency_analyzer);
            if(frequency > 0) {
                subghz->last_settings->frequency = frequency;
                // Disable Hopping before opening the receiver scene!
                if(subghz->last_settings->enable_hopping) {
                    subghz->last_settings->enable_hopping = false;
                }
                subghz_last_settings_save(subghz->last_settings);
            }

            return true;
        } else if(event.event == SubGhzCustomEventViewFreqAnalOkLong) {
#ifndef PUBLIC_BUILD
            // Auto-select the demodulator preset matching the detected modulation.
            subghz_fa_apply_preset_for_mod(subghz);
#endif
            // Don't need to save, we already saved on short event (and on exit event too)
            subghz_rx_key_state_set(subghz, SubGhzRxKeyStateIDLE);
            scene_manager_previous_scene(subghz->scene_manager); // Stops the worker
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneReceiver);
            return true;
        }
    }
    return false;
}

void subghz_scene_frequency_analyzer_on_exit(void* context) {
    SubGhz* subghz = context;
    notification_message(subghz->notifications, &sequence_reset_rgb);

    // Persist analyzer settings, then free the lazily-allocated view+worker to return its
    // internal RAM. Whole block guarded so a NULL (alloc-failed) analyzer is safe.
    if(subghz->subghz_frequency_analyzer) {
        subghz->last_settings->frequency_analyzer_feedback_level =
            subghz_frequency_analyzer_feedback_level(subghz->subghz_frequency_analyzer, 0, false);
        subghz->last_settings->frequency_analyzer_trigger =
            subghz_frequency_analyzer_get_trigger_level(subghz->subghz_frequency_analyzer);
        subghz_last_settings_save(subghz->last_settings);

        view_dispatcher_remove_view(subghz->view_dispatcher, SubGhzViewIdFrequencyAnalyzer);
        subghz_frequency_analyzer_free(subghz->subghz_frequency_analyzer);
        subghz->subghz_frequency_analyzer = NULL;
    }
}
