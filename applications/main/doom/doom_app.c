#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_display.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <dialogs/dialogs.h>
#include <bt/bt_service/bt.h>

#include <esp_system.h>
#include <esp_bt.h>
#include <esp_heap_caps.h>

#include <esp_lcd_panel_ops.h>
#include <furi_hal_spi_bus.h>
#include <furi_hal_speaker.h>

#include "lib/doomgeneric/doomgeneric.h"
#include "lib/doomgeneric/doomkeys.h"
#include "lib/doomgeneric/doomtype.h"
#include "lib/doomgeneric/doom_i2s.h"

/* Splash image (320x170 RGB565, byte-swapped for ST7789). */
extern const uint16_t doom_splash_rgb565[320 * 170];
#define DOOM_SPLASH_W 320
#define DOOM_SPLASH_H 170

/* Defined in m_menu.c. menuactive is true while Doom's in-game menu is
 * visible; messageToPrint is non-zero while a Y/N message box is shown
 * (e.g. the Quit Game confirmation). */
extern boolean menuactive;
extern int     messageToPrint;

#define DOOM_WAD_PATH "/ext/apps_data/doom/doom1.wad"

#define TAG "Doom"

void doom_flipper_runtime_init(void* panel, uint16_t w, uint16_t h);
void doom_flipper_push_key(int pressed, unsigned char doom_key);
void doom_flipper_alloc_stripe(void);
void doom_flipper_free_stripe(void);

typedef struct {
    FuriPubSub* input;
    FuriPubSubSubscription* input_sub;
    Gui* gui;
    Canvas* canvas;
    volatile bool quit;
    FuriTimer* release_left;
    FuriTimer* release_right;
    FuriTimer* release_fire;
    FuriTimer* release_use;
} DoomApp;

static void doom_release_left_cb(void* ctx) {
    UNUSED(ctx);
    doom_flipper_push_key(0, KEY_STRAFE_L); /* strafe-left pulse release */
}

static void doom_release_right_cb(void* ctx) {
    UNUSED(ctx);
    doom_flipper_push_key(0, KEY_STRAFE_R); /* strafe-right pulse release */
}

static void doom_release_fire_cb(void* ctx) {
    UNUSED(ctx);
    doom_flipper_push_key(0, KEY_FIRE);
}

static void doom_release_use_cb(void* ctx) {
    UNUSED(ctx);
    doom_flipper_push_key(0, KEY_USE);
}

/* Full Cardputer-keyboard mapping.
 *
 * Gameplay:
 *   Arrow keys  → HELD forward/back (Up/Down) + turn left/right (Left/Right).
 *   OK/Enter    → HELD fire.
 *   Space       → fire (pulse).      E → use/open.
 *   A / ,       → strafe left.       D / . → strafe right.
 *   1..7        → select weapon.     M / Tab → automap.
 *   Back (short)→ use.  Back (long) or Esc → open Doom menu.
 * Menu:
 *   Arrows tap  → navigate.  OK/Enter → select.  Back/Esc → back/close.
 *   Y/N box     → Y or N keys (or Left=No / Right=Yes).
 *
 * Nav keys (arrows/OK/Back) carry true press/release, so movement can be held.
 * Printable letters arrive as one-shot InputTypeText events (key = ASCII), so
 * action keys are pulsed (held ~120-160 ms so one Doom tic samples them).
 */
static void doom_input_callback(const void* value, void* ctx) {
    DoomApp* app = ctx;
    const InputEvent* event = value;
    const bool in_menu = menuactive || messageToPrint;

    /* ---- Printable keyboard keys (one-shot text events) ---- */
    if(event->type == InputTypeText) {
        char c = (char)event->key;
        if(c >= 'A' && c <= 'Z') c += 32; /* to lower */

        if(messageToPrint) { /* Y/N confirm box */
            if(c == 'y' || c == 'n') {
                doom_flipper_push_key(1, (unsigned char)c);
                doom_flipper_push_key(0, (unsigned char)c);
            }
            return;
        }
        if(c >= '1' && c <= '7') { /* weapon select (Doom reads the ASCII digit) */
            doom_flipper_push_key(1, (unsigned char)c);
            doom_flipper_push_key(0, (unsigned char)c);
            return;
        }
        switch(c) {
        case ' ':
            doom_flipper_push_key(1, KEY_FIRE);
            furi_timer_start(app->release_fire, pdMS_TO_TICKS(120));
            break;
        case 'e':
            doom_flipper_push_key(1, KEY_USE);
            furi_timer_start(app->release_use, pdMS_TO_TICKS(120));
            break;
        case 'a':
        case ',':
            doom_flipper_push_key(1, KEY_STRAFE_L);
            furi_timer_start(app->release_left, pdMS_TO_TICKS(160));
            break;
        case 'd':
        case '.':
            doom_flipper_push_key(1, KEY_STRAFE_R);
            furi_timer_start(app->release_right, pdMS_TO_TICKS(160));
            break;
        case 'm':
        case '\t':
            doom_flipper_push_key(1, KEY_TAB);
            doom_flipper_push_key(0, KEY_TAB);
            break;
        default:
            break;
        }
        return;
    }

    /* ---- Back: USE (short) in gameplay / ESC otherwise ---- */
    if(event->key == InputKeyBack) {
        if(event->type == InputTypeShort) {
            if(in_menu) {
                doom_flipper_push_key(1, KEY_ESCAPE);
                doom_flipper_push_key(0, KEY_ESCAPE);
            } else {
                doom_flipper_push_key(1, KEY_USE);
                furi_timer_start(app->release_use, pdMS_TO_TICKS(120));
            }
        } else if(event->type == InputTypeLong) {
            doom_flipper_push_key(1, KEY_ESCAPE);
            doom_flipper_push_key(0, KEY_ESCAPE);
        }
        return;
    }

    /* ---- OK/Enter: select in menu, HELD fire in gameplay ---- */
    if(event->key == InputKeyOk) {
        if(in_menu) {
            if(event->type == InputTypeShort) {
                doom_flipper_push_key(1, KEY_ENTER);
                doom_flipper_push_key(0, KEY_ENTER);
            }
        } else {
            if(event->type == InputTypePress)
                doom_flipper_push_key(1, KEY_FIRE);
            else if(event->type == InputTypeRelease)
                doom_flipper_push_key(0, KEY_FIRE);
        }
        return;
    }

    /* ---- Arrows: HELD move/turn in gameplay, tap-navigate in menu ---- */
    if(event->key == InputKeyUp || event->key == InputKeyDown ||
       event->key == InputKeyLeft || event->key == InputKeyRight) {
        unsigned char dk;
        switch(event->key) {
        case InputKeyUp:   dk = KEY_UPARROW; break;
        case InputKeyDown: dk = KEY_DOWNARROW; break;
        case InputKeyLeft: dk = KEY_LEFTARROW; break;
        default:           dk = KEY_RIGHTARROW; break;
        }
        if(in_menu) {
            if(event->type == InputTypeShort) {
                if(messageToPrint) {
                    unsigned char yn =
                        (event->key == InputKeyLeft || event->key == InputKeyUp) ? 'n' : 'y';
                    doom_flipper_push_key(1, yn);
                    doom_flipper_push_key(0, yn);
                } else {
                    doom_flipper_push_key(1, dk);
                    doom_flipper_push_key(0, dk);
                }
            }
        } else {
            if(event->type == InputTypePress)
                doom_flipper_push_key(1, dk);
            else if(event->type == InputTypeRelease)
                doom_flipper_push_key(0, dk);
        }
        return;
    }
}

static bool doom_wad_present(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FileInfo info;
    FS_Error err = storage_common_stat(storage, DOOM_WAD_PATH, &info);
    furi_record_close(RECORD_STORAGE);
    return err == FSE_OK && (info.flags & FSF_DIRECTORY) == 0;
}

int32_t doom_app(void* p) {
    UNUSED(p);

    DoomApp app = {0};
    app.release_left = furi_timer_alloc(doom_release_left_cb, FuriTimerTypeOnce, NULL);
    app.release_right = furi_timer_alloc(doom_release_right_cb, FuriTimerTypeOnce, NULL);
    app.release_fire = furi_timer_alloc(doom_release_fire_cb, FuriTimerTypeOnce, NULL);
    app.release_use = furi_timer_alloc(doom_release_use_cb, FuriTimerTypeOnce, NULL);
    app.gui = furi_record_open(RECORD_GUI);
    app.input = furi_record_open(RECORD_INPUT_EVENTS);
    app.input_sub = furi_pubsub_subscribe(app.input, doom_input_callback, &app);

    if(!doom_wad_present()) {
        DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
        DialogMessage* msg = dialog_message_alloc();
        dialog_message_set_header(msg, "Doom", 64, 2, AlignCenter, AlignTop);
        dialog_message_set_text(
            msg,
            "Copy doom1.wad to:\n/ext/apps_data/doom/",
            64, 32, AlignCenter, AlignCenter);
        dialog_message_set_buttons(msg, NULL, NULL, "OK");
        dialog_message_show(dialogs, msg);
        dialog_message_free(msg);
        furi_record_close(RECORD_DIALOGS);

        furi_pubsub_unsubscribe(app.input, app.input_sub);
        furi_record_close(RECORD_INPUT_EVENTS);
        furi_record_close(RECORD_GUI);
        return 0;
    }

    /* Take exclusive display access — GUI service suspends its own commits. */
    app.canvas = gui_direct_draw_acquire(app.gui);

    /* Doom needs all the internal DRAM it can get. Since we reboot on exit,
     * we can tear down BT and release its controller memory (~80 KB). */
    FURI_LOG_I(TAG, "Free DRAM before BT stop: %zu", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    Bt* bt = furi_record_open(RECORD_BT);
    bt_stop_stack(bt);
    furi_record_close(RECORD_BT);
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    FURI_LOG_I(TAG, "Free DRAM after BT release: %zu", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* Release the default speaker HAL so we can own I2S_NUM_0 outright for
     * Doom's SFX + music mixer. */
    furi_hal_speaker_deinit();
    if(!doom_i2s_init()) {
        FURI_LOG_E(TAG, "doom_i2s_init failed — Doom will run silent");
    }

    esp_lcd_panel_handle_t panel = furi_hal_display_get_panel_handle();
    uint16_t w = furi_hal_display_get_h_res();
    uint16_t h = furi_hal_display_get_v_res();

    if(!panel) {
        FURI_LOG_E(TAG, "no panel handle");
        goto cleanup;
    }
    FURI_LOG_I(TAG, "takeover %ux%u, booting doomgeneric", w, h);

    /* Show the splash image straight away so the user has something to look
     * at during WAD loading and Zone init (both take a few seconds). We
     * blit the RGB565 bitmap straight to the panel; the ST7789 driver
     * copies via DMA so we don't need to allocate a staging buffer. */
    if(DOOM_SPLASH_W == w && DOOM_SPLASH_H == h) {
        furi_hal_spi_bus_lock();
        esp_lcd_panel_draw_bitmap(panel, 0, 0, w, h, doom_splash_rgb565);
        furi_hal_spi_bus_unlock();
    }

    doom_flipper_runtime_init(panel, w, h);

    /* Boot Doom. doomgeneric_Create() blocks until Doom has fully initialized
     * (WAD loaded, menu ready) — we then pump doomgeneric_Tick() in a loop. */
    char arg0[] = "doom";
    char arg_iwad[] = "-iwad";
    char arg_wad[] = DOOM_WAD_PATH;
    char* argv[] = {arg0, arg_iwad, arg_wad, NULL};
    int argc = 3;

    doom_flipper_alloc_stripe();
    doomgeneric_Create(argc, argv);

    while(!app.quit) {
        doomgeneric_Tick();
    }

cleanup:
    doom_flipper_free_stripe();
    furi_pubsub_unsubscribe(app.input, app.input_sub);
    gui_direct_draw_release(app.gui);
    furi_record_close(RECORD_INPUT_EVENTS);
    furi_record_close(RECORD_GUI);

    FURI_LOG_I(TAG, "exit -> esp_restart()");
    esp_restart();
    return 0;
}
