#pragma once

// cam_hunter — background worker that finds IP cameras over 2.4 GHz Wi-Fi and
// tests them for default credentials. It passively sniffs the air for camera-
// vendor devices, scans for camera APs, and connects ONLY to a camera's OWN
// open/default-key AP to audit it (it never joins the operator's networks).
// Once on a cam's own AP it ARP-sweeps for live hosts, TCP-probes camera ports,
// fingerprints the vendor, and sprays a built-in default-credential list over
// HTTP (Basic/Digest) and RTSP. Findings stream to a fixed table + a CSV on SD.
//
// Authorized-use tool: run only against cameras you own or are permitted to test.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    CamHuntModeAuto = 0,
} CamHuntMode;

typedef enum {
    CamPhaseInit = 0,
    CamPhaseWifi,
    CamPhaseSniff, // promiscuous channel-hop: find cameras that are clients on a net
    CamPhaseScan,
    CamPhaseJoin,
    CamPhaseSweep,
    CamPhaseProbe,
    CamPhaseDone,
} CamPhase;

// Audited = connected to the cam's own AP and probed it; Detected = seen on the air
// (client cam on a network, or an AP we couldn't join) — cannot audit without its net.
typedef enum {
    CamKindAudited = 0,
    CamKindDetected,
} CamFindingKind;

#define CAM_HUNT_MAX_FINDINGS 40

typedef struct {
    uint8_t kind; // CamFindingKind
    char vendor[24];
    // --- Audited fields ---
    char ip[16];
    uint16_t port;
    char proto[6]; // "HTTP" / "RTSP"
    bool cracked; // creds accepted (or open)
    char cred[40]; // "admin:12345" / "open (no auth)" / "unauthenticated"
    char url[80]; // rtsp://... or http://ip:port
    // --- Detected fields ---
    char mac[18]; // "aa:bb:cc:dd:ee:ff"
    char ssid[24]; // AP SSID if known, else ""
    int8_t rssi; // signal strength (proximity)
    uint8_t channel; // Wi-Fi channel it was seen on
    bool is_ap; // true = camera broadcasts its OWN AP (auditable no-join); false = client
    uint8_t authmode; // its own AP's wifi_auth_mode_t (0 = WIFI_AUTH_OPEN) when is_ap
} CamFinding;

typedef struct {
    CamPhase phase;
    char phase_text[48]; // human-readable status line
    uint16_t aps_seen; // camera-like APs found (auto mode)
    uint16_t detected; // possible cameras seen on the air (sniff + non-joined APs)
    uint16_t hosts_alive; // live hosts on the current subnet
    uint16_t ports_open; // open camera ports found
    uint16_t creds_hit; // cameras cracked / open
    uint8_t progress_pct; // 0..100
    uint32_t elapsed_sec;
    bool done;
} CamHuntStatus;

typedef struct CamHunt CamHunt;

// Start the worker. ssid/pass are unused by the passive scan and kept only for
// signature stability — pass "" for both. Returns a handle or NULL on error.
CamHunt* cam_hunt_start(CamHuntMode mode, const char* ssid, const char* pass);

bool cam_hunt_is_done(CamHunt* h);
CamHuntStatus cam_hunt_status(CamHunt* h);

// Snapshot access to the findings table (safe to read after done, or live).
uint8_t cam_hunt_finding_count(CamHunt* h);
const CamFinding* cam_hunt_finding(CamHunt* h, uint8_t idx);

const char* cam_hunt_csv_path(CamHunt* h);

void cam_hunt_cancel(CamHunt* h); // request stop (non-blocking)
void cam_hunt_free(CamHunt* h); // join + release (NULL-safe)

// ---- On-demand audit of ONE selected finding (no-join: OWN-AP cams only) ----
// Connects to the selected camera's OWN open/default-key AP and runs the full
// port + default-credential probe against it, appending the results to the same
// findings table. Only works for a finding with is_ap set (a camera that
// broadcasts its own AP); a client cam on someone's network stays detect-only.
// Returns false if the finding isn't own-AP-auditable or an audit is already running.
bool cam_audit_start(CamHunt* h, uint8_t finding_idx);
bool cam_audit_running(CamHunt* h); // audit thread alive
bool cam_audit_done(CamHunt* h); // audit finished (result in the findings table)
const char* cam_audit_phase(CamHunt* h); // live status line
void cam_audit_join(CamHunt* h); // join + free the audit thread (NULL-safe)
