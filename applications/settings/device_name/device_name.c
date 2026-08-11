/*
 * Device Name settings — rename the device (up to 8 alphanumeric chars) using the
 * standard on-screen keyboard, so the keys are visible and it can be driven over
 * qFlipper's on-screen D-pad as well as by typing on a physical keyboard
 * (Cardputer). The name is persisted to NVS via the namechanger path: applied to
 * the running on-screen name immediately, and to the CLI prompt / BLE name on the
 * next reboot. Leave the field empty to revert to the default name.
 */

#include <furi.h>
#include <furi_hal_version.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/text_input.h>
#include <string.h>

#define TAG "DeviceName"

#define DEVICE_NAME_MAX 8

typedef enum {
    DeviceNameViewInput,
} DeviceNameViewId;

typedef enum {
    DeviceNameEventSave,
} DeviceNameEvent;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    TextInput* text_input;
    char buf[DEVICE_NAME_MAX + 1];
} DeviceNameApp;

/* Reject anything that is not a letter or digit — the name feeds the CLI prompt
 * and BLE advertisement, which only accept a plain alphanumeric identifier. */
static bool device_name_validator(const char* text, FuriString* error, void* context) {
    UNUSED(context);
    for(; *text; ++text) {
        const char c = *text;
        if((c < '0' || c > '9') && (c < 'A' || c > 'Z') && (c < 'a' || c > 'z')) {
            furi_string_printf(error, "Letters and\nnumbers only!");
            return false;
        }
    }
    return true;
}

static void device_name_input_callback(void* context) {
    DeviceNameApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, DeviceNameEventSave);
}

static bool device_name_custom_event_callback(void* context, uint32_t event) {
    DeviceNameApp* app = context;
    if(event == DeviceNameEventSave) {
        /* Empty reverts to default; set_name_persist handles NULL/empty. */
        furi_hal_version_set_name_persist(app->buf[0] ? app->buf : NULL);
        FURI_LOG_I(TAG, "saved name '%s'", app->buf);
        view_dispatcher_stop(app->view_dispatcher);
        return true;
    }
    return false;
}

static bool device_name_navigation_callback(void* context) {
    DeviceNameApp* app = context;
    view_dispatcher_stop(app->view_dispatcher); /* Back exits without saving */
    return true;
}

int32_t device_name_app(void* p) {
    UNUSED(p);
    DeviceNameApp* app = malloc(sizeof(DeviceNameApp));
    memset(app, 0, sizeof(DeviceNameApp));

    /* Pre-fill with the current name so it can be edited in place. */
    const char* cur = furi_hal_version_get_name_ptr();
    bool have_name = false;
    if(cur) {
        strncpy(app->buf, cur, DEVICE_NAME_MAX);
        app->buf[DEVICE_NAME_MAX] = '\0';
        have_name = app->buf[0] != '\0';
    }

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    app->text_input = text_input_alloc();

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, device_name_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, device_name_navigation_callback);
    view_dispatcher_attach_to_gui(
        app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_add_view(
        app->view_dispatcher, DeviceNameViewInput, text_input_get_view(app->text_input));

    text_input_set_header_text(app->text_input, "Device name (empty=default)");
    text_input_set_validator(app->text_input, device_name_validator, NULL);
    text_input_set_minimum_length(app->text_input, 0); /* allow empty to reset */
    text_input_set_result_callback(
        app->text_input,
        device_name_input_callback,
        app,
        app->buf,
        sizeof(app->buf),
        have_name); /* highlight the current name so the first key replaces it */

    view_dispatcher_switch_to_view(app->view_dispatcher, DeviceNameViewInput);
    view_dispatcher_run(app->view_dispatcher);

    view_dispatcher_remove_view(app->view_dispatcher, DeviceNameViewInput);
    text_input_free(app->text_input);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
