# nRF24 Fusion Spectrum Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a private-gated, full-color 2.4 GHz "fusion spectrum" for the nRF24 app that overlays nRF24 carrier-occupancy, Wi-Fi RSSI, and BLE RSSI into one classified heat-waterfall rendered in RGB565 via display takeover.

**Architecture:** A new scene (`scene_fusion_spectrum.c`) takes over the ST7789 panel (doom/wolf3d `gui_direct_draw_acquire` + `furi_hal_display_get_panel_handle()` pattern) and renders RGB565 itself. Three workers feed one mutex-guarded `FusedSpectrum` model: an nRF24 RPD sweep worker, a time-shared internal-radio (Wi-Fi then BLE) worker, and a render worker. All fusion math (channel mapping, dBm↔percent, heat palette, waterfall ring, classification) lives in pure host-tested helpers.

**Tech Stack:** C (gnu11), ESP-IDF v5.4.1, FreeRTOS, esp_lcd (ST7789), esp_wifi, NimBLE, Furi (FuriThread/FuriMutex), host tests via gcc.

**Spec:** `docs/superpowers/specs/2026-08-15-nrf24-fusion-spectrum-design.md`

## Global Constraints

- **Board / target:** primary is Cardputer ADV (`FLIPPER_BOARD=m5stack_cardputer_adv`, COM12); canonical source `src_m5stick_s3`, mirror to `src_cardputer_adv` before the Cardputer build.
- **Gating:** the entire scene, its files' registration, and the menu entry are wrapped in `#ifndef PUBLIC_BUILD`. Public build must still compile (feature absent) — verify both preprocessor paths.
- **No-PSRAM discipline:** Cardputer ADV has no PSRAM. Never allocate a full-frame RGB565 buffer (240×135×2 ≈ 65 KB). Render + DMA-blit in horizontal **stripes** reusing one small `MALLOC_CAP_DMA` buffer. Waterfall ring (≈11 KB) uses regular heap.
- **Channel space:** `NRF24_FUSION_CH = 126`, channel `ch` = frequency_MHz − 2400 (ch0 = 2400 MHz … ch125 = 2525 MHz).
- **dBm sentinel:** `int8_t` fields use `-128` to mean "no data".
- **Host tests:** assertion-based C `main()` printing `"<name>: ALL PASS"`, built by a `tools/decoder_host_test/build_test_*.sh` gcc script; mirror the style of `test_nrf24_spectrum.c`.
- **Commit trailer:** end each commit body with
  `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`
  `Claude-Session: https://claude.ai/code/session_01K3z6e2uqCZzxoVVpVJJS9P`
- **Build gotcha:** new source files require a CMake **reconfigure** (FAM globs the app dir at configure time); run via PowerShell `Start-Process cmd /c <abs .bat>` per [[mob-cardputer-adv-build-workflow]], never trust a stale log — verify fresh mtime + `SENTINEL_BUILD_OK`.

---

## File structure

Created under `applications/main/nrf24/`:
- `helpers/nrf24_chanmap.{h,c}` — Wi-Fi/BLE↔nRF channel mapping, dBm↔percent, RGB565 byte-swap. **Pure.**
- `helpers/nrf24_heatmap.{h,c}` — heat palette LUT, intensity→color, waterfall ring buffer. **Pure.**
- `helpers/nrf24_fusion_model.{h,c}` — `FusedSpectrum` struct, `FuriMutex`, `nrf24_fuse_intensity`, `nrf24_classify`, snapshot/update. Fusion math is **pure**; mutex glue is device.
- `helpers/nrf24_font5x7.{h,c}` — tiny bitmap font + `draw` into an RGB565 stripe. **Pure.**
- `nrf24_radio_internal.{h,c}` — time-shared Wi-Fi/BLE worker (wraps `nrf24_wifi_scan` + `ble_tracker_hal`). Device.
- `scenes/scene_fusion_spectrum.c` — takeover, 3 workers, render loop, controls, export. Device.

Modified:
- `nrf24_scene.h` / `nrf24_scene_config.h` (or equivalent scene registry) — register `FusionSpectrum` scene.
- `scenes/scene_menu.c` — add private-gated "Fusion Spectrum" menu item.
- `nrf24_app.h` — add `Nrf24ViewFusion` view id if a View is used (the scene uses direct-draw, but still needs a View object to own input; follow `scene_spectrum.c`'s view wiring).

Host tests under `tools/decoder_host_test/`:
- `test_nrf24_chanmap.c` + `build_test_nrf24_chanmap.sh`
- `test_nrf24_heatmap.c` + `build_test_nrf24_heatmap.sh`
- `test_nrf24_fusion.c` + `build_test_nrf24_fusion.sh`
- `test_nrf24_font5x7.c` + `build_test_nrf24_font5x7.sh`

---

## Stage 1 — Color takeover + nRF24 waterfall

### Task 1: Channel mapping + dBm/percent (`nrf24_chanmap`)

**Files:**
- Create: `applications/main/nrf24/helpers/nrf24_chanmap.h`
- Create: `applications/main/nrf24/helpers/nrf24_chanmap.c`
- Test: `tools/decoder_host_test/test_nrf24_chanmap.c`
- Test build: `tools/decoder_host_test/build_test_nrf24_chanmap.sh`

**Interfaces:**
- Produces:
  - `uint16_t nrf24_ch_to_mhz(uint8_t ch)` → `2400 + ch`
  - `int nrf24_wifi_ch_to_nrf(uint8_t wifi_ch)` → nRF center channel for Wi-Fi 1..14, else `-1`
  - `void nrf24_wifi_span(uint8_t wifi_ch, uint8_t* lo, uint8_t* hi)` → center ±11 clamped to [0,125]; on invalid Wi-Fi ch sets `*lo=1,*hi=0` (empty)
  - `int nrf24_ble_adv_to_nrf(uint8_t adv_ch)` → 37→2, 38→26, 39→80, else `-1`
  - `uint8_t nrf24_dbm_to_pct(int8_t dbm)` → maps [−95..−35] dBm to [0..100], clamped
  - `uint16_t nrf24_rgb565_swap(uint16_t c)` → byte-swap for ST7789 SPI order
  - `#define NRF24_FUSION_CH 126`

- [ ] **Step 1: Write the failing test** — `tools/decoder_host_test/test_nrf24_chanmap.c`

```c
#include <assert.h>
#include <stdio.h>
#include "../../applications/main/nrf24/helpers/nrf24_chanmap.h"

int main(void) {
    assert(nrf24_ch_to_mhz(0) == 2400);
    assert(nrf24_ch_to_mhz(125) == 2525);

    /* Wi-Fi centers: ch1=2412->12, ch6=2437->37, ch11=2462->62, ch13=2472->72, ch14=2484->84 */
    assert(nrf24_wifi_ch_to_nrf(1) == 12);
    assert(nrf24_wifi_ch_to_nrf(6) == 37);
    assert(nrf24_wifi_ch_to_nrf(11) == 62);
    assert(nrf24_wifi_ch_to_nrf(13) == 72);
    assert(nrf24_wifi_ch_to_nrf(14) == 84);
    assert(nrf24_wifi_ch_to_nrf(0) == -1);
    assert(nrf24_wifi_ch_to_nrf(15) == -1);

    /* Wi-Fi span = center +-11 clamped */
    uint8_t lo, hi;
    nrf24_wifi_span(6, &lo, &hi);   assert(lo == 26 && hi == 48);
    nrf24_wifi_span(1, &lo, &hi);   assert(lo == 1  && hi == 23);   /* 12-11=1 */
    nrf24_wifi_span(99, &lo, &hi);  assert(lo > hi);                /* invalid -> empty */

    /* BLE adv */
    assert(nrf24_ble_adv_to_nrf(37) == 2);
    assert(nrf24_ble_adv_to_nrf(38) == 26);
    assert(nrf24_ble_adv_to_nrf(39) == 80);
    assert(nrf24_ble_adv_to_nrf(0) == -1);

    /* dBm -> percent */
    assert(nrf24_dbm_to_pct(-100) == 0);
    assert(nrf24_dbm_to_pct(-95) == 0);
    assert(nrf24_dbm_to_pct(-35) == 100);
    assert(nrf24_dbm_to_pct(-20) == 100);
    assert(nrf24_dbm_to_pct(-65) == 50);   /* midpoint */

    /* byte swap */
    assert(nrf24_rgb565_swap(0x1234) == 0x3412);

    printf("nrf24_chanmap: ALL PASS\n");
    return 0;
}
```

- [ ] **Step 2: Write the build script** — `tools/decoder_host_test/build_test_nrf24_chanmap.sh`

```bash
#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
HT=tools/decoder_host_test
gcc -std=gnu11 -g -O1 -Wall -Wextra \
  "$HT/test_nrf24_chanmap.c" \
  applications/main/nrf24/helpers/nrf24_chanmap.c \
  -o "$HT/test_nrf24_chanmap"
echo "BUILD OK"; "$HT/test_nrf24_chanmap"
```

- [ ] **Step 3: Run to verify it fails**

Run: `bash tools/decoder_host_test/build_test_nrf24_chanmap.sh`
Expected: compile error (no `nrf24_chanmap.h`).

- [ ] **Step 4: Write the header** — `nrf24_chanmap.h`

```c
#pragma once
#include <stdint.h>

#define NRF24_FUSION_CH 126

uint16_t nrf24_ch_to_mhz(uint8_t ch);
int      nrf24_wifi_ch_to_nrf(uint8_t wifi_ch);
void     nrf24_wifi_span(uint8_t wifi_ch, uint8_t* lo, uint8_t* hi);
int      nrf24_ble_adv_to_nrf(uint8_t adv_ch);
uint8_t  nrf24_dbm_to_pct(int8_t dbm);
uint16_t nrf24_rgb565_swap(uint16_t c);
```

- [ ] **Step 5: Write the implementation** — `nrf24_chanmap.c`

```c
#include "nrf24_chanmap.h"

uint16_t nrf24_ch_to_mhz(uint8_t ch) {
    return (uint16_t)(2400 + ch);
}

int nrf24_wifi_ch_to_nrf(uint8_t wifi_ch) {
    if(wifi_ch >= 1 && wifi_ch <= 13) return 12 + (wifi_ch - 1) * 5;
    if(wifi_ch == 14) return 84; /* 2484 MHz, non-linear */
    return -1;
}

void nrf24_wifi_span(uint8_t wifi_ch, uint8_t* lo, uint8_t* hi) {
    int c = nrf24_wifi_ch_to_nrf(wifi_ch);
    if(c < 0) { *lo = 1; *hi = 0; return; } /* empty: lo>hi */
    int l = c - 11, h = c + 11;
    if(l < 0) l = 0;
    if(h > NRF24_FUSION_CH - 1) h = NRF24_FUSION_CH - 1;
    *lo = (uint8_t)l; *hi = (uint8_t)h;
}

int nrf24_ble_adv_to_nrf(uint8_t adv_ch) {
    switch(adv_ch) {
    case 37: return 2;
    case 38: return 26;
    case 39: return 80;
    default: return -1;
    }
}

uint8_t nrf24_dbm_to_pct(int8_t dbm) {
    int d = dbm;
    if(d <= -95) return 0;
    if(d >= -35) return 100;
    return (uint8_t)(((d + 95) * 100) / 60);
}

uint16_t nrf24_rgb565_swap(uint16_t c) {
    return (uint16_t)((c >> 8) | (c << 8));
}
```

- [ ] **Step 6: Run to verify it passes**

Run: `bash tools/decoder_host_test/build_test_nrf24_chanmap.sh`
Expected: `nrf24_chanmap: ALL PASS`

- [ ] **Step 7: Commit**

```bash
git add applications/main/nrf24/helpers/nrf24_chanmap.h \
        applications/main/nrf24/helpers/nrf24_chanmap.c \
        tools/decoder_host_test/test_nrf24_chanmap.c \
        tools/decoder_host_test/build_test_nrf24_chanmap.sh
git commit -m "feat(nrf24): channel mapping + dBm/percent helper (host-tested)"
```

---

### Task 2: Heat palette + waterfall ring (`nrf24_heatmap`)

**Files:**
- Create: `applications/main/nrf24/helpers/nrf24_heatmap.h`
- Create: `applications/main/nrf24/helpers/nrf24_heatmap.c`
- Test: `tools/decoder_host_test/test_nrf24_heatmap.c`
- Test build: `tools/decoder_host_test/build_test_nrf24_heatmap.sh`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `void nrf24_heat_build_lut(uint16_t lut[256])` — fills a native-RGB565 blue→cyan→green→yellow→red ramp.
  - `uint16_t nrf24_heat_color(const uint16_t lut[256], uint8_t intensity_0_100)` — indexes `lut` at `intensity*255/100`.
  - `typedef struct { uint8_t* rows; int width; int height; int head; } Nrf24Waterfall;`
  - `void nrf24_wf_init(Nrf24Waterfall* wf, uint8_t* backing, int width, int height)`
  - `void nrf24_wf_push(Nrf24Waterfall* wf, const uint8_t* row)` — copies `width` bytes, advances head.
  - `const uint8_t* nrf24_wf_row(const Nrf24Waterfall* wf, int age)` — `age=0` = newest pushed row.

- [ ] **Step 1: Write the failing test** — `test_nrf24_heatmap.c`

```c
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../applications/main/nrf24/helpers/nrf24_heatmap.h"

static void rgb(uint16_t c, int* r, int* g, int* b) {
    *r = (c >> 11) & 0x1F; *g = (c >> 5) & 0x3F; *b = c & 0x1F;
}

int main(void) {
    uint16_t lut[256];
    nrf24_heat_build_lut(lut);
    int r0,g0,b0, r255,g255,b255;
    rgb(lut[0], &r0,&g0,&b0);
    rgb(lut[255], &r255,&g255,&b255);
    /* cold end is blue-dominant, hot end is red-dominant */
    assert(b0 > r0);
    assert(r255 > b255);
    /* intensity mapping picks lut ends */
    assert(nrf24_heat_color(lut, 0)   == lut[0]);
    assert(nrf24_heat_color(lut, 100) == lut[255]);

    /* waterfall ring: newest-first ordering */
    uint8_t backing[3 * 4];
    Nrf24Waterfall wf;
    nrf24_wf_init(&wf, backing, 4, 3);
    uint8_t a[4] = {1,1,1,1}, b[4] = {2,2,2,2}, c[4] = {3,3,3,3};
    nrf24_wf_push(&wf, a);
    nrf24_wf_push(&wf, b);
    nrf24_wf_push(&wf, c);
    assert(nrf24_wf_row(&wf, 0)[0] == 3); /* newest */
    assert(nrf24_wf_row(&wf, 1)[0] == 2);
    assert(nrf24_wf_row(&wf, 2)[0] == 1);
    uint8_t d[4] = {4,4,4,4};
    nrf24_wf_push(&wf, d);                 /* evicts oldest (a) */
    assert(nrf24_wf_row(&wf, 0)[0] == 4);
    assert(nrf24_wf_row(&wf, 2)[0] == 2);

    printf("nrf24_heatmap: ALL PASS\n");
    return 0;
}
```

- [ ] **Step 2: Write the build script** — `build_test_nrf24_heatmap.sh`

```bash
#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
HT=tools/decoder_host_test
gcc -std=gnu11 -g -O1 -Wall -Wextra \
  "$HT/test_nrf24_heatmap.c" \
  applications/main/nrf24/helpers/nrf24_heatmap.c \
  -o "$HT/test_nrf24_heatmap"
echo "BUILD OK"; "$HT/test_nrf24_heatmap"
```

- [ ] **Step 3: Run to verify it fails**

Run: `bash tools/decoder_host_test/build_test_nrf24_heatmap.sh`
Expected: compile error (no header).

- [ ] **Step 4: Write the header** — `nrf24_heatmap.h`

```c
#pragma once
#include <stdint.h>

void nrf24_heat_build_lut(uint16_t lut[256]);
uint16_t nrf24_heat_color(const uint16_t lut[256], uint8_t intensity_0_100);

typedef struct {
    uint8_t* rows;   /* height * width bytes */
    int width;
    int height;
    int head;        /* index of most-recently-written row */
} Nrf24Waterfall;

void nrf24_wf_init(Nrf24Waterfall* wf, uint8_t* backing, int width, int height);
void nrf24_wf_push(Nrf24Waterfall* wf, const uint8_t* row);
const uint8_t* nrf24_wf_row(const Nrf24Waterfall* wf, int age);
```

- [ ] **Step 5: Write the implementation** — `nrf24_heatmap.c`

```c
#include "nrf24_heatmap.h"
#include <string.h>

static uint16_t pack565(int r, int g, int b) { /* r,g,b in 0..255 */
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void nrf24_heat_build_lut(uint16_t lut[256]) {
    /* 4 segments: blue->cyan->green->yellow->red, 64 entries each */
    for(int i = 0; i < 256; i++) {
        int r, g, b;
        if(i < 64)      { r = 0;            g = i * 4;        b = 255; }       /* blue->cyan */
        else if(i < 128){ r = 0;            g = 255;          b = 255 - (i - 64) * 4; } /* cyan->green */
        else if(i < 192){ r = (i - 128) * 4; g = 255;         b = 0; }        /* green->yellow */
        else            { r = 255;          g = 255 - (i - 192) * 4; b = 0; } /* yellow->red */
        if(g > 255) g = 255;
        lut[i] = pack565(r, g, b);
    }
}

uint16_t nrf24_heat_color(const uint16_t lut[256], uint8_t intensity_0_100) {
    int idx = (intensity_0_100 * 255) / 100;
    if(idx < 0) idx = 0;
    if(idx > 255) idx = 255;
    return lut[idx];
}

void nrf24_wf_init(Nrf24Waterfall* wf, uint8_t* backing, int width, int height) {
    wf->rows = backing;
    wf->width = width;
    wf->height = height;
    wf->head = -1;
    memset(backing, 0, (size_t)width * height);
}

void nrf24_wf_push(Nrf24Waterfall* wf, const uint8_t* row) {
    wf->head = (wf->head + 1) % wf->height;
    memcpy(wf->rows + (size_t)wf->head * wf->width, row, wf->width);
}

const uint8_t* nrf24_wf_row(const Nrf24Waterfall* wf, int age) {
    int idx = ((wf->head - age) % wf->height + wf->height) % wf->height;
    return wf->rows + (size_t)idx * wf->width;
}
```

- [ ] **Step 6: Run to verify it passes**

Run: `bash tools/decoder_host_test/build_test_nrf24_heatmap.sh`
Expected: `nrf24_heatmap: ALL PASS`

- [ ] **Step 7: Commit**

```bash
git add applications/main/nrf24/helpers/nrf24_heatmap.* \
        tools/decoder_host_test/test_nrf24_heatmap.c \
        tools/decoder_host_test/build_test_nrf24_heatmap.sh
git commit -m "feat(nrf24): heat palette LUT + waterfall ring (host-tested)"
```

---

### Task 3: Fusion math + classification (`nrf24_fusion_model` pure core)

**Files:**
- Create: `applications/main/nrf24/helpers/nrf24_fusion_model.h`
- Create: `applications/main/nrf24/helpers/nrf24_fusion_model.c`
- Test: `tools/decoder_host_test/test_nrf24_fusion.c`
- Test build: `tools/decoder_host_test/build_test_nrf24_fusion.sh`

**Interfaces:**
- Consumes: `nrf24_dbm_to_pct` (Task 1), `NRF24_FUSION_CH`.
- Produces:
  - `typedef enum { SRC_NONE=0, SRC_HOPPER=1, SRC_WIFI=2, SRC_BLE=3 } Nrf24SourceClass;`
  - `uint8_t nrf24_fuse_intensity(uint8_t occ, int8_t wifi_dbm, int8_t ble_dbm)`
  - `Nrf24SourceClass nrf24_classify(uint8_t occ, int8_t wifi_dbm, int8_t ble_dbm, uint8_t occ_threshold)`
  - `FusedSpectrum` struct + `nrf24_fusion_model_reset(FusedSpectrum*)` (device users fill arrays under a mutex owned by the scene; the struct itself is defined here).

- [ ] **Step 1: Write the failing test** — `test_nrf24_fusion.c`

```c
#include <assert.h>
#include <stdio.h>
#include "../../applications/main/nrf24/helpers/nrf24_fusion_model.h"

int main(void) {
    /* fuse = max(occ, pct(wifi), pct(ble)); -128 = ignore */
    assert(nrf24_fuse_intensity(30, -128, -128) == 30);
    assert(nrf24_fuse_intensity(30, -35, -128) == 100); /* strong wifi wins */
    assert(nrf24_fuse_intensity(80, -95, -128) == 80);  /* weak wifi -> occ wins */
    assert(nrf24_fuse_intensity(0, -128, -65) == 50);   /* ble midpoint */

    /* classify: wifi > ble > hopper(occ>=thr) > none */
    assert(nrf24_classify(10, -40, -128, 40) == SRC_WIFI);
    assert(nrf24_classify(10, -128, -50, 40) == SRC_BLE);
    assert(nrf24_classify(70, -128, -128, 40) == SRC_HOPPER);
    assert(nrf24_classify(10, -128, -128, 40) == SRC_NONE);
    /* wifi beats ble when both present */
    assert(nrf24_classify(10, -40, -50, 40) == SRC_WIFI);

    printf("nrf24_fusion: ALL PASS\n");
    return 0;
}
```

- [ ] **Step 2: Write the build script** — `build_test_nrf24_fusion.sh`

```bash
#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
HT=tools/decoder_host_test
gcc -std=gnu11 -g -O1 -Wall -Wextra \
  "$HT/test_nrf24_fusion.c" \
  applications/main/nrf24/helpers/nrf24_fusion_model.c \
  applications/main/nrf24/helpers/nrf24_chanmap.c \
  -o "$HT/test_nrf24_fusion"
echo "BUILD OK"; "$HT/test_nrf24_fusion"
```

- [ ] **Step 3: Run to verify it fails**

Run: `bash tools/decoder_host_test/build_test_nrf24_fusion.sh`
Expected: compile error (no header).

- [ ] **Step 4: Write the header** — `nrf24_fusion_model.h`

```c
#pragma once
#include <stdint.h>
#include "nrf24_chanmap.h"

typedef enum { SRC_NONE = 0, SRC_HOPPER = 1, SRC_WIFI = 2, SRC_BLE = 3 } Nrf24SourceClass;

typedef struct {
    uint8_t  occ[NRF24_FUSION_CH];
    uint8_t  peak[NRF24_FUSION_CH];
    int8_t   wifi_dbm[NRF24_FUSION_CH];
    int8_t   ble_dbm[NRF24_FUSION_CH];
    uint8_t  klass[NRF24_FUSION_CH];   /* Nrf24SourceClass */
    uint16_t sweep_count;
    uint32_t wifi_epoch;
    uint32_t ble_epoch;
} FusedSpectrum;

uint8_t nrf24_fuse_intensity(uint8_t occ, int8_t wifi_dbm, int8_t ble_dbm);
Nrf24SourceClass nrf24_classify(uint8_t occ, int8_t wifi_dbm, int8_t ble_dbm, uint8_t occ_threshold);
void nrf24_fusion_model_reset(FusedSpectrum* m);
```

- [ ] **Step 5: Write the implementation** — `nrf24_fusion_model.c`

```c
#include "nrf24_fusion_model.h"
#include <string.h>

uint8_t nrf24_fuse_intensity(uint8_t occ, int8_t wifi_dbm, int8_t ble_dbm) {
    uint8_t v = occ;
    if(wifi_dbm != -128) { uint8_t p = nrf24_dbm_to_pct(wifi_dbm); if(p > v) v = p; }
    if(ble_dbm  != -128) { uint8_t p = nrf24_dbm_to_pct(ble_dbm);  if(p > v) v = p; }
    return v;
}

Nrf24SourceClass nrf24_classify(uint8_t occ, int8_t wifi_dbm, int8_t ble_dbm, uint8_t occ_threshold) {
    if(wifi_dbm != -128) return SRC_WIFI;
    if(ble_dbm  != -128) return SRC_BLE;
    if(occ >= occ_threshold) return SRC_HOPPER;
    return SRC_NONE;
}

void nrf24_fusion_model_reset(FusedSpectrum* m) {
    memset(m->occ, 0, sizeof(m->occ));
    memset(m->peak, 0, sizeof(m->peak));
    memset(m->klass, 0, sizeof(m->klass));
    for(int i = 0; i < NRF24_FUSION_CH; i++) { m->wifi_dbm[i] = -128; m->ble_dbm[i] = -128; }
    m->sweep_count = 0; m->wifi_epoch = 0; m->ble_epoch = 0;
}
```

- [ ] **Step 6: Run to verify it passes**

Run: `bash tools/decoder_host_test/build_test_nrf24_fusion.sh`
Expected: `nrf24_fusion: ALL PASS`

- [ ] **Step 7: Commit**

```bash
git add applications/main/nrf24/helpers/nrf24_fusion_model.* \
        tools/decoder_host_test/test_nrf24_fusion.c \
        tools/decoder_host_test/build_test_nrf24_fusion.sh
git commit -m "feat(nrf24): fusion intensity + source classification (host-tested)"
```

---

### Task 4: Tiny bitmap font (`nrf24_font5x7`)

**Files:**
- Create: `applications/main/nrf24/helpers/nrf24_font5x7.h`
- Create: `applications/main/nrf24/helpers/nrf24_font5x7.c`
- Test: `tools/decoder_host_test/test_nrf24_font5x7.c`
- Test build: `tools/decoder_host_test/build_test_nrf24_font5x7.sh`

**Interfaces:**
- Produces:
  - `const uint8_t* nrf24_font5x7_glyph(char c)` — returns 5 column bytes (bit0=top row … bit6=row7). Unknown/unsupported char → the space glyph (all zero).
  - `void nrf24_font5x7_draw(uint16_t* buf, int buf_w, int buf_h, int x, int y, const char* s, uint16_t color)` — draws string into an RGB565 buffer, clipping to bounds, 1px inter-glyph gap.

**Font source:** Use the public-domain 5×7 column-major glyph set. Provide glyphs for exactly the characters the readout uses (YAGNI): space, `0-9`, `-`, `%`, `.`, `:`, `/`, and uppercase `A B D E F H I L M N O P W Z` (for `MHz`, `dBm`, `WIFI`, `BLE`, `HOP`, `NONE`). Each glyph is 5 bytes. Encode each column so bit N set = pixel at row N is on.

- [ ] **Step 1: Write the failing test** — `test_nrf24_font5x7.c`

```c
#include <assert.h>
#include <stdio.h>
#include "../../applications/main/nrf24/helpers/nrf24_font5x7.h"

int main(void) {
    /* space is blank */
    const uint8_t* sp = nrf24_font5x7_glyph(' ');
    assert(sp[0]==0 && sp[1]==0 && sp[2]==0 && sp[3]==0 && sp[4]==0);
    /* unknown char falls back to space */
    const uint8_t* q = nrf24_font5x7_glyph('~');
    assert(q[0]==0 && q[4]==0);
    /* a supported digit has at least one lit column */
    const uint8_t* z = nrf24_font5x7_glyph('0');
    int lit = 0; for(int i=0;i<5;i++) lit |= z[i];
    assert(lit != 0);

    /* draw clips safely and writes into buffer */
    uint16_t buf[8*8]; for(int i=0;i<64;i++) buf[i]=0;
    nrf24_font5x7_draw(buf, 8, 8, 0, 0, "0", 0xFFFF);
    int any = 0; for(int i=0;i<64;i++) if(buf[i]) any=1;
    assert(any == 1);
    /* off-screen draw does not crash / write */
    nrf24_font5x7_draw(buf, 8, 8, 100, 100, "0", 0xFFFF);

    printf("nrf24_font5x7: ALL PASS\n");
    return 0;
}
```

- [ ] **Step 2: Write the build script** — `build_test_nrf24_font5x7.sh`

```bash
#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
HT=tools/decoder_host_test
gcc -std=gnu11 -g -O1 -Wall -Wextra \
  "$HT/test_nrf24_font5x7.c" \
  applications/main/nrf24/helpers/nrf24_font5x7.c \
  -o "$HT/test_nrf24_font5x7"
echo "BUILD OK"; "$HT/test_nrf24_font5x7"
```

- [ ] **Step 3: Run to verify it fails**

Run: `bash tools/decoder_host_test/build_test_nrf24_font5x7.sh`
Expected: compile error (no header).

- [ ] **Step 4: Write header** — `nrf24_font5x7.h`

```c
#pragma once
#include <stdint.h>

const uint8_t* nrf24_font5x7_glyph(char c);
void nrf24_font5x7_draw(uint16_t* buf, int buf_w, int buf_h, int x, int y,
                        const char* s, uint16_t color);
```

- [ ] **Step 5: Write implementation** — `nrf24_font5x7.c`

Encode each glyph as 5 column bytes (bit0=top). Author the listed characters. Example rows shown for a few; complete the rest following the same 5×7 column convention (each digit/letter drawn in a 5-wide, 7-tall cell). The space glyph MUST be all-zero.

```c
#include "nrf24_font5x7.h"

/* 5 columns per glyph, bitN = row N (0=top). */
typedef struct { char c; uint8_t col[5]; } Glyph;

static const Glyph GLYPHS[] = {
    {' ', {0x00,0x00,0x00,0x00,0x00}},
    {'0', {0x3E,0x51,0x49,0x45,0x3E}},
    {'1', {0x00,0x42,0x7F,0x40,0x00}},
    {'2', {0x42,0x61,0x51,0x49,0x46}},
    {'3', {0x21,0x41,0x45,0x4B,0x31}},
    {'4', {0x18,0x14,0x12,0x7F,0x10}},
    {'5', {0x27,0x45,0x45,0x45,0x39}},
    {'6', {0x3C,0x4A,0x49,0x49,0x30}},
    {'7', {0x01,0x71,0x09,0x05,0x03}},
    {'8', {0x36,0x49,0x49,0x49,0x36}},
    {'9', {0x06,0x49,0x49,0x29,0x1E}},
    {'-', {0x08,0x08,0x08,0x08,0x08}},
    {'%', {0x23,0x13,0x08,0x64,0x62}},
    {'.', {0x00,0x60,0x60,0x00,0x00}},
    {':', {0x00,0x36,0x36,0x00,0x00}},
    {'/', {0x20,0x10,0x08,0x04,0x02}},
    {'A', {0x7E,0x11,0x11,0x11,0x7E}},
    {'B', {0x7F,0x49,0x49,0x49,0x36}},
    {'D', {0x7F,0x41,0x41,0x22,0x1C}},
    {'E', {0x7F,0x49,0x49,0x49,0x41}},
    {'F', {0x7F,0x09,0x09,0x09,0x01}},
    {'H', {0x7F,0x08,0x08,0x08,0x7F}},
    {'I', {0x00,0x41,0x7F,0x41,0x00}},
    {'L', {0x7F,0x40,0x40,0x40,0x40}},
    {'M', {0x7F,0x02,0x0C,0x02,0x7F}},
    {'N', {0x7F,0x04,0x08,0x10,0x7F}},
    {'O', {0x3E,0x41,0x41,0x41,0x3E}},
    {'P', {0x7F,0x09,0x09,0x09,0x06}},
    {'W', {0x7F,0x20,0x18,0x20,0x7F}},
    {'Z', {0x61,0x51,0x49,0x45,0x43}},
};

static const uint8_t SPACE[5] = {0,0,0,0,0};

const uint8_t* nrf24_font5x7_glyph(char c) {
    for(unsigned i = 0; i < sizeof(GLYPHS)/sizeof(GLYPHS[0]); i++)
        if(GLYPHS[i].c == c) return GLYPHS[i].col;
    return SPACE;
}

void nrf24_font5x7_draw(uint16_t* buf, int buf_w, int buf_h, int x, int y,
                        const char* s, uint16_t color) {
    int cx = x;
    for(const char* p = s; *p; p++) {
        const uint8_t* g = nrf24_font5x7_glyph(*p);
        for(int col = 0; col < 5; col++) {
            int px = cx + col;
            if(px < 0 || px >= buf_w) continue;
            for(int row = 0; row < 7; row++) {
                if(!(g[col] & (1 << row))) continue;
                int py = y + row;
                if(py < 0 || py >= buf_h) continue;
                buf[py * buf_w + px] = color;
            }
        }
        cx += 6; /* 5px glyph + 1px gap */
    }
}
```

- [ ] **Step 6: Run to verify it passes**

Run: `bash tools/decoder_host_test/build_test_nrf24_font5x7.sh`
Expected: `nrf24_font5x7: ALL PASS`

- [ ] **Step 7: Commit**

```bash
git add applications/main/nrf24/helpers/nrf24_font5x7.* \
        tools/decoder_host_test/test_nrf24_font5x7.c \
        tools/decoder_host_test/build_test_nrf24_font5x7.sh
git commit -m "feat(nrf24): 5x7 bitmap font for takeover readouts (host-tested)"
```

---

### Task 5: Fusion Spectrum scene — takeover + nRF24 waterfall (Stage 1 device deliverable)

**Files:**
- Create: `applications/main/nrf24/scenes/scene_fusion_spectrum.c`
- Modify: scene registry header (mirror how `scene_spectrum` is registered — grep `Nrf24SceneSpectrum` to find the enum/config macro list) — add `Nrf24SceneFusionSpectrum`, private-gated.
- Modify: `applications/main/nrf24/scenes/scene_menu.c` — add private-gated menu item that runs the scene.
- Modify: `applications/main/nrf24/nrf24_app.h` — add `Nrf24ViewFusion` if the input View is separate (follow `scene_spectrum` view wiring).

**Interfaces:**
- Consumes: `nrf24_hw_acquire/release/probe/deinit/power_down`, `nrf24_hw_listen_rpd_dwell` (existing); `FusedSpectrum` + `nrf24_fusion_model_reset` + `nrf24_fuse_intensity` + `nrf24_classify` (Task 3); heatmap LUT + waterfall (Task 2); `nrf24_font5x7_draw` (Task 4); `nrf24_rgb565_swap` (Task 1); `gui_direct_draw_acquire/release` (`components/gui/gui.h`), `furi_hal_display_get_panel_handle/get_h_res/get_v_res` (`furi_hal_display.h`), `esp_lcd_panel_draw_bitmap`.
- Produces: `void nrf24_app_scene_fusion_spectrum_on_enter/on_event/on_exit(void*)`.

**Reference pattern:** read `applications/main/doom/doom_app.c` and `applications/debug/direct_draw/direct_draw.c` for the acquire → hold SPI bus lock → `esp_lcd_panel_draw_bitmap(stripe)` → release sequence. Read the existing `scene_spectrum.c` (Task context above) for the worker/model idiom.

Structure to implement (Stage 1 = nRF24 occupancy only; Wi-Fi/BLE arrays stay `-128`):

- Context holds: `FusedSpectrum model` + `FuriMutex* lock`; `uint16_t heat_lut[256]`; `Nrf24Waterfall wf` + heap backing (`NRF24_FUSION_CH * WF_ROWS`, `WF_ROWS≈90`); one DMA stripe buffer (`h_res * STRIPE_H` u16, `STRIPE_H≈16`, `MALLOC_CAP_DMA`); `volatile bool stop`; cursor/maxhold/freeze/palette state; `FuriThread* sweep_worker` and `render_worker`.
- **Sweep worker** (copy `scene_spectrum.c:nrf24_spectrum_worker` loop): per channel `rpd = nrf24_hw_listen_rpd_dwell(ch, dwell)`, EMA into `model.occ[ch]` under `lock`; update `peak` if maxhold; `sweep_count++`.
- **Render worker** (~15–20 FPS, `furi_delay_ms(50)`): under `lock` snapshot occ/peak/dbm/klass into locals + compute a fresh waterfall row `row[ch] = fuse_intensity(...)` and `nrf24_wf_push`; unlock; then draw. Draw sequence per stripe band `[y0..y0+STRIPE_H)`:
  1. Clear stripe to background (0x0000).
  2. If band is in the **top half** (bars): for each channel compute `x = ch * h_res / NRF24_FUSION_CH`; bar top `= v_res/2 - inten*(v_res/2)/100`; fill columns `[x, x+bar_w)` from bar-top down using `nrf24_heat_color(heat_lut, inten_at_row)`; draw peak-hold line pixel; glow = one lighter pixel above bar top.
  3. If band is in the **bottom half** (waterfall): row index `age = y - v_res/2`; source `wf_row(&wf, age)`; per channel color `= nrf24_heat_color(heat_lut, row[ch] & 0x3F)`.
  4. Cursor overlay: if `cursor_on`, paint the cursor column white.
  5. Byte-swap every pixel via `nrf24_rgb565_swap` (or build LUT pre-swapped — pick one and be consistent), then `esp_lcd_panel_draw_bitmap(panel, 0, y0, h_res, y0+STRIPE_H, stripe)` while holding the SPI bus lock.
  6. After all bands, draw the readout string (`"2437MHZ 62% -41DBM WIFI"`) — render into a small RGB565 scratch and blit it as its own stripe (Stage 1 shows `MHZ`+`%` only; dBm/class come in later stages).
- **on_enter:** alloc ctx, `nrf24_fusion_model_reset`, `nrf24_heat_build_lut`, alloc waterfall + stripe buffers (NULL-check → clean abort), `nrf24_hw_init`, `gui_direct_draw_acquire`, cache `h_res/v_res`, start both workers.
- **on_event:** handle custom cursor/maxhold/freeze/palette/export events (full set wired in Task 10; Stage 1 wires cursor L/R + exit).
- **on_exit:** `stop=true`, join+free both workers, `nrf24_hw_power_down`+`deinit`, `gui_direct_draw_release`, free buffers, free ctx.

- [ ] **Step 1: Implement the scene** per the structure above (Stage 1 scope: occupancy-only). Register the scene + private-gated menu entry.

- [ ] **Step 2: Re-run all host tests** (guard against helper regressions)

Run: `for t in chanmap heatmap fusion font5x7; do bash tools/decoder_host_test/build_test_nrf24_$t.sh; done`
Expected: four `ALL PASS` lines.

- [ ] **Step 3: Mirror new/changed nrf24 files to the Cardputer tree**

Copy the six new `helpers/*.{c,h}` + `scene_fusion_spectrum.c` + modified `scene_menu.c`/registry/`nrf24_app.h` from `src_m5stick_s3/applications/main/nrf24/` to `src_cardputer_adv/applications/main/nrf24/` (CR-normalize as done for the pingequa graft; preserve `nrf24_hw.c` board deltas — do not overwrite it).

- [ ] **Step 4: Build the Cardputer bin (reconfigure — new files added)**

Run (PowerShell): `Start-Process cmd.exe -ArgumentList '/c',"C:\Users\stone\Downloads\_fzcard_work\NEW_v2\src_cardputer_adv\build_pingequa.bat" -RedirectStandardOutput <log> -RedirectStandardError <err> -WindowStyle Hidden -Wait`
Expected: `SENTINEL_BUILD_OK`; fresh `Flipper-cardputer_adv-merged.bin`; confirm graft compiled: `find build_cardputer_adv -name "scene_fusion_spectrum.c.obj"` non-empty; `grep -ac "MHZ" build_cardputer_adv/furi_esp32.elf` (or a unique readout string) ≥ 1.

- [ ] **Step 5: Flash + on-device verify**

Run: `python -m esptool --chip esp32s3 --port COM12 -b 460800 --after hard-reset write-flash 0x0 <merged.bin>` → "Hash of data verified".
Verify on device: open nRF24 → Fusion Spectrum; expect a **color** bar spectrum + scrolling waterfall reacting to 2.4 GHz activity (wave a phone/BT device near it); cursor L/R moves a white column with a `MHZ`/`%` readout. HackRF ground-truth optional per [[mob-cardputer-adv-build-workflow]].

- [ ] **Step 6: Commit**

```bash
git add applications/main/nrf24/scenes/scene_fusion_spectrum.c \
        applications/main/nrf24/scenes/scene_menu.c \
        applications/main/nrf24/nrf24_app.h
# plus the scene-registry header you modified
git commit -m "feat(nrf24): fusion spectrum scene — color takeover + nRF24 waterfall (private)"
```

---

## Stage 2 — Wi-Fi fusion

### Task 6: Internal-radio worker, Wi-Fi phase (`nrf24_radio_internal`)

**Files:**
- Create: `applications/main/nrf24/nrf24_radio_internal.h`
- Create: `applications/main/nrf24/nrf24_radio_internal.c`
- Modify: `scenes/scene_fusion_spectrum.c` — start/stop the worker; consume its output.

**Interfaces:**
- Consumes: existing `nrf24_wifi_scan(wifi_ap_record_t**, uint16_t*, uint16_t)`; `nrf24_wifi_span` (Task 1); `FusedSpectrum` + `lock`.
- Produces:
  - `typedef struct Nrf24RadioInternal Nrf24RadioInternal;`
  - `Nrf24RadioInternal* nrf24_radio_internal_start(FusedSpectrum* model, FuriMutex* lock);` — spawns the FreeRTOS task (Stage 2: Wi-Fi phase only).
  - `void nrf24_radio_internal_stop(Nrf24RadioInternal*);`

**Behavior (Stage 2):** the task loops: `nrf24_wifi_scan(&recs, &n, 32)`; under `lock`, clear all `wifi_dbm[]` to `-128`, then for each AP `r`, `nrf24_wifi_span(r.primary, &lo, &hi)`; for `ch in [lo,hi]` set `wifi_dbm[ch] = max(existing, r.rssi)` (treat `-128` as "unset"); set `klass[ch] = nrf24_classify(occ[ch], wifi_dbm[ch], ble_dbm[ch], OCC_THR)`; bump `wifi_epoch`. `free(recs)`. Sleep ~1 s, repeat. Honor a stop flag between scans.

- [ ] **Step 1:** Add a pure host test for the AP→channel fill by extracting the spread-merge into a testable helper `void nrf24_wifi_merge(int8_t* wifi_dbm, uint8_t primary, int8_t rssi)` in `nrf24_chanmap.c`; test in `test_nrf24_chanmap.c`:

```c
    int8_t w[NRF24_FUSION_CH];
    for(int i=0;i<NRF24_FUSION_CH;i++) w[i] = -128;
    nrf24_wifi_merge(w, 6, -50);
    assert(w[37] == -50 && w[26] == -50 && w[48] == -50 && w[25] == -128);
    nrf24_wifi_merge(w, 6, -40);            /* stronger overwrites */
    assert(w[37] == -40);
    nrf24_wifi_merge(w, 6, -60);            /* weaker does not */
    assert(w[37] == -40);
```

Add the impl to `nrf24_chanmap.c`:

```c
void nrf24_wifi_merge(int8_t* wifi_dbm, uint8_t primary, int8_t rssi) {
    uint8_t lo, hi; nrf24_wifi_span(primary, &lo, &hi);
    if(lo > hi) return;
    for(uint8_t ch = lo; ch <= hi; ch++)
        if(wifi_dbm[ch] == -128 || rssi > wifi_dbm[ch]) wifi_dbm[ch] = rssi;
}
```
(declare in `nrf24_chanmap.h`.) Run `build_test_nrf24_chanmap.sh` → expect fail, then pass.

- [ ] **Step 2:** Implement `nrf24_radio_internal.c` (Wi-Fi phase) using `xTaskCreatePinnedToCore(..., 6*1024, ..., core 0)` (match `nrf24_wifi_scan`'s task context needs). Use the reference-counted stop pattern from `nrf24_wifi_scan.c` so a stop during an 8 s scan can't use-after-free.

- [ ] **Step 3:** Wire into scene `on_enter`/`on_exit`; render worker already consumes `wifi_dbm[]` via `fuse_intensity`. Add Wi-Fi tick markers at Wi-Fi centers in the bars band.

- [ ] **Step 4:** Mirror to Cardputer tree, reconfigure-build, flash (as Task 5 steps 3–5). Verify Wi-Fi channels light up with real signal strength (stronger/closer AP = hotter/taller); readout now shows `-NNDBM WIFI` on Wi-Fi channels.

- [ ] **Step 5: Commit**

```bash
git add applications/main/nrf24/nrf24_radio_internal.* \
        applications/main/nrf24/helpers/nrf24_chanmap.* \
        applications/main/nrf24/scenes/scene_fusion_spectrum.c \
        tools/decoder_host_test/test_nrf24_chanmap.c
git commit -m "feat(nrf24): Wi-Fi RSSI fusion into spectrum (host-tested merge)"
```

---

## Stage 3 — BLE fusion

### Task 7: Internal-radio worker, BLE phase + hopper classification

**Files:**
- Modify: `applications/main/nrf24/nrf24_radio_internal.c` — add a BLE phase to the cycle.
- Modify: `scenes/scene_fusion_spectrum.c` — BLE tint + markers.

**Interfaces:**
- Consumes: `ble_tracker_hal_start_scan()`, the tracker's device list (`rssi`/`best_rssi`) + `ble_tracker_hal_stop_scan()` (`applications/main/ble_spam/ble_tracker_hal.h`); `nrf24_ble_adv_to_nrf` (Task 1).
- Produces: extends the internal worker; no new public symbols.

**Behavior:** after the Wi-Fi phase and its ~1 s settle, the same task deinits Wi-Fi and runs a BLE scan window (~2 s): `ble_tracker_hal_start_scan()`, wait, read devices, `stop_scan`. BLE advertisers appear on all 3 adv channels; under `lock`, clear `ble_dbm[]` to `-128`, then for each of nRF ch {2,26,80} set `ble_dbm[ch] = max(existing, strongest observed adv rssi)`; recompute `klass` for affected channels; bump `ble_epoch`. Then reclassify hoppers: for every channel, `klass[ch] = nrf24_classify(occ[ch], wifi_dbm[ch], ble_dbm[ch], OCC_THR)` so occupancy with no Wi-Fi/BLE overlap becomes `SRC_HOPPER`.

- [ ] **Step 1:** Extend `nrf24_classify` test coverage in `test_nrf24_fusion.c` for the hopper case adjacent to BLE channels (already covered generically; add: `assert(nrf24_classify(70, -128, -128, 40) == SRC_HOPPER);` present — add a BLE-present-on-adv-channel case `assert(nrf24_classify(70, -128, -55, 40) == SRC_BLE);`). Run → pass (logic already handles it; this locks the contract).

- [ ] **Step 2:** Implement the BLE phase in `nrf24_radio_internal.c`. Ensure Wi-Fi is fully `esp_wifi_deinit`'d before BLE controller init and vice-versa (no simultaneous internal-radio use — this is the time-share contract).

- [ ] **Step 3:** Scene render: source-tint the waterfall — read the class stored in the row's high 2 bits (pack `klass` into the waterfall row byte on push: `row[ch] = (inten & 0x3F) | (klass << 6)`), and shift the heat color toward cool-white (Wi-Fi), magenta (BLE), or leave pure (hopper). Add BLE markers at nRF ch {2,26,80}. Update the readout to show class text (`WIFI`/`BLE`/`HOP`/`NONE`).

- [ ] **Step 4:** Mirror, reconfigure-build, flash. Verify a BLE device (phone advertising / BLE beacon) lights the 3 adv channels magenta with real RSSI; an unknown hopper (e.g., a 2.4 GHz wireless mouse dongle) shows as `HOP`.

- [ ] **Step 5: Commit**

```bash
git add applications/main/nrf24/nrf24_radio_internal.c \
        applications/main/nrf24/scenes/scene_fusion_spectrum.c \
        tools/decoder_host_test/test_nrf24_fusion.c
git commit -m "feat(nrf24): BLE RSSI fusion + hopper classification"
```

---

## Stage 4 — Polish & export

### Task 8: Controls — palette, freeze, max-hold, dwell

**Files:**
- Modify: `scenes/scene_fusion_spectrum.c` — input handling + render flags.

**Interfaces:**
- Consumes: input events from the scene's View (follow `scene_spectrum.c` custom-event wiring); `nrf24_dwell_presets`/`nrf24_dwell_next` (existing spectrum_calc).
- Produces: no new public symbols.

**Behavior:** map Cardputer keys → custom events → render/worker flags: `W` toggle bars⇄waterfall emphasis, `M` max-hold on/off (renderer draws `peak[]` line; off clears peaks), `space` freeze (render worker stops pushing new rows + snapshotting), `P` cycle palette (add a 2nd LUT variant, e.g. "inferno"; store `active_lut` pointer), `D` dwell cycle (sets sweep worker `dwell_us`). Add a pure test for a palette registry accessor if you add one: `const uint16_t* nrf24_palette_get(int idx, uint16_t lut_a[256], uint16_t lut_b[256])` returning A for even, B for odd — trivial, host-test the index wrap.

- [ ] **Step 1:** Implement input → flags; ensure freeze also pauses the internal-radio writes (skip publishing under freeze).
- [ ] **Step 2:** Mirror, build, flash; verify each key behaves; verify freeze halts the waterfall and radios resume cleanly on unfreeze.
- [ ] **Step 3: Commit** `feat(nrf24): fusion spectrum controls (palette/freeze/maxhold/dwell)`

### Task 9: Export — CSV + PNG snapshot

**Files:**
- Modify: `applications/main/nrf24/helpers/nrf24_export.c` (extend) or add `nrf24_export_fusion(const FusedSpectrum*)`.
- Modify: `scenes/scene_fusion_spectrum.c` — `S` key → export.

**Interfaces:**
- Consumes: existing `nrf24_export.c` FlipperFormat/`/ext/apps_data/nrf24/` idiom; the live `FusedSpectrum`; the current rendered stripe path for PNG.
- Produces: `bool nrf24_export_fusion_csv(const FusedSpectrum* m, const char* path);` (host-testable: format one row per channel `ch,mhz,occ,peak,wifi_dbm,ble_dbm,class`).

- [ ] **Step 1:** Add a host test that formats a small `FusedSpectrum` into a CSV string buffer and asserts the header + one row (extract the row-formatting into a pure `int nrf24_fusion_csv_row(char* out, int cap, const FusedSpectrum* m, int ch)`); run fail→pass.
- [ ] **Step 2:** Implement CSV write (reuse `nrf24_export.c` file-open path). PNG: encode the last rendered frame — if a PNG encoder isn't already vendored, write a raw `.565`/BMP instead (simpler, no new dep) and note it; do NOT add a heavy PNG lib (YAGNI). Confirm the chosen format in the commit message.
- [ ] **Step 3:** Mirror, build, flash; press `S`; confirm a file lands in `/ext/apps_data/nrf24/` (pull via WebUI/SD per [[cardputer-adv-bruce-serial]] conventions) with sane values.
- [ ] **Step 4: Commit** `feat(nrf24): fusion spectrum CSV + frame export`

### Task 10: Private-gating verification + finalize

**Files:**
- Verify: all new files compile out cleanly under `PUBLIC_BUILD`.

- [ ] **Step 1:** Confirm every `#include`/registration of the fusion scene + menu entry is inside `#ifndef PUBLIC_BUILD`. If the new helper `.c` files are always-compiled (harmless, pure), that's fine; only the *scene + menu wiring* must gate. Ensure no public symbol references the gated scene.
- [ ] **Step 2:** Build the **public** Cardputer variant (per [[m5sticks3-public-private-build]] — the public env) to prove it compiles with the feature absent. Expected: clean build, no `scene_fusion_spectrum` object, menu count adjusts.
- [ ] **Step 3:** Build + flash the **private** Cardputer bin one final time; full smoke test of all 4 stages.
- [ ] **Step 4:** Update memory: append fusion-spectrum status to `nrf24-pingequa-graft.md` or a new `nrf24-fusion-spectrum.md`, and MEMORY.md index line. Record branch `feat/nrf24-fusion-spectrum`, staged status, HW-test state.
- [ ] **Step 5: Commit** `chore(nrf24): verify public/private gating for fusion spectrum`

---

## Self-review notes (author)

- **Spec coverage:** §2 files → Tasks 1–5 (+6,7 radio); §3 data model → Task 3; §4 workers/scheduler → Tasks 5–7; §5 render pipeline → Task 5 (+2,4); §6 controls/errors/testing → Tasks 5,8; §7 staging → Tasks 5/6/7/8–10; gating → Task 10. No uncovered section.
- **Type consistency:** `FusedSpectrum`, `Nrf24SourceClass` (`SRC_*`), `nrf24_fuse_intensity`, `nrf24_classify`, `nrf24_wifi_span`, `nrf24_wifi_merge`, `Nrf24Waterfall`/`nrf24_wf_*`, `nrf24_heat_*`, `nrf24_font5x7_*`, `nrf24_rgb565_swap`, `nrf24_radio_internal_*` — used consistently across tasks.
- **Known soft spots (verify during impl, not placeholders):** exact scene-registry macro names (grep `Nrf24SceneSpectrum`); exact ble_tracker device-list accessor signature (read `ble_tracker_hal.h`); exact Cardputer `h_res/v_res` (read at runtime); PNG-vs-raw export format decision (Task 9 step 2).
