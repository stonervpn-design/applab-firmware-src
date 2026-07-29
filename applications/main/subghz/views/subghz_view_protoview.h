#pragma once
#include <gui/view.h>

typedef struct SubGhzViewProtoview SubGhzViewProtoview;

typedef enum {
    ProtoviewModeScope, // live/frozen waveform
    ProtoviewModeEdit, // bit editor
} ProtoviewMode;

// Events emitted through the view callback; the scene maps these to radio/file actions.
typedef enum {
    ProtoviewEvtFreeze,
    ProtoviewEvtDecode,
    ProtoviewEvtEdit,
    ProtoviewEvtReplay,
    ProtoviewEvtSave,
    ProtoviewEvtBack,
} ProtoviewEvent;

typedef void (*SubGhzViewProtoviewCallback)(uint32_t event, void* context);

SubGhzViewProtoview* subghz_view_protoview_alloc(void);
void subghz_view_protoview_free(SubGhzViewProtoview* v);
View* subghz_view_protoview_get_view(SubGhzViewProtoview* v);
void subghz_view_protoview_set_callback(
    SubGhzViewProtoview* v,
    SubGhzViewProtoviewCallback cb,
    void* ctx);
void subghz_view_protoview_set_frozen(SubGhzViewProtoview* v, bool frozen);
void subghz_view_protoview_set_bits(SubGhzViewProtoview* v, const char* bits, const char* label);
void subghz_view_protoview_set_status(SubGhzViewProtoview* v, const char* freq_preset);

// Read the (possibly edited) bitstring back out for replay/save.
const char* subghz_view_protoview_get_bits(SubGhzViewProtoview* v);
