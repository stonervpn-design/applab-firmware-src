// Meshtastic Sniffer for the Cap LoRa-1262 (SX1262). Listens on the Meshtastic
// LongFast preset, parses the 16-byte packet header, and AES-CTR decrypts the
// payload with the DEFAULT channel key to surface text messages.
//
// Recon/monitoring of an open mesh you're authorized to test. Default-key channels
// only (custom PSKs won't decrypt). NOT hardware-tested — validate on real traffic.
// Region default = EU 869.525 MHz; Up/Down retunes to US 906.875 and back.

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/view.h>

#include <sx1262/sx1262.h>

#include <string.h>
#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/aes.h>

#define MESH_VIEW 0

// Meshtastic default channel key (PSK index 1 -> well-known 16-byte key).
static const uint8_t MESH_DEFAULT_KEY[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};

typedef struct {
    ViewDispatcher* view_dispatcher;
    View* view;
    Gui* gui;
    FuriMutex* mutex;

    volatile bool running;
    volatile bool worker_alive;
    TaskHandle_t worker;
    bool radio_ok;
    uint32_t frequency;

    // shared
    uint32_t pkt_count;
    uint32_t last_from;
    uint32_t last_id;
    uint8_t last_hop;
    uint8_t last_channel;
    int16_t last_rssi;
    int8_t last_snr;
    uint32_t last_port;
    char last_text[64];
    bool last_text_valid;
} MeshApp;

static void mesh_decrypt(uint32_t from, uint32_t id, const uint8_t* in, size_t len, uint8_t* out) {
    uint8_t nonce[16] = {0};
    memcpy(nonce, &id, 4); // nonce[0..3] = packet id (LE)
    memcpy(nonce + 8, &from, 4); // nonce[8..11] = sender (LE)
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, MESH_DEFAULT_KEY, 128);
    size_t nc_off = 0;
    uint8_t stream[16] = {0};
    mbedtls_aes_crypt_ctr(&ctx, len, &nc_off, nonce, stream, in, out);
    mbedtls_aes_free(&ctx);
}

// Minimal protobuf parse of meshtastic.Data: field1=portnum(varint), field2=payload(bytes).
static uint32_t mesh_parse_data(const uint8_t* p, size_t len, char* text, size_t textmax) {
    uint32_t portnum = 0;
    text[0] = 0;
    size_t i = 0;
    while(i < len) {
        uint8_t tag = p[i++];
        uint8_t field = tag >> 3, wire = tag & 7;
        if(field == 1 && wire == 0) {
            uint32_t v = 0;
            int shift = 0;
            while(i < len) {
                uint8_t b = p[i++];
                v |= (uint32_t)(b & 0x7F) << shift;
                if(!(b & 0x80)) break;
                shift += 7;
            }
            portnum = v;
        } else if(field == 2 && wire == 2) {
            if(i >= len) break;
            uint8_t l = p[i++];
            size_t n = (l < textmax - 1) ? l : (textmax - 1);
            if(i + l <= len) {
                memcpy(text, &p[i], n);
                text[n] = 0;
            }
            i += l;
        } else if(wire == 0) {
            while(i < len && (p[i++] & 0x80)) {
            }
        } else if(wire == 2) {
            if(i < len) {
                uint8_t l = p[i++];
                i += l;
            }
        } else {
            break;
        }
    }
    return portnum;
}

static void mesh_worker(void* ctx) {
    MeshApp* app = ctx;
    uint8_t buf[255];
    while(app->running) {
        size_t len = 0;
        int16_t rssi = 0;
        int8_t snr = 0;
        if(sx1262_rx_poll(buf, sizeof(buf), &len, &rssi, &snr) && len >= 16) {
            uint32_t to, from, id;
            memcpy(&to, buf, 4);
            memcpy(&from, buf + 4, 4);
            memcpy(&id, buf + 8, 4);
            uint8_t flags = buf[12];
            uint8_t channel = buf[13];
            uint8_t hop = flags & 0x07;

            size_t plen = len - 16;
            if(plen > 200) plen = 200;
            uint8_t dec[200];
            mesh_decrypt(from, id, buf + 16, plen, dec);
            char text[64];
            uint32_t port = mesh_parse_data(dec, plen, text, sizeof(text));

            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->pkt_count++;
            app->last_from = from;
            app->last_id = id;
            app->last_hop = hop;
            app->last_channel = channel;
            app->last_rssi = rssi;
            app->last_snr = snr;
            app->last_port = port;
            app->last_text_valid = (port == 1 && text[0]); // TEXT_MESSAGE_APP
            if(app->last_text_valid) {
                strncpy(app->last_text, text, sizeof(app->last_text) - 1);
                app->last_text[sizeof(app->last_text) - 1] = 0;
            }
            furi_mutex_release(app->mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    app->worker_alive = false;
    vTaskDelete(NULL);
}

static void mesh_apply_freq(MeshApp* app) {
    Sx1262Config cfg;
    sx1262_meshtastic_config(&cfg, app->frequency);
    sx1262_configure(&cfg);
    sx1262_start_rx();
}

static void mesh_draw(Canvas* canvas, void* model) {
    MeshApp* app = *(MeshApp**)model;
    uint32_t pc, from, id, port;
    uint8_t hop, ch;
    int16_t rssi;
    int8_t snr;
    char text[64];
    bool tv;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    pc = app->pkt_count;
    from = app->last_from;
    id = app->last_id;
    port = app->last_port;
    hop = app->last_hop;
    ch = app->last_channel;
    rssi = app->last_rssi;
    snr = app->last_snr;
    tv = app->last_text_valid;
    memcpy(text, app->last_text, sizeof(text));
    furi_mutex_release(app->mutex);

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "Meshtastic Sniffer");
    canvas_set_font(canvas, FontSecondary);

    char buf[52];
    if(!app->radio_ok) {
        canvas_draw_str(canvas, 2, 26, "SX1262 init FAILED");
        canvas_draw_str(canvas, 2, 38, "check LoRa cap");
        canvas_draw_str(canvas, 2, 58, "Back=exit");
        return;
    }
    snprintf(buf, sizeof(buf), "%lu.%03lu MHz  pkts:%lu",
             (unsigned long)(app->frequency / 1000000u),
             (unsigned long)((app->frequency / 1000u) % 1000u), (unsigned long)pc);
    canvas_draw_str(canvas, 2, 23, buf);

    if(pc) {
        snprintf(buf, sizeof(buf), "from %08lX hop%u ch%02X", (unsigned long)from, hop, ch);
        canvas_draw_str(canvas, 2, 34, buf);
        snprintf(buf, sizeof(buf), "rssi %d snr %d port %lu", rssi, snr, (unsigned long)port);
        canvas_draw_str(canvas, 2, 44, buf);
        if(tv) {
            snprintf(buf, sizeof(buf), "MSG: %.40s", text);
            canvas_draw_str(canvas, 2, 54, buf);
        }
    } else {
        canvas_draw_str(canvas, 2, 34, "listening (LongFast)...");
    }
    canvas_draw_str(canvas, 2, 63, "Up/Dn=EU/US  Back=exit");
}

static bool mesh_input(InputEvent* event, void* ctx) {
    MeshApp* app = ctx;
    if(event->type == InputTypeShort || event->type == InputTypePress) {
        if(event->key == InputKeyBack) {
            view_dispatcher_stop(app->view_dispatcher);
            return true;
        }
        if(event->key == InputKeyUp) {
            app->frequency = 869525000u; // EU_868 LongFast
            if(app->radio_ok) mesh_apply_freq(app);
            return true;
        }
        if(event->key == InputKeyDown) {
            app->frequency = 906875000u; // US LongFast
            if(app->radio_ok) mesh_apply_freq(app);
            return true;
        }
    }
    return false;
}

static void mesh_tick(void* ctx) {
    MeshApp* app = ctx;
    with_view_model(app->view, MeshApp** m, { *m = app; }, true);
}

int32_t meshtastic_app(void* p) {
    UNUSED(p);
    MeshApp* app = malloc(sizeof(MeshApp));
    if(!app) return 0;
    memset(app, 0, sizeof(*app));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->frequency = 869525000u;

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_tick_event_callback(app->view_dispatcher, mesh_tick, 250);

    app->view = view_alloc();
    view_allocate_model(app->view, ViewModelTypeLockFree, sizeof(MeshApp*));
    with_view_model(app->view, MeshApp** m, { *m = app; }, false);
    view_set_context(app->view, app);
    view_set_draw_callback(app->view, mesh_draw);
    view_set_input_callback(app->view, mesh_input);
    view_dispatcher_add_view(app->view_dispatcher, MESH_VIEW, app->view);
    view_dispatcher_switch_to_view(app->view_dispatcher, MESH_VIEW);

    app->radio_ok = sx1262_init();
    if(app->radio_ok) {
        mesh_apply_freq(app);
        app->running = true;
        app->worker_alive = true;
        if(xTaskCreate(mesh_worker, "mesh", 4096, app, tskIDLE_PRIORITY + 4, &app->worker) !=
           pdPASS) {
            app->worker_alive = false;
        }
    }

    view_dispatcher_run(app->view_dispatcher);

    app->running = false;
    for(int i = 0; i < 50 && app->worker_alive; i++) furi_delay_ms(20);
    if(app->radio_ok) sx1262_deinit();

    view_dispatcher_remove_view(app->view_dispatcher, MESH_VIEW);
    view_free(app->view);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
