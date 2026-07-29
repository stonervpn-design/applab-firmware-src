# Network LAN Red-Team Suite Phase 1 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use checkboxes (`- [ ]`): write failing test → run it (fails) → implement → run (passes) → build → commit.
> **Model policy:** opus/haiku only (Sonnet trips the cyber-safety gate — do not use it for any step here).

**Spec:** `docs/superpowers/specs/2026-07-11-network-lan-suite-design.md`

**Goal:** Ship Phase 1 of the LAN/network red-team suite — authorized, internal-network penetration-testing tools running on the native ESP32-S3 radio. Scope: passive LAN recon (ARP sweep, TCP port scan, banner grab), LLMNR/NBT-NS/mDNS poisoning with NTLMv2 credential capture, and on-device NTLMv2 HMAC-MD5 dictionary cracker. Every attack's pure logic is host-testable; every scene cleans up its sockets and tasks on exit. Authorization gate locks active frame-injecting attacks.

**Architecture:**
- **`lib/net_hal/`** — shared library with pure, host-testable cores (DHCP/NTLM/WPA packet builders/parsers, ARP/TCP scan helpers, HMAC-MD5/PBKDF2-SHA1 hashing). Socket wrappers encapsulate lwIP UDP/TCP lifecycle. CSV writers stream results to SD.
- **`applications/main/lan_attack/`** — UI orchestration (scenes, views, scene manager). Spawns FreeRTOS tasks for recon scanner and LLMNR responder; tasks consume `net_hal` primitives. Each task joins on scene exit, sockets close cleanly.
- **Wordlists empty by default** at `/ext/lan_attack/wordlists/` (do not bundle).
- **Single worker task** (v1 single-core; dual-core speedup is documented Phase-2 future work).
- **Built-in `A_*` menu icon** (no Pillow).

**Tech Stack:** ESP-IDF v5.4.1 (ESP32-S3), C. Flipper firmware app. Host unit tests: WSL `gcc` on Windows. Board: `m5stack_cardputer_adv` (no PSRAM).

## Global Constraints

- **Firmware build** (synchronous, never backgrounded, timeout 600000 ms):
  ```
  MSYS_NO_PATHCONV=1 cmd.exe /c "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_ci.bat" > build_<tag>.log 2>&1; echo "EXIT=$?"
  ```
  Success = log contains `Project build complete` **and** `=== BUILD OK ===`, and `EXIT=0`.
- **Host test** (run from Git Bash, project root — uses WSL gcc):
  ```
  wsl.exe -- bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15 && gcc -Wall -Wextra -o /tmp/t tests/<test>.c <pure-source(s)> && /tmp/t"
  ```
  Pure modules must NOT include `furi.h`, `esp_*.h`, or `gui/*` — only `<stdint.h>`, `<stdbool.h>`, `<string.h>`, `<stdio.h>`, `<math.h>`. This is what lets the `.c` compile on the host.
- **Repo is not a git repo per the harness env, BUT prior plans commit per task;** if `git` is available run commits, otherwise skip and note it.
- **App source is auto-globbed:** `applications/main/lan_attack/` including `scenes/` is auto-compiled. Just create the files.

---

## Task 1 — `net_hal` pure cores: DHCP/NTLM/WPA/ARP/TCP builders + parsers (host-tested)

**Files:**
- Create `lib/net_hal/net_hal.h` (public interface, pure section + firmware section marked)
- Create `lib/net_hal/net_hal_dhcp.c` (DHCP builders/parsers; pure C)
- Create `lib/net_hal/net_hal_ntlm.c` (NTLM-v2 response compute + parser; pure C)
- Create `lib/net_hal/net_hal_wpa.c` (WPA/WPA2 PMK/PTK/MIC; pure C + embedded SW SHA1/HMAC-MD5)
- Create `lib/net_hal/net_hal_scan.c` (ARP request/reply builders; pure C)
- Create `tests/test_net_hal_dhcp.c` (DHCP frame builder test, host-compiled)
- Create `tests/test_net_hal_ntlm.c` (NTLM-v2 response compute test, host-compiled)
- Create `tests/test_net_hal_wpa.c` (WPA PMK compute test, host-compiled)

**Interfaces produced:**
```c
// net_hal.h — PURE SECTION (top, before any esp/furi includes)
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// ============ DHCP frame builders (pure) ============
#define DHCP_BUILD_MAX 548
typedef struct { uint8_t buf[DHCP_BUILD_MAX]; size_t len; } DhcpFrame;

size_t dhcp_build_discover(DhcpFrame* out, const uint8_t client_mac[6]);
size_t dhcp_build_offer(DhcpFrame* out, const uint8_t client_mac[6], 
                        uint32_t xid, uint32_t offered_ip, uint32_t gateway, 
                        uint32_t dns, uint16_t lease_time_sec);
size_t dhcp_build_ack(DhcpFrame* out, const uint8_t client_mac[6], 
                      uint32_t xid, uint32_t assigned_ip, uint32_t gateway, 
                      uint32_t dns, uint16_t lease_time_sec);
size_t dhcp_build_nak(DhcpFrame* out, const uint8_t client_mac[6], uint32_t xid);

typedef struct {
    uint8_t msg_type;      // DHCP_DISCOVER=1, REQUEST=3, ...
    uint8_t client_mac[6];
    uint32_t xid;
    uint32_t client_ip;    // 0 if none
} DhcpParsed;
bool dhcp_parse(const uint8_t* frame, size_t len, DhcpParsed* out);

// ============ NTLM-v2 response compute + parser (pure) ============
// Compute the NTLM-v2 response blob (the HMAC-MD5 result) given password + challenge.
void ntlmv2_response_compute(const char* password, const char* username, const char* domain,
                              const uint8_t challenge[8], const uint8_t context[8],
                              uint8_t out_response[16]);

typedef struct {
    char username[256];
    char domain[256];
    uint8_t challenge[8];
    uint8_t response[16];  // the v2 response (HMAC-MD5 blob)
} NtlmV2Parsed;
bool ntlm_parse_type3(const uint8_t* frame, size_t len, NtlmV2Parsed* out);

// ============ WPA/WPA2 PMK/PTK/MIC (pure) ============
// Compute PMK (Pairwise Master Key) from PSK (password) + SSID.
// ssid_len should be strlen(ssid) or explicit SSID length.
bool wpa_pmk_compute(const char* password, const char* ssid, size_t ssid_len,
                      uint8_t out_pmk[32]);

// Compute PTK (Pairwise Transient Key) from PMK + handshake nonces + MACs.
bool wpa_ptk_compute(const uint8_t pmk[32], 
                      const uint8_t anonce[32], const uint8_t snonce[32],
                      const uint8_t ap_mac[6], const uint8_t sta_mac[6],
                      uint8_t out_ptk[48]);

// Verify the MIC in an EAPOL frame (M4 from the handshake) given the PTK.
// ptk[0..15] is the MIC key; eapol_frame points to the full EAPOL (with MIC zeroed for comparison).
bool wpa_mic_verify(const uint8_t ptk[48], const uint8_t eapol_frame[/*eapol_len*/], 
                     size_t eapol_len, const uint8_t captured_mic[16]);

// ============ ARP request builder (pure) ============
// Build a raw ARP request to "who-has" target_ip on the network.
// Returns the size written, 0 on error. out must be at least 28 bytes (+ optional padding).
size_t arp_build_request(uint8_t out[/*>=28*/], size_t out_max,
                          const uint8_t sender_mac[6], uint32_t sender_ip,
                          uint32_t target_ip);

// Parse ARP reply. Returns true if it's a reply and sets out_mac to the responder MAC.
bool arp_parse_reply(const uint8_t* frame, size_t len, uint8_t out_mac[6], uint32_t* out_ip);

// ============ TCP banner capture builder (pure) ============
// Build probe strings for banner grab. Returns the size.
size_t tcp_build_ssh_probe(uint8_t out[/*>=4*/], size_t out_max);
size_t tcp_build_http_probe(uint8_t out[/*>=40*/], size_t out_max);
```

- [ ] **Step 1: Write failing tests.** Create `tests/test_net_hal_dhcp.c`, `test_net_hal_ntlm.c`, `test_net_hal_wpa.c` with real test data:

  **test_net_hal_dhcp.c** (includes only `net_hal.h`, `<stdio.h>`, `<string.h>`):
  - `test_dhcp_discover_builder`: build a DHCP Discover frame from MAC `{0x00,0x11,0x22,0x33,0x44,0x55}`, verify:
    - size > 0 and < DHCP_BUILD_MAX
    - byte 0 (opcode) = 1 (request)
    - bytes 236-239 (magic cookie) = `{0x63, 0x82, 0x53, 0x63}`
    - MAC bytes present in options
  - `test_dhcp_offer_builder`: build Offer with `offered_ip=0xc0a80102` (192.168.1.2), `gateway=0xc0a80101`, `dns=0x08080808`, `lease=3600`; verify:
    - opcode = 2 (reply)
    - DHCP message type option (53) = 2 (offer)
    - assigned IP option (50) = `0xc0a80102`
    - gateway option (3) = `0xc0a80101`
    - DNS option (6) = `0x08080808`
    - lease time option (51) = 3600 (big-endian)
  - `test_dhcp_parse`: feed the built Offer frame back into parser, verify parsed fields.
  - `main()` runs all, prints `ALL DHCP TESTS PASSED`.

  **test_net_hal_ntlm.c** (includes only `net_hal.h`, `<stdio.h>`, `<string.h>`, `<ctype.h>` for hex):
  - Hardcode a known NTLMv2 test vector: password `"Password"`, username `"User"`, domain `"DOMAIN"`, challenge `{0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef}`, context `{0xfe,0xdc,0xba,0x98,0x76,0x54,0x32,0x10}`.
  - Call `ntlmv2_response_compute()` → response (16 bytes).
  - Expected response (from reference HMAC-MD5 lib or pre-computed): verify `memcmp(response, expected, 16)==0`.
  - `test_ntlm_parse_type3`: construct a minimal Type-3 message (binary), feed to parser, verify username/domain/challenge/response extracted.
  - `main()` → `ALL NTLM TESTS PASSED`.

  **test_net_hal_wpa.c** (includes only `net_hal.h`, `<stdio.h>`, `<string.h>`):
  - Hardcode a known WPA2 test vector (from standard WPA2 handshake capture):
    - SSID: `"MyNetwork"`, PSK: `"TestPassword123"` (43 chars)
    - Expected PMK (32 bytes, from reference PBKDF2-SHA1): pre-computed hex string
  - Call `wpa_pmk_compute("TestPassword123", "MyNetwork", 9, out_pmk)` → verify `memcmp(out_pmk, expected, 32)==0`.
  - `main()` → `ALL WPA TESTS PASSED`.

- [ ] **Step 2: Run tests → ALL FAIL** (headers/functions missing).
  ```bash
  wsl.exe -- bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15 && gcc -Wall -Wextra -o /tmp/t tests/test_net_hal_dhcp.c lib/net_hal/net_hal_dhcp.c && /tmp/t"
  ```

- [ ] **Step 3: Implement `net_hal_dhcp.c`** — pure C, no esp/furi/socket deps.
  - DHCP frame format: standard RFC 2131 (opcode, xid, dhcp_secs, flags, ciaddr, yiaddr, siaddr, giaddr, chaddr, options).
  - `dhcp_build_discover`: craft Discover message (opcode=1, set client MAC in chaddr, magic cookie, append message-type=1 option).
  - `dhcp_build_offer`: reply with message-type=2, set offered_ip (yiaddr), options (subnet mask, gateway, dns, lease time).
  - `dhcp_parse`: read opcode, client MAC from chaddr, xid, scan options.
  - Helpers for option encoding (type-length-value). No over-read bounds; return 0 on error.

- [ ] **Step 4: Implement `net_hal_ntlm.c`** — pure C.
  - Embed a reference SW SHA1 + HMAC implementation (if not in FreeRTOS/mbedTLS headers; reference: RFC 3174 or existing Flipper code).
  - Or check if `mbedtls/md5.h` is available in the firmware — if so, reuse it (already linked).
  - `ntlmv2_response_compute`: `HMAC_MD5(password_bytes, challenge || context)` → 16-byte response.
  - `ntlm_parse_type3`: scan NTLM Type-3 message for user/domain buffers (binary offsets), extract challenge, response blob.
  - Bounds-check all buffer reads.

- [ ] **Step 5: Implement `net_hal_wpa.c`** — pure C.
  - `wpa_pmk_compute`: `PBKDF2_HMAC_SHA1(password, ssid, 4096 iterations, 32 bytes)`.
    - Embed SW SHA1 (RFC 3174) or check mbedTLS availability: `#include <mbedtls/sha1.h>` and use if present.
    - PBKDF2: standard HMAC iteration (RFC 2898).
  - `wpa_ptk_compute`: PRF-SHA1 over PMK + nonces + MACs (IEEE 802.11i section 8.5.1.4).
  - `wpa_mic_verify`: zero the MIC field in the EAPOL frame, compute `HMAC_MD5(ptk[0:16], eapol)`, compare to captured.
  - Bounds-check; return false on parse error, true on success or verified.

- [ ] **Step 6: Implement `net_hal_scan.c`** — pure C.
  - `arp_build_request`: write ARP request frame (Ethernet header + ARP, no padding). Return size.
  - `arp_parse_reply`: check ARP opcode=2, extract responder MAC and IP.
  - `tcp_build_ssh_probe`: `"\r\n"` (2 bytes).
  - `tcp_build_http_probe`: `"GET / HTTP/1.0\r\nHost: localhost\r\nConnection: close\r\n\r\n"` (~50 bytes).

- [ ] **Step 7: Run all host tests → PASS**.
  ```bash
  wsl.exe -- bash -lc "... && gcc -Wall -Wextra -o /tmp/t tests/test_net_hal_dhcp.c lib/net_hal/net_hal_dhcp.c && /tmp/t"
  wsl.exe -- bash -lc "... && gcc -Wall -Wextra -o /tmp/t tests/test_net_hal_ntlm.c lib/net_hal/net_hal_ntlm.c && /tmp/t"
  wsl.exe -- bash -lc "... && gcc -Wall -Wextra -o /tmp/t tests/test_net_hal_wpa.c lib/net_hal/net_hal_wpa.c && /tmp/t"
  ```

- [ ] **Step 8: Write `net_hal.h` public section** (pure frames above + firmware section placeholder).

- [ ] **Step 9: Create `lib/net_hal/SConscript`** or CMake glue to register as a `lib` module (match `lib/wifi_raw/SConscript` pattern; declare `env.StaticLibrary("net_hal", sources=[...])` for the pure `.c` files).

- [ ] **Step 10: Build** → `build_t1.log`. Expect `=== BUILD OK ===`.

- [ ] **Step 11: Commit** `feat(net_hal): DHCP/NTLM/WPA/ARP pure cores (host-tested)`.

---

## Task 2 — `net_hal` socket wrappers + CSV writers (firmware, task-safe)

**Files:**
- Create `lib/net_hal/net_hal_socket.c` (UDP/TCP lifecycle wrappers)
- Create `lib/net_hal/net_hal_csv.c` (CSV logging, hashcat format writers)
- Extend `lib/net_hal/net_hal.h` with firmware section (socket + CSV APIs)
- Create `tests/test_net_hal_socket.c` (optional: mock socket test; can defer if no mock layer)

**Interfaces produced:**
```c
// net_hal.h — FIRMWARE SECTION (after #include <esp_wifi.h> guards)
#ifdef ESP_IDF_VERSION  // firmware only

typedef struct NetHalSocket NetHalSocket;  // opaque

// UDP
NetHalSocket* net_hal_udp_bind(uint16_t port, bool broadcast);
bool net_hal_udp_send(NetHalSocket* sock, const struct sockaddr_in* dest, 
                       const uint8_t* data, size_t len);
bool net_hal_udp_recv(NetHalSocket* sock, struct sockaddr_in* src, 
                       uint8_t* data, size_t max_len, size_t* out_len, uint32_t timeout_ms);

// TCP
NetHalSocket* net_hal_tcp_bind(uint16_t port);
NetHalSocket* net_hal_tcp_accept(NetHalSocket* listen_sock, struct sockaddr_in* client_addr, 
                                  uint32_t timeout_ms);
bool net_hal_tcp_send(NetHalSocket* sock, const uint8_t* data, size_t len);
bool net_hal_tcp_recv(NetHalSocket* sock, uint8_t* data, size_t max_len, 
                       size_t* out_len, uint32_t timeout_ms);
void net_hal_socket_close(NetHalSocket* sock);
bool net_hal_socket_is_open(NetHalSocket* sock);

// CSV logging (to SD, line-buffered)
typedef struct NetHalCsvLog NetHalCsvLog;

NetHalCsvLog* net_hal_csv_open(const char* path);
void net_hal_csv_write_line(NetHalCsvLog* log, const char* csv_line);
void net_hal_csv_close(NetHalCsvLog* log);

// Convenience: write NTLMv2 hash in hashcat -m 5500 format (username::domain:challenge_hex:response_hex)
void net_hal_write_ntlmv2_hashcat(NetHalCsvLog* log, const char* username, const char* domain,
                                   const uint8_t challenge[8], const uint8_t response[16]);

#endif  // ESP_IDF_VERSION
```

- [ ] **Step 1: Implement `net_hal_socket.c`** — wrappers around `lwip/sockets.h`.
  - Each `net_hal_socket_*` call opens a socket (UDP or TCP), binds, sets socket options (non-blocking if needed, SO_REUSEADDR).
  - `net_hal_udp_recv`: poll or select with timeout; return on data or timeout.
  - `net_hal_tcp_accept`: poll listen socket with timeout, accept one connection.
  - All return NULL/false on error; log to ESP_LOG* at WARN level.
  - Store `fd` opaquely in the struct; no caller access to raw FDs.

- [ ] **Step 2: Implement `net_hal_csv.c`** — SD file I/O via Furi storage.
  - `net_hal_csv_open`: ensure `/ext/lan_attack/` exists (call `storage_common_mkdir`); open `/ext/lan_attack/captures/...csv` in append mode.
  - `net_hal_csv_write_line`: `fprintf(f, "%s\n", line);` + fflush.
  - `net_hal_csv_close`: fclose.
  - `net_hal_write_ntlmv2_hashcat`: format `username::domain:challenge_hex:response_hex\n` (challenge and response as uppercase hex), call `write_line`.

- [ ] **Step 3: Build** → `build_t2.log`. Expect BUILD OK.

- [ ] **Step 4: Commit** `feat(net_hal): socket wrappers + CSV writers (firmware)`.

---

## Task 3 — `lan_attack` app skeleton + registration

**Files:**
- Create `applications/main/lan_attack/application.fam`
- Create `applications/main/lan_attack/lan_attack_app.h`
- Create `applications/main/lan_attack/lan_attack_app_i.h`
- Create `applications/main/lan_attack/lan_attack_app.c`
- Create `applications/main/lan_attack/lan_attack_custom_event.h`
- Create `applications/main/lan_attack/scenes/lan_attack_scene.h`
- Create `applications/main/lan_attack/scenes/lan_attack_scene.c`
- Create `applications/main/lan_attack/scenes/lan_attack_scene_config.h`
- Create `applications/main/lan_attack/scenes/lan_attack_scene_main.c`
- Create `applications/main/lan_attack/scenes/lan_attack_scene_authorize.c`
- Modify `fam_config.py` (add `"lan_attack"` to `APPS`)

**Interfaces produced:** `int32_t lan_attack_app(void* p)` entry point; `struct LanAttackApp`; `LanAttackScene*` / `LanAttackCustomEvent` enums.

- [ ] **Step 1: Manifest.** Create `applications/main/lan_attack/application.fam`:
  ```python
  App(
      appid="lan_attack",
      name="LAN Attack",
      apptype=FlipperAppType.MENUEXTERNAL,
      entry_point="lan_attack_app",
      stack_size=8 * 1024,
      icon="A_Plugins_14",
      order=70,
      fap_libs=["assets", "net_hal"],
  )
  ```

- [ ] **Step 2: Register.** In `fam_config.py`, add `"lan_attack",` to `APPS` after `"defender",`.

- [ ] **Step 3: Custom events + scene IDs.** Create `lan_attack_custom_event.h`:
  ```c
  #pragma once
  typedef enum {
      LanAttackEvtRecon = 100,
      LanAttackEvtPoisoner,
      LanAttackEvtNtlmCracker,
      LanAttackEvtAuthorize,
  } LanAttackCustomEvent;
  ```
  Create `scenes/lan_attack_scene_config.h`:
  ```c
  ADD_SCENE(lan_attack, main, Main)
  ADD_SCENE(lan_attack, authorize, Authorize)
  ```
  Create `scenes/lan_attack_scene.h` and `scenes/lan_attack_scene.c` by copying `gpio/scenes/gpio_scene.h` and `gpio_scene.c` verbatim and replacing every `gpio`/`Gpio` token with `lan_attack`/`LanAttack`.

- [ ] **Step 4: App header + internal header.** `lan_attack_app.h`:
  ```c
  #pragma once
  typedef struct LanAttackApp LanAttackApp;
  ```
  `lan_attack_app_i.h`:
  ```c
  #pragma once
  #include "lan_attack_app.h"
  #include "lan_attack_custom_event.h"
  #include "scenes/lan_attack_scene.h"
  #include <gui/gui.h>
  #include <gui/view_dispatcher.h>
  #include <gui/scene_manager.h>
  #include <gui/modules/submenu.h>
  #include <notification/notification_messages.h>

  typedef enum {
      LanAttackViewSubmenu,
  } LanAttackView;

  struct LanAttackApp {
      Gui* gui;
      NotificationApp* notifications;
      ViewDispatcher* view_dispatcher;
      SceneManager* scene_manager;
      Submenu* submenu;
      bool authorized;  // authorization flag (session-scoped, reset to false on exit)
  };
  ```

- [ ] **Step 5: App alloc/free/entry.** `lan_attack_app.c`:
  ```c
  #include "lan_attack_app_i.h"

  static LanAttackApp* lan_attack_app_alloc(void) {
      LanAttackApp* app = malloc(sizeof(LanAttackApp));
      app->gui = furi_record_open(RECORD_GUI);
      app->notifications = furi_record_open(RECORD_NOTIFICATION);
      app->view_dispatcher = view_dispatcher_alloc();
      app->scene_manager = scene_manager_alloc(&lan_attack_scene_handlers, app);
      app->submenu = submenu_alloc();
      app->authorized = false;
      view_dispatcher_enable_queue(app->view_dispatcher);
      view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
      view_dispatcher_add_view(app->view_dispatcher, LanAttackViewSubmenu, submenu_get_view(app->submenu));
      scene_manager_next_scene(app->scene_manager, LanAttackSceneMain);
      return app;
  }

  static void lan_attack_app_free(LanAttackApp* app) {
      scene_manager_free(app->scene_manager);
      view_dispatcher_remove_view(app->view_dispatcher, LanAttackViewSubmenu);
      submenu_free(app->submenu);
      view_dispatcher_free(app->view_dispatcher);
      furi_record_close(RECORD_NOTIFICATION);
      furi_record_close(RECORD_GUI);
      free(app);
  }

  int32_t lan_attack_app(void* p) {
      UNUSED(p);
      LanAttackApp* app = lan_attack_app_alloc();
      view_dispatcher_run(app->view_dispatcher);
      lan_attack_app_free(app);
      return 0;
  }
  ```

- [ ] **Step 6: Main scene.** `scenes/lan_attack_scene_main.c`:
  - `on_enter`: `submenu_reset`, set header "LAN Attack", add items:
    - "Recon" → `LanAttackEvtRecon`
    - "LLMNR/NBT Poison" → `LanAttackEvtPoisoner` (hidden if not authorized)
    - "NTLMv2 Crack" → `LanAttackEvtNtlmCracker`
    - "Authorize" → `LanAttackEvtAuthorize`
  - `view_dispatcher_switch_to_view(app->view_dispatcher, LanAttackViewSubmenu)`
  - `on_event`: for T3, consume custom events (no scene pushes yet; later tasks add branches).
  - `on_exit`: `submenu_reset`

- [ ] **Step 7: Authorize scene.** `scenes/lan_attack_scene_authorize.c` (stub; full implementation in Task 7):
  - For now: just a text display "Authorize active attacks?" with OK button.
  - On OK, set `app->authorized = true`, pop scene.

- [ ] **Step 8: Build** → `build_t3.log`. Expect BUILD OK, "LAN Attack" appears in Tools menu (order 70).

- [ ] **Step 9: Commit** `feat(lan_attack): app skeleton + registration + authorize scene stub`.

---

## Task 4 — LAN Recon scene (ARP sweep, TCP port scan, banner grab)

**Files:**
- Create `applications/main/lan_attack/lan_attack_recon.h` (pure scan task starter)
- Create `applications/main/lan_attack/lan_attack_recon.c` (recon FreeRTOS task, calls net_hal sockets)
- Create `applications/main/lan_attack/scenes/lan_attack_scene_recon.c` (UI scene, renders live scan status)
- Create `applications/main/lan_attack/views/lan_attack_view_scan_status.h`/`.c` (canvas view: scan progress, host count, ports found)

**Interfaces produced:**
```c
// lan_attack_recon.h
typedef struct LanAttackReconState LanAttackReconState;

// Task-safe starter; returns opaque state; caller must join/free.
LanAttackReconState* lan_attack_recon_start(const char* target_subnet, uint32_t gw_ip);

// Poll for result (non-blocking). Returns true if scan is done (state->done=true).
bool lan_attack_recon_is_done(LanAttackReconState* state);

// Fetch live results snapshot (host count, open ports, current progress).
// Returns a struct with {host_count, total_checked, results_csv_path, ...}
typedef struct {
    uint16_t hosts_alive;
    uint16_t ports_scanned;
    uint16_t ports_found;
    uint8_t scan_progress_pct;
    bool done;
} LanAttackReconStatus;
LanAttackReconStatus lan_attack_recon_status(LanAttackReconState* state);

// Join and free.
void lan_attack_recon_join(LanAttackReconState* state);
void lan_attack_recon_free(LanAttackReconState* state);
```

- [ ] **Step 1: Implement `lan_attack_recon.c`** — spawns a FreeRTOS task that:
  - Runs ARP sweep on the local subnet (e.g., `192.168.1.0/24`) by calling `net_hal_udp_bind(68, true)` + loop over target IPs, building ARP requests, sending, listening for replies.
  - For each alive host, spawn TCP port scan (loop over ports 22, 80, 443, 445, 3306, 5432, 8080, etc.; attempt connect with 500 ms timeout).
  - For each open port, send banner probe (SSH or HTTP per port) and read first 256 bytes.
  - Write CSV to `/ext/lan_attack/captures/nmap_<timestamp>.csv` (IP,MAC,Vendor,Ports,Service,Banner snippet).
  - Store scan status in state struct (atomic counters for live view updates).
  - Use `net_hal_socket_*` functions.

- [ ] **Step 2: Implement `lan_attack_scene_recon.c`** — manages the recon scan UI flow:
  - `on_enter`: prompt user for target subnet (or auto-detect from connected network), call `lan_attack_recon_start()`.
  - Show live scan status view (progress %, hosts found, ports detected).
  - Poll `lan_attack_recon_is_done()` in a timer or view-update callback.
  - On done, display "Scan complete, X hosts alive, Y ports open" + CSV path + back button.

- [ ] **Step 3: Implement scan status view** (`lan_attack_view_scan_status.c`) — canvas-based:
  - Draw progress bar (0–100%).
  - Text: "Hosts: 12/254", "Ports: 42", "Elapsed: 45s".
  - Buttons: "Cancel" (stops scan + cleanup).

- [ ] **Step 4: Wire into main scene.** In `lan_attack_scene_main.c` `on_event`, add `case LanAttackEvtRecon: scene_manager_next_scene(app->scene_manager, LanAttackSceneRecon);`.

- [ ] **Step 5: Update `lan_attack_scene_config.h`** to add `ADD_SCENE(lan_attack, recon, Recon)`.

- [ ] **Step 6: Build** → `build_t4.log`. Expect BUILD OK.

- [ ] **Step 7: Commit** `feat(lan_attack): LAN recon scene (ARP sweep + TCP port scan + banner grab)`.

---

## Task 5 — LLMNR/NBT-NS/mDNS Poisoner + NTLMv2 Capture scene

**Files:**
- Create `applications/main/lan_attack/lan_attack_responder.h` (poisoner task starter)
- Create `applications/main/lan_attack/lan_attack_responder.c` (LLMNR/NBT-NS listener + auth listener spawner, captures NTLMv2)
- Create `applications/main/lan_attack/lan_attack_auth_listener.c` (minimal SMB/HTTP auth responder, sends NTLM challenge, captures Type-3)
- Create `applications/main/lan_attack/scenes/lan_attack_scene_poisoner.c` (UI: input target hostname, run poisoner, display captured hashes)

**Interfaces produced:**
```c
// lan_attack_responder.h
typedef struct LanAttackResponderState LanAttackResponderState;

// Start LLMNR/NBT-NS/mDNS poisoner listening on UDP 5355/137/5353.
// target_hostname: the name to poison (e.g., "admin"), or "*" for all queries.
// responder_ip: the IP to claim in replies (attacker's SoftAP or DHCP-assigned IP).
LanAttackResponderState* lan_attack_responder_start(const char* target_hostname, uint32_t responder_ip);

// Poll for status (hash capture count, etc.)
typedef struct {
    uint32_t queries_received;
    uint16_t hashes_captured;
    bool running;
} LanAttackResponderStatus;
LanAttackResponderStatus lan_attack_responder_status(LanAttackResponderState* state);

// Stop and free.
void lan_attack_responder_stop(LanAttackResponderState* state);
void lan_attack_responder_free(LanAttackResponderState* state);

// Fetch captured hashes (CSV path).
const char* lan_attack_responder_csv_path(LanAttackResponderState* state);
```

- [ ] **Step 1: Implement `lan_attack_responder.c`** — two parallel tasks:
  - **UDP responder task:** bind UDP 5355 (LLMNR), 137 (NBT-NS), 5353 (mDNS) on separate sockets; listen for queries; on query for target hostname, craft unicast reply to querier IP claiming to be the target (spoofed response).
  - **Auth listener task:** bind TCP 445 (SMB) and/or 8080 (HTTP); accept one connection at a time; on SMB connect, send NTLM Negotiate (Type-1); on client response, parse Type-3 and extract username/domain/challenge/response; write NTLMv2 hash to CSV via `net_hal_write_ntlmv2_hashcat()`, close connection, loop.
  - Both tasks coordinate via shared atomics (query count, hash count, stop flag).
  - Use `net_hal_udp_*` and `net_hal_tcp_*` functions.

- [ ] **Step 2: Implement `lan_attack_auth_listener.c`** — minimal NTLM handshake:
  - On TCP accept, send NTLM Type-1 (negotiate) frame with version + flags.
  - Recv Type-3 response.
  - Parse username/domain/challenge/response via pure `ntlm_parse_type3()` (from Task 1).
  - Write hash CSV.

- [ ] **Step 3: Implement `lan_attack_scene_poisoner.c`** — UI:
  - `on_enter`: text-input for target hostname (default "*" for all), start poisoner.
  - Show live view: "Queries: 123", "Hashes: 5", each hash as a summary line.
  - Buttons: "Stop" (join poisoner task + cleanup), "Back".

- [ ] **Step 4: Wire into main scene.** Add `LanAttackEvtPoisoner` branch in `scene_main.c` (only visible if `app->authorized`).

- [ ] **Step 5: Update `lan_attack_scene_config.h`** to add `ADD_SCENE(lan_attack, poisoner, Poisoner)`.

- [ ] **Step 6: Build** → `build_t5.log`. Expect BUILD OK.

- [ ] **Step 7: Commit** `feat(lan_attack): LLMNR/NBT-NS/mDNS poisoner + NTLMv2 capture scene`.

---

## Task 6 — NTLMv2 Cracker scene

**Files:**
- Create `applications/main/lan_attack/lan_attack_cracker.h` (pure wordlist streaming + cracker task)
- Create `applications/main/lan_attack/lan_attack_cracker.c` (FreeRTOS task: load CSV, stream wordlist, HMAC-MD5 dict loop)
- Create `applications/main/lan_attack/scenes/lan_attack_scene_ntlmv2_crack.c` (UI: load hash, pick wordlist, run cracker, display result)
- Create `tests/test_lan_attack_cracker.c` (host test: crack a canned hash with known password)

**Interfaces produced:**
```c
// lan_attack_cracker.h
typedef struct LanAttackCrackerState LanAttackCrackerState;

// Load captured hash (CSV path) and start cracking with wordlist.
LanAttackCrackerState* lan_attack_cracker_start(const char* hash_csv_path, const char* wordlist_path);

// Poll for status (passwords tested/sec, elapsed, cracked?)
typedef struct {
    uint64_t passwords_tested;
    uint32_t elapsed_sec;
    bool done;
    bool cracked;
    char cracked_password[256];  // "" if not cracked or still running
} LanAttackCrackerStatus;
LanAttackCrackerStatus lan_attack_cracker_status(LanAttackCrackerState* state);

void lan_attack_cracker_stop(LanAttackCrackerState* state);
void lan_attack_cracker_free(LanAttackCrackerState* state);
```

- [ ] **Step 1: Write failing test.** `tests/test_lan_attack_cracker.c` (host-compiled, includes only net_hal + stdio/string):
  - Hardcode a NTLMv2 hash (from canned capture with known password, e.g., "MyPassword123").
  - Write a tiny wordlist file to `/tmp/tiny_wordlist.txt` (first line: "WrongPassword", second: "MyPassword123", third: "AnotherWrong").
  - Call cracker on the hash and wordlist (with a test harness that doesn't require FreeRTOS).
  - Expect `cracked=true` and `cracked_password="MyPassword123"`.

- [ ] **Step 2: Run test → FAIL** (cracker logic missing).

- [ ] **Step 3: Implement `lan_attack_cracker.c`** — FreeRTOS task:
  - Load NTLMv2 hash CSV (parse via `ntlm_parse_type3()` or direct field extraction).
  - Open wordlist file (`/ext/lan_attack/wordlists/passwords.txt` or user-specified).
  - Loop: read one line (password), call `ntlmv2_response_compute(password, username, domain, challenge, context)` → response blob.
  - Compare response to captured hash via `memcmp(computed, captured, 16)`.
  - On match: store password in state, set `done=true, cracked=true`.
  - On EOF: set `done=true, cracked=false`.
  - Update `passwords_tested` and `elapsed_sec` atomically for live view.

- [ ] **Step 4: Run host test → PASS**.

- [ ] **Step 5: Implement `lan_attack_scene_ntlmv2_crack.c`** — UI:
  - `on_enter`: submenu to pick from captured hashes CSV (list hashes in `/ext/lan_attack/captures/ntlmv2_*.csv`); text-input for wordlist path (default `/ext/lan_attack/wordlists/passwords.txt`); start cracker.
  - Show live view: "Tested: 12,345 pwd/s", "Elapsed: 45s", "Status: Cracking...".
  - On done: "Password found: MyPassword123" or "Not found in wordlist" + back button.

- [ ] **Step 6: Wire into main scene.** Add `LanAttackEvtNtlmCracker` branch.

- [ ] **Step 7: Update `lan_attack_scene_config.h`** to add `ADD_SCENE(lan_attack, ntlmv2_crack, Ntlmv2Crack)`.

- [ ] **Step 8: Build** → `build_t6.log`. Expect BUILD OK.

- [ ] **Step 9: Commit** `feat(lan_attack): NTLMv2 cracker scene (HMAC-MD5 dictionary)`.

---

## Task 7 — Authorization gate + app menu finalization

**Files:**
- Modify `applications/main/lan_attack/scenes/lan_attack_scene_authorize.c` (full two-step unlock)
- Modify `applications/main/lan_attack/scenes/lan_attack_scene_main.c` (show/hide active-attack items based on `app->authorized`)
- Modify `applications/main/lan_attack/lan_attack_app_i.h` (add authorization token path)

**Interfaces produced:** Full authorization flow with session-scoped unlock + menu gate.

- [ ] **Step 1: Implement full `lan_attack_scene_authorize.c`**:
  - First screen: warning dialog — "LLMNR poisoning will interfere with network name resolution. Continue?"
  - Buttons: "No" (go back), "Yes" (go to confirmation).
  - Second screen: "Active testing only. Authorized by network owner?" with "No"/"Yes" buttons.
  - On "Yes" (both steps): set `app->authorized = true`, optional write `/ext/lan_attack/AUTHORIZED` token, pop scene back to main.
  - On any "No": revert, go back to main.

- [ ] **Step 2: Update `lan_attack_scene_main.c`** to filter menu items:
  - Always show: "Recon", "NTLMv2 Crack", "Authorize".
  - Show only if `app->authorized`: "LLMNR/NBT Poison".
  - Add hint text: footer line "Authorized testing only" if authorized, else "Unlock advanced features".

- [ ] **Step 3: Update `lan_attack_app.c`** — in `_free`, set `app->authorized = false` (session-scoped reset).

- [ ] **Step 4: Build** → `build_t7.log`. Expect BUILD OK.

- [ ] **Step 5: Commit** `feat(lan_attack): authorization gate (two-step unlock, session-scoped)`.

---

## Task 8 — Build + end-to-end integration

**Files:** (no new files, just integration)

**Goal:** All host tests pass, firmware builds green, basic on-device validation.

- [ ] **Step 1: Host-test all pure cores** (re-run from Task 1-6):
  ```bash
  wsl.exe -- bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15 && \
    gcc -Wall -Wextra -o /tmp/t1 tests/test_net_hal_dhcp.c lib/net_hal/net_hal_dhcp.c && /tmp/t1 && \
    gcc -Wall -Wextra -o /tmp/t2 tests/test_net_hal_ntlm.c lib/net_hal/net_hal_ntlm.c && /tmp/t2 && \
    gcc -Wall -Wextra -o /tmp/t3 tests/test_net_hal_wpa.c lib/net_hal/net_hal_wpa.c && /tmp/t3 && \
    gcc -Wall -Wextra -o /tmp/t4 tests/test_lan_attack_cracker.c lib/net_hal/net_hal_ntlm.c && /tmp/t4 && \
    echo ALL HOST TESTS PASSED"
  ```

- [ ] **Step 2: Firmware full build**:
  ```bash
  MSYS_NO_PATHCONV=1 cmd.exe /c "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_ci.bat" > build_t8.log 2>&1; echo "EXIT=$?"
  ```
  Verify `=== BUILD OK ===` in log + EXIT=0.

- [ ] **Step 3: Verify app registration** — check build log for "LAN Attack" app registered (order 70).

- [ ] **Step 4: Optional on-device validation (if device is flashed):**
  - Flash firmware to Cardputer-ADV.
  - Launch "LAN Attack" from Tools menu.
  - Main screen shows: Recon, NTLMv2 Crack, Authorize (no LLMNR item yet).
  - Enter "Authorize" → see two-step dialog, cancel → revert.
  - Re-enter "Authorize" → complete both steps → authorized flag set.
  - Main screen now shows "LLMNR/NBT Poison" item.
  - "Recon" → prompt for subnet → start scan (should show progress or error if no network connected).
  - Exit app → restart → LLMNR item hidden again (session-scoped reset).

- [ ] **Step 5: Commit** `test(lan_attack): Phase 1 build + integration (all tests pass, firmware green)`.

---

## Phase 2 Outline (Deferred — Titles Only)

The following features are Phase-2 scope and will NOT be detailed here:

1. **Rogue DHCP Server + Starvation** — listen UDP 67, issue arbitrary IPs + gateway override.
2. **WPAD/PAC Proxy Autoconfiguration** — pair with evil-portal SoftAP, serve PAC template, capture proxy traffic.
3. **WPA/WPA2 Cracker Scene** — load handshake (from wifi_attack app .pcap or CSV), stream wordlist, dictionary-crack via PBKDF2-SHA1 (pure core already in Task 1).
4. **Telnet Honeypot** — listen TCP 23, capture plaintext credentials, log to SD.
5. **Dual-core Cracker Speedup** — spawn cracker tasks on Core 1 if Core 0 is busy.
6. **mDNS/SSDP/UPnP Enum** — active device discovery (printer detection, service scanning).
7. **TCP Tunnel/Redirect** — accept connections, relay to another host/port (MITM enabler).

---

## Self-Review Checklist

- [ ] All 8 tasks have concrete file paths and real code signatures (no placeholders).
- [ ] Pure cores (DHCP, NTLM, WPA, ARP, cracker) are host-testable (no esp/furi includes; test with WSL gcc).
- [ ] Each scene explicitly cleans up sockets and tasks on exit (join before free).
- [ ] Authorization gate is session-scoped (resets to `false` on app exit).
- [ ] Menu items are filtered by `app->authorized` flag.
- [ ] CSV writes are async (SD I/O in task or at scene exit, never blocking main UI thread).
- [ ] No-PSRAM discipline: recon results spilled to SD if exceeding small RAM table; wordlist streamed line-by-line; no full-frame buffering.
- [ ] Build is green at every task (incremental check).

---

## Riskiest Task

**Task 5 (LLMNR/NBT-NS/mDNS Poisoner + NTLMv2 Capture)** — highest feasibility risk:

1. **UDP responder jitter:** Crafting raw LLMNR/NBT-NS replies with correct byte layout + unicast to querier IP requires careful frame construction. Host-testable parser in Task 1 mitigates, but on-air response latency under load may exceed query timeout (~100–200 ms).
2. **NTLM Type-3 parser:** Binary frame format (field offsets, length-delimited buffers) is error-prone. Bounds-checking critical.
3. **Minimal SMB server:** Sending NTLM Type-1 negotiate frame triggers Windows clients' NTLM handshake, but frame format must be exact (signature "NTLMSSP\0", flags, domain/version fields). Reference: RFC 8446 or Wireshark dissector.

**Mitigation:**
- Task 1 hosts the pure NTLM parser tests (frame construction + parsing on canned data).
- Responder tasks run in separate FreeRTOS context (latency jitter won't block UI).
- If real handshakes fail, fallback to passive sniffer (capture LLMNR/NBT traffic without active reply) as v1.1.

---

## Open Items to Confirm During Implementation

1. **lwIP DHCP server:** Confirm whether `dhcp_server_start()` API is available, or if Task 2 (Rogue DHCP) must build DHCP from scratch via raw socket.
2. **Wordlist bundling:** Decide whether to pre-ship a small wordlist (e.g., rockyou-top-1k.txt) in `/ext/lan_attack/wordlists/` or require user to load via SD.
3. **NTLM frame format validation:** Compare Task 1's Type-3 parser against real Windows capture (Wireshark) to confirm offset/length assumptions.
4. **TCP connect timeout:** Verify lwIP socket's `SO_RCVTIMEO` is respected on non-blocking connect attempts; if not, add manual polling with timer.
5. **CSV path creation:** Confirm `storage_common_mkdir()` creates parent dirs recursively or if we need to call it on `/ext`, `/ext/lan_attack`, `/ext/lan_attack/captures` separately.
6. **Icon rendering:** Confirm `A_Plugins_14` built-in icon is available; if not, substitute with another A_* icon or create minimal checked-in `.c` icon array (no Pillow).

---

## Estimated Effort per Task

- Task 1: 8–10 hours (pure C coding + test harness).
- Task 2: 4–6 hours (lwIP wrappers + SD I/O).
- Task 3: 3–4 hours (app skeleton, reuse GPIO pattern).
- Task 4: 6–8 hours (recon task, multi-socket I/O, CSV export).
- Task 5: 8–12 hours (UDP responder + NTLM handshake, hardest).
- Task 6: 4–6 hours (wordlist streaming + cracker loop).
- Task 7: 2–3 hours (UI flows, menu gating).
- Task 8: 2–3 hours (integration + testing).

**Total Phase 1: 37–52 hours solo, or 20–25 hours with parallel workers** (Tasks 1–6 can fan out; 7–8 are sequential/blocking).

