#pragma once
// Heuristic camera classifier: is a device / AP likely a camera?
//   - by MAC OUI (vendor prefix) — curated camera/NVR-maker list (not exhaustive;
//     a match means "likely from a camera vendor", the user verifies physically).
//   - by SSID keyword.
// Used by both the AP-scan path and the promiscuous-sniff path so the app finds
// cameras that broadcast their own AP AND cameras that are clients on a network.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

// Confidence that an OUI means "camera", used to decide passive-detect vs active-probe.
//   High = a camera/NVR-specific maker: safe to actively probe (default-key/join attempt).
//   Low  = a vendor that ALSO makes ordinary routers / hubs / generic IoT (Netgear,
//          TP-Link, D-Link, Ubiquiti, Amazon, Nest, Tuya, ESP32): DETECT-ONLY, so the
//          active default-WPA-key attempt never fires at a normal router just from its OUI.
typedef enum {
    CamConfNone = 0,
    CamConfLow,
    CamConfHigh,
} CamConf;

typedef struct {
    uint8_t oui[3];
    const char* vendor;
    CamConf conf;
} CamOui;

// Curated OUI prefixes for common IP-camera / NVR / smart-cam makers. Extend
// freely — this is a best-effort heuristic, not an authoritative registry.
static const CamOui kCamOuis[] = {
    // Hikvision / EZVIZ — camera-specific
    {{0x44, 0x19, 0xB6}, "Hikvision", CamConfHigh},
    {{0x4C, 0xBD, 0x8F}, "Hikvision", CamConfHigh},
    {{0xC0, 0x56, 0xE3}, "Hikvision", CamConfHigh},
    {{0x28, 0x57, 0xBE}, "Hikvision", CamConfHigh},
    {{0xBC, 0xAD, 0x28}, "Hikvision", CamConfHigh},
    {{0xE0, 0xCA, 0x3C}, "EZVIZ", CamConfHigh},
    // Dahua / Amcrest / Lorex — camera-specific
    {{0x3C, 0xEF, 0x8C}, "Dahua", CamConfHigh},
    {{0x90, 0x02, 0xA9}, "Dahua", CamConfHigh},
    {{0x08, 0xED, 0xED}, "Dahua", CamConfHigh},
    {{0x14, 0xA7, 0x8B}, "Dahua", CamConfHigh},
    {{0x9C, 0x8E, 0xCD}, "Amcrest", CamConfHigh},
    // Reolink — camera-specific
    {{0xEC, 0x71, 0xDB}, "Reolink", CamConfHigh},
    // Wyze — camera-specific
    {{0x2C, 0xAA, 0x8E}, "Wyze", CamConfHigh},
    {{0x7C, 0x78, 0xB2}, "Wyze", CamConfHigh},
    // Ring — camera/doorbell-specific
    {{0x44, 0x73, 0xD6}, "Ring", CamConfHigh},
    // Axis — camera-specific
    {{0x00, 0x40, 0x8C}, "Axis", CamConfHigh},
    {{0xAC, 0xCC, 0x8E}, "Axis", CamConfHigh},
    // Foscam — camera-specific
    {{0x00, 0x62, 0x6E}, "Foscam", CamConfHigh},
    // Vivotek — camera-specific
    {{0x00, 0x02, 0xD1}, "Vivotek", CamConfHigh},
    // Additional verified Hikvision prefixes (IEEE MA-L, Hangzhou Hikvision)
    {{0xC4, 0x2F, 0x90}, "Hikvision", CamConfHigh},
    {{0xA4, 0x14, 0x37}, "Hikvision", CamConfHigh},
    {{0x54, 0xC4, 0x15}, "Hikvision", CamConfHigh},
    {{0xF8, 0x4D, 0xFC}, "Hikvision", CamConfHigh},
    {{0x68, 0x6D, 0xBC}, "Hikvision", CamConfHigh},
    // Additional verified Dahua prefixes (Zhejiang Dahua)
    {{0x6C, 0x1C, 0x71}, "Dahua", CamConfHigh},
    {{0x74, 0xC9, 0x29}, "Dahua", CamConfHigh},
    // Additional verified Axis prefixes (Axis Communications AB)
    {{0xB8, 0xA4, 0x4F}, "Axis", CamConfHigh},
    {{0xE8, 0x27, 0x25}, "Axis", CamConfHigh},
    // Uniview — camera/NVR-specific (Zhejiang Uniview Technologies)
    {{0x48, 0xEA, 0x63}, "Uniview", CamConfHigh},
    // Hanwha / Wisenet (Samsung Techwin) — camera-specific
    {{0x00, 0x09, 0x18}, "Hanwha", CamConfHigh},
    {{0xE4, 0x30, 0x22}, "Hanwha", CamConfHigh},
    // ---- Low confidence: vendor also makes routers/hubs/generic IoT (detect-only) ----
    // Amazon (Echo/FireTV/etc., not just Ring)
    {{0xFC, 0xA1, 0x83}, "Amazon", CamConfLow},
    {{0x74, 0xC2, 0x46}, "Amazon", CamConfLow},
    // Nest / Google (thermostats/speakers as well as cams)
    {{0x18, 0xB4, 0x30}, "Nest", CamConfLow},
    {{0x64, 0x16, 0x66}, "Nest", CamConfLow},
    // Arlo / Netgear (mostly routers/APs)
    {{0x9C, 0xD3, 0x6D}, "Netgear", CamConfLow},
    {{0xB0, 0x7F, 0xB9}, "Netgear", CamConfLow},
    // Ubiquiti (UniFi Protect cams, but mostly APs/routers/switches)
    {{0x24, 0x5A, 0x4C}, "Ubiquiti", CamConfLow},
    {{0x78, 0x8A, 0x20}, "Ubiquiti", CamConfLow},
    {{0xFC, 0xEC, 0xDA}, "Ubiquiti", CamConfLow},
    // TP-Link / Tapo / Kasa (routers + plugs/bulbs as well as Tapo cams)
    {{0x50, 0xC7, 0xBF}, "TP-Link", CamConfLow},
    {{0x54, 0xAF, 0x97}, "Tapo", CamConfLow},
    // D-Link (routers as well as cams)
    {{0x00, 0x1B, 0x11}, "D-Link", CamConfLow},
    {{0x28, 0x10, 0x7B}, "D-Link", CamConfLow},
    // Tuya generic IoT (plugs/bulbs/cams)
    {{0xD8, 0x1F, 0x12}, "Tuya", CamConfLow},
    {{0x68, 0x57, 0x2D}, "Tuya", CamConfLow},
    // Espressif — DIY ESP32-CAM but overwhelmingly non-cameras
    {{0x24, 0x0A, 0xC4}, "ESP32?", CamConfLow},
    {{0x84, 0xCC, 0xA8}, "ESP32?", CamConfLow},
    {{0x7C, 0x9E, 0xBD}, "ESP32?", CamConfLow},
    {{0x30, 0xAE, 0xA4}, "ESP32?", CamConfLow},
    {{0x24, 0x6F, 0x28}, "ESP32?", CamConfLow},
    {{0x3C, 0x71, 0xBF}, "ESP32?", CamConfLow},
};
#define CAM_OUI_COUNT (sizeof(kCamOuis) / sizeof(kCamOuis[0]))

// Vendor label if `mac`'s OUI is a known camera maker, else NULL.
static inline const char* cam_vendor_from_oui(const uint8_t mac[6]) {
    for(size_t i = 0; i < CAM_OUI_COUNT; i++) {
        if(mac[0] == kCamOuis[i].oui[0] && mac[1] == kCamOuis[i].oui[1] &&
           mac[2] == kCamOuis[i].oui[2]) {
            return kCamOuis[i].vendor;
        }
    }
    return NULL;
}

// Confidence tier for `mac`'s OUI (CamConfNone if not a known camera-maker prefix).
static inline CamConf cam_oui_confidence(const uint8_t mac[6]) {
    for(size_t i = 0; i < CAM_OUI_COUNT; i++) {
        if(mac[0] == kCamOuis[i].oui[0] && mac[1] == kCamOuis[i].oui[1] &&
           mac[2] == kCamOuis[i].oui[2]) {
            return kCamOuis[i].conf;
        }
    }
    return CamConfNone;
}

static const char* kCamSsidKw[] = {
    "ipcam", "ip-cam", "ipc", "camera", "cam-", "-cam", "dvr", "nvr",  "onvif",
    "wyze",  "reol",   "ring", "foscam", "ezviz", "tapo", "amcrest", "dahua",
    "hik",   "gopro",  "insta360", "yi-",  "v380", "sricam", "wificam", "p2p",
    "ptz",   "spycam", "hisee", "jooan", "esp32-cam", "doorbell", "vstarcam",
};
#define CAM_SSID_KW_COUNT (sizeof(kCamSsidKw) / sizeof(kCamSsidKw[0]))

static inline bool cam_ci_contains(const char* hay, const char* needle) {
    size_t nl = strlen(needle);
    if(!nl || !hay) return false;
    for(const char* p = hay; *p; p++) {
        size_t i = 0;
        while(i < nl && p[i] && ((p[i] | 0x20) == (needle[i] | 0x20))) i++;
        if(i == nl) return true;
    }
    return false;
}

// True if the SSID contains a camera-ish keyword.
static inline bool cam_ssid_looks_like_cam(const char* ssid) {
    if(!ssid || !ssid[0]) return false;
    for(size_t i = 0; i < CAM_SSID_KW_COUNT; i++) {
        if(cam_ci_contains(ssid, kCamSsidKw[i])) return true;
    }
    return false;
}
