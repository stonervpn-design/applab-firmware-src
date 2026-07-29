#pragma once

#include <gui/view.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * lan_attack_view_scan_status — canvas view showing live LAN-recon progress:
 * a progress bar plus hosts/ports/elapsed counters, and a Cancel hint. When
 * done it shows a completion summary and the CSV file name.
 */

typedef struct {
    uint8_t progress_pct;  // 0..100
    uint16_t hosts_alive;  // alive hosts discovered
    uint16_t hosts_total;  // sweep size (254)
    uint16_t ports_found;  // open ports
    uint32_t elapsed_sec;  // seconds elapsed
    bool done;             // scan finished
    char csv_name[40];     // CSV basename (shown when done)
} LanAttackScanStatusModel;

View* lan_attack_view_scan_status_alloc(void);
void lan_attack_view_scan_status_free(View* view);

// Push a fresh snapshot into the view model (thread-safe via the view lock).
void lan_attack_view_scan_status_update(
    View* view,
    uint8_t progress_pct,
    uint16_t hosts_alive,
    uint16_t hosts_total,
    uint16_t ports_found,
    uint32_t elapsed_sec,
    bool done,
    const char* csv_name);
