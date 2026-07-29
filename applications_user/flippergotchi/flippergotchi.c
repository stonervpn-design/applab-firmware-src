// Flippergotchi v2.0 — Pwnagotchi-style WiFi harvester with an AI channel engine,
// active clientless PMKID solicitation, per-AP dedup, a nameable pet with XP +
// achievements, full pwngrid social, a 4-screen UI, and optional GPS-tagged loot.
// For authorized WiFi security testing only.

#include "fg_app.h"
#include "fg_net_table.h"
#include "fg_ai.h"
#include "fg_progress.h"
#include "fg_pwngrid.h"
#include "fg_geo.h"
#include "fg_capture.h"
#include "fg_attack.h"
#include "fg_ui.h"

#include <esp_wifi.h>
#include <string.h>

#include "wlan_app/wlan_hal.h"
#include "wlan_app/wlan_pcap.h"

#define FG_VIEW 0

FgApp* g_fg = NULL;

// ---- worker-context thunks (run via wlan_hal_run_in_worker on the radio channel) ----
static void fg_attack_worker_fn(void* arg) {
    fg_attack_run((FgApp*)arg);
}
static void fg_adv_worker_fn(void* arg) {
    FgApp* app = arg;
    esp_wifi_80211_tx(WIFI_IF_STA, app->adv_frame, app->adv_len, false);
}

// ---- promiscuous callback (WiFi task) ----
static void fg_promisc_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    UNUSED(type);
    FgApp* app = g_fg;
    if(!app || !app->running) return;
    const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
    fg_capture_cb(app, p->payload, p->rx_ctrl.sig_len);
}

// ---- worker task ----
static void fg_worker(void* ctx) {
    FgApp* app = ctx;
    uint32_t loops = 0;
    uint8_t last_peer_count = 0;
    while(app->running) {
        // drain captured EAPOL frames
        while(app->rd != app->wr) {
            FgFrame* f = &app->ring[app->rd];
            fg_capture_process(app, f->data, f->len);
            app->rd = (app->rd + 1) % FG_RING;
        }

        // AI channel selection
        fg_ai_recount(&app->ai, &app->nets, app->clients, app->client_count);
        uint8_t ch = fg_ai_next_channel(&app->ai);
        wlan_hal_set_channel(ch);
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->channel = ch;
        bool attack = app->attack_on;
        furi_mutex_release(app->mutex);

        if(attack) wlan_hal_run_in_worker(fg_attack_worker_fn, app);

        // advertise to the grid ~every 2s
        if((loops % 5) == 0) {
            uint32_t up = (furi_get_tick() - app->start_tick) / configTICK_RATE_HZ;
            const char* mood;
            const char* face = fg_ui_face(app, furi_get_tick(), &mood);
            app->adv_len = fg_pwngrid_build_adv(
                app->adv_frame, app->prog.name, face, app->prog.total_pwns, app->session_pwns, up);
            wlan_hal_run_in_worker(fg_adv_worker_fn, app);
        }

        // decay AI recent-catch scores ~every 4s
        if((loops % 10) == 0) fg_ai_decay(&app->ai);

        // achievements + friends sync ~every 5s
        if((loops % 12) == 0) {
            uint32_t up = (furi_get_tick() - app->start_tick) / configTICK_RATE_HZ;
            uint8_t chans = 0;
            for(int b = 1; b <= 13; b++)
                if(app->chan_hit_mask & (1u << b)) chans++;
            // persist newly-seen peers to the lifetime friends registry
            uint8_t pc = app->peers.count;
            if(pc > last_peer_count) {
                for(uint8_t i = last_peer_count; i < pc; i++)
                    fg_friends_note(app->storage, app->peers.peers[i].name);
                fg_friends_load(app->storage, &app->friends_total);
                last_peer_count = pc;
            }
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            fg_award_check(
                &app->prog, app->handshakes, app->pmkids, app->friends_total, app->full_hs, up,
                chans);
            furi_mutex_release(app->mutex);
        }

        uint16_t dwell = fg_ai_dwell_ms(&app->ai, ch);
        furi_delay_ms(dwell);
        loops++;
    }
    app->worker_alive = false;
    vTaskDelete(NULL);
}

static void fg_tick(void* ctx) {
    FgApp* app = ctx;
    with_view_model(app->view, FgApp** m, { *m = app; }, true);
}

int32_t flippergotchi_app(void* p) {
    UNUSED(p);
    FgApp* app = malloc(sizeof(FgApp));
    memset(app, 0, sizeof(*app));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    g_fg = app;

    app->storage = furi_record_open(RECORD_STORAGE);
    if(!fg_progress_load(app->storage, &app->prog)) {
        fg_progress_default_name(&app->prog, furi_get_tick() | 1u);
    }
    fg_friends_load(app->storage, &app->friends_total);
    fg_geo_init();
    storage_common_mkdir(app->storage, "/ext/wifi");
    app->pcap = wlan_pcap_open(app->storage, FG_PCAP_PATH);
    fg_ai_init(&app->ai, furi_get_tick() | 1u);

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_tick_event_callback(app->view_dispatcher, fg_tick, 250);

    app->view = view_alloc();
    view_allocate_model(app->view, ViewModelTypeLockFree, sizeof(FgApp*));
    with_view_model(app->view, FgApp** m, { *m = app; }, false);
    view_set_context(app->view, app);
    view_set_draw_callback(app->view, fg_ui_draw);
    view_set_input_callback(app->view, fg_ui_input);
    view_dispatcher_add_view(app->view_dispatcher, FG_VIEW, app->view);
    view_dispatcher_switch_to_view(app->view_dispatcher, FG_VIEW);

    app->start_tick = furi_get_tick();
    app->last_pwn_tick = furi_get_tick();
    app->screen = FG_SCR_FACE;
    app->attack_on = true;
    app->solicit_on = true;

    if(!wlan_hal_is_started()) wlan_hal_start();
    app->running = true;
    app->worker_alive = true;
    wlan_hal_set_promiscuous(true, fg_promisc_cb);
    if(xTaskCreate(fg_worker, "fgotchi", 4096, app, tskIDLE_PRIORITY + 4, &app->worker) != pdPASS) {
        app->worker_alive = false;
    }

    view_dispatcher_run(app->view_dispatcher);

    // teardown
    app->running = false;
    wlan_hal_set_promiscuous(false, NULL);
    for(int i = 0; i < 50 && app->worker_alive; i++) furi_delay_ms(20);
    wlan_hal_stop();

    if(app->session_pwns > app->prog.best_session) app->prog.best_session = app->session_pwns;
    fg_progress_save(app->storage, &app->prog);
    if(app->pcap) wlan_pcap_close(app->pcap);

    view_dispatcher_remove_view(app->view_dispatcher, FG_VIEW);
    view_free(app->view);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_STORAGE);
    g_fg = NULL;
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
