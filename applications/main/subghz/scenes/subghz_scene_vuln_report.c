#include "../subghz_i.h"
#include "../helpers/subghz_custom_event.h"
#include "../helpers/vehicle_vuln_scan.h"

#ifndef PUBLIC_BUILD
// Vuln Report: review the findings collected this session and export a
// human-readable report to /ext/reports/.

static void subghz_scene_vuln_report_button_cb(GuiButtonType result, InputType type, void* context) {
    furi_assert(context);
    SubGhz* subghz = context;
    if(type != InputTypeShort) return;
    if(result == GuiButtonTypeRight) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneVulnReportExport);
    } else if(result == GuiButtonTypeCenter) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneVulnReportClear);
    } else if(result == GuiButtonTypeLeft) {
        view_dispatcher_send_custom_event(
            subghz->view_dispatcher, SubGhzCustomEventSceneVulnReportBack);
    }
}

// status != NULL shows a one-line result banner instead of the buttons.
static void subghz_scene_vuln_report_redraw(SubGhz* subghz, const char* status) {
    uint16_t n = vehicle_vuln_count();
    FuriString* body = furi_string_alloc();

    if(status) {
        furi_string_cat_printf(body, "\e#%s\e#\n\n", status);
    }

    if(n == 0) {
        furi_string_cat(body, "No findings yet.\nCapture a signal in Vuln\nScan and press Add.");
    } else {
        uint16_t sev[5] = {0};
        for(uint16_t i = 0; i < n; i++) {
            const VulnFinding* f = vehicle_vuln_get(i);
            if(f && f->severity <= VulnSevCritical) sev[f->severity]++;
        }
        furi_string_cat_printf(
            body,
            "Findings: %u\nC%u H%u M%u L%u I%u\n",
            (unsigned)n,
            sev[VulnSevCritical],
            sev[VulnSevHigh],
            sev[VulnSevMedium],
            sev[VulnSevLow],
            sev[VulnSevInfo]);
        uint16_t show = n < 3 ? n : 3;
        for(uint16_t i = 0; i < show; i++) {
            const VulnFinding* f = vehicle_vuln_get(i);
            if(!f) continue;
            furi_string_cat_printf(
                body,
                "%s %s\n",
                vehicle_vuln_severity_str(f->severity),
                f->make);
        }
        if(n > show) furi_string_cat_printf(body, "+%u more (in report)\n", (unsigned)(n - show));
    }

    widget_reset(subghz->widget);
    widget_add_text_box_element(
        subghz->widget, 0, 0, 128, 52, AlignLeft, AlignTop, furi_string_get_cstr(body), false);
    furi_string_free(body);

    if(!status) {
        widget_add_button_element(
            subghz->widget, GuiButtonTypeLeft, "Back", subghz_scene_vuln_report_button_cb, subghz);
        if(n > 0) {
            widget_add_button_element(
                subghz->widget,
                GuiButtonTypeCenter,
                "Clear",
                subghz_scene_vuln_report_button_cb,
                subghz);
            widget_add_button_element(
                subghz->widget,
                GuiButtonTypeRight,
                "Export",
                subghz_scene_vuln_report_button_cb,
                subghz);
        }
    } else {
        widget_add_button_element(
            subghz->widget, GuiButtonTypeLeft, "Back", subghz_scene_vuln_report_button_cb, subghz);
    }
}

void subghz_scene_vuln_report_on_enter(void* context) {
    SubGhz* subghz = context;
    subghz_scene_vuln_report_redraw(subghz, NULL);
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);
}

bool subghz_scene_vuln_report_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubGhzCustomEventSceneVulnReportExport) {
            const char* path = vehicle_vuln_report_export();
            if(path) {
                // Show the bare filename (path is /ext/reports/vuln-...txt).
                const char* slash = strrchr(path, '/');
                notification_message(subghz->notifications, &sequence_success);
                FuriString* msg = furi_string_alloc();
                furi_string_printf(msg, "Saved %s", slash ? slash + 1 : path);
                subghz_scene_vuln_report_redraw(subghz, furi_string_get_cstr(msg));
                furi_string_free(msg);
            } else {
                notification_message(subghz->notifications, &sequence_error);
                subghz_scene_vuln_report_redraw(subghz, "No storage / no data");
            }
            return true;
        } else if(event.event == SubGhzCustomEventSceneVulnReportClear) {
            vehicle_vuln_scan_reset();
            subghz_scene_vuln_report_redraw(subghz, NULL);
            return true;
        } else if(event.event == SubGhzCustomEventSceneVulnReportBack) {
            scene_manager_previous_scene(subghz->scene_manager);
            return true;
        }
    }
    return false;
}

void subghz_scene_vuln_report_on_exit(void* context) {
    SubGhz* subghz = context;
    widget_reset(subghz->widget);
}
#endif /* !PUBLIC_BUILD */
