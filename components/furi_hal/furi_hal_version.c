#include "furi_hal_version.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <esp_mac.h>
#include <esp_random.h>
#include <nvs_flash.h>
#include <nvs.h>

/* -------------------------------------------------------------------------
 * eFuse-derived device identity
 *
 * Rather than a static "ESP32" name, derive a stable, human-readable name
 * from the factory-burned eFuse MAC — the same approach the original
 * Flipper Zero firmware uses with its OTP memory.
 *
 * The name is selected from the table below using a mixing hash of all 6
 * MAC bytes.  Every device gets a consistent, unique-ish name at first boot
 * without any user interaction.  The user can still override it through the
 * Settings → Desktop → Name screen (stored in /dolphin/name.settings).
 * -------------------------------------------------------------------------*/

static const char* const s_device_name_pool[] = {
    "Axiom",  "Blip",   "Byte",   "Chirp",  "Cipher",
    "Core",   "Data",   "Drift",  "Echo",   "Edge",
    "Flux",   "Frame",  "Glint",  "Hover",  "Ion",
    "Jazz",   "Krypto", "Lumen",  "Motif",  "Nexus",
    "Orbit",  "Pixel",  "Pulse",  "Quark",  "Relic",
    "Ripple", "Spark",  "Trace",  "Vortex", "Warp",
    "Xenon",  "Zap",
};
#define DEVICE_NAME_POOL_SIZE \
    ((uint8_t)(sizeof(s_device_name_pool) / sizeof(s_device_name_pool[0])))

/* Mix 6 MAC bytes into a single byte index using FNV-like accumulation. */
static uint8_t derive_name_index(const uint8_t* mac) {
    uint8_t h = 0x5A; /* non-zero seed */
    for(int i = 0; i < 6; i++) {
        h = (uint8_t)(h * 31u + mac[i]);
    }
    return h % DEVICE_NAME_POOL_SIZE;
}

typedef struct {
    char name[FURI_HAL_VERSION_ARRAY_NAME_LENGTH];
    char device_name[FURI_HAL_VERSION_DEVICE_NAME_LENGTH];
    char ble_local_name[FURI_HAL_BT_ADV_NAME_LENGTH + 1];
    uint8_t ble_mac[6];
    uint8_t uid[6];
} FuriHalVersionState;

static FuriHalVersionState furi_hal_version = {
    .name = "ESP32",
    .device_name = "Furi ESP32",
    .ble_local_name = "ESP32",
    .ble_mac = {0},
    .uid = {0},
};

/* -------------------------------------------------------------------------
 * BLE Stealth opt-in setting, persisted in NVS.
 *
 * The RTC-flag mechanism used previously is RAM-only in this port
 * (furi_hal_rtc_init is a stub; the flags struct resets to 0 every boot), so
 * the setting never survived the reboot that is meant to apply it. NVS
 * survives reboot/power-cycle and is initialised (nvs_flash_init) before
 * furi_hal_version_init runs, so it is available everywhere this is read/set.
 * -------------------------------------------------------------------------*/
bool furi_hal_version_ble_stealth_get(void) {
    nvs_handle_t h;
    if(nvs_open("opsec", NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t v = 0;
    nvs_get_u8(h, "ble_stealth", &v); /* missing key leaves v=0 */
    nvs_close(h);
    return v != 0;
}

void furi_hal_version_ble_stealth_set(bool enabled) {
    nvs_handle_t h;
    if(nvs_open("opsec", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "ble_stealth", enabled ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

/* -------------------------------------------------------------------------
 * Custom device name, persisted in NVS. Same rationale as BLE Stealth above:
 * the SD-card + namechanger path is unreliable at boot on this port, but NVS is
 * read here in furi_hal_version_init (before BLE/display come up), so the name
 * takes effect every boot regardless of SD-mount timing.
 * -------------------------------------------------------------------------*/
#define NAME_NVS_NS  "device"
#define NAME_NVS_KEY "name"

static bool furi_hal_version_name_from_nvs(char* out, size_t out_size) {
    nvs_handle_t h;
    if(nvs_open(NAME_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = out_size;
    esp_err_t err = nvs_get_str(h, NAME_NVS_KEY, out, &len);
    nvs_close(h);
    return err == ESP_OK && out[0] != '\0';
}

void furi_hal_version_set_name_persist(const char* name) {
    nvs_handle_t h;
    if(nvs_open(NAME_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if(name && name[0]) {
            nvs_set_str(h, NAME_NVS_KEY, name);
        } else {
            nvs_erase_key(h, NAME_NVS_KEY); /* empty -> revert to default */
        }
        nvs_commit(h);
        nvs_close(h);
    }
    furi_hal_version_set_name(name); /* apply to the running name immediately */
}

static void furi_hal_version_refresh_names(const char* name) {
    snprintf(furi_hal_version.name, sizeof(furi_hal_version.name), "%s", name);
    snprintf(
        furi_hal_version.device_name,
        sizeof(furi_hal_version.device_name),
        "Furi %s",
        furi_hal_version.name);
    snprintf(
        furi_hal_version.ble_local_name,
        sizeof(furi_hal_version.ble_local_name),
        "%s",
        furi_hal_version.name);

    version_set_custom_name((Version*)version_get(), furi_hal_version.name);
}

void furi_hal_version_init(void) {
    /* Read factory MAC from eFuse — used as hardware UID and BLE address. */
    esp_efuse_mac_get_default(furi_hal_version.uid);
    memcpy(furi_hal_version.ble_mac, furi_hal_version.uid, sizeof(furi_hal_version.uid));

    /* Determine effective name:
     *   1. If a custom name was already loaded (e.g., from namechanger), use it.
     *   2. Otherwise use the hardcoded default name. */
    const char* custom = version_get_custom_name(NULL);
    char nvs_name[FURI_HAL_VERSION_ARRAY_NAME_LENGTH];
    const char* effective;
    if(furi_hal_version_name_from_nvs(nvs_name, sizeof(nvs_name))) {
        effective = nvs_name; /* user-set name from the Device Name app */
    } else if(custom && custom[0]) {
        effective = custom;
    } else {
        effective = "mob";
    }

    /* BLE Stealth (opt-in, NVS-backed): replace the default advertising identity
     * so the device isn't a constant trackable BLE beacon. Fresh random static
     * MAC + generic name per boot; sacrifices bonding persistence, which is the
     * point. */
    if(furi_hal_version_ble_stealth_get()) {
        esp_fill_random(furi_hal_version.ble_mac, 6);
        furi_hal_version.ble_mac[0] = (furi_hal_version.ble_mac[0] & 0x3F) | 0xC0; /* static random */
        effective = "BLE Device";
    }
    furi_hal_version_refresh_names(effective);
}

bool furi_hal_version_do_i_belong_here(void) {
    return true;
}

const char* furi_hal_version_get_model_name(void) {
    return "Flipper Zero";
}

const char* furi_hal_version_get_model_code(void) {
    /* Report the actual silicon so host tools (qFlipper, esptool) see the
     * real chip rather than a hardcoded "ESP32-C6". */
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    return "ESP32-S3";
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
    return "ESP32-C6";
#else
    return "ESP32";
#endif
}

const char* furi_hal_version_get_fcc_id(void) {
    return "N/A";
}

const char* furi_hal_version_get_ic_id(void) {
    return "N/A";
}

const char* furi_hal_version_get_mic_id(void) {
    return "N/A";
}

const char* furi_hal_version_get_srrc_id(void) {
    return "N/A";
}

const char* furi_hal_version_get_ncc_id(void) {
    return "N/A";
}

FuriHalVersionOtpVersion furi_hal_version_get_otp_version(void) {
    return FuriHalVersionOtpVersionEmpty;
}

uint8_t furi_hal_version_get_hw_version(void) {
    return 1;
}

uint8_t furi_hal_version_get_hw_target(void) {
    return version_get_target(NULL);
}

uint8_t furi_hal_version_get_hw_body(void) {
    return 0;
}

FuriHalVersionColor furi_hal_version_get_hw_color(void) {
    return FuriHalVersionColorUnknown;
}

uint8_t furi_hal_version_get_hw_connect(void) {
    return 0;
}

FuriHalVersionRegion furi_hal_version_get_hw_region(void) {
    return FuriHalVersionRegionWorld;
}

const char* furi_hal_version_get_hw_region_name(void) {
    return "WW";
}

FuriHalVersionRegion furi_hal_version_get_hw_region_otp(void) {
    return FuriHalVersionRegionWorld;
}

const char* furi_hal_version_get_hw_region_name_otp(void) {
    return "WW";
}

FuriHalVersionDisplay furi_hal_version_get_hw_display(void) {
    return FuriHalVersionDisplayUnknown;
}

uint32_t furi_hal_version_get_hw_timestamp(void) {
    return 0;
}

const char* furi_hal_version_get_name_ptr(void) {
    return furi_hal_version.name;
}

const char* furi_hal_version_get_device_name_ptr(void) {
    return furi_hal_version.device_name;
}

const char* furi_hal_version_get_ble_local_device_name_ptr(void) {
    return furi_hal_version.ble_local_name;
}

void furi_hal_version_set_name(const char* name) {
    /* Accept NULL or empty string — fall back to the eFuse-derived name. */
    if(name && name[0]) {
        furi_hal_version_refresh_names(name);
    } else {
        const char* derived = s_device_name_pool[derive_name_index(furi_hal_version.uid)];
        furi_hal_version_refresh_names(derived);
    }
}

const uint8_t* furi_hal_version_get_ble_mac(void) {
    return furi_hal_version.ble_mac;
}

const struct Version* furi_hal_version_get_firmware_version(void) {
    return version_get();
}

size_t furi_hal_version_uid_size(void) {
    return sizeof(furi_hal_version.uid);
}

const uint8_t* furi_hal_version_uid(void) {
    return furi_hal_version.uid;
}

const uint8_t* furi_hal_version_uid_default(void) {
    return furi_hal_version.uid;
}
