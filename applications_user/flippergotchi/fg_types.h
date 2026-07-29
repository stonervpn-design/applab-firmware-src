// Flippergotchi v2.0 — shared types + constants. No logic here.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ---- bounds (no-PSRAM board: keep everything fixed + small) ----
#define FG_RING 16
#define FG_FRAME_MAX 288
#define FG_MAX_NETS 96
#define FG_MAX_PEERS 24
#define FG_MAX_CLIENTS 32

// ---- loot / state paths ----
#define FG_STATE_PATH "/ext/apps_data/flippergotchi.dat"
#define FG_LEGACY_SCORE_PATH "/ext/apps_data/flippergotchi.score"
#define FG_FRIENDS_PATH "/ext/apps_data/flippergotchi.friends"
#define FG_PCAP_PATH "/ext/wifi/flippergotchi.pcap"
#define FG_PMKID_PATH "/ext/wifi/pmkid.22000"
#define FG_WARDRIVE_PATH "/ext/wifi/flippergotchi_wardrive.csv"

// ---- per-AP capture flags ----
#define FG_NET_HAS_PMKID 0x01
#define FG_NET_HAS_M2 0x02
#define FG_NET_HAS_M3 0x04
#define FG_NET_HAS_M4 0x08
#define FG_NET_FULL 0x10
#define FG_NET_SOLICITED 0x20

typedef struct {
    uint8_t bssid[6];
    char ssid[33];
    uint8_t channel;
    int8_t rssi;
    uint8_t flags;
    uint32_t last_seen; // tick
    uint32_t solicit_tick; // last PMKID-solicit tick (cooldown)
} FgNet;

typedef struct {
    FgNet nets[FG_MAX_NETS];
    volatile uint16_t count;
} FgNetTable;

// A learned client<->AP association (for targeted, on-channel deauth).
typedef struct {
    uint8_t ap[6];
    uint8_t sta[6];
    uint8_t ch;
} FgClient;

// ---- pwngrid peer ----
typedef struct {
    char name[20];
    uint32_t pwnd;
    char face[8];
    int8_t rssi;
    uint32_t last_seen;
} FgPeer;

typedef struct {
    FgPeer peers[FG_MAX_PEERS];
    volatile uint8_t count;
} FgPeerTable;

// ---- AI channel engine ----
typedef struct {
    uint16_t ap[14]; // index 1..13
    uint16_t cl[14];
    uint16_t catch_recent[14];
    uint32_t rng;
} FgAi;

// ---- progression ----
#define FG_ACH_FIRST 0x01
#define FG_ACH_HS10 0x02
#define FG_ACH_PMKID25 0x04
#define FG_ACH_FRIEND1 0x08
#define FG_ACH_FRIEND5 0x10
#define FG_ACH_FULL 0x20
#define FG_ACH_NIGHTOWL 0x40
#define FG_ACH_SURFER 0x80
#define FG_ACH_COUNT 8

typedef struct {
    char name[13];
    uint32_t total_pwns; // unique nets pwned (lifetime)
    uint32_t xp;
    uint32_t ach; // achievement bitmask
    uint32_t best_session;
    uint32_t best_streak;
    uint32_t cur_streak;
} FgProgress;

// ---- EAPOL frame ring (cb -> worker) ----
typedef struct {
    uint16_t len;
    uint8_t data[FG_FRAME_MAX];
} FgFrame;

// ---- UI screens ----
typedef enum {
    FG_SCR_FACE,
    FG_SCR_NETS,
    FG_SCR_PEERS,
    FG_SCR_STATS,
    FG_SCR_COUNT,
} FgScreen;
