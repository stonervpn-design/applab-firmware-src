#pragma once
#include <furi.h>
#include "can_commander_app.h"
#include "can_commander_custom_event.h"
#include "scenes/can_commander_scene.h"
#include "can_hal.h"
#include "can_frame.h"

#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/popup.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/widget.h>

#define CAN_FRAME_INPUT_LEN 32
#define CAN_MON_ROWS 10
#define CAN_MON_ROW_LEN 24
#define CAN_PRESET_MAX 16
#define CAN_PRESET_NAME_LEN 24
#define CAN_PRESET_PATH "/ext/can_commander/presets.csv"
#define CAN_PRESET_DIR "/ext/can_commander"

typedef enum {
    CanViewSubmenu,
    CanViewTextInput,
    CanViewPopup,
    CanViewVarItemList,
    CanViewWidget,
} CanView;

struct CanCommanderApp {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    Submenu* submenu;
    TextInput* text_input;
    Popup* popup;
    VariableItemList* var_item_list;
    Widget* widget;

    // Session config
    uint32_t bitrate; // 125000/250000/500000/1000000
    CanMode mode;
    uint8_t tx_gpio; // Grove GPIO2 default
    uint8_t rx_gpio; // Grove GPIO1 default

    // Send input buffer ("ID#DATA")
    char frame_input[CAN_FRAME_INPUT_LEN];

    // Monitor worker + display ring
    FuriThread* rx_thread;
    volatile bool rx_run;
    FuriMutex* mon_mutex;
    char mon_rows[CAN_MON_ROWS][CAN_MON_ROW_LEN];
    uint8_t mon_head; // next write slot
    uint8_t mon_fill; // rows populated (<= CAN_MON_ROWS)
    uint32_t mon_total; // total frames seen

    // Periodic TX worker
    FuriThread* tx_thread;
    volatile bool tx_run;
    uint32_t tx_interval_ms;
    volatile uint32_t tx_count;
    CanFrame tx_frame;

    // Fuzz worker (mirrors the periodic TX worker above)
    FuriThread* fuzz_thread;
    volatile bool fuzz_run;
    volatile uint32_t fuzz_count;
    uint8_t fuzz_mode; // CanFuzzMode (see helpers/can_fuzz.h); stored as a
        // plain uint8_t so this header doesn't need to include can_fuzz.h

    // Presets (loaded from SD)
    CanFrame presets[CAN_PRESET_MAX];
    char preset_names[CAN_PRESET_MAX][CAN_PRESET_NAME_LEN];
    uint8_t preset_count;
    char preset_name_input[CAN_PRESET_NAME_LEN];
};

// Preset SD storage helpers (see can_presets.c).
void can_presets_load(CanCommanderApp* app);
bool can_presets_append(CanCommanderApp* app, const char* name, const CanFrame* f);

// Parse the "ID#DATA" text-input string into a CanFrame. Returns false if malformed.
bool can_commander_parse_input(const char* s, CanFrame* out);
