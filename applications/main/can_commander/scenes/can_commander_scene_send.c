#include "../can_commander_app_i.h"
#include <string.h>
#include <stdio.h>

static void can_commander_send_input_cb(void* context) {
    CanCommanderApp* app = context;
    scene_manager_handle_custom_event(app->scene_manager, CanCommanderEvtFrameEntered);
}

// Popup timeout/press: go back to the text input to send another.
static void can_commander_send_popup_cb(void* context) {
    CanCommanderApp* app = context;
    view_dispatcher_switch_to_view(app->view_dispatcher, CanViewTextInput);
}

static bool can_commander_ensure_started(CanCommanderApp* app) {
    if(can_hal_is_running()) return true;
    CanHalConfig cfg = {
        .bitrate = app->bitrate,
        .mode = app->mode,
        .tx_gpio = app->tx_gpio,
        .rx_gpio = app->rx_gpio,
    };
    return can_hal_start(&cfg);
}

void can_commander_scene_send_on_enter(void* context) {
    furi_assert(context);
    CanCommanderApp* app = context;
    text_input_reset(app->text_input);
    text_input_set_header_text(app->text_input, "Frame  ID#DATA (hex)");
    text_input_set_result_callback(
        app->text_input,
        can_commander_send_input_cb,
        app,
        app->frame_input,
        sizeof(app->frame_input),
        false);
    view_dispatcher_switch_to_view(app->view_dispatcher, CanViewTextInput);
}

bool can_commander_scene_send_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    CanCommanderApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;
    if(event.event != CanCommanderEvtFrameEntered) return false;

    CanFrame frame;
    char msg[64];
    bool ok = false;

    if(!can_commander_parse_input(app->frame_input, &frame)) {
        snprintf(msg, sizeof(msg), "Bad frame:\n%s", app->frame_input);
    } else if(!can_commander_ensure_started(app)) {
        snprintf(msg, sizeof(msg), "CAN init failed\n(bitrate/pins?)");
    } else {
        ok = can_hal_transmit(&frame, 60);
        char fmt[32];
        can_frame_format(&frame, fmt, sizeof(fmt));
        if(ok) {
            snprintf(msg, sizeof(msg), "Sent:\n%s", fmt);
        } else {
            snprintf(
                msg,
                sizeof(msg),
                "TX failed:\n%s\n(no ACK / bus?)",
                fmt);
        }
    }

    popup_reset(app->popup);
    popup_set_header(app->popup, ok ? "OK" : "Error", 64, 4, AlignCenter, AlignTop);
    popup_set_text(app->popup, msg, 64, 22, AlignCenter, AlignTop);
    popup_set_context(app->popup, app);
    popup_set_callback(app->popup, can_commander_send_popup_cb);
    popup_set_timeout(app->popup, 1600);
    popup_enable_timeout(app->popup);
    view_dispatcher_switch_to_view(app->view_dispatcher, CanViewPopup);
    return true;
}

void can_commander_scene_send_on_exit(void* context) {
    furi_assert(context);
    CanCommanderApp* app = context;
    // Release the TWAI driver (frees the Grove pins for the NFC I2C bus).
    can_hal_stop();
    text_input_reset(app->text_input);
    popup_reset(app->popup);
}
