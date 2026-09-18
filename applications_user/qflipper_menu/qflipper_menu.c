/* qFlipper — main-menu entry that toggles the background USB bridge.
 *
 * The bridge itself lives in the desktop service
 * (applications/services/desktop/helpers/qflipper_bridge.c); it installs the
 * Flipper USB composite (0483:5740) and pipes an RPC session over CDC so the
 * desktop qFlipper app can talk to the device. This little app is just a
 * main-menu front-end: it shows the current state and toggles it on OK.
 *
 * NOTE: turning the bridge ON switches the ESP32-S3 USB PHY from
 * USB-Serial-JTAG to USB-OTG, so the serial console / esptool auto-flash go
 * away until the next reboot (the on-screen hint reminds the user).
 */
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>

/* Public API of the desktop's qflipper_bridge. Declared here (not via its
 * private header) so this user app doesn't depend on desktop internals; the
 * symbols resolve at final link (the bridge object is always built in). */
bool qflipper_bridge_start(void);
void qflipper_bridge_stop(void);
bool qflipper_bridge_is_active(void);

typedef struct {
    FuriMessageQueue* queue;
    ViewPort* vp;
    bool running;
} QflipperMenuApp;

static void qflipper_menu_draw(Canvas* canvas, void* ctx) {
    UNUSED(ctx);
    const bool on = qflipper_bridge_is_active();

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 6, AlignCenter, AlignTop, "qFlipper");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(
        canvas, 64, 24, AlignCenter, AlignTop, on ? "USB bridge: ON" : "USB bridge: OFF");
    canvas_draw_str_aligned(
        canvas, 64, 38, AlignCenter, AlignTop, on ? "OK: turn off" : "OK: turn on");
    /* Remind the user of the console/flash tradeoff while the bridge is up. */
    canvas_draw_str_aligned(
        canvas, 64, 51, AlignCenter, AlignTop, on ? "(serial off till reboot)" : "Back: exit");
}

static void qflipper_menu_input(InputEvent* event, void* ctx) {
    QflipperMenuApp* app = ctx;
    furi_message_queue_put(app->queue, event, FuriWaitForever);
}

int32_t qflipper_menu_app(void* p) {
    UNUSED(p);
    QflipperMenuApp* app = malloc(sizeof(QflipperMenuApp));
    furi_check(app);
    app->queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->running = true;

    Gui* gui = furi_record_open(RECORD_GUI);
    app->vp = view_port_alloc();
    view_port_draw_callback_set(app->vp, qflipper_menu_draw, app);
    view_port_input_callback_set(app->vp, qflipper_menu_input, app);
    gui_add_view_port(gui, app->vp, GuiLayerFullscreen);

    InputEvent event;
    while(app->running) {
        if(furi_message_queue_get(app->queue, &event, FuriWaitForever) != FuriStatusOk) continue;

        /* Back (tap or hold) exits; OK toggles the bridge. */
        if(event.key == InputKeyBack &&
           (event.type == InputTypeShort || event.type == InputTypeLong)) {
            app->running = false;
        } else if(event.key == InputKeyOk && event.type == InputTypeShort) {
            if(qflipper_bridge_is_active()) {
                qflipper_bridge_stop();
            } else {
                qflipper_bridge_start();
            }
            view_port_update(app->vp);
        }
    }

    gui_remove_view_port(gui, app->vp);
    view_port_free(app->vp);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app->queue);
    free(app);
    return 0;
}
