#include "../subghz_i.h"

#include <lib/subghz/subghz_protocol_registry.h>
#include <lib/subghz/types.h>

#include <furi_hal_version.h>
#include <storage/storage.h>
#include <toolbox/stream/stream.h>
#include <toolbox/stream/file_stream.h>

#define SUBGHZ_TOOLKIT_PROTOCOLS_PATH EXT_PATH("subghz/analysis/protocol_analysis.txt")

extern const SubGhzProtocolRegistry subghz_protocol_registry;

static const char* subghz_scene_toolkit_protocols_type_str(SubGhzProtocolType type) {
    switch(type) {
    case SubGhzProtocolTypeStatic:
        return "Static";
    case SubGhzProtocolTypeDynamic:
        return "Dynamic";
    default:
        return "Unknown";
    }
}

void subghz_scene_toolkit_protocols_on_enter(void* context) {
    SubGhz* subghz = context;

    size_t n = subghz_protocol_registry_count(&subghz_protocol_registry);

    FuriString* body = furi_string_alloc();

    // Open the export file. If it fails we still show the on-screen list.
    Storage* storage = furi_record_open(RECORD_STORAGE);
    Stream* stream = file_stream_alloc(storage);
    bool file_ok = false;

    do {
        storage_simply_mkdir(storage, EXT_PATH("subghz"));
        storage_simply_mkdir(storage, EXT_PATH("subghz/analysis"));

        if(!file_stream_open(
               stream, SUBGHZ_TOOLKIT_PROTOCOLS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
            break;
        }

        stream_write_format(
            stream,
            "==============================================\n"
            "        SubGhz Protocol Analysis\n"
            "==============================================\n\n"
            "Model:  %s\n"
            "HW ver: %u\n"
            "Target: %u\n\n"
            "Total Protocols: %zu\n\n",
            furi_hal_version_get_model_name(),
            furi_hal_version_get_hw_version(),
            furi_hal_version_get_hw_target(),
            n);

        file_ok = true;
    } while(0);

    for(size_t i = 0; i < n; i++) {
        const SubGhzProtocol* p =
            subghz_protocol_registry_get_by_index(&subghz_protocol_registry, i);
        if(p == NULL || p->name == NULL) continue;

        const char* type_str = subghz_scene_toolkit_protocols_type_str(p->type);

        furi_string_cat_printf(
            body, "%s\n  %s  flag=0x%08lX\n\n", p->name, type_str, (uint32_t)p->flag);

        if(file_ok) {
            stream_write_format(
                stream, "%s\n  %s  flag=0x%08lX\n\n", p->name, type_str, (uint32_t)p->flag);
        }
    }

    stream_free(stream);
    furi_record_close(RECORD_STORAGE);

    furi_string_cat_printf(body, "Total Protocols: %zu\n\n", n);
    furi_string_cat_str(body, "Exported:\n/ext/subghz/analysis/\nprotocol_analysis.txt\n");

    widget_reset(subghz->widget);
    widget_add_text_scroll_element(subghz->widget, 0, 0, 128, 64, furi_string_get_cstr(body));

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdWidget);

    // The text-scroll element copies the string (furi_string_alloc_set), so it
    // is safe to free the FuriString immediately (same as subghz_scene_vuln_list).
    furi_string_free(body);
}

bool subghz_scene_toolkit_protocols_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void subghz_scene_toolkit_protocols_on_exit(void* context) {
    SubGhz* subghz = context;
    widget_reset(subghz->widget);
}
