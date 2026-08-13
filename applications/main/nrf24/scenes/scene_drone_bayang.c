/* nRF24 Bayang toy-drone bind/control -- attack^Whobbyist-RC tooling, PRIVATE
 * builds only (whole file gated, same pattern as scene_fuzz.c / scene_sniffer.c).
 *
 * Drives Task 1's nrf24_hw_tx_setup_ex (1 Mbps / CRC16 PHY) + Task 2's
 * drone_bayang_* packet builder (helpers/drone_bayang.h) through a worker
 * thread cloning scene_fuzz.c's stop-flag/join/Tick/on-air/Back-abort shape:
 *   - BIND phase (~2s): repeatedly TX a bind packet on a fixed bind
 *     address/channel so a Bayang-compatible RX can lock onto our txid.
 *   - CONTROL phase: round-robin the 4 txid-derived hop channels, TX a
 *     control packet built from the live throttle/yaw/pitch/roll axes at
 *     the documented ~5ms Bayang frame period, until Back aborts.
 *
 * UNVERIFIED: the whole Bayang wire format here is transcribed from public
 * Deviation/Multiprotocol documentation (see helpers/drone_bayang.h's header
 * comment) and has NOT been tested against a real drone or receiver. This
 * scene shows that verbatim on-screen at all times -- see BANNER_TEXT below.
 * The bind address/channel picked here are likewise UNVERIFIED (drone_bayang
 * only defines the 4 txid-derived CONTROL hop channels, not a bind channel
 * constant -- see the per-field comments below).
 *
 * Two UI states (scene_manager_set_scene_state), matching scene_fuzz.c:
 *   0 Setup   - Widget: axis snapshot + banner, Center=Start
 *   1 Running - Widget: phase (BIND/CONTROL) + axes + on-air + banner,
 *               Left=Axis-, Center=Cycle axis, Right=Axis+,
 *               Back aborts (stop flag + join, same as scene_fuzz.c)
 */
#ifndef PUBLIC_BUILD

#include "../nrf24_app.h"
#include "../nrf24_hw.h"
#include "../helpers/drone_bayang.h"

#include <furi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ~5ms Bayang control-frame period per the ND1 plan Reference; reused for
 * the bind loop too (bind rate isn't separately documented, so this keeps a
 * single well-understood TX cadence). */
#define DRONE_TX_DELAY_MS 5
#define DRONE_BIND_MS     2000 /* ~2s bind phase, per the ND1 plan Task 3 spec */
#define DRONE_AXIS_STEP   0x20u /* ~32 steps across the 0..0x3FF axis range */

typedef enum {
    DroneBayangStateSetup = 0,
    DroneBayangStateRunning = 1,
} DroneBayangState;

typedef enum {
    DroneBayangPhaseBind = 0,
    DroneBayangPhaseControl = 1,
} DroneBayangPhase;

typedef enum {
    DroneAxisThr = 0,
    DroneAxisYaw,
    DroneAxisPitch,
    DroneAxisRoll,
    DroneAxisCount,
} DroneAxisIdx;

#define DRONE_EVT_LEFT   1
#define DRONE_EVT_CENTER 2
#define DRONE_EVT_RIGHT  3

static const char* const DRONE_AXIS_NAMES[DroneAxisCount] = {"Thr", "Yaw", "Pit", "Rol"};

/* Verbatim per the ND1 plan Task 3 spec -- shown on both Setup and Running. */
static const char* const BANNER_TEXT =
    "UNVERIFIED wire format (from public docs) -- may not control a real "
    "drone. HW-test required.";

typedef struct {
    Nrf24App* app;
    FuriThread* worker;
    volatile bool stop; /* Back-abort request while Running */
    volatile bool done; /* set by the worker when its loop exits */
    volatile DroneBayangPhase phase;
    volatile uint32_t bind_frames;
    volatile uint32_t ctrl_frames;

    uint8_t txid[3]; /* fixed bind txid; UI-thread only, never mutated once
                       * a worker is running */
    uint8_t rf_ch[BAYANG_RF_NUM_CH]; /* txid-derived CONTROL hop channels */

    volatile uint16_t axis[DroneAxisCount]; /* throttle/yaw/pitch/roll */
    int sel; /* selected axis index, UI-thread only */
    volatile uint8_t flags;
} DroneBayangCtx;

static DroneBayangCtx* g_ctx = NULL;

/* ---- Worker: BIND then CONTROL, bounded rate, stop-flag abort ---- */
static int32_t drone_bayang_worker(void* context) {
    DroneBayangCtx* ctx = context;
    uint8_t pkt[BAYANG_PKT_LEN];

    /* UNVERIFIED: bind address + bind channel. drone_bayang.h only derives
     * the 4 CONTROL hop channels from txid (no bind-channel constant is
     * part of its interface) -- these are picked here per the ND1 plan
     * Reference's "{0x00,0x00,0x00,0x00,0x00}-style" bind-address guidance
     * and a commonly-mirrored Bayang bind channel; NOT independently
     * re-verified against bayang.c. */
    static const uint8_t bind_addr[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
    const uint8_t bind_ch = 0x4B; /* UNVERIFIED: canonical Bayang bind channel */

    nrf24_hw_init();

    /* Bus-hold discipline: acquire the SPI mutex only around each register op
     * (setup / set_channel / tx_send), NOT for the whole flight session -- the
     * chip keeps its PHY config in-register across a bus release, so the mutex
     * is free during every inter-frame delay. Matches nrf24_tx_raw / mj_scan's
     * per-op granularity; important on shared-SPI boards (e.g. T-Embed CC1101,
     * where nRF24 shares SPI2 with the LCD) so live Tick redraw isn't starved. */

    /* ---- BIND phase ---- */
    ctx->phase = DroneBayangPhaseBind;
    nrf24_hw_acquire();
    nrf24_hw_tx_setup_ex(bind_addr, 5, bind_ch, BAYANG_PKT_LEN, 0 /*1Mbps*/, true /*crc16*/);
    nrf24_hw_release();
    uint32_t bind_start = furi_get_tick();
    while(!ctx->stop && (furi_get_tick() - bind_start) < DRONE_BIND_MS) {
        drone_bayang_build_bind(ctx->txid, pkt);
        nrf24_hw_acquire();
        nrf24_hw_tx_send(pkt, BAYANG_PKT_LEN);
        nrf24_hw_release();
        ctx->bind_frames++;
        furi_delay_ms(DRONE_TX_DELAY_MS);
    }

    /* ---- CONTROL phase: round-robin the 4 txid-derived hop channels ---- */
    if(!ctx->stop) {
        ctx->phase = DroneBayangPhaseControl;
        uint8_t ch_idx = 0;
        while(!ctx->stop) {
            drone_bayang_build_control(
                ctx->txid,
                ctx->axis[DroneAxisThr],
                ctx->axis[DroneAxisYaw],
                ctx->axis[DroneAxisPitch],
                ctx->axis[DroneAxisRoll],
                ctx->flags,
                pkt);
            nrf24_hw_acquire();
            nrf24_hw_set_channel(ctx->rf_ch[ch_idx]);
            nrf24_hw_tx_send(pkt, BAYANG_PKT_LEN);
            nrf24_hw_release();
            ch_idx = (uint8_t)((ch_idx + 1) % BAYANG_RF_NUM_CH);
            ctx->ctrl_frames++;
            furi_delay_ms(DRONE_TX_DELAY_MS);
        }
    }

    nrf24_hw_deinit();

    ctx->done = true;
    return 0;
}

/* ---- Widget button -> custom event plumbing (scene_fuzz.c pattern) ---- */
static void drone_bayang_widget_button_cb(GuiButtonType result, InputType type, void* context) {
    Nrf24App* app = context;
    if(type != InputTypeShort) return;
    uint32_t evt = 0;
    if(result == GuiButtonTypeLeft) evt = DRONE_EVT_LEFT;
    else if(result == GuiButtonTypeCenter) evt = DRONE_EVT_CENTER;
    else if(result == GuiButtonTypeRight) evt = DRONE_EVT_RIGHT;
    if(evt) view_dispatcher_send_custom_event(app->view_dispatcher, evt);
}

/* ---- View builders (do not switch views themselves) ---- */
static void drone_bayang_build_setup_widget(Nrf24App* app, DroneBayangCtx* ctx) {
    widget_reset(app->widget);
    widget_add_text_box_element(
        app->widget, 0, 0, 128, 10, AlignCenter, AlignTop, "\e#Bayang Drone\e#", false);
    widget_add_text_box_element(
        app->widget, 0, 10, 128, 26, AlignCenter, AlignTop, BANNER_TEXT, false);

    char l1[32];
    snprintf(
        l1,
        sizeof(l1),
        "Thr%03X Yaw%03X",
        ctx->axis[DroneAxisThr],
        ctx->axis[DroneAxisYaw]);
    widget_add_string_element(app->widget, 64, 40, AlignCenter, AlignTop, FontSecondary, l1);

    char l2[32];
    snprintf(
        l2,
        sizeof(l2),
        "Pit%03X Rol%03X",
        ctx->axis[DroneAxisPitch],
        ctx->axis[DroneAxisRoll]);
    widget_add_string_element(app->widget, 64, 50, AlignCenter, AlignTop, FontSecondary, l2);

    widget_add_button_element(
        app->widget, GuiButtonTypeCenter, "Start", drone_bayang_widget_button_cb, app);
}

static void drone_bayang_build_running_widget(Nrf24App* app, DroneBayangCtx* ctx) {
    widget_reset(app->widget);

    const char* phase_str = (ctx->phase == DroneBayangPhaseBind) ? "\e#BIND\e#" : "\e#CONTROL\e#";
    widget_add_text_box_element(app->widget, 0, 0, 128, 10, AlignCenter, AlignTop, phase_str, false);

    char l1[32];
    snprintf(
        l1,
        sizeof(l1),
        "%c%s%03X %c%s%03X",
        ctx->sel == DroneAxisThr ? '*' : ' ',
        DRONE_AXIS_NAMES[DroneAxisThr],
        ctx->axis[DroneAxisThr],
        ctx->sel == DroneAxisYaw ? '*' : ' ',
        DRONE_AXIS_NAMES[DroneAxisYaw],
        ctx->axis[DroneAxisYaw]);
    widget_add_string_element(app->widget, 64, 12, AlignCenter, AlignTop, FontSecondary, l1);

    char l2[32];
    snprintf(
        l2,
        sizeof(l2),
        "%c%s%03X %c%s%03X",
        ctx->sel == DroneAxisPitch ? '*' : ' ',
        DRONE_AXIS_NAMES[DroneAxisPitch],
        ctx->axis[DroneAxisPitch],
        ctx->sel == DroneAxisRoll ? '*' : ' ',
        DRONE_AXIS_NAMES[DroneAxisRoll],
        ctx->axis[DroneAxisRoll]);
    widget_add_string_element(app->widget, 64, 22, AlignCenter, AlignTop, FontSecondary, l2);

    char l3[32];
    snprintf(
        l3,
        sizeof(l3),
        "ON AIR  B%lu C%lu",
        (unsigned long)ctx->bind_frames,
        (unsigned long)ctx->ctrl_frames);
    widget_add_string_element(app->widget, 64, 32, AlignCenter, AlignTop, FontSecondary, l3);

    widget_add_text_box_element(app->widget, 0, 42, 128, 22, AlignCenter, AlignTop, BANNER_TEXT, false);

    widget_add_button_element(app->widget, GuiButtonTypeLeft, "-", drone_bayang_widget_button_cb, app);
    widget_add_button_element(
        app->widget, GuiButtonTypeCenter, "Axis", drone_bayang_widget_button_cb, app);
    widget_add_button_element(app->widget, GuiButtonTypeRight, "+", drone_bayang_widget_button_cb, app);
}

/* ---- Scene lifecycle ---- */
void nrf24_app_scene_drone_bayang_on_enter(void* context) {
    Nrf24App* app = context;

    DroneBayangCtx* ctx = malloc(sizeof(DroneBayangCtx));
    if(!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->app = app;

    /* Fixed bind txid, per the ND1 plan Task 3 spec. */
    static const uint8_t default_txid[3] = {0xAA, 0xBB, 0xCC};
    memcpy(ctx->txid, default_txid, sizeof(ctx->txid));
    drone_bayang_rf_channels(ctx->txid, ctx->rf_ch);

    ctx->axis[DroneAxisThr] = 0; /* idle throttle -- safe default */
    ctx->axis[DroneAxisYaw] = BAYANG_AXIS_CENTER;
    ctx->axis[DroneAxisPitch] = BAYANG_AXIS_CENTER;
    ctx->axis[DroneAxisRoll] = BAYANG_AXIS_CENTER;
    ctx->sel = DroneAxisThr;

    g_ctx = ctx;

    scene_manager_set_scene_state(app->scene_manager, Nrf24AppSceneDroneBayang, DroneBayangStateSetup);
    drone_bayang_build_setup_widget(app, ctx);
    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewWidget);
}

bool nrf24_app_scene_drone_bayang_on_event(void* context, SceneManagerEvent event) {
    Nrf24App* app = context;
    if(!g_ctx) return false;

    uint32_t state = scene_manager_get_scene_state(app->scene_manager, Nrf24AppSceneDroneBayang);

    if(event.type == SceneManagerEventTypeTick) {
        if(state == DroneBayangStateRunning) {
            if(g_ctx->done) {
                if(g_ctx->worker) {
                    furi_thread_join(g_ctx->worker);
                    furi_thread_free(g_ctx->worker);
                    g_ctx->worker = NULL;
                }
                scene_manager_set_scene_state(
                    app->scene_manager, Nrf24AppSceneDroneBayang, DroneBayangStateSetup);
                drone_bayang_build_setup_widget(app, g_ctx);
            } else {
                drone_bayang_build_running_widget(app, g_ctx);
            }
        }
        return true;
    }

    if(event.type == SceneManagerEventTypeCustom) {
        if(state == DroneBayangStateSetup) {
            if(event.event == DRONE_EVT_CENTER) {
                g_ctx->stop = false;
                g_ctx->done = false;
                g_ctx->phase = DroneBayangPhaseBind;
                g_ctx->bind_frames = 0;
                g_ctx->ctrl_frames = 0;
                scene_manager_set_scene_state(
                    app->scene_manager, Nrf24AppSceneDroneBayang, DroneBayangStateRunning);
                drone_bayang_build_running_widget(app, g_ctx);

                g_ctx->worker =
                    furi_thread_alloc_ex("Nrf24DroneBayang", 4096, drone_bayang_worker, g_ctx);
                furi_thread_start(g_ctx->worker);
                return true;
            }
            return false;
        }

        if(state == DroneBayangStateRunning) {
            if(event.event == DRONE_EVT_LEFT) {
                uint16_t v = g_ctx->axis[g_ctx->sel];
                v = (v >= DRONE_AXIS_STEP) ? (uint16_t)(v - DRONE_AXIS_STEP) : 0;
                g_ctx->axis[g_ctx->sel] = v;
                drone_bayang_build_running_widget(app, g_ctx);
                return true;
            }
            if(event.event == DRONE_EVT_CENTER) {
                g_ctx->sel = (g_ctx->sel + 1) % DroneAxisCount;
                drone_bayang_build_running_widget(app, g_ctx);
                return true;
            }
            if(event.event == DRONE_EVT_RIGHT) {
                uint32_t v = (uint32_t)g_ctx->axis[g_ctx->sel] + DRONE_AXIS_STEP;
                if(v > BAYANG_AXIS_MAX) v = BAYANG_AXIS_MAX;
                g_ctx->axis[g_ctx->sel] = (uint16_t)v;
                drone_bayang_build_running_widget(app, g_ctx);
                return true;
            }
            return false;
        }

        return false;
    }

    if(event.type == SceneManagerEventTypeBack) {
        if(state == DroneBayangStateRunning) {
            /* Abort the in-flight loop: the Tick handler above transitions
             * back to Setup once drone_bayang_worker() sets done (on_exit()'s
             * join is the safety net either way, same as scene_fuzz.c). */
            g_ctx->stop = true;
            return true;
        }
        /* Setup: let scene_manager pop to the app menu. */
        return false;
    }

    return false;
}

void nrf24_app_scene_drone_bayang_on_exit(void* context) {
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

    widget_reset(app->widget);
}

#endif /* !PUBLIC_BUILD -- hobbyist RC-toy control, private only */
