/*
 * Device Name settings — type a custom device name (2-8 alphanumeric chars) on
 * the Cardputer's physical keyboard. Saved to the file the namechanger service
 * reads at boot; namechanger applies it to the CLI prompt, BLE name and the
 * on-screen name. Applied to the running name immediately; BLE/CLI on reboot.
 */

#include <furi.h>
#include <furi_hal_version.h>
#include <gui/gui.h>
#include <input/input.h>
#include <string.h>

#define TAG "DeviceName"

#define DEVICE_NAME_MIN 2
#define DEVICE_NAME_MAX 8

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* queue;
    ViewPort* view_port;
    char buf[DEVICE_NAME_MAX + 1];
    uint8_t len;
    const char* status; /* transient message under the field */
} DeviceNameApp;

static bool name_save(const char* name) {
    /* NVS-backed: survives reboot on this port and applies immediately. */
    furi_hal_version_set_name_persist(name);
    FURI_LOG_I(TAG, "saved name '%s'", name);
    return true;
}

static void render_callback(Canvas* canvas, void* ctx) {
    DeviceNameApp* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "Device Name");
    canvas_draw_line(canvas, 0, 14, 127, 14);

    /* input field */
    canvas_draw_frame(canvas, 2, 20, 124, 16);
    canvas_set_font(canvas, FontKeyboard);
    canvas_draw_str(canvas, 6, 31, app->buf);
    /* caret */
    int caret_x = 6 + canvas_string_width(canvas, app->buf) + 1;
    canvas_draw_line(canvas, caret_x, 23, caret_x, 33);

    canvas_set_font(canvas, FontSecondary);
    if(app->status) {
        canvas_draw_str(canvas, 2, 47, app->status);
    } else {
        canvas_draw_str(canvas, 2, 47, "Type A-Z a-z 0-9 (2-8)");
    }
    canvas_draw_str(canvas, 2, 62, "Enter:Save  Del:Erase  Hold Del:Exit");

    furi_mutex_release(app->mutex);
}

static void input_callback(InputEvent* event, void* ctx) {
    DeviceNameApp* app = ctx;
    furi_message_queue_put(app->queue, event, FuriWaitForever);
}

static bool is_name_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

int32_t device_name_app(void* p) {
    UNUSED(p);
    DeviceNameApp* app = malloc(sizeof(DeviceNameApp));
    memset(app, 0, sizeof(DeviceNameApp));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->queue = furi_message_queue_alloc(16, sizeof(InputEvent));

    const char* cur = furi_hal_version_get_name_ptr();
    if(cur) {
        strncpy(app->buf, cur, DEVICE_NAME_MAX);
        app->buf[DEVICE_NAME_MAX] = '\0';
        app->len = (uint8_t)strlen(app->buf);
    }

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, render_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, app->view_port, GuiLayerFullscreen);

    bool running = true;
    InputEvent ev;
    while(running) {
        if(furi_message_queue_get(app->queue, &ev, FuriWaitForever) != FuriStatusOk) continue;

        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->status = NULL;

        if(ev.type == InputTypeText) {
            char ch = (char)ev.key;
            if(is_name_char(ch) && app->len < DEVICE_NAME_MAX) {
                app->buf[app->len++] = ch;
                app->buf[app->len] = '\0';
            }
        } else if(ev.type == InputTypeShort && ev.key == InputKeyBack) {
            if(app->len > 0) {
                app->buf[--app->len] = '\0';
            } else {
                running = false;
            }
        } else if(ev.type == InputTypeLong && ev.key == InputKeyBack) {
            running = false; /* exit without saving */
        } else if(ev.type == InputTypeShort && ev.key == InputKeyOk) {
            if(app->len < DEVICE_NAME_MIN) {
                app->status = "Too short (min 2)";
            } else if(name_save(app->buf)) {
                running = false;
            } else {
                app->status = "Save failed (SD?)";
            }
        }

        furi_mutex_release(app->mutex);
        view_port_update(app->view_port);
    }

    gui_remove_view_port(gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app->queue);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
