#include "fg_capture.h"
#include "fg_net_table.h"
#include "fg_pwngrid.h"
#include "fg_progress.h"
#include "fg_geo.h"
#include "fg_ai.h"
#include <string.h>

#include "wlan_app/wlan_handshake_parser.h"
#include "wlan_app/wlan_pcap.h"

// Learn a client<->AP association seen in a data frame (cb context).
static void fg_note_client(FgApp* app, const uint8_t* ap, const uint8_t* sta) {
    uint8_t n = app->client_count;
    for(uint8_t i = 0; i < n; i++) {
        if(memcmp(app->clients[i].sta, sta, 6) == 0 && memcmp(app->clients[i].ap, ap, 6) == 0) {
            app->clients[i].ch = app->channel;
            return;
        }
    }
    if(n < FG_MAX_CLIENTS) {
        memcpy(app->clients[n].ap, ap, 6);
        memcpy(app->clients[n].sta, sta, 6);
        app->clients[n].ch = app->channel;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        app->client_count = n + 1;
    }
}

void fg_capture_cb(FgApp* app, const uint8_t* fr, int len) {
    if(len < 24 || len > FG_FRAME_MAX) return;

    if(wlan_hs_is_beacon(fr, len)) {
        // A fellow pwnagotchi?
        if(memcmp(&fr[10], FG_PWNGRID_MAC, 6) == 0) {
            FgPeer peer;
            if(fg_pwngrid_parse(fr, len, &peer)) {
                fg_pwngrid_note(&app->peers, &peer, 0, furi_get_tick());
                app->last_friend_tick = furi_get_tick();
            }
            return;
        }
        const uint8_t* bssid = &fr[16];
        int idx = fg_net_observe(&app->nets, bssid, app->channel, 0, furi_get_tick());
        if(idx >= 0) {
            char ssid[33];
            if(wlan_hs_extract_beacon_ssid(fr, len, ssid, sizeof(ssid)) && ssid[0])
                fg_net_set_ssid(&app->nets, idx, ssid);
        }
        return;
    }

    // Only forward EAPOL data frames to the worker.
    if(((fr[0] & 0x0C) >> 2) != 2) return;
    const uint8_t *bssid, *sta, *ap;
    int hlen = 0;
    if(!wlan_hs_parse_addresses(fr, len, &bssid, &sta, &ap, &hlen)) return;
    if(!(sta[0] & 0x01) && !(bssid[0] & 0x01) && memcmp(sta, bssid, 6) != 0)
        fg_note_client(app, bssid, sta);
    if(!wlan_hs_is_eapol(fr, hlen, len)) return;

    uint8_t next = (app->wr + 1) % FG_RING;
    if(next == app->rd) return; // full -> drop
    memcpy(app->ring[app->wr].data, fr, len);
    app->ring[app->wr].len = (uint16_t)len;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    app->wr = next;
}

void fg_capture_process(FgApp* app, const uint8_t* fr, uint16_t len) {
    const uint8_t *bssid, *sta, *ap;
    int hlen = 0;
    if(!wlan_hs_parse_addresses(fr, len, &bssid, &sta, &ap, &hlen)) return;
    const uint8_t* eapol = &fr[hlen + 8];
    int elen = (int)len - hlen - 8;
    if(elen < 4) return;
    uint8_t msg = wlan_hs_get_eapol_msg_num(eapol, elen);

    if(app->pcap) wlan_pcap_write_packet(app->pcap, furi_get_tick() * 1000u, fr, len);

    int idx = fg_net_observe(&app->nets, bssid, app->channel, 0, furi_get_tick());
    if(idx < 0) return;

    uint8_t pmkid[16];
    bool have_pmkid = false;
    // "pwned" = this net already had PMKID or M2 before this frame. A net counts as
    // a unique pwn only on the FIRST time it crosses that line.
    bool was_pwned = fg_net_is_pwned(&app->nets, idx);
    bool added_pwn_flag = false;

    if(msg == 2) {
        added_pwn_flag = fg_net_add_flag(&app->nets, idx, FG_NET_HAS_M2);
    } else if(msg == 1) {
        if(wlan_hs_extract_pmkid(eapol, elen, pmkid)) {
            have_pmkid = true;
            added_pwn_flag = fg_net_add_flag(&app->nets, idx, FG_NET_HAS_PMKID);
        }
    } else if(msg == 3) {
        fg_net_add_flag(&app->nets, idx, FG_NET_HAS_M3);
    } else if(msg == 4) {
        fg_net_add_flag(&app->nets, idx, FG_NET_HAS_M4);
    }

    bool new_pwn = added_pwn_flag && !was_pwned;
    uint8_t f = app->nets.nets[idx].flags;
    // Full handshake = M2 + M3 (the crackable pair); M4 is a bonus.
    bool now_full = (f & FG_NET_HAS_M2) && (f & FG_NET_HAS_M3);
    bool new_full = false;
    if(now_full) new_full = fg_net_add_flag(&app->nets, idx, FG_NET_FULL);

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->eapol_count++;
    if(msg == 2) app->handshakes++;
    if(have_pmkid) app->pmkids++;
    if(new_full) app->full_hs++;
    if(new_pwn) {
        app->prog.total_pwns++;
        app->session_pwns++;
        app->prog.cur_streak++;
        if(app->prog.cur_streak > app->prog.best_streak)
            app->prog.best_streak = app->prog.cur_streak;
        fg_progress_add_xp(&app->prog, have_pmkid ? 3 : 2);
        app->last_pwn_tick = furi_get_tick();
        if(app->channel >= 1 && app->channel <= 13)
            app->chan_hit_mask |= (1u << app->channel);
        fg_ai_reward(&app->ai, app->channel);
    }
    if(new_full) fg_progress_add_xp(&app->prog, 5);
    furi_mutex_release(app->mutex);

    if(have_pmkid) {
        // hashcat 22000 line + optional geo tag
        storage_common_mkdir(app->storage, "/ext/wifi");
        File* pf = storage_file_alloc(app->storage);
        if(storage_file_open(pf, FG_PMKID_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
            char line[160];
            int n = snprintf(line, sizeof(line), "WPA*01*");
            for(int i = 0; i < 16; i++)
                n += snprintf(line + n, sizeof(line) - n, "%02x", pmkid[i]);
            n += snprintf(line + n, sizeof(line) - n, "*");
            for(int i = 0; i < 6; i++) n += snprintf(line + n, sizeof(line) - n, "%02x", bssid[i]);
            n += snprintf(line + n, sizeof(line) - n, "*");
            for(int i = 0; i < 6; i++) n += snprintf(line + n, sizeof(line) - n, "%02x", sta[i]);
            n += snprintf(line + n, sizeof(line) - n, "****\n");
            storage_file_write(pf, line, n);
            storage_file_close(pf);
        }
        storage_file_free(pf);
    }
    if(new_pwn)
        fg_geo_tag(app->storage, bssid, app->nets.nets[idx].ssid, app->channel, 0);
}
