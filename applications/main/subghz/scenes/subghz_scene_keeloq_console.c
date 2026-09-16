#include "../subghz_i.h" // IWYU pragma: keep

// KeeLoq Console = master-key attack surface (browse+generate across every
// keystore family) — PRIVATE builds only. The whole scene compiles to nothing
// in public; its ADD_SCENE (subghz_scene_config.h) and its only route to it
// (the Car Keys hub's "KeeLoq Console" row, subghz_scene_car_keys.c — itself
// already whole-file PUBLIC_BUILD-gated) are also gated. Same precedent as
// subghz_scene_garage.c. Without this, SubGhzSceneKeeloqConsole (referenced
// below) would be undeclared in a public compile: it only exists when the
// private ADD_SCENE line is active.
#ifndef PUBLIC_BUILD
#include "../helpers/subghz_txrx_i.h"
#include "../helpers/subghz_keeloq_registry.h"
#include <lib/subghz/environment.h>
#include <lib/subghz/subghz_keystore.h>
#include <lib/subghz/protocols/keeloq_common.h>
#include <stdlib.h>

#define TAG "SubGhzKeeloqConsole"

// Catalog console: browse EVERY keystore entry (all families, all 216+), routed
// by the Task 1 registry rather than the mfr scene's single flat encodable-only
// list. Encodable entries are grouped by family with a header row per family and
// route into the existing GenType generate flow; decode-only/orphan/car entries
// are listed in a trailing section and only ever show an info popup — they never
// reach a generator, so a stray tap can never crash or send an invalid frame.

// Stable storage for the picked catalog name. GenInfo's `manuf` fields are
// `const char*` pointing at memory that must outlive this scene's on_exit (the
// generate chain reads it several scenes later), so — like the mfr scene — we
// copy into a buffer with static storage duration rather than pointing at the
// keystore's FuriString (which can be reloaded/freed).
static char subghz_keeloq_console_selected[64];

typedef struct {
    KeeloqFamily family;
    const char* header;
} SubghzKeeloqConsoleSection;

// Browsing order for the encodable half of the catalog. Every family the
// registry can mark `encodable` with a real generator lives here; anything not
// listed here that comes back encodable would be a registry/console mismatch,
// so subghz_scene_keeloq_console_fill_defaults()'s default case intentionally
// leaves gen_info incomplete rather than guessing (see its comment).
static const SubghzKeeloqConsoleSection subghz_keeloq_console_sections[] = {
    {KlFamGeneric, "-- KeeLoq (generic) --"},
    {KlFamFaacSLH, "-- FAAC SLH --"},
    {KlFamKingGates, "-- KingGates --"},
    {KlFamJarolift, "-- Jarolift --"},
    {KlFamBeninca, "-- Beninca ARC --"},
};

static SubGhzKeyArray_t* subghz_scene_keeloq_console_get_keys(SubGhz* subghz) {
    SubGhzEnvironment* environment = subghz->txrx->environment;
    SubGhzKeystore* keystore = subghz_environment_get_keystore(environment);
    if(keystore == NULL) {
        return NULL;
    }
    return subghz_keystore_get_data(keystore);
}

// Populate gen_info for a fresh entry into the SetSerial/SetButton/SetCounter(/
// SetSeed) byte-input chain. This scene is reached directly from the Car Keys
// hub, NOT via SubGhzSceneSetType — so unlike the by-manufacturer picker (which
// only ever runs right after subghz_scene_set_type_fill_generation_infos() has
// already filled gen_info), nothing else initializes gen_info->mod/freq for us.
// subghz->gen_info is malloc'd (not calloc'd) in subghz_alloc(), so skipping
// this would hand the generator a garbage `mod` pointer — a crash, not a soft
// failure. Defaults mirror subghz_scene_set_type_fill_generation_infos()'s
// per-type cases so this behaves the same as the existing Add-Manually paths;
// serial/btn/cnt are fully user-editable on the very next screens regardless of
// their starting value.
static void subghz_scene_keeloq_console_fill_defaults(SubGhz* subghz, GenType generator) {
    subghz_gen_info_reset(subghz->gen_info);
    subghz->gen_info->type = generator;
    subghz->gen_info->mod = "AM650";
    subghz->gen_info->freq = 433920000;

    uint32_t key = (uint32_t)rand();
    switch(generator) {
    case GenKeeloq:
        subghz->gen_info->keeloq.serial = key & 0x0FFFFFFF;
        subghz->gen_info->keeloq.btn = 0x02;
        subghz->gen_info->keeloq.cnt = 0x03;
        break;
    case GenFaacSLH:
        subghz->gen_info->faac_slh.serial = ((key & 0x00FFFFF0) | 0xA0000006) >> 4;
        subghz->gen_info->faac_slh.btn = 0x06;
        subghz->gen_info->faac_slh.cnt = 0x02;
        subghz->gen_info->faac_slh.seed = key;
        break;
    case GenKingGatesStylo4k:
        subghz->gen_info->kinggates_stylo_4k.serial = key;
        subghz->gen_info->kinggates_stylo_4k.btn = 0x0E;
        subghz->gen_info->kinggates_stylo_4k.cnt = 0x03;
        break;
    case GenJarolift:
        subghz->gen_info->jarolift.serial = key & 0xFFFFF00;
        subghz->gen_info->jarolift.btn = 0x02;
        subghz->gen_info->jarolift.cnt = 0x03;
        break;
    case GenBenincaARC:
        subghz->gen_info->beninca_arc.serial = key & 0x00FFFFFF;
        subghz->gen_info->beninca_arc.btn = 0x02;
        subghz->gen_info->beninca_arc.cnt = 0x03;
        break;
    default:
        // Not reachable: subghz_keeloq_console_sections only lists families the
        // registry can return with .encodable == true and one of the generators
        // handled above. Leave gen_info->type set (from above) but otherwise
        // zeroed rather than fabricating fields for an unknown generator — the
        // set_serial/set_button/set_counter dispatch tables furi_crash() on an
        // unrecognized GenType anyway, so this is a loud failure, not a silent
        // wrong-key TX.
        break;
    }
}

// Header rows ("-- KeeLoq (generic) --" etc.) are inert: they exist only to
// label a family's entries in the flat submenu, never to be "selected".
static void subghz_scene_keeloq_console_header_callback(void* context, uint32_t index) {
    UNUSED(context);
    UNUSED(index);
}

// Tapping an encodable entry: same handoff the by-manufacturer picker uses
// (gen_info->keeloq.manuf + keeloq_mfr_emulate_flow + SubGhzSceneSetSerial),
// generalized to also select the per-family generator and (for the families
// whose generator actually reads a manufacturer name back out of the keystore)
// the matching manuf field.
static void subghz_scene_keeloq_console_generate_callback(void* context, uint32_t index) {
    SubGhz* subghz = context;

    SubGhzKeyArray_t* data = subghz_scene_keeloq_console_get_keys(subghz);
    if(data == NULL) {
        return;
    }

    // Re-walk the SAME family-grouped, encodable-only order used to build the
    // submenu in on_enter, so index -> entry stays in sync (same idiom the mfr
    // scene uses for its single flat list).
    uint32_t i = 0;
    const char* found_name = NULL;
    KeeloqTypeInfo found_info = {KlFamGeneric, GenKeeloq, false, false};
    for(size_t s = 0; s < COUNT_OF(subghz_keeloq_console_sections) && found_name == NULL; s++) {
        KeeloqFamily fam = subghz_keeloq_console_sections[s].family;
        for
            M_EACH(key, *data, SubGhzKeyArray_t) {
                const char* name = key->name;
                KeeloqTypeInfo info = subghz_keeloq_registry_type_info(key->type, name);
                if(!info.encodable || info.family != fam) {
                    continue;
                }
                if(i == index) {
                    found_name = name;
                    found_info = info;
                    break;
                }
                i++;
            }
    }

    if(found_name == NULL) {
        return;
    }

    subghz_scene_keeloq_console_fill_defaults(subghz, found_info.generator);

    // Only the generic-KeeLoq and FAAC SLH generators actually look the manuf
    // name back up in the keystore (subghz_protocol_keeloq_create_data /
    // subghz_protocol_faac_slh_create_data match by name). KingGates/Jarolift/
    // Beninca match their single hardcoded-by-type key regardless of which
    // catalog entry was tapped (verified against lib/subghz/protocols/*.c), so
    // there is no manuf field to set for them.
    if(found_info.family == KlFamGeneric || found_info.family == KlFamFaacSLH) {
        strncpy(
            subghz_keeloq_console_selected, found_name, sizeof(subghz_keeloq_console_selected) - 1);
        subghz_keeloq_console_selected[sizeof(subghz_keeloq_console_selected) - 1] = '\0';
        if(found_info.family == KlFamGeneric) {
            subghz->gen_info->keeloq.manuf = subghz_keeloq_console_selected;
        } else {
            subghz->gen_info->faac_slh.manuf = subghz_keeloq_console_selected;
        }
    }

    // Mark that this generation came from a catalog picker, so the
    // generation-success path offers Emulate/Save/Clone instead of jumping to
    // SaveName (same flag the mfr scene sets).
    subghz->keeloq_mfr_emulate_flow = true;

    // Enter the existing serial/button/counter(/seed) byte-input flow. Every
    // generator reachable from this scene has a real (non-crashing) arm in
    // subghz_scene_set_serial.c / set_button.c / set_counter.c / set_seed.c —
    // verified by reading each dispatch before wiring this route.
    scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSetSerial);
}

// Tapping a decode-only/orphan/car entry: show a short info popup and STAY in
// this scene (Back returns to the catalog list) — never enter a generator.
static void subghz_scene_keeloq_console_decode_callback(void* context, uint32_t index) {
    SubGhz* subghz = context;

    SubGhzKeyArray_t* data = subghz_scene_keeloq_console_get_keys(subghz);
    if(data == NULL) {
        return;
    }

    uint32_t i = 0;
    const char* found_name = NULL;
    for
        M_EACH(key, *data, SubGhzKeyArray_t) {
            const char* name = key->name;
            KeeloqTypeInfo info = subghz_keeloq_registry_type_info(key->type, name);
            if(info.encodable) {
                continue;
            }
            if(i == index) {
                found_name = name;
                break;
            }
            i++;
        }

    if(found_name == NULL) {
        return;
    }

    FuriString* msg = furi_string_alloc_printf(
        "%s\nDecode-only.\nCapture this fob\nto decode / emulate.", found_name);
    widget_reset(subghz->widget);
    widget_add_string_multiline_element(
        subghz->widget, 64, 32, AlignCenter, AlignCenter, FontSecondary, furi_string_get_cstr(msg));
    furi_string_free(msg);

    // scene_state doubles as "the popup, not the list, is on screen" so
    // on_event knows a Back press should return to the submenu rather than
    // leave the whole console scene.
    scene_manager_set_scene_state(subghz->scene_manager, SubGhzSceneKeeloqConsole, 1);
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);
}

void subghz_scene_keeloq_console_on_enter(void* context) {
    SubGhz* subghz = context;

    scene_manager_set_scene_state(subghz->scene_manager, SubGhzSceneKeeloqConsole, 0);

    SubGhzKeyArray_t* data = subghz_scene_keeloq_console_get_keys(subghz);

    size_t total = 0;
    if(data != NULL) {
        for
            M_EACH(key, *data, SubGhzKeyArray_t) {
                UNUSED(key);
                total++;
            }
    }

    if(data == NULL || total == 0) {
        widget_reset(subghz->widget);
        widget_add_string_multiline_element(
            subghz->widget,
            64,
            32,
            AlignCenter,
            AlignCenter,
            FontSecondary,
            "No keystore data\nAdd keeloq_mfcodes_user\nto SD card");
        view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);
        return;
    }

    submenu_reset(subghz->submenu);
    submenu_set_header(subghz->submenu, "KeeLoq Console (all)");

    // Encodable half, grouped by family with one header row per non-empty family.
    uint32_t gen_index = 0;
    for(size_t s = 0; s < COUNT_OF(subghz_keeloq_console_sections); s++) {
        KeeloqFamily fam = subghz_keeloq_console_sections[s].family;
        bool header_added = false;
        for
            M_EACH(key, *data, SubGhzKeyArray_t) {
                const char* name = key->name;
                KeeloqTypeInfo info = subghz_keeloq_registry_type_info(key->type, name);
                if(!info.encodable || info.family != fam) {
                    continue;
                }
                if(!header_added) {
                    submenu_add_item(
                        subghz->submenu,
                        subghz_keeloq_console_sections[s].header,
                        0,
                        subghz_scene_keeloq_console_header_callback,
                        subghz);
                    header_added = true;
                }
                submenu_add_item(
                    subghz->submenu,
                    name,
                    gen_index,
                    subghz_scene_keeloq_console_generate_callback,
                    subghz);
                gen_index++;
            }
    }

    // Decode-only/orphan/car half, single trailing section.
    uint32_t decode_index = 0;
    bool decode_header_added = false;
    for
        M_EACH(key, *data, SubGhzKeyArray_t) {
            const char* name = key->name;
            KeeloqTypeInfo info = subghz_keeloq_registry_type_info(key->type, name);
            if(info.encodable) {
                continue;
            }
            if(!decode_header_added) {
                submenu_add_item(
                    subghz->submenu,
                    "-- Decode-only (capture to use) --",
                    0,
                    subghz_scene_keeloq_console_header_callback,
                    subghz);
                decode_header_added = true;
            }
            submenu_add_item(
                subghz->submenu, name, decode_index, subghz_scene_keeloq_console_decode_callback, subghz);
            decode_index++;
        }

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdMenu);
}

bool subghz_scene_keeloq_console_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;

    if(event.type == SceneManagerEventTypeBack &&
       scene_manager_get_scene_state(subghz->scene_manager, SubGhzSceneKeeloqConsole) == 1) {
        // Returning from the decode-only info popup back to the catalog list —
        // consume the Back press instead of leaving the scene.
        scene_manager_set_scene_state(subghz->scene_manager, SubGhzSceneKeeloqConsole, 0);
        widget_reset(subghz->widget);
        view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdMenu);
        return true;
    }

    return false;
}

void subghz_scene_keeloq_console_on_exit(void* context) {
    SubGhz* subghz = context;
    submenu_reset(subghz->submenu);
    widget_reset(subghz->widget);
}
#endif /* !PUBLIC_BUILD — KeeLoq Console is car/master-key-only, stripped from public */
