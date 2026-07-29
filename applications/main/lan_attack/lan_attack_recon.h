#pragma once
#include <stdint.h>
#include <stdbool.h>

/**
 * lan_attack_recon — LAN recon worker (ARP sweep -> TCP connect-scan -> banner
 * grab) running on a FuriThread. Results stream to CSV via net_csv (no large
 * in-RAM table; this target has no PSRAM). Live progress is exposed through
 * atomic counters read by the UI scene.
 */

typedef struct LanAttackReconState LanAttackReconState;

// Live snapshot of scan progress (all fields are plain-copied atomically).
typedef struct {
    uint16_t hosts_alive;      // hosts that answered ARP (alive on the /24)
    uint16_t ports_scanned;    // TCP connect attempts made so far
    uint16_t ports_found;      // open ports discovered
    uint8_t scan_progress_pct; // 0..100
    uint32_t elapsed_sec;      // seconds since start
    bool done;                 // true once the worker finished / was cancelled
} LanAttackReconStatus;

/**
 * Start the recon worker. target_subnet (optional, dotted "a.b.c.0") overrides
 * the auto-detected /24; pass NULL to derive it from the connected interface.
 * gw_ip (host byte order, 0 = none) is an optional gateway hint. Returns an
 * opaque state the caller must cancel/join/free, or NULL on error.
 */
LanAttackReconState* lan_attack_recon_start(const char* target_subnet, uint32_t gw_ip);

// Non-blocking: true once the scan finished (or was cancelled).
bool lan_attack_recon_is_done(LanAttackReconState* state);

// Non-blocking snapshot of the live counters.
LanAttackReconStatus lan_attack_recon_status(LanAttackReconState* state);

// Path of the CSV the worker is writing (empty string until opened). Stable
// for the lifetime of state.
const char* lan_attack_recon_csv_path(LanAttackReconState* state);

// Request a clean stop; the worker unwinds at the next checkpoint. Non-blocking.
void lan_attack_recon_cancel(LanAttackReconState* state);

// Block until the worker thread exits (cancel first for a fast stop).
void lan_attack_recon_join(LanAttackReconState* state);

// Join (if needed) and release all resources. NULL-safe.
void lan_attack_recon_free(LanAttackReconState* state);
