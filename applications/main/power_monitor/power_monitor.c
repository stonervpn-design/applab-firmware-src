#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <power/power_service/power.h>
#include "views/power_view.h"
#include "power_estimate.h"

#define POWER_MONITOR_CAPACITY_MAH 1400u // Cardputer-ADV nominal pack; tune later

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Power* power;
    View* view;
} PowerMonitorApp;

enum { PowerMonitorViewMain = 0 };

static uint32_t power_monitor_exit(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static void power_monitor_tick(void* context) {
    PowerMonitorApp* app = context;
    PowerInfo info;
    power_get_info(app->power, &info);
    int32_t current_ma = (int32_t)(info.current_gauge * 1000.0f);
    uint32_t mag = (uint32_t)(current_ma < 0 ? -current_ma : current_ma);
    int32_t est = power_estimate_minutes(
        info.is_charging, mag, POWER_MONITOR_CAPACITY_MAH, info.charge);
    power_view_update(app->view, info.charge, info.voltage_gauge,
                      current_ma, info.is_charging, est);
}

int32_t power_monitor_app(void* p) {
    UNUSED(p);
    PowerMonitorApp* app = malloc(sizeof(PowerMonitorApp));
    app->gui = furi_record_open(RECORD_GUI);
    app->power = furi_record_open(RECORD_POWER);
    app->view_dispatcher = view_dispatcher_alloc();
    app->view = power_view_alloc();
    view_set_previous_callback(app->view, power_monitor_exit);
    view_dispatcher_add_view(app->view_dispatcher, PowerMonitorViewMain, app->view);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_tick_event_callback(app->view_dispatcher, power_monitor_tick, 1000);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    power_monitor_tick(app); // seed one reading
    view_dispatcher_switch_to_view(app->view_dispatcher, PowerMonitorViewMain);
    view_dispatcher_run(app->view_dispatcher);

    view_dispatcher_remove_view(app->view_dispatcher, PowerMonitorViewMain);
    power_view_free(app->view);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_POWER);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
