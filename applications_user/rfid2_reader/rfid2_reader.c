// RFID2 Reader — M5Stack Unit RFID2 (WS1850S, 13.56 MHz, I2C @0x28).
// Polls for ISO14443-A cards and shows the UID. Press OK on a present card to do
// a FULL read (MIFARE Classic: default-key dictionary auth + all blocks;
// Ultralight/NTAG: all pages), view it scrollably, and save it to
// /ext/rfid2/<uid>.txt. Back exits (or leaves the dump view).
//
// NOT hardware-tested — validate on the unit. If nothing reads, confirm the Grove
// port is on the shared I2C bus (see RFID2_I2C_PORT in components/rfid2/rfid2.h).

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/view.h>
#include <storage/storage.h>

#include <rfid2/rfid2.h>

#include <string.h>
#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define R2_VIEW    0
#define R2_DUMP_CAP 16384u

typedef enum {
    R2ModeScan = 0, // polling for a card, showing UID
    R2ModeBusy, // full read in progress
    R2ModeDump, // showing a scrollable dump
} R2Mode;

typedef struct {
    ViewDispatcher* view_dispatcher;
    View* view;
    Gui* gui;
    FuriMutex* mutex;

    volatile bool running;
    volatile bool worker_alive;
    volatile bool dump_req; // OK pressed → worker should full-read
    TaskHandle_t worker;
    bool radio_ok;
    uint8_t version;

    volatile R2Mode mode;

    // shared card state
    uint32_t read_count;
    size_t uid_len;
    uint8_t uid[10];
    bool card_present;
    uint8_t scan[16];
    int scan_n;

    // dump result
    char* dump; // R2_DUMP_CAP heap buffer
    int dump_rows;
    int dump_lines;
    char dump_type[24];
    int scroll;
    char save_path[80];
    bool saved;
} R2App;

static void r2_scan_str(R2App* app, char* out, size_t n) {
    int off = snprintf(out, n, "I2C:");
    if(app->scan_n <= 0) {
        snprintf(out + off, n - off, app->scan_n == 0 ? " (empty)" : " bus err");
        return;
    }
    for(int i = 0; i < app->scan_n && off < (int)n - 4; i++)
        off += snprintf(out + off, n - off, " %02X", app->scan[i]);
}

static int count_lines(const char* s) {
    int n = 0;
    for(; *s; s++)
        if(*s == '\n') n++;
    return n;
}

// Save the current dump to /ext/rfid2/<uid>.txt. Returns true on success.
static bool r2_save(R2App* app) {
    char uidhex[24] = {0};
    int o = 0;
    for(size_t i = 0; i < app->uid_len && i < 10; i++)
        o += snprintf(uidhex + o, sizeof(uidhex) - o, "%02X", app->uid[i]);
    snprintf(app->save_path, sizeof(app->save_path), "/ext/rfid2/%s.txt", uidhex);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, "/ext/rfid2");
    File* f = storage_file_alloc(storage);
    bool ok = false;
    if(storage_file_open(f, app->save_path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        char hdr[96];
        int h = snprintf(
            hdr, sizeof(hdr), "RFID2 dump\nType: %s\nUID: %s\n\n", app->dump_type, uidhex);
        storage_file_write(f, hdr, h);
        storage_file_write(f, app->dump, strlen(app->dump));
        storage_file_close(f);
        ok = true;
    }
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

static void r2_worker(void* ctx) {
    R2App* app = ctx;
    while(app->running) {
        if(app->dump_req) {
            app->dump_req = false;
            app->mode = R2ModeBusy;

            size_t ulen = 0;
            uint8_t sak = 0;
            app->dump_type[0] = 0;
            int rows = rfid2_dump_text(
                app->dump, R2_DUMP_CAP, app->uid, sizeof(app->uid), &ulen, &sak,
                app->dump_type, sizeof(app->dump_type));

            furi_mutex_acquire(app->mutex, FuriWaitForever);
            if(rows >= 0) {
                app->uid_len = ulen;
                app->dump_rows = rows;
                app->dump_lines = count_lines(app->dump);
                app->scroll = 0;
                app->saved = r2_save(app);
                app->mode = R2ModeDump;
            } else {
                // card left the field mid-read
                app->mode = R2ModeScan;
            }
            furi_mutex_release(app->mutex);
            continue;
        }

        if(app->mode == R2ModeScan) {
            uint8_t uid[10];
            size_t len = 0;
            uint8_t sak = 0;
            bool found = rfid2_read_uid(uid, sizeof(uid), &len, &sak);
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            if(found && len) {
                if(app->uid_len != len || memcmp(app->uid, uid, len) != 0) app->read_count++;
                app->uid_len = len;
                memcpy(app->uid, uid, len);
                app->card_present = true;
            } else {
                app->card_present = false;
            }
            furi_mutex_release(app->mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(app->mode == R2ModeDump ? 80 : 200));
    }
    app->worker_alive = false;
    vTaskDelete(NULL);
}

static void r2_draw_dump(Canvas* canvas, R2App* app) {
    canvas_set_font(canvas, FontSecondary);
    char top[52];
    snprintf(top, sizeof(top), "%s  %s", app->dump_type, app->saved ? "saved" : "no SD");
    canvas_draw_str(canvas, 2, 9, top);

    // walk to the scroll-th line, draw up to 6 rows
    const char* p = app->dump;
    for(int i = 0; i < app->scroll && *p; i++) {
        while(*p && *p != '\n') p++;
        if(*p == '\n') p++;
    }
    int y = 20;
    for(int row = 0; row < 6 && *p; row++) {
        char line[40];
        int c = 0;
        while(*p && *p != '\n' && c < (int)sizeof(line) - 1) line[c++] = *p++;
        line[c] = 0;
        if(*p == '\n') p++;
        canvas_draw_str(canvas, 2, y, line);
        y += 9;
    }
    canvas_set_font(canvas, FontSecondary);
    char foot[40];
    snprintf(
        foot, sizeof(foot), "%d rows  Up/Dn  Back", app->dump_rows);
    canvas_draw_str(canvas, 2, 63, foot);
}

static void r2_draw(Canvas* canvas, void* model) {
    R2App* app = *(R2App**)model;
    R2Mode mode;
    uint32_t rc;
    size_t len;
    uint8_t uid[10];
    bool present;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    mode = app->mode;
    rc = app->read_count;
    len = app->uid_len;
    memcpy(uid, app->uid, sizeof(uid));
    present = app->card_present;
    furi_mutex_release(app->mutex);

    canvas_clear(canvas);

    if(mode == R2ModeDump) {
        r2_draw_dump(canvas, app);
        return;
    }

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "RFID2 Reader");
    canvas_set_font(canvas, FontSecondary);

    char buf[52];
    if(!app->radio_ok) {
        canvas_draw_str(canvas, 2, 24, "WS1850S not found");
        snprintf(buf, sizeof(buf), "ver 0x%02X (G2/G1, 0x28)", app->version);
        canvas_draw_str(canvas, 2, 35, buf);
        r2_scan_str(app, buf, sizeof(buf));
        canvas_draw_str(canvas, 2, 47, buf);
        canvas_draw_str(canvas, 2, 62, "Back=exit");
        return;
    }

    if(mode == R2ModeBusy) {
        canvas_draw_str(canvas, 2, 30, "Reading card...");
        canvas_draw_str(canvas, 2, 42, "hold card still");
        return;
    }

    snprintf(buf, sizeof(buf), "Reads: %lu   %s", (unsigned long)rc, present ? "CARD" : "-");
    canvas_draw_str(canvas, 2, 26, buf);

    if(len) {
        char hex[33];
        for(size_t i = 0; i < len && i < 10; i++) snprintf(&hex[i * 3], 4, "%02X ", uid[i]);
        hex[len * 3] = 0;
        canvas_draw_str(canvas, 2, 40, "UID:");
        canvas_draw_str(canvas, 24, 40, hex);
        canvas_draw_str(canvas, 2, 51, present ? "OK=full read" : "present a card...");
    } else {
        canvas_draw_str(canvas, 2, 40, "present a card...");
    }
    canvas_draw_str(canvas, 2, 62, "Back=exit");
}

static bool r2_input(InputEvent* event, void* ctx) {
    R2App* app = ctx;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    if(app->mode == R2ModeDump) {
        if(event->key == InputKeyBack) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->mode = R2ModeScan;
            furi_mutex_release(app->mutex);
            return true;
        }
        if(event->key == InputKeyUp) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            if(app->scroll > 0) app->scroll--;
            furi_mutex_release(app->mutex);
            return true;
        }
        if(event->key == InputKeyDown) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            if(app->scroll < app->dump_lines - 1) app->scroll++;
            furi_mutex_release(app->mutex);
            return true;
        }
        return true;
    }

    // scan mode
    if(event->type == InputTypeShort && event->key == InputKeyBack) {
        view_dispatcher_stop(app->view_dispatcher);
        return true;
    }
    if(event->type == InputTypeShort && event->key == InputKeyOk) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        bool present = app->card_present;
        furi_mutex_release(app->mutex);
        if(present && app->dump) app->dump_req = true;
        return true;
    }
    return false;
}

static void r2_tick(void* ctx) {
    R2App* app = ctx;
    with_view_model(app->view, R2App** m, { *m = app; }, true);
}

int32_t rfid2_reader_app(void* p) {
    UNUSED(p);
    R2App* app = malloc(sizeof(R2App));
    if(!app) return 0;
    memset(app, 0, sizeof(*app));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->dump = malloc(R2_DUMP_CAP);

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_tick_event_callback(app->view_dispatcher, r2_tick, 200);

    app->view = view_alloc();
    view_allocate_model(app->view, ViewModelTypeLockFree, sizeof(R2App*));
    with_view_model(app->view, R2App** m, { *m = app; }, false);
    view_set_context(app->view, app);
    view_set_draw_callback(app->view, r2_draw);
    view_set_input_callback(app->view, r2_input);
    view_dispatcher_add_view(app->view_dispatcher, R2_VIEW, app->view);
    view_dispatcher_switch_to_view(app->view_dispatcher, R2_VIEW);

    app->scan_n = rfid2_scan(app->scan, sizeof(app->scan));
    app->radio_ok = rfid2_init();
    app->version = rfid2_version();
    if(app->radio_ok) {
        app->running = true;
        app->worker_alive = true;
        if(xTaskCreate(r2_worker, "rfid2", 6144, app, tskIDLE_PRIORITY + 4, &app->worker) != pdPASS) {
            app->worker_alive = false;
        }
    }

    view_dispatcher_run(app->view_dispatcher);

    app->running = false;
    // The worker can be mid-I2C-transaction; it clears worker_alive before
    // vTaskDelete, so wait unconditionally before freeing shared state.
    while(app->worker_alive) furi_delay_ms(20);

    view_dispatcher_remove_view(app->view_dispatcher, R2_VIEW);
    view_free(app->view);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    furi_mutex_free(app->mutex);
    if(app->dump) free(app->dump);
    free(app);
    return 0;
}
