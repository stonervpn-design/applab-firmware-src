#include "fg_pwngrid.h"
#include <furi.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

const uint8_t FG_PWNGRID_MAC[6] = {0xde, 0xad, 0xbe, 0xef, 0xde, 0xad};

// Scan the frame (from offset 38) for `key`, copy the value until '"' or ',' into out.
static int scan_val(const uint8_t* fr, int len, const char* key, char* out, int out_cap) {
    int klen = (int)strlen(key);
    for(int i = 38; i + klen < len; i++) {
        bool match = true;
        for(int j = 0; j < klen; j++)
            if(fr[i + j] != (uint8_t)key[j]) {
                match = false;
                break;
            }
        if(!match) continue;
        int s = i + klen, k = 0;
        while(s < len && fr[s] != '"' && fr[s] != ',' && k < out_cap - 1) out[k++] = (char)fr[s++];
        out[k] = '\0';
        return k;
    }
    out[0] = '\0';
    return 0;
}

bool fg_pwngrid_parse(const uint8_t* frame, int len, FgPeer* out) {
    memset(out, 0, sizeof(*out));
    if(scan_val(frame, len, "name\":\"", out->name, sizeof(out->name)) == 0) return false;
    char num[12];
    if(scan_val(frame, len, "pwnd_tot\":", num, sizeof(num)) > 0)
        out->pwnd = (uint32_t)strtoul(num, NULL, 10);
    scan_val(frame, len, "face\":\"", out->face, sizeof(out->face));
    return true;
}

int fg_pwngrid_note(FgPeerTable* t, const FgPeer* peer, int8_t rssi, uint32_t now) {
    uint8_t n = t->count;
    for(uint8_t i = 0; i < n; i++) {
        if(strcmp(t->peers[i].name, peer->name) == 0) {
            t->peers[i].pwnd = peer->pwnd;
            t->peers[i].rssi = rssi;
            t->peers[i].last_seen = now;
            if(peer->face[0]) memcpy(t->peers[i].face, peer->face, sizeof(t->peers[i].face));
            return i;
        }
    }
    if(n >= FG_MAX_PEERS) return -1;
    FgPeer* p = &t->peers[n];
    *p = *peer;
    p->rssi = rssi;
    p->last_seen = now;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    t->count = n + 1;
    return n;
}

uint16_t fg_pwngrid_build_adv(
    uint8_t* out,
    const char* name,
    const char* face,
    uint32_t pwnd_tot,
    uint32_t pwnd_run,
    uint32_t uptime) {
    uint16_t n = 0;
    out[n++] = 0x80; // beacon
    out[n++] = 0x00;
    out[n++] = 0x00;
    out[n++] = 0x00; // duration
    for(int i = 0; i < 6; i++) out[n++] = 0xff; // DA broadcast
    for(int i = 0; i < 6; i++) out[n++] = FG_PWNGRID_MAC[i]; // SA
    for(int i = 0; i < 6; i++) out[n++] = FG_PWNGRID_MAC[i]; // BSSID
    out[n++] = 0x00;
    out[n++] = 0x00; // seq
    for(int i = 0; i < 8; i++) out[n++] = 0x00; // timestamp
    out[n++] = 0x64;
    out[n++] = 0x00; // beacon interval
    out[n++] = 0x00;
    out[n++] = 0x00; // capability
    char json[180];
    int jl = snprintf(
        json, sizeof(json),
        "{\"name\":\"%s\",\"version\":\"2.0\",\"pwnd_tot\":%lu,\"pwnd_run\":%lu,"
        "\"uptime\":%lu,\"face\":\"%s\"}",
        name, (unsigned long)pwnd_tot, (unsigned long)pwnd_run, (unsigned long)uptime, face);
    if(jl < 0) jl = 0;
    if(jl > (int)sizeof(json)) jl = sizeof(json);
    out[n++] = 0xde; // pwngrid chunk tag
    out[n++] = (uint8_t)jl;
    for(int i = 0; i < jl; i++) out[n++] = (uint8_t)json[i];
    return n;
}

// ---- friends registry: "name,seen" lines --------------------------------------

void fg_friends_load(Storage* s, uint32_t* out_total_unique) {
    uint32_t uniq = 0;
    File* f = storage_file_alloc(s);
    if(storage_file_open(f, FG_FRIENDS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char buf[512] = {0};
        uint16_t rd = storage_file_read(f, buf, sizeof(buf) - 1);
        buf[rd] = '\0';
        for(const char* p = buf; *p; p++)
            if(*p == '\n') uniq++;
        storage_file_close(f);
    }
    storage_file_free(f);
    if(out_total_unique) *out_total_unique = uniq;
}

void fg_friends_note(Storage* s, const char* name) {
    if(!name || !name[0]) return;
    // Best-effort: append the name once if not already present.
    File* f = storage_file_alloc(s);
    bool present = false;
    if(storage_file_open(f, FG_FRIENDS_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char buf[512] = {0};
        uint16_t rd = storage_file_read(f, buf, sizeof(buf) - 1);
        buf[rd] = '\0';
        // crude contains-check on "name,"
        char needle[24];
        int nl = snprintf(needle, sizeof(needle), "%s,", name);
        if(nl > 0 && strstr(buf, needle)) present = true;
        storage_file_close(f);
    }
    storage_file_free(f);
    if(present) return;
    storage_common_mkdir(s, "/ext/apps_data");
    File* g = storage_file_alloc(s);
    if(storage_file_open(g, FG_FRIENDS_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        char line[32];
        int n = snprintf(line, sizeof(line), "%s,1\n", name);
        storage_file_write(g, line, n);
        storage_file_close(g);
    }
    storage_file_free(g);
}
