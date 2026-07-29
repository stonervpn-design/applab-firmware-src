#include "lan_attack_recon.h"

#include "net_scan.h"
#include "net_socket.h"
#include "net_csv.h"

#include <furi.h>
#include <string.h>
#include <stdio.h>

#include <lwip/sockets.h> // htonl/ntohl
#include <lwip/netif.h>
#include <lwip/etharp.h>
#include <lwip/ip4_addr.h>
#include <lwip/tcpip.h>

#define TAG "LanRecon"

// Common ports probed on each alive host.
static const uint16_t recon_ports[] = {22, 80, 443, 445, 3306, 5432, 8080};
#define RECON_PORT_COUNT (sizeof(recon_ports) / sizeof(recon_ports[0]))

#define RECON_MAX_HOSTS 32 // ARP-table sized; no big buffering (no PSRAM)
#define RECON_CONNECT_TIMEOUT_MS 500
#define RECON_BANNER_TIMEOUT_MS 800
#define RECON_BANNER_MAX 256
#define RECON_SWEEP_BATCH 4

typedef struct {
    uint32_t ip; // network byte order
    uint8_t mac[6];
} ReconHost;

struct LanAttackReconState {
    FuriThread* thread;

    volatile bool cancel;
    volatile bool done;

    // Live counters (single writer = worker; readers snapshot-copy).
    volatile uint16_t hosts_alive;
    volatile uint16_t ports_scanned;
    volatile uint16_t ports_found;
    volatile uint8_t progress_pct;
    uint32_t start_tick;

    // Config / derived subnet.
    uint32_t base_host;  // host-order /24 base, e.g. 0xC0A80100
    uint32_t own_ip_net; // own IP (network order) to skip
    uint32_t gw_ip;      // host-order gateway hint
    char subnet_override[20];
    bool have_override;

    // Sweep params for the tcpip callback (only one in flight at a time).
    volatile uint16_t sweep_from;
    volatile uint16_t sweep_count;

    // Discovered hosts (populated in tcpip thread, read after sweep).
    ReconHost hosts[RECON_MAX_HOSTS];
    volatile uint8_t host_count;

    char csv_path[96];
};

// ---- helpers ----------------------------------------------------------------

static bool recon_parse_ipv4(const char* s, uint32_t* out_host) {
    uint32_t parts[4];
    int idx = 0;
    uint32_t v = 0;
    int digits = 0;
    for(const char* p = s;; p++) {
        if(*p >= '0' && *p <= '9') {
            v = v * 10 + (uint32_t)(*p - '0');
            if(v > 255) return false;
            digits++;
        } else if(*p == '.' || *p == '\0') {
            if(!digits || idx > 3) return false;
            parts[idx++] = v;
            v = 0;
            digits = 0;
            if(*p == '\0') break;
        } else {
            return false;
        }
    }
    if(idx != 4) return false;
    *out_host = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return true;
}

static void recon_fmt_mac(const uint8_t m[6], char* out, size_t cap) {
    snprintf(
        out, cap, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void recon_fmt_ip(uint32_t ip_host, char* out, size_t cap) {
    snprintf(
        out,
        cap,
        "%u.%u.%u.%u",
        (unsigned)((ip_host >> 24) & 0xFF),
        (unsigned)((ip_host >> 16) & 0xFF),
        (unsigned)((ip_host >> 8) & 0xFF),
        (unsigned)(ip_host & 0xFF));
}

static const char* recon_service_name(uint16_t port) {
    switch(port) {
    case 22: return "ssh";
    case 80: return "http";
    case 443: return "https";
    case 445: return "smb";
    case 3306: return "mysql";
    case 5432: return "postgres";
    case 8080: return "http-alt";
    default: return "unknown";
    }
}

static void recon_host_add(LanAttackReconState* st, uint32_t ip_net, const uint8_t* mac) {
    if(st->host_count >= RECON_MAX_HOSTS) return;
    if(ip_net == 0 || ip_net == st->own_ip_net) return;
    // Only accept hosts inside the /24 we are sweeping.
    if((ntohl(ip_net) & 0xFFFFFF00) != st->base_host) return;
    for(uint8_t i = 0; i < st->host_count; i++) {
        if(st->hosts[i].ip == ip_net) return;
    }
    st->hosts[st->host_count].ip = ip_net;
    memcpy(st->hosts[st->host_count].mac, mac, 6);
    st->host_count++;
}

// Runs in the lwIP tcpip thread: harvests the ARP table, then fires a batch of
// ARP requests for the next slice of the /24.
static void recon_arp_cb(void* ctx) {
    LanAttackReconState* st = ctx;
    struct netif* nif = netif_default;
    if(!nif) return;

    for(size_t i = 0; i < ARP_TABLE_SIZE; i++) {
        ip4_addr_t* ip_ret = NULL;
        struct netif* nif_ret = NULL;
        struct eth_addr* eth_ret = NULL;
        if(etharp_get_entry(i, &ip_ret, &nif_ret, &eth_ret) == 1) {
            recon_host_add(st, ip_ret->addr, eth_ret->addr);
        }
    }

    for(uint16_t k = 0; k < st->sweep_count; k++) {
        uint16_t octet = (uint16_t)(st->sweep_from + k);
        if(octet < 1 || octet > 254) continue;
        ip4_addr_t target;
        target.addr = htonl(st->base_host | octet);
        if(target.addr == st->own_ip_net) continue;
        etharp_request(nif, &target);
    }
}

static void recon_resolve_subnet(LanAttackReconState* st) {
    snprintf(
        st->csv_path,
        sizeof(st->csv_path),
        "/ext/lan_attack/captures/nmap_%lu.csv",
        (unsigned long)furi_get_tick());

    uint32_t own = 0, mask = 0;
    struct netif* nif = netif_default;
    if(nif) {
        own = netif_ip4_addr(nif)->addr;     // network order
        mask = netif_ip4_netmask(nif)->addr; // network order
    }
    st->own_ip_net = own;

    uint32_t override_host = 0;
    if(st->have_override && recon_parse_ipv4(st->subnet_override, &override_host)) {
        st->base_host = override_host & 0xFFFFFF00;
        return;
    }
    if(own && mask) {
        st->base_host = ntohl(own & mask) & 0xFFFFFF00; // sweep the /24
    } else if(st->gw_ip) {
        st->base_host = st->gw_ip & 0xFFFFFF00;
    } else {
        st->base_host = 0; // no interface -> nothing to sweep
    }
}

// Send the port-appropriate probe and capture a sanitized banner snippet.
static void recon_grab_banner(NetSocket* sock, uint16_t port, char* out, size_t cap) {
    out[0] = '\0';

    uint8_t probe[64];
    size_t plen = 0;
    if(port == 22) {
        plen = tcp_build_ssh_probe(probe, sizeof(probe));
    } else if(port == 80 || port == 8080 || port == 443) {
        plen = tcp_build_http_probe(probe, sizeof(probe));
    }
    if(plen) net_socket_tcp_send(sock, probe, plen);

    uint8_t buf[RECON_BANNER_MAX];
    size_t got = 0;
    if(!net_socket_tcp_recv(sock, buf, sizeof(buf), &got, RECON_BANNER_TIMEOUT_MS) || got == 0) {
        return;
    }

    // Collapse control/CSV-breaking bytes to single spaces; keep it printable.
    size_t j = 0;
    for(size_t i = 0; i < got && j < cap - 1; i++) {
        char c = (char)buf[i];
        if(c < 0x20 || c >= 0x7f || c == ',') {
            if(j > 0 && out[j - 1] == ' ') continue;
            c = ' ';
        }
        out[j++] = c;
    }
    while(j > 0 && out[j - 1] == ' ') j--;
    out[j] = '\0';
}

static void recon_arp_sweep(LanAttackReconState* st) {
    if(st->base_host == 0) return; // no interface

    for(uint16_t octet = 1; octet <= 254 && !st->cancel; octet += RECON_SWEEP_BATCH) {
        st->sweep_from = octet;
        st->sweep_count = RECON_SWEEP_BATCH;
        if(tcpip_callback(recon_arp_cb, st) == ERR_OK) {
            furi_delay_ms(20); // let the callback run + replies trickle in
        }
        st->hosts_alive = st->host_count;
        st->progress_pct = (uint8_t)((octet * 50U) / 254U);
    }

    // Settle: a few more table reads (no new requests) to catch late replies.
    for(int r = 0; r < 4 && !st->cancel; r++) {
        st->sweep_from = 1;
        st->sweep_count = 0;
        if(tcpip_callback(recon_arp_cb, st) == ERR_OK) furi_delay_ms(120);
        st->hosts_alive = st->host_count;
    }
}

static void recon_port_scan(LanAttackReconState* st, NetCsvLog* csv) {
    uint8_t n = st->host_count;
    for(uint8_t h = 0; h < n && !st->cancel; h++) {
        uint32_t ip_host = ntohl(st->hosts[h].ip);
        char ipstr[16];
        char macstr[18];
        recon_fmt_ip(ip_host, ipstr, sizeof(ipstr));
        recon_fmt_mac(st->hosts[h].mac, macstr, sizeof(macstr));

        for(uint8_t p = 0; p < RECON_PORT_COUNT && !st->cancel; p++) {
            uint16_t port = recon_ports[p];
            NetSocket* sock = net_socket_tcp_connect(ip_host, port, RECON_CONNECT_TIMEOUT_MS);
            st->ports_scanned++;
            if(!sock) continue;

            st->ports_found++;
            char banner[RECON_BANNER_MAX];
            recon_grab_banner(sock, port, banner, sizeof(banner));
            net_socket_close(sock);

            char line[RECON_BANNER_MAX + 64];
            snprintf(
                line,
                sizeof(line),
                "%s,%s,%u,%s,%s",
                ipstr,
                macstr,
                (unsigned)port,
                recon_service_name(port),
                banner);
            net_csv_write_line(csv, line);
        }

        if(n > 0) st->progress_pct = (uint8_t)(50U + ((uint32_t)(h + 1) * 50U) / n);
    }
}

static int32_t recon_worker(void* arg) {
    LanAttackReconState* st = arg;
    st->start_tick = furi_get_tick();

    recon_resolve_subnet(st);

    NetCsvLog* csv = net_csv_open(st->csv_path);
    net_csv_write_line(csv, "ip,mac,port,service,banner");

    recon_arp_sweep(st);
    st->progress_pct = 50;

    if(!st->cancel) recon_port_scan(st, csv);

    net_csv_close(csv);
    st->progress_pct = 100;
    st->done = true;
    FURI_LOG_I(
        TAG,
        "recon done: %u hosts, %u/%u ports open",
        (unsigned)st->hosts_alive,
        (unsigned)st->ports_found,
        (unsigned)st->ports_scanned);
    return 0;
}

// ---- public API -------------------------------------------------------------

LanAttackReconState* lan_attack_recon_start(const char* target_subnet, uint32_t gw_ip) {
    LanAttackReconState* st = malloc(sizeof(LanAttackReconState));
    if(!st) return NULL;
    memset(st, 0, sizeof(*st));
    st->gw_ip = gw_ip;
    if(target_subnet && target_subnet[0]) {
        strncpy(st->subnet_override, target_subnet, sizeof(st->subnet_override) - 1);
        st->have_override = true;
    }

    st->thread = furi_thread_alloc();
    furi_thread_set_name(st->thread, "LanRecon");
    furi_thread_set_stack_size(st->thread, 4096);
    furi_thread_set_context(st->thread, st);
    furi_thread_set_callback(st->thread, recon_worker);
    furi_thread_start(st->thread);
    return st;
}

bool lan_attack_recon_is_done(LanAttackReconState* state) {
    return state ? state->done : true;
}

LanAttackReconStatus lan_attack_recon_status(LanAttackReconState* state) {
    LanAttackReconStatus s;
    memset(&s, 0, sizeof(s));
    if(!state) return s;
    s.hosts_alive = state->hosts_alive;
    s.ports_scanned = state->ports_scanned;
    s.ports_found = state->ports_found;
    s.scan_progress_pct = state->progress_pct;
    s.done = state->done;
    uint32_t freq = furi_kernel_get_tick_frequency();
    if(freq == 0) freq = 1000;
    s.elapsed_sec = (furi_get_tick() - state->start_tick) / freq;
    return s;
}

const char* lan_attack_recon_csv_path(LanAttackReconState* state) {
    return state ? state->csv_path : "";
}

void lan_attack_recon_cancel(LanAttackReconState* state) {
    if(state) state->cancel = true;
}

void lan_attack_recon_join(LanAttackReconState* state) {
    if(state && state->thread) furi_thread_join(state->thread);
}

void lan_attack_recon_free(LanAttackReconState* state) {
    if(!state) return;
    state->cancel = true;
    if(state->thread) {
        furi_thread_join(state->thread);
        furi_thread_free(state->thread);
    }
    free(state);
}
