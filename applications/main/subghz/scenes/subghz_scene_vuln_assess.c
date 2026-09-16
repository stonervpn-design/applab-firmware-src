#include "../subghz_i.h"
#include "../helpers/subghz_custom_event.h"
#include "../helpers/vehicle_vuln_scan.h"
#include <flipper_format/flipper_format.h>

#ifndef PUBLIC_BUILD
// Vuln Assess: classify the captured signal the user opened from the Vuln Scan
// receiver, show the verdict, and let them add it to the session report.

static VulnFinding s_finding;

static void subghz_scene_vuln_assess_button_cb(GuiButtonType result, InputType type, void* context) {
    furi_assert(context);
    SubGhz* subghz = context;
    if(type != InputTypeShort) return;
    if(result == GuiButtonTypeRight) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneVulnAssessAdd);
    } else if(result == GuiButtonTypeLeft) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneVulnAssessBack);
    }
}

void subghz_scene_vuln_assess_on_enter(void* context) {
    SubGhz* subghz = context;
    uint16_t idx = subghz->idx_menu_chosen;

    const char* pname = subghz_history_get_protocol_name(subghz->history, idx);
    uint8_t ptype = subghz_history_get_type_protocol(subghz->history, idx);
    uint32_t freq = subghz_history_get_frequency(subghz->history, idx);
    const char* preset = subghz_history_get_preset(subghz->history, idx);

    // Bit length (if the decoder stored one) for the report.
    uint32_t bits = 0;
    FlipperFormat* raw = subghz_history_get_raw_data(subghz->history, idx);
    if(raw) {
        flipper_format_rewind(raw);
        if(!flipper_format_read_uint32(raw, "Bit", &bits, 1)) bits = 0;
    }

    vehicle_vuln_classify(pname ? pname : "Unknown", ptype, freq, preset, bits, &s_finding);

    FuriString* body = furi_string_alloc();
    furi_string_cat_printf(
        body,
        "\e#%s\e#  %s\n",
        vehicle_vuln_severity_str(s_finding.severity),
        vehicle_vuln_weakness_str(s_finding.weakness));
    furi_string_cat_printf(
        body,
        "%s  |  %s\n",
        s_finding.make,
        s_finding.protocol);
    furi_string_cat_printf(
        body,
        "%lu.%02lu MHz  %s\n",
        (unsigned long)(freq / 1000000UL),
        (unsigned long)((freq % 1000000UL) / 10000UL),
        preset ? preset : "?");
    furi_string_cat_printf(body, "%s", vehicle_vuln_recommendation(s_finding.weakness));

    widget_reset(subghz->widget);
    widget_add_text_box_element(
        subghz->widget, 0, 0, 128, 52, AlignLeft, AlignTop, furi_string_get_cstr(body), false);
    widget_add_button_element(
        subghz->widget, GuiButtonTypeRight, "Add", subghz_scene_vuln_assess_button_cb, subghz);
    widget_add_button_element(
        subghz->widget, GuiButtonTypeLeft, "Back", subghz_scene_vuln_assess_button_cb, subghz);

    furi_string_free(body);
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);
}

bool subghz_scene_vuln_assess_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubGhzCustomEventSceneVulnAssessAdd) {
            bool ok = vehicle_vuln_add(&s_finding);
            notification_message(
                subghz->notifications, ok ? &sequence_success : &sequence_error);
            // Return to the scan to capture the next target.
            scene_manager_previous_scene(subghz->scene_manager);
            return true;
        } else if(event.event == SubGhzCustomEventSceneVulnAssessBack) {
            scene_manager_previous_scene(subghz->scene_manager);
            return true;
        }
    }
    return false;
}

void subghz_scene_vuln_assess_on_exit(void* context) {
    SubGhz* subghz = context;
    widget_reset(subghz->widget);
}
#endif /* !PUBLIC_BUILD */
