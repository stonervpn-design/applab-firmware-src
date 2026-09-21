/* nRF24 Sniffer + Replay -- attack tooling, PRIVATE builds only (whole file
 * gated, same pattern as subghz_scene_garage.c / subghz_scene_keeloq_console.c).
 *
 * Reuses the MouseJack promiscuous-RX hop pattern from scene_mj_scan.c
 * (nrf24_hw_init/probe/acquire/release, nrf24_hw_rx_start_promiscuous(), the
 * ce(false)+set_channel()+ce(true) hop dance) but captures EVERY CRC-valid
 * ESB frame (Task 1's nrf24_capture store), not just MouseJack vendor hits.
 * mj_fingerprint() bundles CRC-validate + vendor-match in one call with no
 * way to get just the validate+extract half, so the validate+extract math
 * lives in the shared helpers/nrf24_esb.c (nrf24_esb_decode_frame()), used by
 * both this scene and scene_keysniffer.c.
 *
 * One scene, four internal UI states (scene_manager_set_scene_state), same
 * technique as camera_hunter_scene_results.c:
 *   0 Scanning  - promiscuous hop + capture, live count (Widget, Center=List)
 *   1 List      - submenu of captures (Back exits the scene)
 *   2 Confirm   - selected capture info, Left=Replay x1 / Right=Replay xN
 *   3 Replaying - on-air indicator while the TX burst worker runs
 */
#ifndef PUBLIC_BUILD

#include "../nrf24_app.h"
#include "../nrf24_hw.h"
#include "../helpers/nrf24_capture.h"
#include "../helpers/nrf24_esb.h"

#include <furi.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "Nrf24Sniffer"

#define SNIFF_CH_MIN       2
#define SNIFF_CH_MAX       84
#define SNIFF_TRIES_PER_CH 6
#define SNIFF_DWELL_US     500
#define SNIFF_BURST_N      10

typedef enum {
    SniffStateScanning = 0,
    SniffStateList = 1,
    SniffStateConfirm = 2,
    SniffStateReplaying = 3,
} SniffState;

#define SNIFF_EVT_STOP_SCAN 1
#define SNIFF_EVT_REPLAY_1  2
#define SNIFF_EVT_REPLAY_N  3
#define SNIFF_PICK_BASE     100

typedef struct {
    Nrf24App* app;
    FuriThread* worker;
    volatile bool stop; /* scan-worker stop request */
    volatile bool hw_checked;
    volatile bool hw_ok;
    volatile uint8_t cur_channel;
    volatile uint16_t sweeps;
    volatile bool replay_done; /* set by the replay worker when its burst finishes */
    uint8_t replay_index;
    uint8_t replay_repeats;
} SnifferCtx;

static SnifferCtx* g_ctx = NULL;

/* Abort-aware TX burst, defined in nrf24_capture_tx.c but deliberately not
 * part of nrf24_capture.h's public surface (see that file's comment) --
 * forward-declared here, its only caller. */
extern void nrf24_capture_replay_abortable(
    const Nrf24Capture* c,
    uint8_t repeats,
    volatile bool* abort);

/* ---- Scan worker: hop + capture (mirrors scene_mj_scan.c's mj_scan_worker) ---- */
static int32_t sniffer_scan_worker(void* context) {
    SnifferCtx* ctx = context;

    nrf24_hw_init();

    nrf24_hw_acquire();
    bool ok = nrf24_hw_probe();
    nrf24_hw_release();

    ctx->hw_ok = ok;
    ctx->hw_checked = true;

    if(!ok) {
        FURI_LOG_W(TAG, "NRF24 probe failed");
        nrf24_hw_deinit();
        return 0;
    }

    nrf24_hw_acquire();
    nrf24_hw_rx_start_promiscuous(SNIFF_CH_MIN);
    nrf24_hw_release();

    while(!ctx->stop) {
        for(uint8_t ch = SNIFF_CH_MIN; ch <= SNIFF_CH_MAX && !ctx->stop; ch++) {
            nrf24_hw_acquire();
            nrf24_hw_ce(false);
            nrf24_hw_set_channel(ch);
            nrf24_hw_flush_rx();
            nrf24_hw_ce(true);

            for(int t = 0; t < SNIFF_TRIES_PER_CH && !ctx->stop; t++) {
                esp_rom_delay_us(SNIFF_DWELL_US);
                if(nrf24_hw_rx_data_ready()) {
                    uint8_t buf[32];
                    nrf24_hw_rx_read_payload(buf, sizeof(buf));

                    uint8_t addr[5];
                    uint8_t payload[32];
                    uint8_t len = 0;
                    if(nrf24_esb_decode_frame(buf, sizeof(buf), addr, payload, &len)) {
                        Nrf24Capture cap;
                        memset(&cap, 0, sizeof(cap));
                        memcpy(cap.addr, addr, sizeof(cap.addr));
                        cap.channel = ch;
                        memcpy(cap.payload, payload, len);
                        cap.len = len;
                        cap.ts = furi_get_tick();
                        cap.proto = N24_RAW;
                        nrf24_capture_add(&cap);
                    }
                }
            }
            nrf24_hw_release();
            ctx->cur_channel = ch;
        }
        ctx->sweeps++;
        furi_delay_ms(2);
    }

    nrf24_hw_acquire();
    nrf24_hw_rx_stop();
    nrf24_hw_power_down();
    nrf24_hw_release();
    nrf24_hw_deinit();
    return 0;
}

/* ---- Replay worker: TX burst, abortable via ctx->stop (the same flag the
 * scan worker honors -- reused here rather than adding a second field) ---- */
static int32_t sniffer_replay_worker(void* context) {
    SnifferCtx* ctx = context;
    const Nrf24Capture* c = nrf24_capture_get(ctx->replay_index);
    if(c) {
        Nrf24Capture snap = *c; /* stable copy: the store could keep evolving in theory */
        nrf24_capture_replay_abortable(&snap, ctx->replay_repeats, &ctx->stop);
    }
    ctx->replay_done = true;
    return 0;
}

/* ---- Widget button -> custom event plumbing (archive_scene_delete.c pattern) ---- */
static void sniffer_widget_button_cb(GuiButtonType result, InputType type, void* context) {
    Nrf24App* app = context;
    if(type != InputTypeShort) return;
    uint32_t evt = 0;
    if(result == GuiButtonTypeCenter) evt = SNIFF_EVT_STOP_SCAN;
    else if(result == GuiButtonTypeLeft) evt = SNIFF_EVT_REPLAY_1;
    else if(result == GuiButtonTypeRight) evt = SNIFF_EVT_REPLAY_N;
    if(evt) view_dispatcher_send_custom_event(app->view_dispatcher, evt);
}

static void sniffer_list_cb(void* context, uint32_t index) {
    Nrf24App* app = context;
    scene_manager_handle_custom_event(app->scene_manager, index);
}

/* ---- View builders ---- */
static void sniffer_build_scanning_widget(Nrf24App* app, SnifferCtx* ctx) {
    widget_reset(app->widget);
    widget_add_text_box_element(
        app->widget, 0, 0, 128, 14, AlignCenter, AlignTop, "\e#nRF24 Sniffer\e#", false);

    if(ctx->hw_checked && !ctx->hw_ok) {
        widget_add_text_box_element(
            app->widget, 0, 26, 128, 16, AlignCenter, AlignCenter, "NRF24 not found", false);
        return;
    }

    char line1[24];
    snprintf(line1, sizeof(line1), "Ch %3u", ctx->cur_channel);
    widget_add_string_element(app->widget, 64, 22, AlignCenter, AlignTop, FontSecondary, line1);

    char line2[24];
    snprintf(line2, sizeof(line2), "%u captures", (unsigned)nrf24_capture_count());
    widget_add_string_element(app->widget, 64, 34, AlignCenter, AlignTop, FontSecondary, line2);

    widget_add_button_element(
        app->widget, GuiButtonTypeCenter, "List", sniffer_widget_button_cb, app);
}

static void sniffer_build_list(Nrf24App* app) {
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Sniffer captures");

    size_t n = nrf24_capture_count();
    if(n == 0) {
        submenu_add_item(app->submenu, "No captures yet", 0, sniffer_list_cb, app);
    } else {
        char label[32];
        for(size_t i = 0; i < n; i++) {
            const Nrf24Capture* c = nrf24_capture_get(i);
            if(!c) break;
            snprintf(
                label,
                sizeof(label),
                "%02X:%02X:%02X ch%u L%u",
                c->addr[2],
                c->addr[3],
                c->addr[4],
                c->channel,
                c->len);
            submenu_add_item(app->submenu, label, SNIFF_PICK_BASE + (uint32_t)i, sniffer_list_cb, app);
        }
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewSubmenu);
}

static void sniffer_build_confirm(Nrf24App* app, SnifferCtx* ctx) {
    const Nrf24Capture* c = nrf24_capture_get(ctx->replay_index);
    widget_reset(app->widget);

    if(!c) {
        widget_add_text_box_element(
            app->widget, 0, 20, 128, 24, AlignCenter, AlignCenter, "Capture gone", false);
        view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewWidget);
        return;
    }

    char hdr[24];
    snprintf(
        hdr, sizeof(hdr), "%02X:%02X:%02X ch%u", c->addr[2], c->addr[3], c->addr[4], c->channel);
    widget_add_text_box_element(app->widget, 0, 0, 128, 14, AlignCenter, AlignTop, hdr, false);

    char sub[24];
    snprintf(sub, sizeof(sub), "%u bytes", c->len);
    widget_add_string_element(app->widget, 64, 22, AlignCenter, AlignTop, FontSecondary, sub);
    widget_add_string_element(
        app->widget, 64, 34, AlignCenter, AlignTop, FontSecondary, "Replay?");

    widget_add_button_element(
        app->widget, GuiButtonTypeLeft, "x1", sniffer_widget_button_cb, app);
    char nbtn[8];
    snprintf(nbtn, sizeof(nbtn), "x%u", SNIFF_BURST_N);
    widget_add_button_element(
        app->widget, GuiButtonTypeRight, nbtn, sniffer_widget_button_cb, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewWidget);
}

static void sniffer_build_replaying(Nrf24App* app) {
    widget_reset(app->widget);
    widget_add_text_box_element(
        app->widget, 0, 24, 128, 16, AlignCenter, AlignCenter, "\e#TX on air...\e#", false);
    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewWidget);
}

/* ---- Scene lifecycle ---- */
void nrf24_app_scene_sniffer_on_enter(void* context) {
    Nrf24App* app = context;

    SnifferCtx* ctx = malloc(sizeof(SnifferCtx));
    if(!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->app = app;
    ctx->cur_channel = SNIFF_CH_MIN;
    g_ctx = ctx;

    scene_manager_set_scene_state(app->scene_manager, Nrf24AppSceneSniffer, SniffStateScanning);
    sniffer_build_scanning_widget(app, ctx);
    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewWidget);

    ctx->worker = furi_thread_alloc_ex("Nrf24Sniff", 4096, sniffer_scan_worker, ctx);
    furi_thread_start(ctx->worker);
}

bool nrf24_app_scene_sniffer_on_event(void* context, SceneManagerEvent event) {
    Nrf24App* app = context;
    if(!g_ctx) return false;

    uint32_t state = scene_manager_get_scene_state(app->scene_manager, Nrf24AppSceneSniffer);

    if(event.type == SceneManagerEventTypeTick) {
        if(state == SniffStateScanning) {
            sniffer_build_scanning_widget(app, g_ctx);
        } else if(state == SniffStateReplaying && g_ctx->replay_done) {
            if(g_ctx->worker) {
                furi_thread_join(g_ctx->worker);
                furi_thread_free(g_ctx->worker);
                g_ctx->worker = NULL;
            }
            scene_manager_set_scene_state(app->scene_manager, Nrf24AppSceneSniffer, SniffStateList);
            sniffer_build_list(app);
        }
        return true;
    }

    if(event.type == SceneManagerEventTypeCustom) {
        if(state == SniffStateScanning && event.event == SNIFF_EVT_STOP_SCAN) {
            /* Fully stop the worker BEFORE switching views -- otherwise the
             * radio keeps sweeping channels underneath the list (scene_mj_scan.c
             * precedent). */
            g_ctx->stop = true;
            if(g_ctx->worker) {
                furi_thread_join(g_ctx->worker);
                furi_thread_free(g_ctx->worker);
                g_ctx->worker = NULL;
            }
            scene_manager_set_scene_state(app->scene_manager, Nrf24AppSceneSniffer, SniffStateList);
            sniffer_build_list(app);
            return true;
        }

        if(state == SniffStateList && event.event >= SNIFF_PICK_BASE) {
            size_t idx = event.event - SNIFF_PICK_BASE;
            if(idx < nrf24_capture_count()) {
                g_ctx->replay_index = (uint8_t)idx;
                scene_manager_set_scene_state(
                    app->scene_manager, Nrf24AppSceneSniffer, SniffStateConfirm);
                sniffer_build_confirm(app, g_ctx);
            }
            return true;
        }

        if(state == SniffStateConfirm &&
           (event.event == SNIFF_EVT_REPLAY_1 || event.event == SNIFF_EVT_REPLAY_N)) {
            g_ctx->replay_repeats = (event.event == SNIFF_EVT_REPLAY_1) ? 1 : SNIFF_BURST_N;
            g_ctx->replay_done = false;
            /* Clear a stale stop from an earlier scan/replay -- this worker's
             * abort flag is the same field, and it must start unset. */
            g_ctx->stop = false;
            scene_manager_set_scene_state(
                app->scene_manager, Nrf24AppSceneSniffer, SniffStateReplaying);
            sniffer_build_replaying(app);

            g_ctx->worker = furi_thread_alloc_ex("Nrf24Replay", 4096, sniffer_replay_worker, g_ctx);
            furi_thread_start(g_ctx->worker);
            return true;
        }

        return false;
    }

    if(event.type == SceneManagerEventTypeBack) {
        if(state == SniffStateConfirm) {
            /* Confirm -> back to the list, not out of the scene. */
            scene_manager_set_scene_state(app->scene_manager, Nrf24AppSceneSniffer, SniffStateList);
            sniffer_build_list(app);
            return true;
        }
        if(state == SniffStateReplaying) {
            /* Abort the in-flight burst: reuse the scan worker's stop flag so
             * sniffer_replay_worker's loop (via nrf24_capture_replay_abortable)
             * breaks between sends instead of running to completion. Swallow
             * the event -- the Tick handler above returns to List once
             * replay_done flips (on_exit()'s join is the safety net either
             * way). */
            g_ctx->stop = true;
            return true;
        }
        /* Scanning or List: let scene_manager pop to the app menu. on_exit()
         * always stops+joins any worker regardless of navigation path (same
         * pattern as scene_mj_scan.c / scene_jam.c) -- this is the Back-abort. */
        return false;
    }

    return false;
}

void nrf24_app_scene_sniffer_on_exit(void* context) {
    Nrf24App* app = context;
    if(!g_ctx) return;

    g_ctx->stop = true;
    if(g_ctx->worker) {
        furi_thread_join(g_ctx->worker);
        furi_thread_free(g_ctx->worker);
        g_ctx->worker = NULL;
    }
    free(g_ctx);
    g_ctx = NULL;

    submenu_reset(app->submenu);
    widget_reset(app->widget);
}

#endif /* !PUBLIC_BUILD -- attack tooling, private only */
