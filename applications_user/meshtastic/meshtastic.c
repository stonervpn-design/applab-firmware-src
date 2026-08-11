// Meshtastic Sniffer for the Cap LoRa-1262 (SX1262). Listens on the Meshtastic
// LongFast preset, parses the 16-byte packet header, and AES-CTR decrypts the
// payload with the DEFAULT channel key to surface text messages.
//
// Recon/monitoring of an open mesh you're authorized to test. Decrypts the default
// channel and, if you fill in MESH_CUSTOM_KEY below, a second custom-PSK channel
// alongside it. NOT hardware-tested — validate on real traffic.
// Region default = EU 869.525 MHz; Up/Down retunes to US 906.875 and back.

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/view.h>

#include <sx1262/sx1262.h>
#include <gui/modules/text_input.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/aes.h>
#include <esp_random.h>

#define MESH_VIEW       0
#define MESH_INPUT_VIEW 1

// Meshtastic default channel key (PSK index 1 -> well-known 16-byte key).
static const uint8_t MESH_DEFAULT_KEY[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};

// Optional custom-channel PSK. Meshtastic packets carry no MIC, so a device can't
// be told which channel a frame belongs to without trying the key; we simply try
// this key alongside the default on RX. Paste the channel's 16-byte AES-128 PSK
// here (the raw bytes of the base64 "psk" from the channel URL / QR). Leaving it
// all-zero disables the custom channel — only the default key is used.
static const uint8_t MESH_CUSTOM_KEY[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// True once MESH_CUSTOM_KEY has been filled in (not all-zero).
static bool mesh_custom_key_active(void) {
    for(int i = 0; i < 16; i++)
        if(MESH_CUSTOM_KEY[i]) return true;
    return false;
}

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
    char last_summary[64]; // decoded, human-readable (text / lat,lon / name / battery…)
    bool last_summary_valid;

    // unique nodes heard this session
    uint32_t nodes[32];
    uint8_t node_count;

    // TX (two-way): our identity + compose buffer. The GUI thread stages a message
    // and the RX worker sends it, so the SX1262 SPI is only touched from one thread.
    TextInput* input;
    char input_buf[64];
    uint32_t my_node;
    char tx_text[64];
    volatile bool do_tx;
    volatile bool do_retune; // set by GUI Up/Down; the worker owns the SPI/radio
    char tx_status[40];
} MeshApp;

// Meshtastic portnums we decode.
#define MESH_PORT_TEXT      1
#define MESH_PORT_POSITION  3
#define MESH_PORT_NODEINFO  4
#define MESH_PORT_TELEMETRY 67

static void mesh_decrypt(
    uint32_t from, uint32_t id, const uint8_t* in, size_t len, uint8_t* out, const uint8_t* key) {
    uint8_t nonce[16] = {0};
    memcpy(nonce, &id, 4); // nonce[0..3] = packet id (LE)
    memcpy(nonce + 8, &from, 4); // nonce[8..11] = sender (LE)
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, key, 128); // default or custom channel PSK
    size_t nc_off = 0;
    uint8_t stream[16] = {0};
    mbedtls_aes_crypt_ctr(&ctx, len, &nc_off, nonce, stream, in, out);
    mbedtls_aes_free(&ctx);
}

// Channel hash for the primary "LongFast" default channel: xor(name) ^ xor(psk).
// We prefer the byte observed from RX (exact match to the live mesh); this is the
// fallback used before any packet has been heard.
static uint8_t mesh_default_channel_hash(void) {
    uint8_t h = 0;
    const char* name = "LongFast";
    for(const char* c = name; *c; c++) h ^= (uint8_t)*c;
    for(int i = 0; i < 16; i++) h ^= MESH_DEFAULT_KEY[i];
    return h;
}

// Encode text as a meshtastic.Data protobuf, AES-CTR encrypt with the default key
// (symmetric — mesh_decrypt also encrypts), frame the 16-byte header (broadcast,
// hop-limit 3) and transmit. Channel hash matches observed RX when available.
static bool mesh_send_text(MeshApp* app, const char* text) {
    uint8_t data[80];
    size_t dl = 0;
    data[dl++] = 0x08; // field 1 (portnum) varint
    data[dl++] = MESH_PORT_TEXT;
    size_t tl = strlen(text);
    if(tl > 60) tl = 60;
    data[dl++] = 0x12; // field 2 (payload) length-delimited
    data[dl++] = (uint8_t)tl;
    memcpy(&data[dl], text, tl);
    dl += tl;

    uint32_t id = esp_random();
    uint8_t enc[80];
    mesh_decrypt(app->my_node, id, data, dl, enc, MESH_DEFAULT_KEY); // TX on the default channel

    uint8_t pkt[16 + 80];
    uint32_t to = 0xFFFFFFFFu; // broadcast
    memcpy(pkt + 0, &to, 4);
    memcpy(pkt + 4, &app->my_node, 4);
    memcpy(pkt + 8, &id, 4);
    pkt[12] = 0x63; // flags: hop_limit=3, hop_start=3
    pkt[13] = app->last_channel ? app->last_channel : mesh_default_channel_hash();
    pkt[14] = 0; // next_hop
    pkt[15] = 0; // relay_node
    memcpy(pkt + 16, enc, dl);

    bool ok = sx1262_tx(pkt, 16 + (uint32_t)dl, 2000);
    sx1262_start_rx(); // resume listening
    return ok;
}

// ---- minimal protobuf helpers ----
static uint64_t pb_varint(const uint8_t* p, size_t len, size_t* i) {
    uint64_t v = 0;
    int s = 0;
    while(*i < len) {
        uint8_t b = p[(*i)++];
        v |= (uint64_t)(b & 0x7F) << s;
        if(!(b & 0x80)) break;
        s += 7;
        if(s > 63) break;
    }
    return v;
}
// Skip one field's value given its wire type (i points just after the tag).
static void pb_skip(const uint8_t* p, size_t len, size_t* i, uint8_t wire) {
    if(wire == 0)
        pb_varint(p, len, i);
    else if(wire == 2) {
        uint64_t l = pb_varint(p, len, i);
        *i += (size_t)l;
    } else if(wire == 5)
        *i += 4;
    else if(wire == 1)
        *i += 8;
    else
        *i = len;
}

// meshtastic.Data: field1=portnum(varint), field2=payload(bytes). Returns portnum
// and points *payload/*plen at the inner protobuf.
static uint32_t
    mesh_extract_data(const uint8_t* p, size_t len, const uint8_t** payload, size_t* plen) {
    uint32_t portnum = 0;
    *payload = NULL;
    *plen = 0;
    size_t i = 0;
    while(i < len) {
        uint8_t tag = p[i++];
        uint8_t field = tag >> 3, wire = tag & 7;
        if(field == 1 && wire == 0) {
            portnum = (uint32_t)pb_varint(p, len, &i);
        } else if(field == 2 && wire == 2) {
            uint64_t l = pb_varint(p, len, &i);
            if(i + l <= len) {
                *payload = &p[i];
                *plen = (size_t)l;
            }
            i += (size_t)l;
        } else {
            pb_skip(p, len, &i, wire);
        }
    }
    return portnum;
}

// meshtastic.Position: field1 latitude_i (sfixed32), field2 longitude_i, field3 altitude(int).
static void mesh_decode_position(const uint8_t* p, size_t len, char* out, size_t outmax) {
    int32_t lat = 0, lon = 0;
    bool have = false;
    size_t i = 0;
    while(i < len) {
        uint8_t tag = p[i++];
        uint8_t field = tag >> 3, wire = tag & 7;
        if(field == 1 && wire == 5 && i + 4 <= len) {
            memcpy(&lat, &p[i], 4);
            i += 4;
            have = true;
        } else if(field == 2 && wire == 5 && i + 4 <= len) {
            memcpy(&lon, &p[i], 4);
            i += 4;
            have = true;
        } else {
            pb_skip(p, len, &i, wire);
        }
    }
    if(have)
        snprintf(
            out, outmax, "%ld.%06ld,%ld.%06ld", (long)(lat / 10000000L),
            (long)(labs(lat % 10000000L) / 10L), (long)(lon / 10000000L),
            (long)(labs(lon % 10000000L) / 10L));
    else
        snprintf(out, outmax, "(no fix)");
}

// meshtastic.User: field2 long_name(string), field3 short_name(string).
static void mesh_decode_user(const uint8_t* p, size_t len, char* out, size_t outmax) {
    char longn[28] = {0}, shortn[8] = {0};
    size_t i = 0;
    while(i < len) {
        uint8_t tag = p[i++];
        uint8_t field = tag >> 3, wire = tag & 7;
        if(wire == 2) {
            uint64_t l = pb_varint(p, len, &i);
            if(i + l <= len) {
                if(field == 2) {
                    size_t n = l < sizeof(longn) - 1 ? (size_t)l : sizeof(longn) - 1;
                    memcpy(longn, &p[i], n);
                    longn[n] = 0;
                } else if(field == 3) {
                    size_t n = l < sizeof(shortn) - 1 ? (size_t)l : sizeof(shortn) - 1;
                    memcpy(shortn, &p[i], n);
                    shortn[n] = 0;
                }
            }
            i += (size_t)l;
        } else {
            pb_skip(p, len, &i, wire);
        }
    }
    snprintf(out, outmax, "%s %s", shortn[0] ? shortn : "?", longn);
}

// meshtastic.Telemetry: field2 = DeviceMetrics{ f1 battery_level(int), f2 voltage(float) }.
static void mesh_decode_telemetry(const uint8_t* p, size_t len, char* out, size_t outmax) {
    int bat = -1;
    float volt = -1.0f;
    size_t i = 0;
    while(i < len) {
        uint8_t tag = p[i++];
        uint8_t field = tag >> 3, wire = tag & 7;
        if(field == 2 && wire == 2) {
            uint64_t l = pb_varint(p, len, &i);
            size_t end = i + (size_t)l;
            if(end > len) end = len;
            while(i < end) {
                uint8_t t2 = p[i++];
                uint8_t f2 = t2 >> 3, w2 = t2 & 7;
                if(f2 == 1 && w2 == 0)
                    bat = (int)pb_varint(p, end, &i);
                else if(f2 == 2 && w2 == 5 && i + 4 <= end) {
                    memcpy(&volt, &p[i], 4);
                    i += 4;
                } else
                    pb_skip(p, end, &i, w2);
            }
        } else {
            pb_skip(p, len, &i, wire);
        }
    }
    if(bat >= 0 && volt >= 0)
        snprintf(out, outmax, "bat %d%% %d.%02dV", bat, (int)volt, (int)(volt * 100) % 100);
    else if(bat >= 0)
        snprintf(out, outmax, "bat %d%%", bat);
    else
        snprintf(out, outmax, "(metrics)");
}

// Decode the meshtastic.Data payload into a human summary; returns the portnum.
static uint32_t mesh_decode(const uint8_t* data, size_t len, char* out, size_t outmax) {
    const uint8_t* pl;
    size_t pll;
    uint32_t port = mesh_extract_data(data, len, &pl, &pll);
    out[0] = 0;
    if(!pl) return port;
    switch(port) {
    case MESH_PORT_TEXT: {
        size_t n = pll < outmax - 1 ? pll : outmax - 1;
        memcpy(out, pl, n);
        out[n] = 0;
        break;
    }
    case MESH_PORT_POSITION:
        mesh_decode_position(pl, pll, out, outmax);
        break;
    case MESH_PORT_NODEINFO:
        mesh_decode_user(pl, pll, out, outmax);
        break;
    case MESH_PORT_TELEMETRY:
        mesh_decode_telemetry(pl, pll, out, outmax);
        break;
    default:
        break;
    }
    return port;
}

// A decrypt "looks right" when it lands on a portnum we recognise — the signal we
// use to decide whether the default or the custom-channel key was the correct one.
static bool mesh_port_known(uint32_t port) {
    return port == MESH_PORT_TEXT || port == MESH_PORT_POSITION ||
           port == MESH_PORT_NODEINFO || port == MESH_PORT_TELEMETRY;
}

static void mesh_worker(void* ctx) {
    MeshApp* app = ctx;
    uint8_t buf[255];
    while(app->running) {
        // Retune (staged by the Up/Down keys) runs on THIS thread so the GUI thread
        // never touches the shared SPI bus / radio state.
        if(app->do_retune) {
            app->do_retune = false;
            if(app->radio_ok) mesh_apply_freq(app);
        }
        // TX (staged by the compose view) happens here so RX + TX share one thread.
        if(app->do_tx) {
            app->do_tx = false;
            bool ok = mesh_send_text(app, app->tx_text);
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            snprintf(
                app->tx_status, sizeof(app->tx_status), ok ? "TX> %.28s" : "TX failed",
                app->tx_text);
            furi_mutex_release(app->mutex);
        }
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
            char summary[64];
            // Default channel first.
            mesh_decrypt(from, id, buf + 16, plen, dec, MESH_DEFAULT_KEY);
            uint32_t port = mesh_decode(dec, plen, summary, sizeof(summary));
            // If that didn't land on a known portnum, retry with the custom PSK
            // (when configured) and keep it only if it decodes more plausibly.
            if(!mesh_port_known(port) && mesh_custom_key_active()) {
                uint8_t dec2[200];
                char summary2[64];
                mesh_decrypt(from, id, buf + 16, plen, dec2, MESH_CUSTOM_KEY);
                uint32_t port2 = mesh_decode(dec2, plen, summary2, sizeof(summary2));
                if(mesh_port_known(port2)) {
                    port = port2;
                    memcpy(summary, summary2, sizeof(summary));
                }
            }

            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->pkt_count++;
            app->last_from = from;
            app->last_id = id;
            app->last_hop = hop;
            app->last_channel = channel;
            app->last_rssi = rssi;
            app->last_snr = snr;
            app->last_port = port;
            app->last_summary_valid = (summary[0] != 0);
            strncpy(app->last_summary, summary, sizeof(app->last_summary) - 1);
            app->last_summary[sizeof(app->last_summary) - 1] = 0;
            // track unique nodes heard
            bool known = false;
            for(uint8_t k = 0; k < app->node_count; k++)
                if(app->nodes[k] == from) {
                    known = true;
                    break;
                }
            if(!known && app->node_count < 32) app->nodes[app->node_count++] = from;
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

static const char* mesh_port_tag(uint32_t port) {
    switch(port) {
    case MESH_PORT_TEXT:
        return "MSG";
    case MESH_PORT_POSITION:
        return "POS";
    case MESH_PORT_NODEINFO:
        return "NODE";
    case MESH_PORT_TELEMETRY:
        return "TELE";
    default:
        return "PKT";
    }
}

static void mesh_draw(Canvas* canvas, void* model) {
    MeshApp* app = *(MeshApp**)model;
    uint32_t pc, from, port;
    uint8_t hop, ch, nc;
    int16_t rssi;
    int8_t snr;
    char summary[64], txs[40];
    bool sv;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    pc = app->pkt_count;
    from = app->last_from;
    port = app->last_port;
    hop = app->last_hop;
    ch = app->last_channel;
    rssi = app->last_rssi;
    snr = app->last_snr;
    sv = app->last_summary_valid;
    nc = app->node_count;
    memcpy(summary, app->last_summary, sizeof(summary));
    memcpy(txs, app->tx_status, sizeof(txs));
    furi_mutex_release(app->mutex);
    (void)ch;

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
    snprintf(buf, sizeof(buf), "%lu.%03lu MHz pkts:%lu nds:%u",
             (unsigned long)(app->frequency / 1000000u),
             (unsigned long)((app->frequency / 1000u) % 1000u), (unsigned long)pc, nc);
    canvas_draw_str(canvas, 2, 23, buf);

    if(pc) {
        snprintf(buf, sizeof(buf), "%s %08lX r%d s%d h%u", mesh_port_tag(port),
                 (unsigned long)from, rssi, snr, hop);
        canvas_draw_str(canvas, 2, 34, buf);
        if(sv)
            snprintf(buf, sizeof(buf), "%.42s", summary);
        else
            snprintf(buf, sizeof(buf), "port %lu (undecoded)", (unsigned long)port);
        canvas_draw_str(canvas, 2, 45, buf);
    } else {
        canvas_draw_str(canvas, 2, 34, "listening (LongFast)...");
    }
    if(txs[0]) canvas_draw_str(canvas, 2, 55, txs);
    canvas_draw_str(canvas, 2, 63, "Ok=Send U/D=freq Back=exit");
}

// Submit from the compose keyboard: stage the text for the worker to TX, return.
static void mesh_input_done(void* ctx) {
    MeshApp* app = ctx;
    if(app->input_buf[0]) {
        strncpy(app->tx_text, app->input_buf, sizeof(app->tx_text) - 1);
        app->tx_text[sizeof(app->tx_text) - 1] = 0;
        app->do_tx = true;
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, MESH_VIEW);
}
// Back from the compose keyboard -> back to the sniffer view.
static uint32_t mesh_input_back(void* ctx) {
    UNUSED(ctx);
    return MESH_VIEW;
}

static bool mesh_input(InputEvent* event, void* ctx) {
    MeshApp* app = ctx;
    if(event->type == InputTypeShort || event->type == InputTypePress) {
        if(event->key == InputKeyBack) {
            view_dispatcher_stop(app->view_dispatcher);
            return true;
        }
        if(event->key == InputKeyOk && app->radio_ok) {
            app->input_buf[0] = 0;
            text_input_reset(app->input);
            text_input_set_header_text(app->input, "Mesh message");
            text_input_set_result_callback(
                app->input, mesh_input_done, app, app->input_buf, sizeof(app->input_buf), true);
            view_dispatcher_switch_to_view(app->view_dispatcher, MESH_INPUT_VIEW);
            return true;
        }
        if(event->key == InputKeyUp) {
            app->frequency = 869525000u; // EU_868 LongFast
            app->do_retune = true; // worker applies it (single SPI owner)
            return true;
        }
        if(event->key == InputKeyDown) {
            app->frequency = 906875000u; // US LongFast
            app->do_retune = true; // worker applies it (single SPI owner)
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

    // compose (TX) view — on-screen keyboard for custom mesh text
    app->input = text_input_alloc();
    view_set_previous_callback(text_input_get_view(app->input), mesh_input_back);
    view_dispatcher_add_view(
        app->view_dispatcher, MESH_INPUT_VIEW, text_input_get_view(app->input));
    app->my_node = esp_random() | 0xF0000000u; // our node id (high nibble = 'custom')

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
    // Wait UNBOUNDED for the worker to exit: sx1262_tx() can block up to ~2s, longer
    // than a 1s cap, so a bounded wait frees app + deinits the radio under a live
    // worker = use-after-free. The worker clears worker_alive before vTaskDelete.
    while(app->worker_alive) furi_delay_ms(20);
    if(app->radio_ok) sx1262_deinit();

    view_dispatcher_remove_view(app->view_dispatcher, MESH_VIEW);
    view_dispatcher_remove_view(app->view_dispatcher, MESH_INPUT_VIEW);
    view_free(app->view);
    text_input_free(app->input);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
