#include "../infrared_app_i.h"

#include "common/infrared_scene_universal_common.h"

// TV-B-Gone = blast every TV "Power" code. The old implementation used a handful of
// hand-fabricated ("simplified") raw patterns that matched no real TV. Instead reuse
// the universal-remote brute-force engine over the real, captured tv.ir database (295
// Power signals) — the same path the Universal TV remote's Power button uses.
void infrared_scene_universal_tvbgone_on_enter(void* context) {
    InfraredApp* infrared = context;
    ButtonPanel* button_panel = infrared->button_panel;
    InfraredBruteForce* brute_force = infrared->brute_force;

    infrared_brute_force_set_db_filename(brute_force, EXT_PATH("infrared/assets/tv.ir"));

    button_panel_reserve(button_panel, 1, 1);
    uint32_t i = 0;
    button_panel_add_item(
        button_panel,
        i,
        0,
        0,
        6,
        16,
        &I_power_19x20,
        &I_power_hover_19x20,
        infrared_scene_universal_common_item_callback,
        context);
    button_panel_add_icon(button_panel, 4, 38, &I_power_text_24x5);
    infrared_brute_force_add_record(brute_force, i++, "Power");

    // calculate_messages() requires EVERY signal name present in the DB to have a
    // registered record (it NULL-derefs otherwise — furi_assert is compiled out in
    // release). tv.ir contains Power/Mute/Vol_up/Vol_dn/Ch_next/Ch_prev, so register
    // them all even though only the Power button is shown/blasted.
    infrared_brute_force_add_record(brute_force, i++, "Mute");
    infrared_brute_force_add_record(brute_force, i++, "Vol_up");
    infrared_brute_force_add_record(brute_force, i++, "Vol_dn");
    infrared_brute_force_add_record(brute_force, i++, "Ch_next");
    infrared_brute_force_add_record(brute_force, i++, "Ch_prev");

    button_panel_add_label(button_panel, 2, 10, FontPrimary, "TV-B-Gone");

    infrared_scene_universal_common_on_enter(context);
}

bool infrared_scene_universal_tvbgone_on_event(void* context, SceneManagerEvent event) {
    return infrared_scene_universal_common_on_event(context, event);
}

void infrared_scene_universal_tvbgone_on_exit(void* context) {
    infrared_scene_universal_common_on_exit(context);
}
