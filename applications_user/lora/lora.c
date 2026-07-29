// LoRa Monitor for the Cap LoRa-1262 (SX1262) on the Cardputer-ADV.
// Listens on 868 MHz SF7/BW125 and shows received packets (len, RSSI, SNR, hex).
// OK sends a short test packet. Back exits.
//
// NOT hardware-tested — the SX1262 driver is a first draft; validate on the module.

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/view.h>

#include <sx1262/sx1262.h>

#include <string.h>
#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define LORA_VIEW 0

typedef struct {
    ViewDispatcher* view_dispatcher;
    View* view;
    Gui* gui;
    FuriMutex* mutex;

    volatile bool running;
    volatile bool worker_alive;
    TaskHandle_t worker;
    bool radio_ok;
    Sx1262Config cfg;

    // shared (guarded by mutex)
    uint32_t rx_count;
    size_t last_len;
    int16_t last_rssi;
    int8_t last_snr;
    uint8_t last_data[32];
    volatile bool do_tx;
    uint32_t tx_count;
    bool last_tx_ok;
    volatile bool tx_busy;
    uint16_t dev_err; // SX1262 GetDeviceErrors (diagnostic)
    uint8_t chip_status;
} LoraApp;

static void lora_worker(void* ctx) {
    LoraApp* app = ctx;
    uint8_t buf[255];
    while(app->running) {
        if(app->do_tx) {
            app->do_tx = false;
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->tx_busy = true;
            furi_mutex_release(app->mutex);

            const char* msg = "cardputer ping";
            bool ok = sx1262_tx((const uint8_t*)msg, strlen(msg), 2000);
            sx1262_start_rx(); // return to listening

            uint16_t err = sx1262_device_errors();
            uint8_t st = sx1262_status();
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->tx_busy = false;
            app->tx_count++;
            app->last_tx_ok = ok;
            app->dev_err = err;
            app->chip_status = st;
            furi_mutex_release(app->mutex);
        }
        size_t len = 0;
        int16_t rssi = 0;
        int8_t snr = 0;
        if(sx1262_rx_poll(buf, sizeof(buf), &len, &rssi, &snr)) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->rx_count++;
            app->last_len = len;
            app->last_rssi = rssi;
            app->last_snr = snr;
            memcpy(app->last_data, buf, len > sizeof(app->last_data) ? sizeof(app->last_data) : len);
            furi_mutex_release(app->mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    app->worker_alive = false;
    vTaskDelete(NULL);
}

static void lora_draw(Canvas* canvas, void* model) {
    LoraApp* app = *(LoraApp**)model;
    uint32_t rxc;
    size_t len;
    int16_t rssi;
    int8_t snr;
    uint8_t data[32];
    uint32_t txc;
    bool tx_busy, tx_ok;
    uint16_t dev_err;
    uint8_t chip_status;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    rxc = app->rx_count;
    len = app->last_len;
    rssi = app->last_rssi;
    snr = app->last_snr;
    memcpy(data, app->last_data, sizeof(data));
    txc = app->tx_count;
    tx_busy = app->tx_busy;
    tx_ok = app->last_tx_ok;
    dev_err = app->dev_err;
    chip_status = app->chip_status;
    furi_mutex_release(app->mutex);

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "LoRa Monitor");
    canvas_set_font(canvas, FontSecondary);

    char buf[52];
    if(!app->radio_ok) {
        canvas_draw_str(canvas, 2, 26, "SX1262 init FAILED");
        canvas_draw_str(canvas, 2, 38, "check cap / SD (SPI3)");
        canvas_draw_str(canvas, 2, 58, "Back=exit");
        return;
    }
    snprintf(buf, sizeof(buf), "%lu MHz SF%d  RX:%lu",
             (unsigned long)(app->cfg.frequency / 1000000u), app->cfg.spreading_factor,
             (unsigned long)rxc);
    canvas_draw_str(canvas, 2, 24, buf);

    if(len) {
        snprintf(buf, sizeof(buf), "len %u  rssi %d  snr %d", (unsigned)len, rssi, snr);
        canvas_draw_str(canvas, 2, 36, buf);
        char hex[49];
        size_t show = len > 12 ? 12 : len;
        for(size_t i = 0; i < show; i++) snprintf(&hex[i * 3], 4, "%02X ", data[i]);
        hex[show * 3] = 0;
        canvas_draw_str(canvas, 2, 47, hex);
    } else {
        canvas_draw_str(canvas, 2, 36, "listening...");
        // diagnostic: SX1262 device-error word + chip status (read this to me)
        snprintf(buf, sizeof(buf), "err:%04X st:%02X", dev_err, chip_status);
        canvas_draw_str(canvas, 2, 47, buf);
    }
    if(tx_busy) {
        snprintf(buf, sizeof(buf), "TX sending...   Back=exit");
    } else if(txc) {
        snprintf(buf, sizeof(buf), "TX:%lu %s  OK=send", (unsigned long)txc, tx_ok ? "OK" : "FAIL");
    } else {
        snprintf(buf, sizeof(buf), "OK=send  Back=exit");
    }
    canvas_draw_str(canvas, 2, 58, buf);
}

static bool lora_input(InputEvent* event, void* ctx) {
    LoraApp* app = ctx;
    if(event->type == InputTypeShort) {
        if(event->key == InputKeyBack) {
            view_dispatcher_stop(app->view_dispatcher);
            return true;
        }
        if(event->key == InputKeyOk) {
            app->do_tx = true;
            return true;
        }
    }
    return false;
}

static void lora_tick(void* ctx) {
    LoraApp* app = ctx;
    with_view_model(app->view, LoraApp** m, { *m = app; }, true);
}

int32_t lora_app(void* p) {
    UNUSED(p);
    LoraApp* app = malloc(sizeof(LoraApp));
    if(!app) return 0;
    memset(app, 0, sizeof(*app));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    sx1262_default_config(&app->cfg);

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_tick_event_callback(app->view_dispatcher, lora_tick, 250);

    app->view = view_alloc();
    view_allocate_model(app->view, ViewModelTypeLockFree, sizeof(LoraApp*));
    with_view_model(app->view, LoraApp** m, { *m = app; }, false);
    view_set_context(app->view, app);
    view_set_draw_callback(app->view, lora_draw);
    view_set_input_callback(app->view, lora_input);
    view_dispatcher_add_view(app->view_dispatcher, LORA_VIEW, app->view);
    view_dispatcher_switch_to_view(app->view_dispatcher, LORA_VIEW);

    app->radio_ok = sx1262_init();
    if(app->radio_ok) {
        sx1262_configure(&app->cfg);
        app->dev_err = sx1262_device_errors();
        app->chip_status = sx1262_status();
        sx1262_start_rx();
        app->running = true;
        app->worker_alive = true;
        if(xTaskCreate(lora_worker, "lora", 4096, app, tskIDLE_PRIORITY + 4, &app->worker) != pdPASS) {
            app->worker_alive = false;
        }
    }

    view_dispatcher_run(app->view_dispatcher);

    app->running = false;
    // The worker can be blocked inside sx1262_tx() for up to ~2s, longer than any
    // fixed cap; freeing app/mutex while it still runs is a use-after-free. It
    // always clears worker_alive before vTaskDelete, so wait unconditionally.
    while(app->worker_alive) furi_delay_ms(20);
    if(app->radio_ok) sx1262_deinit();

    view_dispatcher_remove_view(app->view_dispatcher, LORA_VIEW);
    view_free(app->view);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
