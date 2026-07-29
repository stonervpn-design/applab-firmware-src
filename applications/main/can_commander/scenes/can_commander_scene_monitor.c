#include "../can_commander_app_i.h"
#include <gui/canvas.h>
#include <string.h>
#include <stdio.h>

static int32_t can_commander_rx_worker(void* context) {
    CanCommanderApp* app = context;
    CanFrame f;
    while(app->rx_run) {
        if(can_hal_receive(&f, 100)) {
            char row[CAN_MON_ROW_LEN];
            can_frame_format(&f, row, sizeof(row));
            furi_mutex_acquire(app->mon_mutex, FuriWaitForever);
            strncpy(app->mon_rows[app->mon_head], row, CAN_MON_ROW_LEN - 1);
            app->mon_rows[app->mon_head][CAN_MON_ROW_LEN - 1] = '\0';
            app->mon_head = (app->mon_head + 1) % CAN_MON_ROWS;
            if(app->mon_fill < CAN_MON_ROWS) app->mon_fill++;
            app->mon_total++;
            furi_mutex_release(app->mon_mutex);
        }
    }
    return 0;
}

void can_commander_scene_monitor_on_enter(void* context) {
    furi_assert(context);
    CanCommanderApp* app = context;

    furi_mutex_acquire(app->mon_mutex, FuriWaitForever);
    app->mon_head = 0;
    app->mon_fill = 0;
    app->mon_total = 0;
    furi_mutex_release(app->mon_mutex);

    widget_reset(app->widget);

    CanHalConfig cfg = {
        .bitrate = app->bitrate,
        .mode = app->mode,
        .tx_gpio = app->tx_gpio,
        .rx_gpio = app->rx_gpio,
    };
    if(!can_hal_start(&cfg)) {
        widget_add_string_multiline_element(
            app->widget,
            64,
            28,
            AlignCenter,
            AlignCenter,
            FontSecondary,
            "CAN init failed\nCheck bitrate / pins");
    } else {
        app->rx_run = true;
        app->rx_thread = furi_thread_alloc_ex("CanRx", 2048, can_commander_rx_worker, app);
        furi_thread_start(app->rx_thread);
        widget_add_string_multiline_element(
            app->widget, 2, 12, AlignLeft, AlignTop, FontSecondary, "Listening...");
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, CanViewWidget);
}

bool can_commander_scene_monitor_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    CanCommanderApp* app = context;
    if(event.type != SceneManagerEventTypeTick) return false;
    if(!app->rx_thread) return false;

    char buf[CAN_MON_ROWS * (CAN_MON_ROW_LEN + 1) + 32];
    size_t pos = 0;
    int w;

    furi_mutex_acquire(app->mon_mutex, FuriWaitForever);
    w = snprintf(buf + pos, sizeof(buf) - pos, "RX %lu frames\n", (unsigned long)app->mon_total);
    if(w > 0) pos += (size_t)w;
    for(uint8_t i = 0; i < app->mon_fill && pos < sizeof(buf) - 1; i++) {
        uint8_t idx = (uint8_t)((app->mon_head + CAN_MON_ROWS - 1 - i) % CAN_MON_ROWS);
        w = snprintf(buf + pos, sizeof(buf) - pos, "%s\n", app->mon_rows[idx]);
        if(w <= 0) break;
        pos += (size_t)w;
    }
    furi_mutex_release(app->mon_mutex);
    if(pos >= sizeof(buf)) pos = sizeof(buf) - 1;
    buf[pos] = '\0';

    widget_reset(app->widget);
    widget_add_text_scroll_element(app->widget, 0, 0, 128, 64, buf);
    return true;
}

void can_commander_scene_monitor_on_exit(void* context) {
    furi_assert(context);
    CanCommanderApp* app = context;
    if(app->rx_thread) {
        app->rx_run = false;
        furi_thread_join(app->rx_thread);
        furi_thread_free(app->rx_thread);
        app->rx_thread = NULL;
    }
    can_hal_stop();
    widget_reset(app->widget);
}
