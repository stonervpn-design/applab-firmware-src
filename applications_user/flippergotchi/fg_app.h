// The full runtime app state. Integration modules (capture/attack/ui/lifecycle)
// include this; the pure modules only include fg_types.h.
#pragma once

#include "fg_types.h"
#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/view.h>
#include <storage/storage.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

typedef struct FgApp {
    ViewDispatcher* view_dispatcher;
    View* view;
    Gui* gui;
    FuriMutex* mutex;
    Storage* storage;
    File* pcap;

    volatile bool running;
    volatile bool worker_alive;
    TaskHandle_t worker;

    // cb -> worker EAPOL ring (single producer / single consumer)
    FgFrame ring[FG_RING];
    volatile uint8_t wr, rd;

    // module tables
    FgNetTable nets;
    FgPeerTable peers;
    FgAi ai;
    FgProgress prog;
    FgClient clients[FG_MAX_CLIENTS];
    volatile uint8_t client_count;

    // live stats (mutex-protected where written by worker + read by GUI)
    uint32_t handshakes; // M2 count (raw)
    uint32_t pmkids; // PMKID count (raw)
    uint32_t full_hs; // full 4-way count
    uint32_t eapol_count;
    uint32_t session_pwns; // this-run unique pwns
    uint32_t friends_total; // lifetime unique friends (from registry)
    uint32_t chan_hit_mask; // bit per channel we've caught on (achievement)
    uint8_t channel;
    uint32_t last_pwn_tick;
    uint32_t last_friend_tick;
    uint32_t start_tick;

    // toggles
    bool attack_on;
    bool solicit_on;

    // our advertisement (built in worker)
    uint8_t adv_frame[256];
    uint16_t adv_len;

    // UI state
    FgScreen screen;
    uint8_t list_sel; // scroll index for NETS/PEERS
    bool renaming;
    uint8_t rename_pos;
    char rename_buf[13];
} FgApp;

extern FgApp* g_fg;
