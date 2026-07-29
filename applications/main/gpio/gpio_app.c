#include "gpio_app_i.h"
#include "gpio_driver.h"

#include <furi.h>
#include <furi_hal.h>
#include <string.h>

static bool gpio_app_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    GpioApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool gpio_app_back_event_callback(void* context) {
    furi_assert(context);
    GpioApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void gpio_app_tick_event_callback(void* context) {
    furi_assert(context);
    GpioApp* app = context;
    scene_manager_handle_tick_event(app->scene_manager);
}

GpioApp* gpio_app_alloc(void) {
    GpioApp* app = malloc(sizeof(GpioApp));

    app->expansion = furi_record_open(RECORD_EXPANSION);
    expansion_disable(app->expansion);

    app->gui = furi_record_open(RECORD_GUI);
    app->gpio_items = gpio_items_alloc();

    app->power = furi_record_open(RECORD_POWER);

    // Manual Control state
    gpio_driver_init();
    memset(app->pin_state, 0, sizeof(app->pin_state));
    for(size_t i = 0; i < COUNT_OF(app->pin_state); i++) {
        app->pin_state[i].mode = GpioPinModeOff;
        app->pin_state[i].pull = GpioDriverPullNone;
        app->pin_state[i].pwm_freq = 1000; // Task 4 default
        app->pin_state[i].pwm_duty = 50; // Task 4 default
    }
    app->show_advanced = false;
    app->edit_index = 0;
    app->manual_timer = NULL;

    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&gpio_scene_handlers, app);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);

    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, gpio_app_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, gpio_app_back_event_callback);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, gpio_app_tick_event_callback, 100);

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    // Dialog view
    app->dialog = dialog_ex_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, GpioAppViewExitConfirm, dialog_ex_get_view(app->dialog));

    app->var_item_list = variable_item_list_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        GpioAppViewVarItemList,
        variable_item_list_get_view(app->var_item_list));
    app->manual_list = gpio_manual_list_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, GpioAppViewManualList, gpio_manual_list_get_view(app->manual_list));

    app->manual_edit = gpio_manual_edit_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, GpioAppViewManualEdit, gpio_manual_edit_get_view(app->manual_edit));

    app->widget = widget_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, GpioAppViewUsbUartCloseRpc, widget_get_view(app->widget));

    app->gpio_usb_uart = gpio_usb_uart_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, GpioAppViewUsbUart, gpio_usb_uart_get_view(app->gpio_usb_uart));

    view_dispatcher_add_view(
        app->view_dispatcher,
        GpioAppViewUsbUartCfg,
        variable_item_list_get_view(app->var_item_list));

    scene_manager_next_scene(app->scene_manager, GpioSceneStart);

    return app;
}

void gpio_app_free(GpioApp* app) {
    furi_assert(app);

    /* Safety net: ensure no pin stays driven if the app is torn down by any path
       other than a Back out of the Manual Control list. Idempotent. */
    gpio_driver_release_all();

    // Views
    view_dispatcher_remove_view(app->view_dispatcher, GpioAppViewVarItemList);
    view_dispatcher_remove_view(app->view_dispatcher, GpioAppViewManualList);
    view_dispatcher_remove_view(app->view_dispatcher, GpioAppViewManualEdit);
    view_dispatcher_remove_view(app->view_dispatcher, GpioAppViewUsbUart);
    view_dispatcher_remove_view(app->view_dispatcher, GpioAppViewUsbUartCfg);
    view_dispatcher_remove_view(app->view_dispatcher, GpioAppViewUsbUartCloseRpc);
    view_dispatcher_remove_view(app->view_dispatcher, GpioAppViewExitConfirm);
    variable_item_list_free(app->var_item_list);
    widget_free(app->widget);
    gpio_manual_list_free(app->manual_list);
    gpio_manual_edit_free(app->manual_edit);
    gpio_usb_uart_free(app->gpio_usb_uart);
    dialog_ex_free(app->dialog);

    // View dispatcher
    view_dispatcher_free(app->view_dispatcher);
    scene_manager_free(app->scene_manager);

    // Close records
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_POWER);

    expansion_enable(app->expansion);
    furi_record_close(RECORD_EXPANSION);

    gpio_items_free(app->gpio_items);
    free(app);
}

int32_t gpio_app(void* p) {
    UNUSED(p);
    GpioApp* gpio_app = gpio_app_alloc();

    view_dispatcher_run(gpio_app->view_dispatcher);

    gpio_app_free(gpio_app);

    return 0;
}
