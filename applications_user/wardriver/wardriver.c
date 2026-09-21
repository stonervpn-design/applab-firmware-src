// GPS Wardriver for the Cap LoRa-1262 (ATGM336H GNSS) on the Cardputer-ADV.
// Runs periodic Wi-Fi scans, dedups by BSSID, and logs each AP together with the
// current GPS fix to a Wigle-1.4 CSV on the SD card (/ext/apps_data/wardriver/).
//
// GNSS comes from the shared <gnss/gnss.h> component (UART @115200, RX=G15).
// Wi-Fi uses esp_wifi directly (same init sequence as the nRF24 scan helper).

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/view.h>
#include <storage/storage.h>
#include <furi_hal_rtc.h>

#include <gnss/gnss.h>

#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>

#define TAG            "wardriver"
#define WD_MAX_SEEN    600 // dedup capacity (6 bytes each)
#define WD_SCAN_MAX    40 // APs per scan pass
#define WD_VIEW        0

typedef struct {
    ViewDispatcher* view_dispatcher;
    View* view;
    Gui* gui;

    Storage* storage;
    File* file;
    bool file_open;

    volatile bool running;
    volatile bool worker_alive;
    TaskHandle_t worker;
    FuriMutex* mutex;

    // shared state (guarded by mutex)
    GnssFix fix;
    uint32_t ap_count; // unique APs logged
    uint32_t scan_passes;
    uint8_t seen[WD_MAX_SEEN][6];
    uint16_t seen_count;
} WardriverApp;

// ---- dedup ----
static bool wd_seen_add(WardriverApp* app, const uint8_t bssid[6]) {
    for(uint16_t i = 0; i < app->seen_count; i++) {
        if(memcmp(app->seen[i], bssid, 6) == 0) return false; // already known
    }
    if(app->seen_count < WD_MAX_SEEN) {
        memcpy(app->seen[app->seen_count++], bssid, 6);
    }
    return true; // new
}

static const char* wd_auth_str(wifi_auth_mode_t a) {
    switch(a) {
    case WIFI_AUTH_OPEN: return "[ESS]";
    case WIFI_AUTH_WEP: return "[WEP][ESS]";
    case WIFI_AUTH_WPA_PSK: return "[WPA-PSK-CCMP][ESS]";
    case WIFI_AUTH_WPA2_PSK: return "[WPA2-PSK-CCMP][ESS]";
    case WIFI_AUTH_WPA_WPA2_PSK: return "[WPA-WPA2-PSK-CCMP][ESS]";
    case WIFI_AUTH_WPA3_PSK: return "[WPA3-SAE][ESS]";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "[WPA2-WPA3-PSK][ESS]";
    default: return "[ESS]";
    }
}

static void wd_write_header(WardriverApp* app) {
    const char* h1 =
        "WigleWifi-1.4,appRelease=cardputer,model=Cardputer-ADV,release=1.0,"
        "device=Cardputer,display=,board=esp32s3,brand=M5Stack\n";
    const char* h2 =
        "MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,"
        "AltitudeMeters,AccuracyMeters,Type\n";
    storage_file_write(app->file, h1, strlen(h1));
    storage_file_write(app->file, h2, strlen(h2));
}

static void wd_log_ap(WardriverApp* app, const wifi_ap_record_t* ap, const GnssFix* fix) {
    char ssid[33];
    strncpy(ssid, (const char*)ap->ssid, sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = 0;
    // sanitize commas/newlines in SSID so the CSV row stays aligned
    for(char* p = ssid; *p; p++)
        if(*p == ',' || *p == '\n' || *p == '\r') *p = ' ';

    char line[220];
    int n = snprintf(
        line,
        sizeof(line),
        "%02X:%02X:%02X:%02X:%02X:%02X,%s,%s,%04u-%02u-%02u %02u:%02u:%02u,%d,%d,%.6f,%.6f,%.1f,%.1f,WIFI\n",
        ap->bssid[0], ap->bssid[1], ap->bssid[2], ap->bssid[3], ap->bssid[4], ap->bssid[5],
        ssid,
        wd_auth_str(ap->authmode),
        fix->year, fix->month, fix->day, fix->hour, fix->minute, fix->second,
        ap->primary,
        ap->rssi,
        fix->lat, fix->lon, fix->altitude_m,
        fix->hdop_x10 ? (fix->hdop_x10 / 10.0) * 5.0 : 10.0);
    // snprintf returns the length it WOULD have written; if the row was
    // truncated (n >= sizeof line) writing n bytes reads past the stack buffer
    // and persists that garbage to the CSV. Clamp to the real buffer length.
    if(n > 0) {
        size_t wlen = ((size_t)n < sizeof(line)) ? (size_t)n : sizeof(line) - 1;
        storage_file_write(app->file, line, wlen);
    }
}

// ---- wifi ----
static bool wd_wifi_init(void) {
    if(esp_netif_init() != ESP_OK) return false;
    esp_event_loop_create_default(); // ok if already exists
    if(!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if(esp_wifi_init(&cfg) != ESP_OK) return false;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    if(esp_wifi_start() != ESP_OK) {
        esp_wifi_deinit();
        return false;
    }
    esp_wifi_set_max_tx_power(84); // 21 dBm ceiling for maximum range
    return true;
}

static void wd_worker(void* ctx) {
    WardriverApp* app = ctx;
    if(!wd_wifi_init()) {
        app->worker_alive = false;
        vTaskDelete(NULL);
        return;
    }
    wifi_scan_config_t scan_cfg = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        // Longer per-channel dwell so weaker/more distant APs are logged.
        .scan_time = {.active = {.min = 120, .max = 300}},
    };
    wifi_ap_record_t records[WD_SCAN_MAX];

    while(app->running) {
        if(esp_wifi_scan_start(&scan_cfg, true) == ESP_OK) {
            uint16_t num = WD_SCAN_MAX;
            if(esp_wifi_scan_get_ap_records(&num, records) == ESP_OK) {
                GnssFix fix;
                gnss_get_fix(&fix);
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                app->fix = fix;
                app->scan_passes++;
                for(uint16_t i = 0; i < num; i++) {
                    if(wd_seen_add(app, records[i].bssid)) {
                        if(fix.valid && app->file_open) {
                            wd_log_ap(app, &records[i], &fix);
                            app->ap_count++;
                        }
                    }
                }
                furi_mutex_release(app->mutex);
                if(app->file_open) storage_file_sync(app->file);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1500));
    }

    esp_wifi_stop();
    esp_wifi_deinit();
    app->worker_alive = false;
    vTaskDelete(NULL);
}

// ---- view ----
static void wd_draw(Canvas* canvas, void* model) {
    WardriverApp* app = *(WardriverApp**)model;
    GnssFix fix;
    uint32_t aps, passes, lines;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    fix = app->fix;
    aps = app->ap_count;
    passes = app->scan_passes;
    furi_mutex_release(app->mutex);
    lines = gnss_line_count();

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "GPS Wardriver");
    canvas_set_font(canvas, FontSecondary);

    char buf[48];
    if(fix.valid) {
        snprintf(buf, sizeof(buf), "FIX %d sats  %.5f", fix.satellites, fix.lat);
        canvas_draw_str(canvas, 2, 24, buf);
        snprintf(buf, sizeof(buf), "     lon %.5f", fix.lon);
        canvas_draw_str(canvas, 2, 34, buf);
    } else {
        snprintf(buf, sizeof(buf), "No GPS fix (nmea %lu)", (unsigned long)lines);
        canvas_draw_str(canvas, 2, 24, buf);
        canvas_draw_str(canvas, 2, 34, lines ? "GPS ok, acquiring..." : "No GPS data - check cap");
    }
    snprintf(buf, sizeof(buf), "APs logged: %lu", (unsigned long)aps);
    canvas_draw_str(canvas, 2, 47, buf);
    snprintf(buf, sizeof(buf), "scans: %lu   Back=exit", (unsigned long)passes);
    canvas_draw_str(canvas, 2, 58, buf);
}

static bool wd_input(InputEvent* event, void* ctx) {
    WardriverApp* app = ctx;
    if(event->type == InputTypeShort && event->key == InputKeyBack) {
        view_dispatcher_stop(app->view_dispatcher);
        return true;
    }
    return false;
}

static void wd_tick(void* ctx) {
    WardriverApp* app = ctx;
    // force a redraw so live counts update
    with_view_model(app->view, WardriverApp** m, { *m = app; }, true);
}

static void wd_open_csv(WardriverApp* app) {
    app->storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(app->storage, EXT_PATH("apps_data"));
    storage_simply_mkdir(app->storage, EXT_PATH("apps_data/wardriver"));
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    FuriString* path = furi_string_alloc();
    furi_string_printf(
        path,
        "%s/wardrive_%04u%02u%02u_%02u%02u%02u.csv",
        EXT_PATH("apps_data/wardriver"),
        dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    app->file = storage_file_alloc(app->storage);
    if(storage_file_open(
           app->file, furi_string_get_cstr(path), FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        app->file_open = true;
        wd_write_header(app);
    }
    furi_string_free(path);
}

static WardriverApp* wardriver_alloc(void) {
    WardriverApp* app = malloc(sizeof(WardriverApp));
    if(!app) return NULL;
    memset(app, 0, sizeof(*app));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    nmea_fix_init(&app->fix);

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_tick_event_callback(app->view_dispatcher, wd_tick, 500);

    app->view = view_alloc();
    view_allocate_model(app->view, ViewModelTypeLockFree, sizeof(WardriverApp*));
    with_view_model(app->view, WardriverApp** m, { *m = app; }, false);
    view_set_context(app->view, app);
    view_set_draw_callback(app->view, wd_draw);
    view_set_input_callback(app->view, wd_input);
    view_dispatcher_add_view(app->view_dispatcher, WD_VIEW, app->view);
    view_dispatcher_switch_to_view(app->view_dispatcher, WD_VIEW);
    return app;
}

static void wardriver_free(WardriverApp* app) {
    view_dispatcher_remove_view(app->view_dispatcher, WD_VIEW);
    view_free(app->view);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);

    if(app->file_open) {
        storage_file_close(app->file);
    }
    if(app->file) storage_file_free(app->file);
    if(app->storage) furi_record_close(RECORD_STORAGE);
    furi_mutex_free(app->mutex);
    free(app);
}

int32_t wardriver_app(void* p) {
    UNUSED(p);
    WardriverApp* app = wardriver_alloc();
    if(!app) return 0;

    wd_open_csv(app);
    gnss_start();

    app->running = true;
    app->worker_alive = true;
    if(xTaskCreate(wd_worker, "wardrive", 6144, app, tskIDLE_PRIORITY + 4, &app->worker) != pdPASS) {
        app->worker_alive = false;
    }

    view_dispatcher_run(app->view_dispatcher);

    // stop worker + gnss, wait for worker to release wifi before freeing
    app->running = false;
    // esp_wifi_scan_start(..., true) is synchronous with no cap that reliably
    // bounds it; freeing app/mutex/file while the worker still runs is a
    // use-after-free. The worker clears worker_alive before vTaskDelete, so wait
    // unconditionally.
    while(app->worker_alive) furi_delay_ms(20);
    gnss_stop();

    wardriver_free(app);
    return 0;
}
