#include "../subghz_i.h"
#include "../helpers/subghz_proto_filter.h"

#include <lib/subghz/registry.h>
#include <lib/subghz/subghz_protocol_registry.h>

#include <stdlib.h>
#include <string.h>

#define TAG "SubGhzSceneProtocolList"

// [PROTO FILTER] Toggle list of every protocol in the currently-active registry
// (general set, or the on-demand automotive set when Cars mode is on). Each row
// is ON/OFF; OFF adds the protocol name to the receiver filter so it is dropped
// on decode. A top "== All ==" master row bulk-enables/disables every row.

static const char* const proto_onoff_text[2] = {"ON", "OFF"};
static const char* const proto_master_text[2] = {"All ON", "All OFF"};

typedef struct {
    SubGhz* subghz;
    const char* name; // points into the registry's static name; NULL == master row
    VariableItem* item; // this row's list item, for in-place refresh
} ProtoListRow;

static ProtoListRow* s_rows = NULL;
static size_t s_row_count = 0;

static void subghz_scene_protocol_list_item_changed(VariableItem* item) {
    ProtoListRow* row = variable_item_get_context(item);
    if(row == NULL) return;
    uint8_t idx = variable_item_get_current_value_index(item);
    bool disabled = (idx == 1);

    if(row->name == NULL) {
        // Master row: apply ON/OFF to every protocol (one disk write via the
        // batch guard), then refresh each row's display IN PLACE. We must not
        // rebuild the list here — that would free the item mid-callback.
        subghz_proto_filter_begin_batch();
        const SubGhzProtocolRegistry* reg = subghz_protocol_registry_active();
        size_t count = reg ? subghz_protocol_registry_count(reg) : 0;
        for(size_t i = 0; i < count; i++) {
            const SubGhzProtocol* p = subghz_protocol_registry_get_by_index(reg, i);
            if(p && p->name) subghz_proto_filter_set(p->name, disabled);
        }
        subghz_proto_filter_end_batch();

        for(size_t i = 1; i < s_row_count; i++) {
            if(s_rows[i].item && s_rows[i].name) {
                variable_item_set_current_value_index(s_rows[i].item, disabled ? 1 : 0);
                variable_item_set_current_value_text(
                    s_rows[i].item, proto_onoff_text[disabled ? 1 : 0]);
            }
        }
        variable_item_set_current_value_text(item, proto_master_text[disabled ? 1 : 0]);
        return;
    }

    subghz_proto_filter_set(row->name, disabled);
    variable_item_set_current_value_text(item, proto_onoff_text[disabled ? 1 : 0]);
}

void subghz_scene_protocol_list_on_enter(void* context) {
    SubGhz* subghz = context;
    VariableItemList* vil = subghz->variable_item_list;
    variable_item_list_reset(vil);

    const SubGhzProtocolRegistry* reg = subghz_protocol_registry_active();
    size_t count = reg ? subghz_protocol_registry_count(reg) : 0;

    // Row-context array: index 0 = master row, index i+1 = protocol i.
    free(s_rows);
    s_row_count = count + 1;
    s_rows = malloc(sizeof(ProtoListRow) * s_row_count);
    for(size_t i = 0; i < s_row_count; i++) {
        s_rows[i].subghz = subghz;
        s_rows[i].name = NULL;
        s_rows[i].item = NULL;
    }

    // Master row shows "All OFF" only when every protocol is currently disabled.
    bool all_off = count > 0;
    for(size_t i = 0; i < count; i++) {
        const SubGhzProtocol* p = subghz_protocol_registry_get_by_index(reg, i);
        if(!p || !p->name || !subghz_proto_filter_contains(p->name)) {
            all_off = false;
            break;
        }
    }
    VariableItem* item = variable_item_list_add(
        vil, "== All ==", 2, subghz_scene_protocol_list_item_changed, &s_rows[0]);
    s_rows[0].item = item;
    variable_item_set_current_value_index(item, all_off ? 1 : 0);
    variable_item_set_current_value_text(item, proto_master_text[all_off ? 1 : 0]);

    // Per-protocol rows.
    for(size_t i = 0; i < count; i++) {
        const SubGhzProtocol* p = subghz_protocol_registry_get_by_index(reg, i);
        if(!p || !p->name) continue;
        if(strcmp(p->name, "RAW") == 0) continue; // RAW pseudo-protocol is not filterable

        ProtoListRow* rctx = &s_rows[i + 1];
        rctx->subghz = subghz;
        rctx->name = p->name;

        bool disabled = subghz_proto_filter_contains(p->name);
        item = variable_item_list_add(
            vil, p->name, 2, subghz_scene_protocol_list_item_changed, rctx);
        rctx->item = item;
        variable_item_set_current_value_index(item, disabled ? 1 : 0);
        variable_item_set_current_value_text(item, proto_onoff_text[disabled ? 1 : 0]);
    }

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdVariableItemList);
}

bool subghz_scene_protocol_list_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void subghz_scene_protocol_list_on_exit(void* context) {
    SubGhz* subghz = context;
    variable_item_list_reset(subghz->variable_item_list);
    free(s_rows);
    s_rows = NULL;
    s_row_count = 0;
}
