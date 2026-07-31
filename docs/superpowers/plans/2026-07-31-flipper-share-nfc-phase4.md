# Flipper Share NFC — Phase 4 (standalone app) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship the hardware-proven Flipper Share NFC transfer as a standalone `nfc_share` app: pick any `/ext` file to send, save received files to SD, stream arbitrary sizes (no RAM cap), on both firmware trees, listed in applab.

**Architecture:** Split the proven inline logic (`bus_tools/scenes/bus_tools_scene_st25r_nfc.c`) into a **pure protocol codec** (`nfc_share_proto`, host-testable) and an **engine** (`nfc_share_engine`) that runs the two ST25R3916 worker loops backed by SD file I/O instead of hardcoded RAM buffers. A standalone app (scene manager + submenu + file browser + progress view) drives the engine. The `bus_tools` test scene is left intact.

**Tech Stack:** C (ESP32-S3, esp-idf v5.4.1), this repo's Flipper-port furi APIs (`gui/scene_manager`, `gui/view_dispatcher`, `gui/modules/submenu`, `dialogs`, `storage`), `mbedtls/md5`, the in-tree `furi_hal/st25r3916/st25r3916_nfca` driver. Host unit tests: plain `gcc`.

## Global Constraints

- **Two trees, identical sources:** every `applications/main/nfc_share/` file is created in `NEW_v2/src_m5stick_s3` first, then byte-mirrored into `NEW_v2/src_cardputer_adv` (Task 6). No per-board `#ifdef` in the app.
- **NFC hardware:** M5 Unit NFC (ST25R3916) over Grove I2C on **both** ends. No driver changes.
- **Wire format is frozen** — byte-exact vs. lomalkin's Flipper Share (already interop-verified). Constants below are copied verbatim from the proven impl; do not "improve" them.
- **Build ONLY via the PowerShell tool**, not cmd-through-bash. Incremental: `build_m5stick_s3.bat` (full path), watch `build_m5stick_s3.log` for `SENTINEL_OK`. `IDF_TOOLS_PATH=C:\Users\stone\.espressif`.
- **Flash:** M5Stick S3 = **COM10** (USB-Serial-JTAG; COM7 is the app CDC and write-timeouts). `python -m esptool --chip esp32s3 --port COM10 --baud 921600 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m 0x0 Flipper-m5stick_s3-merged.bin`.
- **FuriThread stacks:** worker threads use `furi_thread_alloc_ex(name, 4096, fn, ctx)` — the ~74 ms FWT window is far tighter than any GUI tick.
- **Frozen protocol constants (verbatim):**
  - `NSH_VERSION=2`; types `ANNOUNCE=1, REQUEST=2, DATA=3`.
  - Transport headers `NFC_TP_HDR_POLL=0x00` (keep-alive), `NFC_TP_HDR_PKT=0x01` (packet). Frame = `[tp_hdr][nsh packet]`.
  - nsh packet = `[version(1)][tx_id(1)][type(1)][payload][crc16(2 LE)]`; crc = CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`, no reflection/xorout) over `[version..payload]`.
  - `NSH_HEADER_LENGTH=3`, `NSH_CRC_LENGTH=2`, `NSH_FILENAME_LENGTH=36`, `NSH_DATA_LENGTH=240`.
  - Control payload (ANNOUNCE **and** REQUEST) = `NSH_CTRL_PL_LEN=56` (parse packet length by TYPE, never by frame length). ANNOUNCE payload = `name[36] + size(u32 LE) + md5[16]`. REQUEST payload = `range_start(u32 LE) + range_end(u32 LE)` in **bytes**, remaining 48 bytes zero-pad but covered by crc.
  - DATA payload = `NSH_DATA_PL_LEN=244` = `block_number(u32 LE) + data[240]`; byte offset = `block*240`.
  - Card identity: UID `F5 0F 5A AE 00 00 01` (7B), ATQA `0x0044`, SAK `0x00`.
  - RX frames arrive **with the trailing 2-byte transport CRC-A still attached** — strip before nsh parse. `listen_tx` TXE wait = **50 ms** (needed for the 250 B DATA frame; already in the driver).
  - Receiver pacing: send keep-alive polls most exchanges; re-send a REQUEST only every ~6th loop (sender streams DATA on polls, re-arms range on REQUEST).
- **Received-file dir:** `/ext/nfc_share/` (create if absent). Sanitize announced name to a leaf; if it exists, append `_1`, `_2`, … before the extension. Never overwrite.
- **Defensive size bound:** reject announced `file_size == 0` or `> 16*1024*1024`.

---

### Task 1: Pure protocol codec `nfc_share_proto` (+ host unit test)

The freestanding byte-level codec: no furi/ESP/storage includes, only `<stdint.h>`/`<string.h>`. This is where crc16 and every frame builder/parser live, so they can be unit-tested on the host. Engine and scenes call into this.

**Files:**
- Create: `applications/main/nfc_share/nfc_share_proto.h`
- Create: `applications/main/nfc_share/nfc_share_proto.c`
- Test: `applications/main/nfc_share/test/nfc_share_proto_test.c`

**Interfaces:**
- Produces (used by Tasks 3–4):
  - `uint16_t nfc_share_crc16(const uint8_t* d, size_t n);`
  - `size_t nfc_share_build_announce(uint8_t* out, uint8_t tx_id, const char* name, uint32_t size, const uint8_t md5[16]);` → returns frame length (62).
  - `size_t nfc_share_build_request(uint8_t* out, uint8_t tx_id, uint32_t range_start, uint32_t range_end);` → 62.
  - `size_t nfc_share_build_data(uint8_t* out, uint8_t tx_id, uint32_t block, const uint8_t* data, uint32_t data_len);` → 250 (zero-pads `data` to 240).
  - Parsers over a received frame `buf/n` (n **includes** tp_hdr + trailing 2-byte CRC-A):
    - `bool nfc_share_parse_announce(const uint8_t* buf, uint8_t n, uint8_t* tx_id, char name[37], uint32_t* size, uint8_t md5[16]);`
    - `bool nfc_share_parse_request(const uint8_t* buf, uint8_t n, uint8_t exp_tx_id, uint32_t* range_start, uint32_t* range_end);`
    - `bool nfc_share_parse_data(const uint8_t* buf, uint8_t n, uint8_t exp_tx_id, uint32_t* block, const uint8_t** data, uint32_t* data_len);` → `*data` points into `buf`, `*data_len`=240.
  - `void nfc_share_dedup_name(const char* base_leaf, unsigned suffix, char* out, size_t out_sz);` → suffix 0 copies `base_leaf`; suffix k inserts `_k` before the last `.`.
  - Constants (macros) from Global Constraints.

- [ ] **Step 1: Write the failing host test**

Create `applications/main/nfc_share/test/nfc_share_proto_test.c`:
```c
#include "../nfc_share_proto.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    /* 1. CRC-16/CCITT-FALSE known vector: "123456789" -> 0x29B1 */
    assert(nfc_share_crc16((const uint8_t*)"123456789", 9) == 0x29B1);

    /* 2. ANNOUNCE build -> parse round trip */
    uint8_t md5[16];
    for(int i = 0; i < 16; i++) md5[i] = (uint8_t)(i * 7 + 1);
    uint8_t f[256];
    size_t fl = nfc_share_build_announce(f, 0x5A, "hello.txt", 1234, md5);
    assert(fl == 62);
    /* emulate the RX side: transport CRC-A adds 2 bytes to the frame */
    uint8_t rx[256];
    memcpy(rx, f, fl);
    rx[fl] = 0xAB; rx[fl + 1] = 0xCD;
    uint8_t txid = 0; char name[37]; uint32_t size = 0; uint8_t gmd5[16];
    assert(nfc_share_parse_announce(rx, (uint8_t)(fl + 2), &txid, name, &size, gmd5));
    assert(txid == 0x5A && size == 1234 && strcmp(name, "hello.txt") == 0);
    assert(memcmp(gmd5, md5, 16) == 0);

    /* 3. REQUEST build -> parse (56-byte control payload, length keyed by type) */
    size_t rl = nfc_share_build_request(f, 0x5A, 240, 1234);
    assert(rl == 62);
    memcpy(rx, f, rl); rx[rl] = 0x11; rx[rl + 1] = 0x22;
    uint32_t rs = 0, re = 0;
    assert(nfc_share_parse_request(rx, (uint8_t)(rl + 2), 0x5A, &rs, &re));
    assert(rs == 240 && re == 1234);
    assert(!nfc_share_parse_request(rx, (uint8_t)(rl + 2), 0x99, &rs, &re)); /* wrong txid */

    /* 4. DATA build -> parse */
    uint8_t payload[240];
    for(int i = 0; i < 240; i++) payload[i] = (uint8_t)(255 - i);
    size_t dl = nfc_share_build_data(f, 0x5A, 3, payload, 240);
    assert(dl == 250);
    memcpy(rx, f, dl); rx[dl] = 0; rx[dl + 1] = 0;
    uint32_t blk = 0; const uint8_t* dp = NULL; uint32_t dn = 0;
    assert(nfc_share_parse_data(rx, (uint8_t)(dl + 2), 0x5A, &blk, &dp, &dn));
    assert(blk == 3 && dn == 240 && memcmp(dp, payload, 240) == 0);

    /* 5. DATA short tail is zero-padded to 240 */
    dl = nfc_share_build_data(f, 0x5A, 0, payload, 5);
    assert(dl == 250);
    assert(f[1 + 3 + 4 + 5] == 0 && f[1 + 3 + 4 + 239] == 0);

    /* 6. dedup name */
    char out[64];
    nfc_share_dedup_name("m5share.txt", 0, out, sizeof(out));
    assert(strcmp(out, "m5share.txt") == 0);
    nfc_share_dedup_name("m5share.txt", 2, out, sizeof(out));
    assert(strcmp(out, "m5share_2.txt") == 0);
    nfc_share_dedup_name("noext", 1, out, sizeof(out));
    assert(strcmp(out, "noext_1") == 0);

    printf("nfc_share_proto: ALL PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails to compile (no impl yet)**

Run (Bash tool):
```bash
cd "C:/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3/applications/main/nfc_share"
gcc -std=c11 -Wall -o /tmp/nsh_test test/nfc_share_proto_test.c nfc_share_proto.c 2>&1 | head
```
Expected: FAIL — `nfc_share_proto.h`/`.c` do not exist yet.

- [ ] **Step 3: Write `nfc_share_proto.h`**

```c
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define NSH_VERSION         2u
#define NSH_PKT_ANNOUNCE    1u
#define NSH_PKT_REQUEST     2u
#define NSH_PKT_DATA        3u
#define NFC_TP_HDR_POLL     0x00u
#define NFC_TP_HDR_PKT      0x01u
#define NSH_HEADER_LENGTH   3u
#define NSH_CRC_LENGTH      2u
#define NSH_FILENAME_LENGTH 36u
#define NSH_DATA_LENGTH     240u
#define NSH_CTRL_PL_LEN     56u                    /* name+size+md5 = 36+4+16 */
#define NSH_DATA_PL_LEN     (4u + NSH_DATA_LENGTH) /* 244 */
#define NSH_ANNOUNCE_FRAME_LEN (1u + NSH_HEADER_LENGTH + NSH_CTRL_PL_LEN + NSH_CRC_LENGTH) /* 62 */
#define NSH_REQUEST_FRAME_LEN  NSH_ANNOUNCE_FRAME_LEN                                        /* 62 */
#define NSH_DATA_FRAME_LEN     (1u + NSH_HEADER_LENGTH + NSH_DATA_PL_LEN + NSH_CRC_LENGTH)   /* 250 */

uint16_t nfc_share_crc16(const uint8_t* d, size_t n);
size_t nfc_share_build_announce(
    uint8_t* out, uint8_t tx_id, const char* name, uint32_t size, const uint8_t md5[16]);
size_t nfc_share_build_request(uint8_t* out, uint8_t tx_id, uint32_t range_start, uint32_t range_end);
size_t nfc_share_build_data(
    uint8_t* out, uint8_t tx_id, uint32_t block, const uint8_t* data, uint32_t data_len);
bool nfc_share_parse_announce(
    const uint8_t* buf, uint8_t n, uint8_t* tx_id, char name[37], uint32_t* size, uint8_t md5[16]);
bool nfc_share_parse_request(
    const uint8_t* buf, uint8_t n, uint8_t exp_tx_id, uint32_t* range_start, uint32_t* range_end);
bool nfc_share_parse_data(
    const uint8_t* buf, uint8_t n, uint8_t exp_tx_id, uint32_t* block,
    const uint8_t** data, uint32_t* data_len);
void nfc_share_dedup_name(const char* base_leaf, unsigned suffix, char* out, size_t out_sz);
```

- [ ] **Step 4: Write `nfc_share_proto.c`**

```c
#include "nfc_share_proto.h"
#include <string.h>
#include <stdio.h>

uint16_t nfc_share_crc16(const uint8_t* d, size_t n) {
    uint16_t crc = 0xFFFF;
    for(size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)((uint16_t)d[i] << 8);
        for(int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* append LE crc16 over p[0..body_len) at p[body_len]; returns 1+body_len+2 frame len */
static size_t finish_packet(uint8_t* out, size_t body_len) {
    uint8_t* p = out + 1; /* out[0] = tp_hdr already set */
    uint16_t crc = nfc_share_crc16(p, body_len);
    p[body_len] = (uint8_t)(crc & 0xFF);
    p[body_len + 1] = (uint8_t)(crc >> 8);
    return 1u + body_len + NSH_CRC_LENGTH;
}

size_t nfc_share_build_announce(
    uint8_t* out, uint8_t tx_id, const char* name, uint32_t size, const uint8_t md5[16]) {
    out[0] = NFC_TP_HDR_PKT;
    uint8_t* p = out + 1;
    p[0] = NSH_VERSION; p[1] = tx_id; p[2] = NSH_PKT_ANNOUNCE;
    uint8_t* pl = p + NSH_HEADER_LENGTH;
    memset(pl, 0, NSH_CTRL_PL_LEN);
    size_t nlen = strlen(name);
    if(nlen > NSH_FILENAME_LENGTH - 1) nlen = NSH_FILENAME_LENGTH - 1;
    memcpy(pl, name, nlen);
    memcpy(pl + NSH_FILENAME_LENGTH, &size, 4);
    memcpy(pl + NSH_FILENAME_LENGTH + 4, md5, 16);
    return finish_packet(out, NSH_HEADER_LENGTH + NSH_CTRL_PL_LEN);
}

size_t nfc_share_build_request(uint8_t* out, uint8_t tx_id, uint32_t rs, uint32_t re) {
    out[0] = NFC_TP_HDR_PKT;
    uint8_t* p = out + 1;
    p[0] = NSH_VERSION; p[1] = tx_id; p[2] = NSH_PKT_REQUEST;
    uint8_t* pl = p + NSH_HEADER_LENGTH;
    memset(pl, 0, NSH_CTRL_PL_LEN);
    memcpy(pl, &rs, 4);
    memcpy(pl + 4, &re, 4);
    return finish_packet(out, NSH_HEADER_LENGTH + NSH_CTRL_PL_LEN);
}

size_t nfc_share_build_data(
    uint8_t* out, uint8_t tx_id, uint32_t block, const uint8_t* data, uint32_t data_len) {
    out[0] = NFC_TP_HDR_PKT;
    uint8_t* p = out + 1;
    p[0] = NSH_VERSION; p[1] = tx_id; p[2] = NSH_PKT_DATA;
    uint8_t* pl = p + NSH_HEADER_LENGTH;
    memcpy(pl, &block, 4);
    if(data_len > NSH_DATA_LENGTH) data_len = NSH_DATA_LENGTH;
    memcpy(pl + 4, data, data_len);
    if(data_len < NSH_DATA_LENGTH) memset(pl + 4 + data_len, 0, NSH_DATA_LENGTH - data_len);
    return finish_packet(out, NSH_HEADER_LENGTH + NSH_DATA_PL_LEN);
}

/* Shared parse preamble: verify tp_hdr, version, type, and crc over body_len.
 * buf/n include tp_hdr + trailing transport CRC-A (which we ignore). Returns the
 * nsh packet pointer or NULL. */
static const uint8_t* parse_common(const uint8_t* buf, uint8_t n, uint8_t type, size_t body_len) {
    if(!buf || buf[0] != NFC_TP_HDR_PKT) return NULL;
    if(n < 1u + body_len + NSH_CRC_LENGTH) return NULL;
    const uint8_t* nsh = buf + 1;
    if(nsh[0] != NSH_VERSION || nsh[2] != type) return NULL;
    uint16_t got = (uint16_t)(nsh[body_len] | ((uint16_t)nsh[body_len + 1] << 8));
    if(nfc_share_crc16(nsh, body_len) != got) return NULL;
    return nsh;
}

bool nfc_share_parse_announce(
    const uint8_t* buf, uint8_t n, uint8_t* tx_id, char name[37], uint32_t* size, uint8_t md5[16]) {
    const uint8_t* nsh = parse_common(buf, n, NSH_PKT_ANNOUNCE, NSH_HEADER_LENGTH + NSH_CTRL_PL_LEN);
    if(!nsh) return false;
    const uint8_t* pl = nsh + NSH_HEADER_LENGTH;
    if(tx_id) *tx_id = nsh[1];
    memset(name, 0, 37);
    memcpy(name, pl, NSH_FILENAME_LENGTH); /* NUL-padded in wire form */
    name[NSH_FILENAME_LENGTH] = '\0';
    memcpy(size, pl + NSH_FILENAME_LENGTH, 4);
    memcpy(md5, pl + NSH_FILENAME_LENGTH + 4, 16);
    return true;
}

bool nfc_share_parse_request(
    const uint8_t* buf, uint8_t n, uint8_t exp_tx_id, uint32_t* rs, uint32_t* re) {
    const uint8_t* nsh = parse_common(buf, n, NSH_PKT_REQUEST, NSH_HEADER_LENGTH + NSH_CTRL_PL_LEN);
    if(!nsh || nsh[1] != exp_tx_id) return false;
    memcpy(rs, nsh + NSH_HEADER_LENGTH, 4);
    memcpy(re, nsh + NSH_HEADER_LENGTH + 4, 4);
    return true;
}

bool nfc_share_parse_data(
    const uint8_t* buf, uint8_t n, uint8_t exp_tx_id, uint32_t* block,
    const uint8_t** data, uint32_t* data_len) {
    const uint8_t* nsh = parse_common(buf, n, NSH_PKT_DATA, NSH_HEADER_LENGTH + NSH_DATA_PL_LEN);
    if(!nsh || nsh[1] != exp_tx_id) return false;
    memcpy(block, nsh + NSH_HEADER_LENGTH, 4);
    *data = nsh + NSH_HEADER_LENGTH + 4;
    *data_len = NSH_DATA_LENGTH;
    return true;
}

void nfc_share_dedup_name(const char* base_leaf, unsigned suffix, char* out, size_t out_sz) {
    if(suffix == 0) {
        snprintf(out, out_sz, "%s", base_leaf);
        return;
    }
    const char* dot = strrchr(base_leaf, '.');
    if(dot && dot != base_leaf) {
        int stem = (int)(dot - base_leaf);
        snprintf(out, out_sz, "%.*s_%u%s", stem, base_leaf, suffix, dot);
    } else {
        snprintf(out, out_sz, "%s_%u", base_leaf, suffix);
    }
}
```

- [ ] **Step 5: Run the host test — verify PASS**

Run (Bash tool):
```bash
cd "C:/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3/applications/main/nfc_share"
gcc -std=c11 -Wall -Werror -o /tmp/nsh_test test/nfc_share_proto_test.c nfc_share_proto.c && /tmp/nsh_test
```
Expected: prints `nfc_share_proto: ALL PASS`, exit 0.

- [ ] **Step 6: Commit**

```bash
cd "C:/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3"
git add applications/main/nfc_share/nfc_share_proto.h applications/main/nfc_share/nfc_share_proto.c applications/main/nfc_share/test/nfc_share_proto_test.c
git commit -m "feat(nfc_share): pure protocol codec + host tests"
```

---

### Task 2: Standalone app scaffold (submenu → file browser → transfer, stub engine)

Produces a launchable app whose menu, file browser, and progress view work, driving an engine stub. This locks the app skeleton before the radio logic lands.

**Files:**
- Create: `applications/main/nfc_share/application.fam`
- Create: `applications/main/nfc_share/nfc_share.h`
- Create: `applications/main/nfc_share/nfc_share_i.h`
- Create: `applications/main/nfc_share/nfc_share.c`
- Create: `applications/main/nfc_share/nfc_share_engine.h` (interface only + stub `.c`)
- Create: `applications/main/nfc_share/nfc_share_engine.c` (stub in this task, real in Tasks 3–4)
- Create: `applications/main/nfc_share/views/nfc_share_progress_view.h`
- Create: `applications/main/nfc_share/views/nfc_share_progress_view.c`
- Create: `applications/main/nfc_share/scenes/nfc_share_scene.h`
- Create: `applications/main/nfc_share/scenes/nfc_share_scene.c`
- Create: `applications/main/nfc_share/scenes/nfc_share_scene_config.h`
- Create: `applications/main/nfc_share/scenes/nfc_share_scene_start.c`
- Create: `applications/main/nfc_share/scenes/nfc_share_scene_file_select.c`
- Create: `applications/main/nfc_share/scenes/nfc_share_scene_transfer.c`

**Interfaces:**
- Consumes: `nfc_share_proto.h` (Task 1).
- Produces (used by Tasks 3–4): the engine interface exactly as in the spec:
```c
// nfc_share_engine.h
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum { NfcShareRoleSend, NfcShareRoleReceive } NfcShareRole;
typedef enum {
    NfcShareStateInit, NfcShareStateWaiting, NfcShareStateRunning,
    NfcShareStateDoneOk, NfcShareStateFail
} NfcShareState;
typedef struct {
    NfcShareState state;
    char     name[40];
    uint32_t file_size;
    uint32_t bytes_done;
    uint32_t blocks_total;
    uint32_t blocks_done;
    uint32_t kbps_x10;
    uint8_t  err;               // 0 none, 1 io, 2 md5, 3 radio, 4 toobig
} NfcShareStatus;

typedef struct NfcShareEngine NfcShareEngine;
NfcShareEngine* nfc_share_engine_alloc(NfcShareRole role, const char* path);
void nfc_share_engine_start(NfcShareEngine* e);
void nfc_share_engine_stop(NfcShareEngine* e);
void nfc_share_engine_status(NfcShareEngine* e, NfcShareStatus* out);
void nfc_share_engine_free(NfcShareEngine* e);
```
  - App struct + view enum from `nfc_share_i.h` (below), scene ids from the X-macro.

- [ ] **Step 1: Write `application.fam`**

```python
App(
    appid="nfc_share",
    name="NFC Share",
    apptype=FlipperAppType.MENUEXTERNAL,
    entry_point="nfc_share_app",
    stack_size=4 * 1024,
    icon="A_NFC_14",
    order=54,
    fap_libs=["assets"],
)
```
(If `A_NFC_14` is not a known asset id in this tree, use `A_GPIO_14` as bus_tools does — verify against `applications/main/*/application.fam` icons before building.)

- [ ] **Step 2: Write the scene X-macro trio**

`scenes/nfc_share_scene_config.h`:
```c
ADD_SCENE(nfc_share, start, Start)
ADD_SCENE(nfc_share, file_select, FileSelect)
ADD_SCENE(nfc_share, transfer, Transfer)
```

`scenes/nfc_share_scene.h` (copy the bus_tools pattern, renamed):
```c
#pragma once
#include <gui/scene_manager.h>
#define ADD_SCENE(prefix, name, id) NfcShareScene##id,
typedef enum {
#include "nfc_share_scene_config.h"
    NfcShareSceneNum,
} NfcShareScene;
#undef ADD_SCENE
extern const SceneManagerHandlers nfc_share_scene_handlers;
#define ADD_SCENE(prefix, name, id) void prefix##_scene_##name##_on_enter(void*);
#include "nfc_share_scene_config.h"
#undef ADD_SCENE
#define ADD_SCENE(prefix, name, id) \
    bool prefix##_scene_##name##_on_event(void* context, SceneManagerEvent event);
#include "nfc_share_scene_config.h"
#undef ADD_SCENE
#define ADD_SCENE(prefix, name, id) void prefix##_scene_##name##_on_exit(void* context);
#include "nfc_share_scene_config.h"
#undef ADD_SCENE
```

`scenes/nfc_share_scene.c` (copy bus_tools_scene.c pattern, renamed prefix + `NfcShareSceneNum`):
```c
#include "nfc_share_scene.h"
#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_enter,
void (*const nfc_share_scene_on_enter_handlers[])(void*) = {
#include "nfc_share_scene_config.h"
};
#undef ADD_SCENE
#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_event,
bool (*const nfc_share_scene_on_event_handlers[])(void*, SceneManagerEvent) = {
#include "nfc_share_scene_config.h"
};
#undef ADD_SCENE
#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_exit,
void (*const nfc_share_scene_on_exit_handlers[])(void*) = {
#include "nfc_share_scene_config.h"
};
#undef ADD_SCENE
const SceneManagerHandlers nfc_share_scene_handlers = {
    .on_enter_handlers = nfc_share_scene_on_enter_handlers,
    .on_event_handlers = nfc_share_scene_on_event_handlers,
    .on_exit_handlers = nfc_share_scene_on_exit_handlers,
    .scene_num = NfcShareSceneNum,
};
```

- [ ] **Step 3: Write `nfc_share.h` and `nfc_share_i.h`**

`nfc_share.h`:
```c
#pragma once
#include <stdint.h>
int32_t nfc_share_app(void* p);
```

`nfc_share_i.h`:
```c
#pragma once
#include "nfc_share.h"
#include "scenes/nfc_share_scene.h"
#include "views/nfc_share_progress_view.h"
#include "nfc_share_engine.h"
#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <dialogs/dialogs.h>
#include <storage/storage.h>

#define NFC_SHARE_BASE_FOLDER "/ext"
#define NFC_SHARE_RECV_FOLDER "/ext/nfc_share"
#define NFC_SHARE_PATH_MAX    256

typedef enum {
    NfcShareViewSubmenu,
    NfcShareViewProgress,
} NfcShareView;

struct NfcShareApp {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    Submenu* submenu;
    DialogsApp* dialogs;
    Storage* storage;
    NfcShareProgressView* progress;   // custom view (Task 2 step 5)
    FuriString* file_path;            // selected send path
    NfcShareRole role;                // set by start scene
    NfcShareEngine* engine;           // owned by transfer scene
};
typedef struct NfcShareApp NfcShareApp;
```

- [ ] **Step 4: Write `nfc_share.c` (app entry — mirror bus_tools_app.c)**

```c
#include "nfc_share_i.h"

static bool nfc_share_custom_cb(void* ctx, uint32_t event) {
    NfcShareApp* app = ctx;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}
static bool nfc_share_back_cb(void* ctx) {
    NfcShareApp* app = ctx;
    return scene_manager_handle_back_event(app->scene_manager);
}
static void nfc_share_tick_cb(void* ctx) {
    NfcShareApp* app = ctx;
    scene_manager_handle_tick_event(app->scene_manager);
}

static NfcShareApp* nfc_share_app_alloc(void) {
    NfcShareApp* app = malloc(sizeof(NfcShareApp));
    memset(app, 0, sizeof(NfcShareApp));
    app->gui = furi_record_open(RECORD_GUI);
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->file_path = furi_string_alloc();
    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&nfc_share_scene_handlers, app);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, nfc_share_custom_cb);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, nfc_share_back_cb);
    view_dispatcher_set_tick_event_callback(app->view_dispatcher, nfc_share_tick_cb, 100);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    app->submenu = submenu_alloc();
    view_dispatcher_add_view(app->view_dispatcher, NfcShareViewSubmenu, submenu_get_view(app->submenu));
    app->progress = nfc_share_progress_view_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, NfcShareViewProgress, nfc_share_progress_view_get_view(app->progress));
    scene_manager_next_scene(app->scene_manager, NfcShareSceneStart);
    return app;
}

static void nfc_share_app_free(NfcShareApp* app) {
    view_dispatcher_remove_view(app->view_dispatcher, NfcShareViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, NfcShareViewProgress);
    nfc_share_progress_view_free(app->progress);
    submenu_free(app->submenu);
    view_dispatcher_free(app->view_dispatcher);
    scene_manager_free(app->scene_manager);
    furi_string_free(app->file_path);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t nfc_share_app(void* p) {
    UNUSED(p);
    NfcShareApp* app = nfc_share_app_alloc();
    view_dispatcher_run(app->view_dispatcher);
    nfc_share_app_free(app);
    return 0;
}
```

- [ ] **Step 5: Write the progress view (`views/nfc_share_progress_view.{h,c}`)**

`.h`:
```c
#pragma once
#include <gui/view.h>
#include "../nfc_share_engine.h"
typedef struct NfcShareProgressView NfcShareProgressView;
NfcShareProgressView* nfc_share_progress_view_alloc(void);
void nfc_share_progress_view_free(NfcShareProgressView* v);
View* nfc_share_progress_view_get_view(NfcShareProgressView* v);
void nfc_share_progress_view_update(NfcShareProgressView* v, const NfcShareStatus* s, bool sending);
```
`.c` — a `View` with a `ViewModel` holding a `NfcShareStatus` + `bool sending`; `draw` renders title ("NFC Send"/"NFC Receive"), filename, a `elements_progress_bar` from `bytes_done/file_size`, a `bytes/size  KB/s` line, and a final `Done - md5 OK` / `Failed (err)` line (mirror the drawing style of `bus_tools_nfc_view.c`). `update` copies the status into the model via `with_view_model(..., true)`. Input callback returns `false` (Back handled by dispatcher).

- [ ] **Step 6: Write the three scenes**

`scenes/nfc_share_scene_start.c` — submenu "Send a file" (idx 0) / "Receive a file" (idx 1):
```c
#include "../nfc_share_i.h"
static void submenu_cb(void* ctx, uint32_t index) {
    NfcShareApp* app = ctx;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}
void nfc_share_scene_start_on_enter(void* context) {
    NfcShareApp* app = context;
    submenu_reset(app->submenu);
    submenu_add_item(app->submenu, "Send a file", 0, submenu_cb, app);
    submenu_add_item(app->submenu, "Receive a file", 1, submenu_cb, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, NfcShareViewSubmenu);
}
bool nfc_share_scene_start_on_event(void* context, SceneManagerEvent event) {
    NfcShareApp* app = context;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == 0) {
            app->role = NfcShareRoleSend;
            scene_manager_next_scene(app->scene_manager, NfcShareSceneFileSelect);
        } else {
            app->role = NfcShareRoleReceive;
            scene_manager_next_scene(app->scene_manager, NfcShareSceneTransfer);
        }
        return true;
    }
    return false;
}
void nfc_share_scene_start_on_exit(void* context) {
    NfcShareApp* app = context;
    submenu_reset(app->submenu);
}
```

`scenes/nfc_share_scene_file_select.c` — mirror `bad_usb_scene_file_select.c`, base path `NFC_SHARE_BASE_FOLDER`, no extension filter (`dialog_file_browser_set_basic_options(&opt, NULL, NULL); opt.base_path = NFC_SHARE_BASE_FOLDER;`), write into `app->file_path`; on success → `NfcShareSceneTransfer`, else `view_dispatcher_stop`.

`scenes/nfc_share_scene_transfer.c` — on_enter: `app->engine = nfc_share_engine_alloc(app->role, app->role==NfcShareRoleSend ? furi_string_get_cstr(app->file_path) : NFC_SHARE_RECV_FOLDER); nfc_share_engine_start(app->engine);` switch to progress view. on_event: on `SceneManagerEventTypeTick`, `NfcShareStatus s; nfc_share_engine_status(app->engine, &s); nfc_share_progress_view_update(app->progress, &s, app->role==NfcShareRoleSend);` return true. on_exit: `nfc_share_engine_stop(app->engine); nfc_share_engine_free(app->engine); app->engine=NULL;`.

- [ ] **Step 7: Write the engine STUB (`nfc_share_engine.c`)**

Stub so the app links and runs this task; replaced in Tasks 3–4:
```c
#include "nfc_share_engine.h"
#include <furi.h>
#include <string.h>
struct NfcShareEngine { NfcShareRole role; char path[256]; NfcShareStatus st; };
NfcShareEngine* nfc_share_engine_alloc(NfcShareRole role, const char* path) {
    NfcShareEngine* e = malloc(sizeof(NfcShareEngine));
    memset(e, 0, sizeof(*e));
    e->role = role;
    strncpy(e->path, path ? path : "", sizeof(e->path) - 1);
    e->st.state = NfcShareStateWaiting;
    return e;
}
void nfc_share_engine_start(NfcShareEngine* e) { (void)e; }
void nfc_share_engine_stop(NfcShareEngine* e) { (void)e; }
void nfc_share_engine_status(NfcShareEngine* e, NfcShareStatus* out) { *out = e->st; }
void nfc_share_engine_free(NfcShareEngine* e) { free(e); }
```

- [ ] **Step 8: Build the firmware, verify clean**

Run (PowerShell tool):
```
& "C:\Users\stone\Downloads\_fzcard_work\NEW_v2\src_m5stick_s3\build_m5stick_s3.bat"
```
Then check `build_m5stick_s3.log` ends with `SENTINEL_OK` and no errors in the `nfc_share` objects. Expected: PASS (app compiles + links; FAM auto-discovers the new app dir).

- [ ] **Step 9: Flash + smoke test**

Flash (PowerShell, COM10 per Global Constraints). On device: launch **NFC Share**, confirm the submenu shows Send/Receive, "Send a file" opens the `/ext` browser, picking a file lands on the progress view (stub shows Waiting). Back exits cleanly.

- [ ] **Step 10: Commit**

```bash
git add applications/main/nfc_share
git commit -m "feat(nfc_share): standalone app scaffold (submenu, file browser, progress view, engine stub)"
```

---

### Task 3: Engine send path (listener worker, file streaming, md5, announce/serve)

Replaces the stub with the real sender: emulate the card, ANNOUNCE the selected file (real name/size/streamed md5), serve DATA blocks read from SD on demand.

**Files:**
- Modify: `applications/main/nfc_share/nfc_share_engine.c` (full implementation of the send role)
- Reference (do not edit): `applications/main/bus_tools/scenes/bus_tools_scene_st25r_nfc.c` (proven worker body), `components/furi_hal/st25r3916/st25r3916_nfca.h`

**Interfaces:**
- Consumes: `nfc_share_proto.h`; `st25r3916_nfca_listen_start/listen_poll/listen_rx/listen_tx/listen_stop`; `Storage` file API; `mbedtls/md5.h`.
- Produces: working `NfcShareRoleSend` behind the Task 2 engine interface.

- [ ] **Step 1: Add send state + md5 helper to the engine**

Extend `struct NfcShareEngine` with: `Storage* storage; File* file; FuriThread* worker; volatile bool run; FuriMutex* lock; NfcShareStatus st;` plus sender fields `uint8_t tx_id; uint32_t total_blocks; bool locked; bool pending; uint32_t send_next, send_last; uint8_t md5[16];`. Open `RECORD_STORAGE` in alloc; free in free.

Streaming md5 over the open file (seek to 0 first, restore after):
```c
static bool compute_file_md5(File* f, uint32_t size, uint8_t out[16]) {
    if(!storage_file_seek(f, 0, true)) return false;
    mbedtls_md5_context c; mbedtls_md5_init(&c); mbedtls_md5_starts(&c);
    uint8_t buf[512]; uint32_t left = size;
    while(left) {
        uint32_t want = left > sizeof(buf) ? sizeof(buf) : left;
        uint16_t got = storage_file_read(f, buf, (uint16_t)want);
        if(got == 0) { mbedtls_md5_free(&c); return false; }
        mbedtls_md5_update(&c, buf, got); left -= got;
    }
    mbedtls_md5_finish(&c, out); mbedtls_md5_free(&c);
    return storage_file_seek(f, 0, true);
}
```

- [ ] **Step 2: Sender worker (adapt the proven `nfc_tp_listener_worker`)**

The proven loop, with `build_announce_frame`/`build_data_frame` replaced by `nfc_share_build_announce` and an SD-backed data builder, and REQUEST parsing by `nfc_share_parse_request`. Data builder reads the block from the open file:
```c
static uint8_t build_data_from_file(NfcShareEngine* e, uint32_t block, uint8_t* out) {
    uint8_t data[NSH_DATA_LENGTH]; memset(data, 0, sizeof(data));
    uint32_t off = block * NSH_DATA_LENGTH;
    uint32_t valid = (off < e->st.file_size) ? (e->st.file_size - off) : 0;
    if(valid > NSH_DATA_LENGTH) valid = NSH_DATA_LENGTH;
    if(valid) {
        storage_file_seek(e->file, off, true);
        storage_file_read(e->file, data, (uint16_t)valid);
    }
    return (uint8_t)nfc_share_build_data(out, e->tx_id, block, data, valid);
}
```
Worker skeleton (identical control flow to the proven listener worker; buffers sized `[64]` RX, `announce[62]`, `data[250]`):
```c
static int32_t send_worker(void* ctx) {
    NfcShareEngine* e = ctx;
    uint8_t rx[64], announce[NSH_ANNOUNCE_FRAME_LEN], data[NSH_DATA_FRAME_LEN];
    uint8_t ka = NFC_TP_HDR_POLL;
    nfc_share_build_announce(announce, e->tx_id, e->st.name, e->st.file_size, e->md5);
    while(e->run) {
        St25rListenState state = St25rListenIdle; uint8_t raw = 0;
        st25r3916_nfca_listen_poll(&state, &raw);
        if(state != St25rListenRx) { furi_delay_ms(1); continue; }
        uint8_t n = 0;
        if(st25r3916_nfca_listen_rx(rx, sizeof(rx), &n) != St25rNfcaOk || n == 0) continue;
        uint32_t rs, re;
        if(nfc_share_parse_request(rx, n, e->tx_id, &rs, &re)) {
            uint32_t first = rs / NSH_DATA_LENGTH;
            uint32_t last = re ? (re - 1) / NSH_DATA_LENGTH : 0;
            if(first >= e->total_blocks) first = e->total_blocks - 1;
            if(last  >= e->total_blocks) last  = e->total_blocks - 1;
            e->send_next = first; e->send_last = last;
            e->pending = (first <= last); e->locked = true;
        }
        const uint8_t* txp; uint8_t txlen; bool is_data = false;
        if(e->pending && e->send_next < e->total_blocks) {
            txlen = build_data_from_file(e, e->send_next, data); txp = data; is_data = true;
            if(e->send_next++ >= e->send_last) e->pending = false;
        } else if(e->locked) { txp = &ka; txlen = 1; }
        else { txp = announce; txlen = NSH_ANNOUNCE_FRAME_LEN; }
        bool txok = (st25r3916_nfca_listen_tx(txp, txlen) == St25rNfcaOk);
        if(is_data && txok) {
            furi_mutex_acquire(e->lock, FuriWaitForever);
            e->st.blocks_done = e->send_next;
            e->st.bytes_done = e->send_next * NSH_DATA_LENGTH;
            if(e->st.bytes_done > e->st.file_size) e->st.bytes_done = e->st.file_size;
            e->st.state = (e->st.blocks_done >= e->total_blocks) ? NfcShareStateDoneOk : NfcShareStateRunning;
            furi_mutex_release(e->lock);
        }
    }
    return 0;
}
```
Note: the Flipper receiver verifies md5 on its end; sender reaching all blocks = `DoneOk`. (True end-to-end md5 is proven by the receiver; the sender cannot observe it.)

- [ ] **Step 3: Wire `nfc_share_engine_start/stop` for send**

`start` (send): open file read-only; if fail → `st.state=Fail, err=1`. `file_size = storage_file_size(file)`; guard `size==0 || size>16MB` → `Fail, err=4`. `total_blocks = ceil(size/240)`. Copy the leaf name into `st.name` (strip path). `compute_file_md5(...)`; on fail `Fail, err=1`. `tx_id=0x5A`. `st25r3916_nfca_listen_start(kShareUid,7,0x0044,0x00)`; on fail `Fail, err=3`. `st.state=Waiting`. Spawn `send_worker` via `furi_thread_alloc_ex("NfcShareSend",4096,send_worker,e)`. `stop`: `run=false`, join+free thread, `st25r3916_nfca_listen_stop()`, close file. Use `kShareUid` from `nfc_share_proto`-adjacent const (define `static const uint8_t kShareUid[7] = {0xF5,0x0F,0x5A,0xAE,0x00,0x00,0x01};` in the engine).

- [ ] **Step 4: Build clean**

PowerShell `build_m5stick_s3.bat`; expect `SENTINEL_OK`, no errors.

- [ ] **Step 5: On-device acceptance — send a real file (Acceptance Test 1)**

Flash COM10. Put a non-240-aligned file on `/ext` (e.g. a ~3 KB `.txt`). NFC Share → Send a file → pick it. Second device = a real Flipper Zero running Flipper Share NFC in **receive**. Expected: Flipper reaches 100%, md5 OK, correct filename, byte-identical content (compare on the Flipper's SD). Progress view on the M5 climbs to Done.

- [ ] **Step 6: Commit**

```bash
git add applications/main/nfc_share/nfc_share_engine.c
git commit -m "feat(nfc_share): send path — listener worker, SD streaming, md5 announce/serve"
```

---

### Task 4: Engine receive path (poller worker, SD streaming write, dedupe, md5 verify)

**Files:**
- Modify: `applications/main/nfc_share/nfc_share_engine.c` (add the receive role)

**Interfaces:**
- Consumes: `nfc_share_proto.h`; `st25r3916_nfca_init/poll/poller_txrx/deinit`; `Storage`; `mbedtls/md5.h`.
- Produces: working `NfcShareRoleReceive`.

- [ ] **Step 1: Add receiver state + dest-path resolver**

Receiver fields: `uint8_t rx_txid; uint32_t total_blocks, blocks_got; uint8_t* bitmap; uint8_t exp_md5[16]; char dest[NFC_SHARE_PATH_MAX]; uint32_t loop;`. Resolve a non-clobbering destination using the pure dedupe helper + storage existence check:
```c
static void resolve_dest(Storage* s, const char* leaf, char* out, size_t out_sz) {
    storage_common_mkdir(s, NFC_SHARE_RECV_FOLDER); /* ok if exists */
    for(unsigned k = 0; k < 1000; k++) {
        char name[64]; nfc_share_dedup_name(leaf, k, name, sizeof(name));
        snprintf(out, out_sz, "%s/%s", NFC_SHARE_RECV_FOLDER, name);
        if(storage_common_stat(s, out, NULL) != FSE_OK) return; /* free slot */
    }
}
```
(Sanitize `leaf`: take the substring after the last `/` or `\\` in the announced name before calling.)

- [ ] **Step 2: Receiver worker (adapt the proven `nfc_rx_poller_worker`)**

Same control flow as the proven poller worker, with three changes: (a) ANNOUNCE handled via `nfc_share_parse_announce`, then `resolve_dest` + `storage_file_open(file, dest, FSAM_WRITE, FSOM_CREATE_ALWAYS)` + allocate the bitmap (`calloc(ceil(blocks/8),1)`), guard size 0/`>16MB` → Fail; (b) each DATA (`nfc_share_parse_data`) writes to SD at offset instead of RAM:
```c
if(nfc_share_parse_data(resp, rn, e->rx_txid, &block, &dp, &dn) && e->st.state == NfcShareStateRunning) {
    if(block < e->total_blocks && !bitmap_have(e, block)) {
        uint32_t off = block * NSH_DATA_LENGTH;
        uint32_t valid = e->st.file_size - off; if(valid > NSH_DATA_LENGTH) valid = NSH_DATA_LENGTH;
        storage_file_seek(e->file, off, true);
        if(storage_file_write(e->file, dp, (uint16_t)valid) == valid) {
            bitmap_mark(e, block); e->blocks_got++;
            /* status under mutex: bytes_done += valid, blocks_done = blocks_got */
        }
        if(e->blocks_got >= e->total_blocks) { /* -> Step 3 finalize */ }
    }
}
```
Request pacing unchanged: `if(state==Running){ fm=first_missing(); if(fm<total && loop%6==0) txlen=nfc_share_build_request(txf, fm*240, file_size), txp=txf; }` else keep-alive.

- [ ] **Step 3: Finalize + md5 verify (re-read from SD)**

On last block: `storage_file_close(e->file)`; reopen `FSAM_READ|FSOM_OPEN_EXISTING`; `compute_file_md5(file, size, got)`; `state = memcmp(got, exp_md5,16)==0 ? DoneOk : Fail(err=2)`; close. On Fail-md5, keep the file (user can inspect) but mark Fail.

- [ ] **Step 4: Wire `start/stop` for receive**

`start` (receive): `st.state=Waiting`; spawn `recv_worker` (`furi_thread_alloc_ex("NfcShareRecv",4096,...)`). The worker calls `st25r3916_nfca_init()` itself (as the proven one does) and tears down (`deinit`, close file, free bitmap) on exit. `stop`: `run=false`, join+free thread.

- [ ] **Step 5: Build clean** — PowerShell `build_m5stick_s3.bat` → `SENTINEL_OK`.

- [ ] **Step 6: On-device acceptance — receive to SD + collision (Acceptance Tests 2 & 4)**

Flash COM10. Flipper runs Flipper Share **send** of a multi-block file. NFC Share → Receive a file. Expected: lands at `/ext/nfc_share/<name>`, progress to Done—md5 OK; pull the file (WebUI/`storage`), byte-compare to source. Repeat with the same name → second copy is `<name>_1`, original untouched.

- [ ] **Step 7: Commit**

```bash
git add applications/main/nfc_share/nfc_share_engine.c
git commit -m "feat(nfc_share): receive path — poller worker, SD streaming write, dedupe, md5 verify"
```

---

### Task 5: Larger-file validation, throughput readout, diagnostic trim

**Files:**
- Modify: `applications/main/nfc_share/nfc_share_engine.c` (kbps calc)
- Modify: `applications/main/nfc_share/views/nfc_share_progress_view.c` (show KB/s; remove any always-on hex dump)

**Interfaces:** Consumes/produces nothing new.

- [ ] **Step 1: Throughput.** In the engine, stamp `uint32_t t0 = furi_get_tick()` when `state` first becomes `Running`; on each status update compute `kbps_x10 = bytes_done*10*1000 / max(1, elapsed_ms) / 1024`. (`furi_get_tick()` is ms here.)
- [ ] **Step 2: Progress view.** Render `%u.%u KB/s` from `kbps_x10`. Ensure no raw last-packet hex is drawn (that was a bus_tools debug aid; keep only an optional single `#ifdef NFC_SHARE_DEBUG` line).
- [ ] **Step 3: Build clean** — `SENTINEL_OK`.
- [ ] **Step 4: On-device acceptance — larger file (Acceptance Test 3).** Transfer a ≥100 KB file **both** directions (M5↔Flipper), md5 OK each way; note KB/s. Confirms streaming + no RAM-cap regression.
- [ ] **Step 5: Commit** — `git commit -m "feat(nfc_share): throughput readout + trim diagnostics; verify large files"`.

---

### Task 6: Mirror to Cardputer ADV + build both trees

**Files:**
- Create (copy): `src_cardputer_adv/applications/main/nfc_share/**` (byte-identical to `src_m5stick_s3`)

**Interfaces:** none new.

- [ ] **Step 1: Copy the app dir**

```bash
cp -r "C:/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3/applications/main/nfc_share" \
      "C:/Users/stone/Downloads/_fzcard_work/NEW_v2/src_cardputer_adv/applications/main/nfc_share"
```
Verify the `application.fam` icon id exists in the cardputer tree too; if not, fall back to `A_GPIO_14`.

- [ ] **Step 2: Build both**

M5Stick (PowerShell): `build_m5stick_s3.bat` → `SENTINEL_OK`.
Cardputer (PowerShell, background ok): `& "C:\Users\stone\Downloads\_fzcard_work\NEW_v2\src_cardputer_adv\build_adv.bat"` → `=== SUCCESS ===`.

- [ ] **Step 3: On-device (Cardputer).** Flash the cardputer merged bin (COM12, `--before default_reset`); confirm NFC Share launches and (with a Unit NFC attached) completes one transfer. Note: no-PSRAM board — confirm the ≥100 KB case still works (bitmap-only RAM).
- [ ] **Step 4: Commit** — `git commit -m "feat(nfc_share): mirror app into cardputer_adv tree"`.

---

### Task 7: applab catalog entry

**Files:**
- Modify: the applab `index.html` `const APPS` array (the custom-build service catalog — see the `applab-custom-build-service` notes for the repo/path).

**Interfaces:** none.

- [ ] **Step 1: Add the catalog record** for `nfc_share` (id, display name "NFC Share", category NFC, short description: "Send/receive files between two M5 Unit NFC devices or a real Flipper over 13.56 MHz", note it needs the ST25R3916 Unit NFC). Match the existing entries' shape.
- [ ] **Step 2: Verify** the app appears in the picker and the subset-build includes `applications/main/nfc_share` (the app-subset mechanism from the build service).
- [ ] **Step 3: Commit/deploy** per the applab deploy flow.

---

## Self-Review

**Spec coverage:**
- Standalone app + engine split → Tasks 1–2. ✅
- Real file selection (send) → Task 2 (file_select) + Task 3 (open/size/md5). ✅
- Save received to `/ext/nfc_share/` + non-clobber → Task 4 (`resolve_dest` + `nfc_share_dedup_name`). ✅
- SD streaming both directions / larger files → Tasks 3–5. ✅
- md5: streamed on send, re-read verify on receive → Tasks 3, 4. ✅
- Both trees + fam → Tasks 2, 6. ✅
- applab → Task 7. ✅
- Trim diagnostics → Task 5. ✅
- Frozen wire format, size bound, pacing, 50 ms TXE, CRC-A strip → Global Constraints + carried into Tasks 3–4. ✅
- bus_tools scene untouched → no task edits it. ✅

**Placeholder scan:** No "TBD"/"add error handling" hand-waves — error paths specify the `err` code and state. Worker bodies are shown; scene bodies either shown or reduced to an exact mirror of a named in-tree file (`bad_usb_scene_file_select.c`, `bus_tools_scene.c`) with the concrete deltas listed.

**Type consistency:** `NfcShareStatus` fields (`state,name,file_size,bytes_done,blocks_total,blocks_done,kbps_x10,err`) are used identically in the engine, the progress view, and the transfer scene. Engine interface (`alloc/start/stop/status/free`) matches between Task 2 (stub) and Tasks 3–4 (real). Proto function names match between Task 1 definitions and Tasks 3–4 call sites (`nfc_share_build_announce/_request/_data`, `nfc_share_parse_announce/_request/_data`, `nfc_share_dedup_name`, `nfc_share_crc16`).

**Open verification (flagged for the implementer, not blockers):**
- `application.fam` icon id `A_NFC_14` — confirm it exists in this tree's assets; else `A_GPIO_14`.
- Exact Storage signatures (`storage_common_stat` args, `storage_file_seek(file, offset, from_start)` bool arg order, `storage_file_size` return type) — confirm against `applications/main/archive/helpers/archive_files.c` in this tree before first build.
