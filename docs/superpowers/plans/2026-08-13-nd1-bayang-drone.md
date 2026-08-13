# ND1 — Bayang Drone Control (nRF24) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** A PRIVATE-only "Drone (Bayang)" control feature in the nRF24 app: a pure, host-tested Bayang packet builder (transcribed from public Deviation/Multiprotocol sources) + a bind/control scene, plus a gated `nrf24_hw_tx_setup_ex` for Bayang's 1 Mbps / CRC-on PHY.

**Architecture:** Pure builder (`drone_bayang`) → the existing `nrf24_hw` TX layer (extended with a rate/CRC-aware setup) → a control scene cloning the nRF24 Fuzz scene's worker/abort shape. Spec: `docs/superpowers/specs/2026-08-13-nd1-bayang-drone-design.md`.

**Tech Stack:** C (ESP-IDF/furi), nRF24 SPI, WSL gcc host tests. Branch `feat/redteam-gapfill`.

## Global Constraints
- **PRIVATE-only.** ALL ND1 additions `#ifndef PUBLIC_BUILD`: the whole `drone_bayang.c` body, the scene, the new `nrf24_hw_tx_setup_ex` function + its use, the `ADD_SCENE`, and the menu entry. Public build compiles ND1 to nothing; nm-scan for `drone_bayang`/`bayang_`/`tx_setup_ex` = 0. Precedent: the nRF24 suite gating block in `scene_config.h` (Sniffer/KeySniffer/Fuzz).
- **UNVERIFIED — honest labeling everywhere.** Wire format is transcribed from public docs, NOT tested against a real drone/capture. Host tests prove internal consistency ONLY (checksum determinism, field packing, channel bounds, clamping), NOT real-frame correctness. The scene shows a verbatim UNVERIFIED banner. No fabricated "bound/flying" verdict.
- **No new radio driver** — reuse `nrf24_hw_*`; the only HAL addition is the gated rate/CRC-aware `nrf24_hw_tx_setup_ex` superset.
- **Bounded output** — `BAYANG_PKT_LEN` buffer; builders never write past it; all axis fields clamped to valid ranges before packing.
- **Fresh-log build method** (delete dir → absolute-path `.bat` → verify fresh log mtime + SENTINEL + obj recompiled). Private: `build_m5stick_s3.bat`, `build_adv_only_private.bat`. Public: `build_m5stick_s3_public.bat`.

## Reference (transcribe faithfully; cite in comments)
The Bayang format constants come from the public GPL **Deviation `protocol/bayang.c`** /
**Multiprotocol `Bayang_nrf24l01.ino`**. The implementer should transcribe from those
documented layouts. Key documented facts to encode (comment each with its source):
- Payload length 15 bytes (buffer `BAYANG_PKT_LEN` = 15).
- Bind packet: `packet[0] = 0xA4`; then the 5-byte rx tx-address + 3-byte txid region per
  the source; remaining bytes per source.
- Control packet: `packet[0] = 0xA5` (flags/normal); throttle/aileron/elevator/rudder are
  big-endian fields with documented bit widths + offsets (typically 0..0x3FF around a
  0x200 center for the stick axes; throttle 0..0x3FF from 0); `packet[1]` flags bits
  (e.g. FLIP/HEADLESS/RTH) per source; `packet[11..13]` = 3-byte txid; `packet[14]` =
  checksum.
- Checksum: documented Bayang sum — `sum = 0; for(i=0;i<14;i++) sum += packet[i];
  packet[14] = (sum + 0x55) & 0xFF;` — TRANSCRIBE THE EXACT rule from the source (verify
  the +0x55 seed and range against `bayang.c` at implementation time; the host test locks
  whatever is transcribed for determinism).
- RF channel hop: 4 channels derived from txid per the documented rule; each ≤ 125.
- Bind uses the documented bind address (`{0x00,0x00,0x00,0x00,0x00}`-style per source) +
  bind channel set.

> If the exact numeric constant for any field is uncertain at implementation time, encode
> the STRUCTURE faithfully, pick the documented value, and note the uncertainty in a
> `// UNVERIFIED:` comment + the report — do NOT invent a plausible-looking constant
> silently. Everything is HW-gated anyway.

---

### Task 1: PHY setup extension `nrf24_hw_tx_setup_ex` (gated)

**Files:**
- Modify: `applications/main/nrf24/nrf24_hw.h` (declare, gated)
- Modify: `applications/main/nrf24/nrf24_hw.c` (implement, gated; existing `nrf24_hw_tx_setup` unchanged in behavior)

**Interfaces:**
- Produces: `#ifndef PUBLIC_BUILD void nrf24_hw_tx_setup_ex(const uint8_t* addr, uint8_t addr_len, uint8_t channel, uint8_t payload_len, uint8_t data_rate /*0=1Mbps,1=2Mbps,2=250k*/, bool crc16); #endif`
- Consumes: the existing register-poke helpers (`nrf24_hw_write_reg`, etc.) already used by `nrf24_hw_tx_setup`.

- [ ] **Step 1: Read** `nrf24_hw.c`'s existing `nrf24_hw_tx_setup` to see exactly how it writes `RF_SETUP` (data rate + PA) and `CONFIG` (EN_CRC/CRCO) and the address/RX_PW registers.
- [ ] **Step 2: Add `nrf24_hw_tx_setup_ex`** (gated `#ifndef PUBLIC_BUILD`) that does everything `nrf24_hw_tx_setup` does, but computes the `RF_SETUP` data-rate bits from `data_rate` (1Mbps = RF_DR_LOW 0 + RF_DR_HIGH 0; 2Mbps = RF_DR_HIGH 1; 250k = RF_DR_LOW 1) and sets `CONFIG` EN_CRC (bit3) + CRCO (bit2 = 1 for 16-bit) from `crc16`. Keep PA max. Do NOT change the public `nrf24_hw_tx_setup` (it stays 2Mbps/CRC-off). No public symbol added (the ex is gated).
- [ ] **Step 3: Private m5stick build (fresh-log).** Delete `build_m5stick_s3`; build; verify `SENTINEL_OK` + `nrf24_hw.c.obj` recompiled.
- [ ] **Step 4: Commit**
```
git add applications/main/nrf24/nrf24_hw.c applications/main/nrf24/nrf24_hw.h
git commit -m "nrf24: add gated nrf24_hw_tx_setup_ex (data-rate + CRC select) for drone PHY (ND1 Task 1)"
```

---

### Task 2: Bayang builder `drone_bayang.{c,h}` + host test

**Files:**
- Create: `applications/main/nrf24/helpers/drone_bayang.h` (furi-free leaf)
- Create: `applications/main/nrf24/helpers/drone_bayang.c` (whole body `#ifndef PUBLIC_BUILD`)
- Test: `tests/test_drone_bayang.c`

**Interfaces:**
- Produces:
  ```c
  #define BAYANG_PKT_LEN     15
  #define BAYANG_RF_NUM_CH   4
  // Bayang flag bits (from public source; transcribe exact values):
  #define BAYANG_FLAG_FLIP   0x01
  #define BAYANG_FLAG_HEADLESS 0x?? // per source
  #define BAYANG_FLAG_RTH    0x??    // per source
  uint8_t drone_bayang_checksum(const uint8_t* pkt, uint8_t len); // documented sum rule
  void drone_bayang_build_bind(const uint8_t txid[3], uint8_t out[BAYANG_PKT_LEN]);
  void drone_bayang_build_control(const uint8_t txid[3], uint16_t throttle, uint16_t yaw,
      uint16_t pitch, uint16_t roll, uint8_t flags, uint8_t out[BAYANG_PKT_LEN]);
  void drone_bayang_rf_channels(const uint8_t txid[3], uint8_t out_ch[BAYANG_RF_NUM_CH]);
  ```
  Axis inputs clamped to their documented ranges before packing. `out` always exactly
  `BAYANG_PKT_LEN` bytes; never writes past it. `rf_channels` entries all ≤ 125.
- Consumes: nothing (pure leaf). furi-free.

- [ ] **Step 1: Write the failing test** `tests/test_drone_bayang.c` (host, `-fsanitize=address`):
```c
#include "drone_bayang.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_checksum_deterministic(void) {
    uint8_t p[BAYANG_PKT_LEN]; memset(p, 0, sizeof(p));
    for(int i = 0; i < 14; i++) p[i] = (uint8_t)(i * 7 + 1);
    uint8_t a = drone_bayang_checksum(p, BAYANG_PKT_LEN);
    uint8_t b = drone_bayang_checksum(p, BAYANG_PKT_LEN);
    assert(a == b);                 /* deterministic */
    p[3] ^= 0xFF;
    assert(drone_bayang_checksum(p, BAYANG_PKT_LEN) != a); /* covers the payload */
    printf("test_checksum_deterministic OK\n");
}

static void test_control_packing_and_checksum(void) {
    uint8_t txid[3] = {0x11, 0x22, 0x33};
    uint8_t out[BAYANG_PKT_LEN];
    drone_bayang_build_control(txid, 0x155, 0x200, 0x200, 0x200, 0x00, out);
    assert(out[0] == 0xA5);                              /* control flags/normal byte */
    assert(out[11] == 0x11 && out[12] == 0x22 && out[13] == 0x33); /* txid */
    assert(out[14] == drone_bayang_checksum(out, BAYANG_PKT_LEN)); /* checksum trails */
    /* deterministic */
    uint8_t out2[BAYANG_PKT_LEN];
    drone_bayang_build_control(txid, 0x155, 0x200, 0x200, 0x200, 0x00, out2);
    assert(memcmp(out, out2, BAYANG_PKT_LEN) == 0);
    printf("test_control_packing_and_checksum OK\n");
}

static void test_bind_differs_from_control(void) {
    uint8_t txid[3] = {1, 2, 3};
    uint8_t bind[BAYANG_PKT_LEN], ctrl[BAYANG_PKT_LEN];
    drone_bayang_build_bind(txid, bind);
    drone_bayang_build_control(txid, 0, 0x200, 0x200, 0x200, 0, ctrl);
    assert(bind[0] == 0xA4);          /* bind byte */
    assert(bind[0] != ctrl[0]);       /* bind vs control distinguishable */
    printf("test_bind_differs_from_control OK\n");
}

static void test_channels_in_band(void) {
    for(int t = 0; t < 256; t++) {
        uint8_t txid[3] = {(uint8_t)t, (uint8_t)(t * 3), (uint8_t)(t * 7)};
        uint8_t ch[BAYANG_RF_NUM_CH];
        drone_bayang_rf_channels(txid, ch);
        for(int i = 0; i < BAYANG_RF_NUM_CH; i++) assert(ch[i] <= 125);
    }
    printf("test_channels_in_band OK\n");
}

static void test_axis_clamp_no_overflow(void) {
    uint8_t txid[3] = {0, 0, 0};
    uint8_t out[BAYANG_PKT_LEN];
    /* absurd inputs must clamp, never overflow the packet (ASAN catches OOB) */
    drone_bayang_build_control(txid, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFF, out);
    assert(out[14] == drone_bayang_checksum(out, BAYANG_PKT_LEN));
    printf("test_axis_clamp_no_overflow OK\n");
}

int main(void) {
    test_checksum_deterministic();
    test_control_packing_and_checksum();
    test_bind_differs_from_control();
    test_channels_in_band();
    test_axis_clamp_no_overflow();
    printf("test_drone_bayang: ALL PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails.** Run: `wsl.exe -e bash -lc 'cd "/mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3" && gcc -std=gnu11 -Wall -Wextra -fsanitize=address -Iapplications/main/nrf24/helpers tests/test_drone_bayang.c applications/main/nrf24/helpers/drone_bayang.c -o /tmp/tdb && /tmp/tdb'` Expected: FAIL (no file).

- [ ] **Step 3: Write `drone_bayang.h`** — the interfaces above (fill the flag-bit values from the source), furi-free, with a header comment citing the Deviation/Multiprotocol source + the UNVERIFIED caveat.

- [ ] **Step 4: Write `drone_bayang.c`** — whole body `#ifndef PUBLIC_BUILD`. Transcribe the Bayang layout from the public source (comment field offsets with their meaning). Clamp each axis to its documented range before packing (so the absurd-input test can't overflow). `checksum` = the documented rule (transcribe exactly; the test locks it for determinism). `rf_channels` derives 4 channels ≤125. Byte 0 = 0xA5 control / 0xA4 bind; txid at [11..13]; checksum at [14]. NO furi includes. Any constant you're unsure of → `// UNVERIFIED:` comment.

- [ ] **Step 5: Run test to verify it passes** (same ASAN command). Expected: `test_drone_bayang: ALL PASS`, `-Wall -Wextra` clean, no ASAN report. (If the plan's asserted byte offsets — e.g. txid at [11..13], flags byte value — differ from the transcribed source, MAKE THE SOURCE-FAITHFUL LAYOUT WIN and update the test assertions to match the real Bayang layout, noting the change in the report. The test encodes the plan author's best understanding; the actual documented format governs.)

- [ ] **Step 6: Commit**
```
git add applications/main/nrf24/helpers/drone_bayang.c applications/main/nrf24/helpers/drone_bayang.h tests/test_drone_bayang.c
git commit -m "nrf24: Bayang drone packet builder (public format, UNVERIFIED) + host test (ND1 Task 2)"
```

---

### Task 3: Bind/control scene + menu wiring (gated) + private build

**Files:**
- Create: `applications/main/nrf24/scenes/scene_drone_bayang.c` (whole body `#ifndef PUBLIC_BUILD`)
- Modify: `applications/main/nrf24/scenes/scene_config.h` (gated `ADD_SCENE(nrf24_app, drone_bayang, DroneBayang)`)
- Modify: the nRF24 menu (`scenes/scene_menu.c`) — gated "Drone (Bayang)" item + enum + dispatch
- (Read: `scenes/scene_fuzz.c` (worker/abort/on-air/Tick pattern to clone), `scene_menu.c` (the gated-entry pattern from the Sniffer/KeySniffer/Fuzzer wiring), `nrf24_hw.h`, `drone_bayang.h`, and the M5Stick 2-button input convention.)

**Interfaces:**
- Consumes: Task 1 `nrf24_hw_tx_setup_ex` + `nrf24_hw_set_channel`/`tx_send`/`acquire`/`release`; Task 2 `drone_bayang_*`.
- Produces: scene id `Nrf24AppSceneDroneBayang`.

- [ ] **Step 1: Scene** — clone `scene_fuzz.c`'s worker/stop-flag/join/Tick shape. State: a bind txid (fixed or generated), the 4 RF channels (`drone_bayang_rf_channels`), current axes (throttle/yaw/pitch/roll, centered), selected-axis index (2-button cycle). Worker: `nrf24_hw_acquire()`; for BIND phase (first ~2s) `nrf24_hw_tx_setup_ex(bind_addr,5,bind_ch,BAYANG_PKT_LEN,0/*1Mbps*/,true/*crc16*/)` + `drone_bayang_build_bind` + `tx_send` in a loop; then CONTROL phase: round-robin the 4 channels (`nrf24_hw_set_channel`), `drone_bayang_build_control(txid, axes…)`, `tx_send`, at the Bayang frame rate; `nrf24_hw_release()` on stop. Buttons adjust the selected axis. UI shows phase + axes + on-air + the verbatim UNVERIFIED banner: "UNVERIFIED wire format (from public docs) -- may not control a real drone. HW-test required." Back aborts (stop-flag + join, same as scene_fuzz). Whole file `#ifndef PUBLIC_BUILD`.
- [ ] **Step 2: Wiring** — gated `ADD_SCENE(nrf24_app, drone_bayang, DroneBayang)` in `scene_config.h` (inside the existing private block); gated "Drone (Bayang)" submenu item + enum value + dispatch `scene_manager_next_scene(app->scene_manager, Nrf24AppSceneDroneBayang)` in `scene_menu.c`, mirroring the Sniffer/KeySniffer/Fuzzer gated entries exactly.
- [ ] **Step 3: Private m5stick build (fresh-log).** Delete `build_m5stick_s3`; build; verify `SENTINEL_OK` + `scene_drone_bayang.c.obj` + `drone_bayang.c.obj` compiled. Fix + rebuild until green.
- [ ] **Step 4: Commit**
```
git add applications/main/nrf24/scenes/scene_drone_bayang.c applications/main/nrf24/scenes/scene_config.h applications/main/nrf24/scenes/scene_menu.c
git commit -m "nrf24: Bayang drone bind/control scene + menu wiring, private only (ND1 Task 3)"
```

---

### Task 4: Full verification + public exclusion + report

**Files:**
- Create: `docs/superpowers/plans/nd1-report.md`

- [ ] **Step 1: Private cardputer build (fresh-log)** — `build_adv_only_private.bat` → `SENTINEL_OK` + objs recompiled.
- [ ] **Step 2: Public exclusion build (fresh-log)** — `build_m5stick_s3_public.bat` → `SENTINEL_OK` + `NOCAR PASS`.
- [ ] **Step 3: nm/map exclusion scan** — GNU `nm` on public `furi_esp32.elf`: total large (tool works) + a known nrf24 public symbol present, then grep for `drone_bayang|bayang_|tx_setup_ex` in ELF **and** map → **0** (use PRECISE patterns; disambiguate any IDF-lib substring matches like WF2's `scan_fill_wps_scan_ie`).
- [ ] **Step 4: Host test** — re-run `test_drone_bayang` (ASAN) → ALL PASS.
- [ ] **Step 5: Report** `docs/superpowers/plans/nd1-report.md` — what shipped, build results (3 targets), 0-public-symbol proof, host-test results (INTERNAL CONSISTENCY only), the UNVERIFIED status prominently (transcribed public format, no drone/capture tested), the PHY note (1Mbps/CRC-on via tx_setup_ex), and the on-hardware bind test as the REQUIRED next step before extending to Syma/ESB/V2x2.
- [ ] **Step 6: Commit** the report.
- [ ] **Step 7: Final whole-branch review** of the ND1 commits (builder bounds/clamp, gating, honest UNVERIFIED framing, scene worker lifecycle, tx_setup_ex register correctness).

## Self-review notes
- **Spec coverage:** PHY ext (T1), builder (T2), scene+wiring+private build (T3), verification incl. public exclusion + report (T4). All mapped.
- **Private-only:** every addition gated; T4 proves 0 public symbols.
- **Honesty:** UNVERIFIED labeled in header, scene banner, and report; host tests explicitly internal-consistency-only; on-HW bind is the real proof.
- **Type consistency:** `drone_bayang_*` + `BAYANG_*` identical across T2/T3; `nrf24_hw_tx_setup_ex` signature identical T1/T3.
- **Source-faithful governs:** if a transcribed constant differs from the plan's example test bytes, the real Bayang format wins and the test is updated (noted in report).
