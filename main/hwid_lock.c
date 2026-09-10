#include "hwid_lock.h"

#ifndef PUBLIC_BUILD

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <furi.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <gui/canvas.h>
#include <input/input.h>
#include <esp_mac.h>
#include <mbedtls/sha256.h>
#include <nvs.h>

#define TAG "HwidLock"

// Trust-on-first-use (TOFU) device binding.
//
// The firmware ships UNBOUND (no owner baked into the binary). On the first boot
// after a fresh flash it reads the chip's factory eFuse MAC, stores its salted
// hash in NVS ("owner"), and from then on only that one device is authorized —
// any other chip gets the full-screen "DEVICE LOCKED" halt. So: whoever flashes
// and first-boots this build, the firmware locks itself to THAT device.
//
// The delivered merged image includes a blank (erased) NVS partition, so every
// flash of it resets the binding and re-enrolls the device it is flashed onto.
//
// Fail-open policy: this only ever LOCKS when a valid stored owner exists and the
// running chip does not match it. Any MAC-read / NVS error boots normally, so a
// storage hiccup can never brick the device.
#define HWID_NVS_NS  "hwidlock"
#define HWID_NVS_KEY "owner"

// Salt baked into the firmware so the value stored in NVS is a hash, never the
// raw MAC.
static const uint8_t HWID_SALT[16] = {
    0x63, 0x62, 0x7b, 0x4d, 0xb2, 0x13, 0xe3, 0x05,
    0x4b, 0x64, 0xb8, 0x01, 0xf0, 0x71, 0x13, 0x09};

static void hwid_hash(const uint8_t mac[6], uint8_t out[32]) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0); // 0 => SHA-256
    mbedtls_sha256_update(&ctx, HWID_SALT, sizeof(HWID_SALT));
    mbedtls_sha256_update(&ctx, mac, 6);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

static bool hwid_read_mac(uint8_t mac[6]) {
    memset(mac, 0, 6);
    return esp_efuse_mac_get_default(mac) == 0; // ESP_OK == 0
}

bool hwid_lock_is_authorized(void) {
    uint8_t mac[6];
    if(!hwid_read_mac(mac)) return true; // fail open: never brick on MAC read failure
    uint8_t h[32];
    hwid_hash(mac, h);

    nvs_handle_t nvs;
    if(nvs_open(HWID_NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) return true; // fail open

    uint8_t stored[32];
    size_t len = sizeof(stored);
    esp_err_t err = nvs_get_blob(nvs, HWID_NVS_KEY, stored, &len);
    bool ok;
    if(err == ESP_ERR_NVS_NOT_FOUND) {
        // First boot on this (freshly flashed) device: bind to it.
        if(nvs_set_blob(nvs, HWID_NVS_KEY, h, sizeof(h)) == ESP_OK) {
            nvs_commit(nvs);
            FURI_LOG_I(
                TAG,
                "enrolled %02X:%02X:%02X:%02X:%02X:%02X - firmware now bound to this device",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
        ok = true; // authorized (just enrolled)
    } else if(err == ESP_OK && len == sizeof(stored)) {
        ok = (memcmp(h, stored, sizeof(h)) == 0);
    } else {
        ok = true; // unexpected NVS state: fail open rather than brick
    }
    nvs_close(nvs);
    return ok;
}

typedef struct {
    uint8_t mac[6];
} HwidLockCtx;

static void hwid_lock_draw(Canvas* canvas, void* context) {
    HwidLockCtx* c = context;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 6, AlignCenter, AlignTop, "DEVICE LOCKED");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 26, AlignCenter, AlignTop, "Unauthorized hardware");
    char macs[24];
    snprintf(
        macs, sizeof(macs), "%02X:%02X:%02X:%02X:%02X:%02X", c->mac[0], c->mac[1], c->mac[2],
        c->mac[3], c->mac[4], c->mac[5]);
    canvas_draw_str_aligned(canvas, 64, 42, AlignCenter, AlignTop, macs);
}

static void hwid_lock_input(InputEvent* event, void* context) {
    UNUSED(event);
    UNUSED(context);
    // Do nothing: the fullscreen viewport captures all input for this layer, so
    // there is no navigation or escape from the locked screen.
}

void hwid_lock_enforce(void) {
    if(hwid_lock_is_authorized()) return;

    static HwidLockCtx ctx; // static: lives forever (we never return)
    hwid_read_mac(ctx.mac);
    FURI_LOG_E(
        TAG,
        "UNAUTHORIZED DEVICE %02X:%02X:%02X:%02X:%02X:%02X - halting",
        ctx.mac[0], ctx.mac[1], ctx.mac[2], ctx.mac[3], ctx.mac[4], ctx.mac[5]);

    Gui* gui = furi_record_open(RECORD_GUI); // blocks until the gui service is up
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, hwid_lock_draw, &ctx);
    view_port_input_callback_set(view_port, hwid_lock_input, &ctx);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    // Never return: the fullscreen viewport owns the display + input.
    while(true) {
        furi_delay_ms(1000);
    }
}

#else /* PUBLIC_BUILD: no HWID lock */

bool hwid_lock_is_authorized(void) {
    return true;
}
void hwid_lock_enforce(void) {
}

#endif
