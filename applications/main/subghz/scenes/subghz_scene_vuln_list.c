#include "../subghz_i.h"
#include "../helpers/vehicle_vuln_db.h"
#include <string.h>

void subghz_scene_vuln_list_on_enter(void* context) {
    SubGhz* subghz = context;

    size_t mk = scene_manager_get_scene_state(subghz->scene_manager, SubGhzSceneVulnMakes);
    const char* make = vehicle_vuln_make(mk);

    FuriString* body = furi_string_alloc();

    if(make != NULL) {
        for(size_t k = 0; k < VEHICLE_VULN_DB_COUNT; k++) {
            const VehicleVuln* v = &VEHICLE_VULN_DB[k];

            bool match = false;
            if(v->makes != NULL) {
                for(size_t m = 0; v->makes[m] != NULL; m++) {
                    if(strcmp(v->makes[m], make) == 0) {
                        match = true;
                        break;
                    }
                }
            }
            if(!match) continue;

            // Models joined by "/"
            if(v->models != NULL) {
                for(size_t m = 0; v->models[m] != NULL; m++) {
                    if(m != 0) furi_string_cat(body, "/");
                    furi_string_cat(body, v->models[m]);
                }
            }
            furi_string_cat_printf(
                body,
                "  %s-%s\n%s  (%s)\n%s\n\n",
                v->year_start,
                v->year_end,
                v->cve,
                v->region,
                v->description);
        }
    }

    if(furi_string_empty(body)) {
        furi_string_cat(body, "No entries.");
    }

    widget_reset(subghz->widget);
    widget_add_text_scroll_element(subghz->widget, 0, 0, 128, 64, furi_string_get_cstr(body));

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);

    // widget_element_text_scroll_create copies the text (furi_string_alloc_set),
    // so it is safe to free the FuriString immediately.
    furi_string_free(body);
}

bool subghz_scene_vuln_list_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void subghz_scene_vuln_list_on_exit(void* context) {
    SubGhz* subghz = context;
    widget_reset(subghz->widget);
}
