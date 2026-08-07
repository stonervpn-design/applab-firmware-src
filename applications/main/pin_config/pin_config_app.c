#include "pin_config_store.h"
#include "pin_config_sd.h"
#include "pin_config_apply.h"

#include <furi.h>
#include <furi_hal_subghz.h>
#include <furi_hal_power.h>
#include <furi_hal_resources.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/variable_item_list.h>
#include <stdio.h>
#include <string.h>

#define PIN_CONFIG_VIEW 0

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    VariableItemList* vil;
    VariableItem* items[PinFuncCount];
    char status_hdr[24];
} PinConfigApp;

/* Live-probe both radios and update the compact header (Y=found, X=not). */
static void pin_config_update_status(PinConfigApp* app) {
    bool cc = furi_hal_subghz_check_connection();
    bool nrf = furi_hal_nrf24_is_connected();
    snprintf(
        app->status_hdr, sizeof(app->status_hdr), "CC1101:%c  NRF24:%c", cc ? 'Y' : 'X',
        nrf ? 'Y' : 'X');
    variable_item_list_set_header(app->vil, app->status_hdr);
}

/* Periodic background re-probe so plugging a module in shows up without any
 * interaction (runs on the view_dispatcher thread). */
static void pin_config_tick_cb(void* context) {
    pin_config_update_status((PinConfigApp*)context);
}

static uint8_t pin_choice_index(uint8_t gpio) {
    for(size_t i = 0; i < pin_config_choices_count; i++) {
        if(pin_config_choices[i] == gpio) return (uint8_t)i;
    }
    return 0;
}

static void pin_config_set_value_text(VariableItem* item, uint8_t gpio) {
    char t[12];
    snprintf(t, sizeof(t), "GPIO%u", gpio);
    variable_item_set_current_value_text(item, t);
}

static void pin_config_item_changed(VariableItem* item) {
    PinConfigApp* app = variable_item_get_context(item);
    int f = -1;
    for(int i = 0; i < PinFuncCount; i++) {
        if(app->items[i] == item) {
            f = i;
            break;
        }
    }
    if(f < 0) return;
    uint8_t idx = variable_item_get_current_value_index(item);
    if(idx >= pin_config_choices_count) return;

    uint8_t cur = pin_config_get((PinFunc)f);
    uint8_t cur_idx = pin_choice_index(cur);

    /* Assigning two functions to one pin hangs the shared SPI3/SD bus and
     * watchdog-reboots the device, so a pin already used by a bus line
     * (SPI SCK/MISO/MOSI, I2C SDA/SCL) or the other radio is never applied.
     *
     * But snapping back to the current pin on a conflict trapped the user below
     * the first used pin: because the list steps one value at a time, any used
     * pin between the current one and a free one reverted the selection, so a
     * free higher/lower pin was unreachable (the "can't set a higher number"
     * bug). Instead, skip forward in the scroll direction to the next FREE pin so
     * scrolling passes over used pins to reach an assignable one. This lets a
     * radio be moved off a pin the other radio needs (e.g. NRF24 off 4/6 to 15),
     * which then frees 4/6 for CC1101. */
    int dir = (idx >= cur_idx) ? 1 : -1;
    int scan = (int)idx;
    PinFunc conflict = PinFuncCount;
    while(scan >= 0 && scan < (int)pin_config_choices_count &&
          pin_config_pin_conflict((PinFunc)f, pin_config_choices[scan], &conflict)) {
        scan += dir;
    }

    if(scan < 0 || scan >= (int)pin_config_choices_count) {
        /* Every pin in this direction is taken — keep the current assignment. */
        FURI_LOG_W(
            "PinConfig", "no free pin for %s past GPIO%u (in use by %s)",
            pin_config_func_name((PinFunc)f), pin_config_choices[idx],
            pin_config_func_name(conflict));
        variable_item_set_current_value_index(item, cur_idx);
        pin_config_set_value_text(item, cur);
        snprintf(
            app->status_hdr, sizeof(app->status_hdr), "In use: %s",
            pin_config_func_name(conflict));
        variable_item_list_set_header(app->vil, app->status_hdr);
        return;
    }

    uint8_t gpio = pin_config_choices[scan];
    if(scan != (int)idx) {
        /* Skipped past used pin(s) — move the selection to the free pin. */
        variable_item_set_current_value_index(item, (uint8_t)scan);
    }

    FURI_LOG_I("PinConfig", "apply %s=GPIO%u", pin_config_func_name((PinFunc)f), gpio);
    pin_config_set((PinFunc)f, gpio);
    pin_config_set_value_text(item, gpio);
    pin_config_apply(); // persists to NVS (single source of truth) + applies now
    pin_config_update_status(app); // live re-probe reflects the new pin now
}

static bool pin_config_nav_cb(void* context) {
    PinConfigApp* app = context;
    view_dispatcher_stop(app->view_dispatcher);
    return true;
}

static void pin_config_refresh_items(PinConfigApp* app) {
    for(int f = 0; f < PinFuncCount; f++) {
        uint8_t gpio = pin_config_get((PinFunc)f);
        variable_item_set_current_value_index(
            app->items[f], pin_config_is_movable((PinFunc)f) ? pin_choice_index(gpio) : 0);
        pin_config_set_value_text(app->items[f], gpio);
    }
}

/* Action rows past the pin items: PinFuncCount = Reset, PinFuncCount+1 = Reboot. */
static void pin_config_enter_cb(void* context, uint32_t index) {
    PinConfigApp* app = context;
    if(index == PinFuncCount) {
        pin_config_reset_defaults();
        pin_config_apply();
        pin_config_refresh_items(app);
        pin_config_update_status(app);
    } else if(index == PinFuncCount + 1) {
        furi_hal_power_reset(); /* no return — pins are already saved to NVS */
    }
}

/* Populate the store from NVS (the authoritative store the HAL applies at boot),
 * so the UI always shows exactly what the hardware uses. Fixed SPI/I2C pins keep
 * their board defaults. This replaces the old SD-file display store, which could
 * drift out of sync with NVS. */
static void pin_config_load_from_nvs(void) {
    pin_config_reset_defaults();
    uint8_t csn, gdo0, ncsn, nce, irtx, irrx;
    furi_hal_module_pins_read_nvs(&csn, &gdo0, &ncsn, &nce, &irtx, &irrx);
    pin_config_set(PinFuncCc1101Csn, csn);
    pin_config_set(PinFuncCc1101Gdo0, gdo0);
    pin_config_set(PinFuncNrf24Csn, ncsn);
    pin_config_set(PinFuncNrf24Ce, nce);
    pin_config_set(PinFuncIrTx, irtx);
    pin_config_set(PinFuncIrRx, irrx);
}

int32_t pin_config_app(void* p) {
    UNUSED(p);
    pin_config_load_from_nvs();

    PinConfigApp* app = malloc(sizeof(PinConfigApp));
    furi_check(app); /* OOM: clean panic instead of a NULL-deref (port malloc can return NULL) */
    memset(app, 0, sizeof(PinConfigApp));
    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    app->vil = variable_item_list_alloc();

    for(int f = 0; f < PinFuncCount; f++) {
        uint8_t gpio = pin_config_get((PinFunc)f);
        VariableItem* it;
        if(pin_config_is_movable((PinFunc)f)) {
            it = variable_item_list_add(
                app->vil,
                pin_config_func_name((PinFunc)f),
                pin_config_choices_count,
                pin_config_item_changed,
                app);
            variable_item_set_current_value_index(it, pin_choice_index(gpio));
        } else {
            /* Fixed pin (SPI/I2C): single value, no change callback -> read-only.
             * Editing it before had no hardware effect and looked like "not saving". */
            it = variable_item_list_add(app->vil, pin_config_func_name((PinFunc)f), 1, NULL, app);
            variable_item_set_current_value_index(it, 0);
        }
        pin_config_set_value_text(it, gpio);
        app->items[f] = it;
    }

    /* Action rows after the pins; handled via the enter callback. */
    variable_item_list_add(app->vil, "Reset to Defaults", 0, NULL, app);
    variable_item_list_add(app->vil, "Reboot & Apply", 0, NULL, app);
    variable_item_list_set_enter_callback(app->vil, pin_config_enter_cb, app);

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, pin_config_nav_cb);
    view_dispatcher_set_tick_event_callback(app->view_dispatcher, pin_config_tick_cb, 2500);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_add_view(
        app->view_dispatcher, PIN_CONFIG_VIEW, variable_item_list_get_view(app->vil));
    view_dispatcher_switch_to_view(app->view_dispatcher, PIN_CONFIG_VIEW);

    /* Probe after the view is live so set_header triggers a redraw right away. */
    pin_config_update_status(app);

    view_dispatcher_run(app->view_dispatcher);

    view_dispatcher_remove_view(app->view_dispatcher, PIN_CONFIG_VIEW);
    variable_item_list_free(app->vil);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
