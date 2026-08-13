# WiFi 802.11 Management-Frame Fuzzer (WF1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a PRIVATE-only 802.11 management-frame fuzzer to the WiFi app: mutate a base mgmt frame (auth/assoc/probe/beacon/deauth) and TX the malformed frames at a target AP/broadcast in a rate-limited, abortable loop.

**Architecture:** A pure, host-tested mutation core (`wlan_mgmt_fuzz`) + base-frame builders (`wlan_mgmt_frames`, reusing `pmkid_frames.c` for auth/assoc) feed two gated scenes (config + run) that clone `scene_network_deauth.c`'s xTask/abort/counter skeleton and TX via `wlan_hal_send_raw`. Spec: `docs/superpowers/specs/2026-08-13-wifi-mgmt-fuzzer-design.md`.

**Tech Stack:** C (ESP-IDF/furi), FuriThread-adjacent `xTaskCreate` worker, WSL gcc host tests. Branch `feat/redteam-gapfill`.

## Global Constraints
- **PRIVATE-only.** Every new scene, the fuzz core's scene-facing use, the base-frame builders' scene use, the menu entry, and the `ADD_SCENE` lines are `#ifndef PUBLIC_BUILD`. Public build compiles the feature to nothing; the public ELF/map nm-scan for `wlan_mgmt_fuzz`/`mgmt_fuzzer`/new builder symbols must be **0**. Gating precedent to mirror: `applications/main/subghz/scenes/subghz_scene_config.h:47-51` + `subghz_scene_start.c:51-57,184-196` (the subghz Protocol Fuzzer). The pure `helpers/wlan_mgmt_fuzz.c` + `helpers/wlan_mgmt_frames.c` bodies are also whole-file `#ifndef PUBLIC_BUILD` (attack tooling; matches `nrf24_esb.c`/`nrf24_capture_tx.c` precedent) so they add 0 symbols to public.
- **No new radio path.** TX only via `wlan_hal_send_raw()` (`wlan_hal.h:79`); channel via `wlan_hal_set_channel()` (`wlan_hal.h:71`); target from `app->target_ap`/`app->connected_ap`.
- **Honest UI.** Run screen shows verbatim: "Effect is target-dependent -- reports frames sent, not a crash verdict." Surface the 128-byte cap + forced-seqctl limits.
- **Fresh-log build method (mandatory).** Delete build dir → invoke `.bat` by absolute path via PowerShell/cmd.exe → verify log mtime current + terminal SENTINEL + changed `.obj` recompiled. Never trust exit codes. Private builds: `build_m5stick_s3.bat`, `build_adv_only_private.bat`. Public exclusion: `build_m5stick_s3_public.bat` (`SENTINEL_OK` + `NOCAR PASS`).
- `WLAN_FUZZ_MAX 256` buffer; `wlan_mgmt_fuzz_next` never writes past `out[256]`, `out_len` in [24,256]; effective TX ceiling 128 bytes (send_raw) documented.

---

### Task 1: Mutation core `wlan_mgmt_fuzz.{c,h}` + host test

**Files:**
- Create: `applications/main/wlan_app/helpers/wlan_mgmt_fuzz.h` (furi-free leaf header)
- Create: `applications/main/wlan_app/helpers/wlan_mgmt_fuzz.c` (whole body `#ifndef PUBLIC_BUILD`)
- Test: `tests/test_wlan_mgmt_fuzz.c`

**Interfaces:**
- Produces: `typedef enum { WMF_BITFLIP, WMF_FIELD, WMF_IE_MANGLE, WMF_LENSWEEP, WMF_RANDOM } WlanFuzzMode;` (`WMF_MODE_COUNT` = 5); `#define WLAN_FUZZ_MAX 256`; `bool wlan_mgmt_fuzz_next(const uint8_t* base, uint16_t base_len, WlanFuzzMode mode, uint32_t seed, uint32_t step, uint8_t* out, uint16_t* out_len);` Returns false (leaving out_* untouched) on NULL args or base_len<24; else writes the mutated frame to `out` (≤256), sets `*out_len` in [24,256].
- Consumes: nothing (pure leaf; furi-free so it host-compiles with only `-Iapplications/main/wlan_app/helpers`). For host testing, the `#ifndef PUBLIC_BUILD` guard is inactive (PUBLIC_BUILD undefined) so the body is live.

- [ ] **Step 1: Write the failing test** `tests/test_wlan_mgmt_fuzz.c`:
```c
#include "wlan_mgmt_fuzz.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* A 30-byte open-auth-ish base: 24-byte header + 6-byte body. */
static void mk_base(uint8_t* b, uint16_t* n) {
    memset(b, 0, 30);
    b[0] = 0xB0; /* mgmt, subtype auth */
    for(int i = 4; i < 22; i++) b[i] = (uint8_t)i; /* addr1/2/3 */
    *n = 30;
}

static void test_determinism(void) {
    uint8_t base[30]; uint16_t bn; mk_base(base, &bn);
    for(int m = 0; m < WMF_MODE_COUNT; m++) {
        uint8_t a[WLAN_FUZZ_MAX], b[WLAN_FUZZ_MAX]; uint16_t la, lb;
        assert(wlan_mgmt_fuzz_next(base, bn, (WlanFuzzMode)m, 0x1234, 7, a, &la));
        assert(wlan_mgmt_fuzz_next(base, bn, (WlanFuzzMode)m, 0x1234, 7, b, &lb));
        assert(la == lb && memcmp(a, b, la) == 0);
        assert(la >= 24 && la <= WLAN_FUZZ_MAX);
    }
    printf("test_determinism OK\n");
}

static void test_bitflip_one_bit(void) {
    uint8_t base[30]; uint16_t bn; mk_base(base, &bn);
    uint8_t out[WLAN_FUZZ_MAX]; uint16_t lo;
    assert(wlan_mgmt_fuzz_next(base, bn, WMF_BITFLIP, 0xAA, 3, out, &lo));
    assert(lo == bn);
    int diff = 0;
    for(int i = 0; i < lo; i++) { uint8_t x = base[i] ^ out[i]; while(x){diff += x&1; x>>=1;} }
    assert(diff == 1); /* exactly one bit changed */
    printf("test_bitflip_one_bit OK\n");
}

static void test_lensweep_bounds(void) {
    uint8_t base[30]; uint16_t bn; mk_base(base, &bn);
    for(uint32_t s = 0; s < 400; s++) {
        uint8_t out[WLAN_FUZZ_MAX]; uint16_t lo;
        assert(wlan_mgmt_fuzz_next(base, bn, WMF_LENSWEEP, 0, s, out, &lo));
        assert(lo >= 24 && lo <= WLAN_FUZZ_MAX);
    }
    printf("test_lensweep_bounds OK\n");
}

static void test_guards(void) {
    uint8_t out[WLAN_FUZZ_MAX]; uint16_t lo; uint8_t base[30]; uint16_t bn; mk_base(base,&bn);
    assert(!wlan_mgmt_fuzz_next(NULL, bn, WMF_BITFLIP, 0, 0, out, &lo));
    assert(!wlan_mgmt_fuzz_next(base, 10, WMF_BITFLIP, 0, 0, out, &lo)); /* <24 */
    assert(!wlan_mgmt_fuzz_next(base, bn, WMF_BITFLIP, 0, 0, NULL, &lo));
    assert(!wlan_mgmt_fuzz_next(base, bn, WMF_BITFLIP, 0, 0, out, NULL));
    printf("test_guards OK\n");
}

int main(void) {
    test_determinism(); test_bitflip_one_bit(); test_lensweep_bounds(); test_guards();
    printf("test_wlan_mgmt_fuzz: ALL PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails** (no header/impl yet)

Run: `wsl.exe -e bash -lc 'cd "/mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3" && gcc -std=gnu11 -Wall -Wextra -Iapplications/main/wlan_app/helpers tests/test_wlan_mgmt_fuzz.c applications/main/wlan_app/helpers/wlan_mgmt_fuzz.c -o /tmp/twf && /tmp/twf'`
Expected: FAIL (compile error — no such file).

- [ ] **Step 3: Write `wlan_mgmt_fuzz.h`** — furi-free: include guard, `<stdint.h>`/`<stdbool.h>`, the `WlanFuzzMode` enum, `WMF_MODE_COUNT`, `WLAN_FUZZ_MAX 256`, doc comment, the `wlan_mgmt_fuzz_next` prototype (as in Interfaces).

- [ ] **Step 4: Write `wlan_mgmt_fuzz.c`** — whole body `#ifndef PUBLIC_BUILD ... #endif`. `#include "wlan_mgmt_fuzz.h"` + `<string.h>`. `static uint32_t sm32(uint32_t* s)` splitmix32 (copy the idiom from `applications/main/nrf24/helpers/nrf24_fuzz.c`). In `wlan_mgmt_fuzz_next`: guard (NULL base/out/out_len or base_len<24 → return false); clamp base_len to WLAN_FUZZ_MAX; `memcpy(out, base, base_len)`; seed `uint32_t st = seed ^ (step*0x9E3779B9u)`. Then per mode:
  - `WMF_BITFLIP`: `*out_len=base_len`; pick `bit = sm32(&st) % (base_len*8)`; flip `out[bit/8] ^= 1<<(bit%8)`.
  - `WMF_FIELD`: `*out_len=base_len`; choose field = `sm32(&st)%4`: 0→`out[0]` (frame-control, keep it a mgmt type by OR-ing a random subtype into the upper nibble), 1→`out[2]/out[3]` (duration), 2→`out[22]/out[23]` (seqctl), 3→`out[24]` if base_len>24 (reason/status/auth-alg first body byte). Set the chosen bytes from `sm32`.
  - `WMF_IE_MANGLE`: `*out_len=base_len`; if base_len>24, treat `out[24..]` as an IE region; pick offset `o = 24 + (sm32(&st) % (base_len-24))`; corrupt: `out[o] = (uint8_t)sm32(&st)` (mangles a tag-id or length byte). Keep out_len == base_len (no growth).
  - `WMF_LENSWEEP`: `uint16_t L = 24 + (step % (WLAN_FUZZ_MAX - 24 + 1))`; if L>base_len fill `out[base_len..L-1]` with `(uint8_t)sm32(&st)`; `*out_len = L`.
  - `WMF_RANDOM`: keep 24-byte header from base; `uint16_t L = 24 + (sm32(&st) % (WLAN_FUZZ_MAX-24+1)); for(i=24;i<L;i++) out[i]=(uint8_t)sm32(&st); *out_len=L;`.
  Return true. Ensure every path sets `*out_len` in [24,256] and never indexes past `out[255]`.

- [ ] **Step 5: Run test to verify it passes** (same command as Step 2). Expected: `test_wlan_mgmt_fuzz: ALL PASS`, `-Wall -Wextra` clean.

- [ ] **Step 6: Commit**
```
git add applications/main/wlan_app/helpers/wlan_mgmt_fuzz.c applications/main/wlan_app/helpers/wlan_mgmt_fuzz.h tests/test_wlan_mgmt_fuzz.c
git commit -m "wlan: 802.11 mgmt-frame fuzz mutators (5 modes) + host test (WF1 Task 1)"
```

---

### Task 2: Base-frame builders `wlan_mgmt_frames.{c,h}` + host test

**Files:**
- Create: `applications/main/wlan_app/helpers/wlan_mgmt_frames.h`
- Create: `applications/main/wlan_app/helpers/wlan_mgmt_frames.c` (whole body `#ifndef PUBLIC_BUILD`)
- Test: `tests/test_wlan_mgmt_frames.c`

**Interfaces:**
- Produces: `uint16_t wlan_build_probe_req(uint8_t* out, uint16_t cap, const uint8_t src[6], const char* ssid);` `uint16_t wlan_build_beacon(uint8_t* out, uint16_t cap, const uint8_t bssid[6], const char* ssid, uint8_t channel);` `uint16_t wlan_build_deauth(uint8_t* out, uint16_t cap, const uint8_t bssid[6], const uint8_t sta[6], uint16_t reason);` Each returns the frame length (0 if cap too small). Hand-packed 802.11 byte layout (frame-control at [0], duration [2-3], addr1 [4-9], addr2 [10-15], addr3 [16-21], seqctl [22-23], then body/IEs) — deauth layout mirrors `scene_network_deauth.c:18-24`'s `deauth_tmpl[26]`.
- Consumes: nothing at build time (auth/assoc seeds come from the existing `pmkid_frames.c` at scene level, not duplicated here). Pure/furi-free for host test.

- [ ] **Step 1: Write the failing test** `tests/test_wlan_mgmt_frames.c` — asserts: `wlan_build_deauth` returns 26, `out[0]==0xC0` (deauth subtype), addr1==sta, addr2/addr3==bssid, reason at [24-25] little-endian; `wlan_build_probe_req` returns ≥ (24+2+len(ssid)+ ...), `out[0]==0x40` (probe-req subtype), SSID tag id 0x00 + length + bytes present; `wlan_build_beacon` returns >36, `out[0]==0x80` (beacon subtype), addr1==FF:FF:FF:FF:FF:FF, SSID tag present, DS-param channel byte == channel; each returns 0 when `cap` is 10.
```c
#include "wlan_mgmt_frames.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    uint8_t out[256];
    uint8_t bssid[6] = {1,2,3,4,5,6}, sta[6] = {7,8,9,10,11,12}, src[6]={0xAA,0,0,0,0,1};
    uint16_t n = wlan_build_deauth(out, sizeof(out), bssid, sta, 7);
    assert(n == 26); assert(out[0] == 0xC0);
    assert(memcmp(out+4, sta, 6) == 0); assert(memcmp(out+10, bssid, 6) == 0);
    assert(memcmp(out+16, bssid, 6) == 0); assert(out[24]==7 && out[25]==0);
    assert(wlan_build_deauth(out, 10, bssid, sta, 7) == 0);
    n = wlan_build_probe_req(out, sizeof(out), src, "Test");
    assert(n >= 24+2+4); assert(out[0] == 0x40); assert(out[24] == 0x00 && out[25] == 4);
    assert(memcmp(out+26, "Test", 4) == 0);
    n = wlan_build_beacon(out, sizeof(out), bssid, "AP", 6);
    assert(n > 36); assert(out[0] == 0x80);
    uint8_t bc[6]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}; assert(memcmp(out+4, bc, 6)==0);
    printf("test_wlan_mgmt_frames: ALL PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails.** Run: `wsl.exe -e bash -lc 'cd "/mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3" && gcc -std=gnu11 -Wall -Wextra -Iapplications/main/wlan_app/helpers tests/test_wlan_mgmt_frames.c applications/main/wlan_app/helpers/wlan_mgmt_frames.c -o /tmp/twfr && /tmp/twfr'` Expected: FAIL (no file).

- [ ] **Step 3: Write `wlan_mgmt_frames.h`** — furi-free prototypes (as in Interfaces) + doc.

- [ ] **Step 4: Write `wlan_mgmt_frames.c`** — whole body `#ifndef PUBLIC_BUILD`. Implement the three builders by hand-packing bytes at the offsets asserted in the test. Deauth: 24-byte header (fc=0xC0,0x00; dur=0; addr1=sta; addr2=addr3=bssid; seqctl=0) + reason (little-endian) = 26 bytes. Probe-req: fc=0x40,0x00; addr1=addr3=broadcast; addr2=src; seqctl=0; then SSID IE (0x00, len, bytes) + supported-rates IE (0x01, 4, 0x02 0x04 0x0B 0x16). Beacon: fc=0x80,0x00; addr1=broadcast; addr2=addr3=bssid; seqctl=0; 8-byte timestamp(0); 2-byte beacon-interval(0x64,0); 2-byte cap(0x01,0x04); SSID IE; rates IE; DS-param IE (0x03,0x01,channel). Each returns 0 if `cap` < required length.

- [ ] **Step 5: Run test to verify it passes.** Expected: `test_wlan_mgmt_frames: ALL PASS`, `-Wall -Wextra` clean.

- [ ] **Step 6: Commit**
```
git add applications/main/wlan_app/helpers/wlan_mgmt_frames.c applications/main/wlan_app/helpers/wlan_mgmt_frames.h tests/test_wlan_mgmt_frames.c
git commit -m "wlan: 802.11 base-frame builders (probe/beacon/deauth) + host test (WF1 Task 2)"
```

---

### Task 3: Config + Run scenes + menu wiring (gated) + private build

**Files:**
- Create: `applications/main/wlan_app/scenes/scene_mgmt_fuzzer_config.c` (whole body `#ifndef PUBLIC_BUILD`)
- Create: `applications/main/wlan_app/scenes/scene_mgmt_fuzzer_run.c` (whole body `#ifndef PUBLIC_BUILD`)
- Modify: `applications/main/wlan_app/scenes/scene_config.h` (add 2 gated `ADD_SCENE`)
- Modify: `applications/main/wlan_app/scenes/scene_main.c` (gated submenu item + dispatch)
- (Read for pattern: `scene_network_deauth.c`, `wlan_deauther_view.c/.h`, `pmkid_frames.h`, `wlan_hal.h`, `wlan_app.h`.)

**Interfaces:**
- Consumes: Task 1 `wlan_mgmt_fuzz_next` + `WlanFuzzMode`; Task 2 `wlan_build_probe_req/beacon/deauth`; existing `pmkid_build_auth_open`/`pmkid_build_assoc_req` (`pmkid_frames.h`); `wlan_hal_send_raw`/`wlan_hal_set_channel` (`wlan_hal.h`); `app->target_ap`/`app->connected_ap` (`wlan_app.h`).
- Produces: scene ids `WlanAppSceneMgmtFuzzerConfig`, `WlanAppSceneMgmtFuzzerRun` (via ADD_SCENE X-macro).

- [ ] **Step 1: Config scene** — a Submenu/VariableItemList to pick frame type (Auth/Assoc/Probe/Beacon/Deauth) + mode (5). Store selections in scene state or `app` fields. On confirm → `scene_manager_next_scene(..., WlanAppSceneMgmtFuzzerRun)`. Whole file `#ifndef PUBLIC_BUILD`.
- [ ] **Step 2: Run scene** — clone `scene_network_deauth.c`: a `static volatile bool s_run`, a `static TaskHandle_t s_task`, worker `mgmt_fuzz_task_fn(void* arg)` that resolves the target (`app->target_ap.ssid[0] ? &target_ap : &connected_ap`, else broadcast), sets channel once via `wlan_hal_set_channel`, then loops: build base (switch on frame type: auth→`pmkid_build_auth_open`, assoc→`pmkid_build_assoc_req`, probe→`wlan_build_probe_req`, beacon→`wlan_build_beacon`, deauth→`wlan_build_deauth`), `wlan_mgmt_fuzz_next(base,len,mode,seed,step++,out,&olen)`, `wlan_hal_send_raw(out, olen)`, `s_frames_sent++`, `vTaskDelay(pdMS_TO_TICKS(15))`, until `!s_run`; `vTaskDelete(NULL)`. Start on enter (seed from `furi_get_tick()`), OK toggles start/stop, Tick polls `s_frames_sent`+step+mode into the view, header text verbatim honest string, on_exit stops+joins (`s_run=false; while(s_task) vTaskDelay(10);`). Reuse `WlanAppViewDeauther`/`wlan_deauther_view` or a Widget — whichever is least invasive; a Widget with text elements is fine. Whole file `#ifndef PUBLIC_BUILD`.
- [ ] **Step 3: Wiring** — in `scenes/scene_config.h` add, wrapped in `#ifndef PUBLIC_BUILD`/`#endif`: `ADD_SCENE(wlan_app, mgmt_fuzzer_config, MgmtFuzzerConfig)` and `ADD_SCENE(wlan_app, mgmt_fuzzer_run, MgmtFuzzerRun)`. In `scene_main.c` add (also gated) a `submenu_add_item(app->submenu, "Mgmt Fuzzer", MainIndexMgmtFuzzer, wlan_app_scene_main_submenu_cb, app);` + a `MainIndexMgmtFuzzer` enum value + a dispatch `case` calling `scene_manager_next_scene(app->scene_manager, WlanAppSceneMgmtFuzzerConfig)`. Mirror the subghz Protocol Fuzzer gating exactly.
- [ ] **Step 4: Private build (fresh-log).** Delete `build_m5stick_s3`; run `build_m5stick_s3.bat` absolute-path via PowerShell/cmd.exe. Verify fresh `build_m5stick_s3.log` `SENTINEL_OK` + `scene_mgmt_fuzzer_run.c.obj` + `wlan_mgmt_fuzz.c.obj` compiled. Fix any errors and rebuild until green.
- [ ] **Step 5: Commit**
```
git add applications/main/wlan_app/scenes/scene_mgmt_fuzzer_config.c applications/main/wlan_app/scenes/scene_mgmt_fuzzer_run.c applications/main/wlan_app/scenes/scene_config.h applications/main/wlan_app/scenes/scene_main.c
git commit -m "wlan: Mgmt Fuzzer scenes + menu wiring, private only (WF1 Task 3)"
```

---

### Task 4: Full verification + public exclusion + report

**Files:**
- Create: `docs/superpowers/plans/wf1-report.md`

- [ ] **Step 1: Private cardputer build (fresh-log).** Delete `build_cardputer_adv`; run `build_adv_only_private.bat` absolute-path. Verify `SENTINEL_OK` + the new `.obj`s compiled.
- [ ] **Step 2: Public exclusion build (fresh-log).** Delete `build_m5stick_s3_public`; run `build_m5stick_s3_public.bat`. Verify `SENTINEL_OK` + `NOCAR PASS`.
- [ ] **Step 3: nm/map exclusion scan.** WSL GNU `nm` on `build_m5stick_s3_public/furi_esp32.elf`: confirm total symbols large (tool works) + a known public wlan symbol present, then grep for `wlan_mgmt_fuzz|wlan_build_probe|wlan_build_beacon|wlan_build_deauth|mgmt_fuzzer` in ELF **and** `furi_esp32.map` → both **0**.
- [ ] **Step 4: Host tests.** Re-run `test_wlan_mgmt_fuzz` + `test_wlan_mgmt_frames` → ALL PASS.
- [ ] **Step 5: Report** `docs/superpowers/plans/wf1-report.md` — what shipped, build results (3 targets, fresh mtimes), 0-public-symbol proof, host-test results, honest limits (128-byte cap, forced seqctl, driver-drop, no crash oracle), HW-deferred status.
- [ ] **Step 6: Commit**
```
git add docs/superpowers/plans/wf1-report.md
git commit -m "wlan: WF1 mgmt-fuzzer verification report (WF1 Task 4)"
```
- [ ] **Step 7: Final whole-branch review** of the WF1 commits (gating symmetry, worker lifecycle/abort, buffer bounds, honesty framing).

## Self-review notes
- **Spec coverage:** mutation core (T1), base builders (T2), scenes+wiring+private build (T3), verification incl. public exclusion + report (T4). All mapped.
- **Private-only:** every helper body + scene + ADD_SCENE + menu entry gated; T4 proves 0 public symbols.
- **Type consistency:** `WlanFuzzMode`/`WMF_*`/`WLAN_FUZZ_MAX`/`wlan_mgmt_fuzz_next` and `wlan_build_probe_req/beacon/deauth` signatures identical across T1/T2/T3.
- **Reuse:** auth/assoc via existing `pmkid_frames.c` (not duplicated); worker/abort via `scene_network_deauth.c`; TX via `wlan_hal_send_raw`.
