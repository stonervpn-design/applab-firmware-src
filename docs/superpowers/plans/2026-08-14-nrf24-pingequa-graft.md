# nRF24 PINGEQUA Graft Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Graft PINGEQUA RF Lab's three net-new features (RPD reactive-BLE jammer, spectrum UI upgrade, CSV export + settings) into the existing ESP32 `applications/main/nrf24` app, reusing its tuned `nrf24_hw.c` driver.

**Architecture:** Additive-only. New reactive primitives in `nrf24_hw.c`; a 10th jammer mode in `scene_jam.c`; the spectrum view gains markers/max-hold/cursor/dwell driven by a host-testable pure-logic module; a new `nrf24_export.c` handles CSV + FlipperFormat settings. No new app, no duplicate scenes.

**Tech Stack:** C, ESP32 (M5Stick S3 / Cardputer ADV) Flipper-port firmware (`furi`, `furi_hal_spi`, `furi_hal_gpio`, `gui/canvas`, `storage`, `flipper_format`). Host tests via the existing `tools/decoder_host_test` gcc/WSL harness pattern.

**Spec:** `docs/superpowers/specs/2026-08-14-nrf24-pingequa-graft-design.md` (read it alongside this plan).

## Global Constraints

- **Private-only:** all new offensive surface stays behind `#ifndef PUBLIC_BUILD`. Verify the `nrf24` app is already private-gated; if any new car/attack string literal could leak, gate it.
- **Superset source:** edit `src_m5stick_s3` (the superset). Mirror every change verbatim into `src_cardputer_adv` in the final task; never hand-diverge.
- **No host test for radio/GUI:** reactive timing, RF, and canvas rendering are hardware-bound — their gate is *compiles clean on both boards* + a documented on-device checklist. Only pure logic (marker math, max-hold, dwell cycle, CSV format, settings round-trip) gets host TDD.
- **Firmware build gotcha (from project memory):** build via the repo's PowerShell `.bat` (NOT Bash); delete the build dir first; invoke by absolute path; verify the log's mtime is fresh; never trust a stale `SENTINEL_OK`.
- **nRF24 driver invariants:** register ops run between `nrf24_hw_acquire()`/`nrf24_hw_release()`; every exit path must `nrf24_hw_ce(false)` then `nrf24_hw_power_down()` (no stuck carrier). CE pin = `nrf24_ce` (file-static in `nrf24_hw.c`); PA level 3 = `PA_MAX`.
- **Channel convention:** nRF channel = MHz − 2400. BLE adv 37/38/39 = nRF {2,26,80}; WiFi 1/6/11 = nRF {12,37,62}. Spectrum spans `NRF24_SPECTRUM_CHANNELS` = 80 (0..79).

---

### Task 1: Reactive driver primitives

**Files:**
- Modify: `applications/main/nrf24/nrf24_hw.h` (append 3 prototypes near the other jammer decls)
- Modify: `applications/main/nrf24/nrf24_hw.c` (append 3 functions after `nrf24_hw_jammer_stop`)

**Interfaces:**
- Consumes (file-static, already in `nrf24_hw.c`): `nrf24_ce`, `nrf24_hw_write_reg`, `nrf24_hw_read_reg`, `nrf24_hw_cmd`, `nrf24_negotiate_rf_setup(pref_rate,pa,cont_wave)`, `nrf24_jam_pa_for_supply(pa)`, `furi_hal_spi_bus_handle_nrf24`, `NRF_CMD_W_TX_PAYLOAD`, `NRF_CMD_FLUSH_TX`, `NRF_REG_*`, `NRF_CONFIG_PWR_UP`, `NRF_CONFIG_PRIM_RX`, `esp_rom_delay_us`.
- Produces (used by Task 2):
  - `void nrf24_hw_reactive_setup(uint8_t channel);`
  - `void nrf24_hw_react_to_tx(void);`
  - `void nrf24_hw_react_to_rx(uint8_t channel);`

- [ ] **Step 1: Add prototypes to `nrf24_hw.h`**

Insert after the existing `nrf24_hw_jammer_stop(void);` declaration:

```c
/* --- RPD reactive jammer (listen on RPD, fire CW only when a carrier appears).
 * All three run inside acquire()/release(). CE is the caller's responsibility.
 * Typical loop: reactive_setup(ch); ce(true); wait 130us; poll RPD; on hit ->
 * ce(false); react_to_tx(); ce(true); wait ~430us; ce(false); react_to_rx(next). */
void nrf24_hw_reactive_setup(uint8_t channel);
void nrf24_hw_react_to_tx(void);
void nrf24_hw_react_to_rx(uint8_t channel);
```

- [ ] **Step 2: Implement in `nrf24_hw.c`**

Append after `nrf24_hw_jammer_stop`:

```c
/* RX + pre-loaded TX FIFO so react_to_tx radiates instantly. CE left LOW. */
void nrf24_hw_reactive_setup(uint8_t channel) {
    furi_hal_gpio_write(&nrf24_ce, false);
    nrf24_hw_write_reg(NRF_REG_EN_AA, 0x00);
    nrf24_hw_write_reg(NRF_REG_SETUP_RETR, 0x00);
    nrf24_hw_write_reg(NRF_REG_CONFIG, NRF_CONFIG_PWR_UP | NRF_CONFIG_PRIM_RX);
    nrf24_hw_write_reg(NRF_REG_RF_CH, channel);
    /* normal 2M (CW off) so RPD is meaningful while listening */
    nrf24_negotiate_rf_setup(1, nrf24_jam_pa_for_supply(3), false);
    /* pre-fill TX FIFO with 32x0xFF */
    uint8_t pl[1 + 32];
    pl[0] = NRF_CMD_W_TX_PAYLOAD;
    memset(&pl[1], 0xFF, 32);
    nrf24_hw_cmd(NRF_CMD_FLUSH_TX);
    furi_hal_spi_bus_tx(&furi_hal_spi_bus_handle_nrf24, pl, sizeof(pl), 100);
    esp_rom_delay_us(1500); /* Tpd2stby */
}

/* RX -> CW/TX flip. Caller must have CE LOW; caller raises CE after. */
void nrf24_hw_react_to_tx(void) {
    nrf24_negotiate_rf_setup(1, nrf24_jam_pa_for_supply(3), true); /* CONT_WAVE|PLL */
    uint8_t cfg = nrf24_hw_read_reg(NRF_REG_CONFIG);
    cfg &= (uint8_t)~NRF_CONFIG_PRIM_RX; /* enter TX */
    nrf24_hw_write_reg(NRF_REG_CONFIG, cfg);
}

/* CW/TX -> RX flip + retune. Caller must have CE LOW; caller raises CE after. */
void nrf24_hw_react_to_rx(uint8_t channel) {
    nrf24_negotiate_rf_setup(1, nrf24_jam_pa_for_supply(3), false); /* CW off */
    uint8_t cfg = nrf24_hw_read_reg(NRF_REG_CONFIG);
    cfg |= NRF_CONFIG_PRIM_RX; /* enter RX */
    nrf24_hw_write_reg(NRF_REG_CONFIG, cfg);
    nrf24_hw_write_reg(NRF_REG_RF_CH, channel);
}
```

- [ ] **Step 3: Build the firmware (M5Stick S3 private env)**

Run the repo's private M5Stick S3 build `.bat` per the build-gotcha constraint (delete build dir, absolute path, PowerShell, fresh-log check).
Expected: compiles clean; no warnings referencing `nrf24_hw.c` new symbols.

- [ ] **Step 4: Commit**

```bash
git add applications/main/nrf24/nrf24_hw.c applications/main/nrf24/nrf24_hw.h
git commit -m "feat(nrf24): add RPD reactive jammer driver primitives"
```

---

### Task 2: Reactive BLE jam mode

**Files:**
- Modify: `applications/main/nrf24/views/nrf24_jam_view.h` (`NRF24_JAM_MODE_COUNT` 9 -> 10)
- Modify: `applications/main/nrf24/views/nrf24_jam_view.c` (mode name at index 9; add `nrf24_jam_mode_is_reactive`)
- Modify: `applications/main/nrf24/scenes/scene_jam.c` (reactive worker branch)

**Interfaces:**
- Consumes: Task 1's `nrf24_hw_reactive_setup/react_to_tx/react_to_rx`; existing `nrf24_hw_ce`, `nrf24_hw_read_reg`, `NRF_REG_RPD`, `nrf24_jam_mode_is_flood`.
- Produces: `bool nrf24_jam_mode_is_reactive(int mode);` (declared in `nrf24_jam_view.h`).

- [ ] **Step 1: Bump mode count + declare helper in `nrf24_jam_view.h`**

Change `#define NRF24_JAM_MODE_COUNT 9` to `10`. Add after `nrf24_jam_mode_is_flood`:

```c
bool nrf24_jam_mode_is_reactive(int mode); /* true = RPD reactive BLE (mode 9) */
```

- [ ] **Step 2: Add the name + reactive predicate in `nrf24_jam_view.c`**

Find the mode-name lookup (the `nrf24_jam_mode_name` switch/array) and add index 9 → `"BLE Reactive"`. Then add:

```c
bool nrf24_jam_mode_is_reactive(int mode) {
    return mode == 9;
}
```
(9 is the new last index; Full Band stays 8.) Ensure `nrf24_jam_mode_is_flood(9)` returns false — reactive is neither flood nor plain-CW.

- [ ] **Step 3: Add the reactive branch to the worker in `scene_jam.c`**

Above `jam_worker`, add the reactive iteration helper:

```c
/* One reactive pass over the 3 BLE advertising channels: listen on RPD, and the
 * instant a carrier appears fire a short CW pulse, then retune to the next adv
 * channel. Runs inside the acquired bus. */
static void jam_reactive_pass(JamCtx* ctx) {
    static const uint8_t BLE_ADV[3] = {2, 26, 80};
    for(int k = 0; k < 3 && !ctx->stop && ctx->desired_running &&
                   nrf24_jam_mode_is_reactive(ctx->mode);
        k++) {
        uint8_t ch = BLE_ADV[k];
        nrf24_hw_reactive_setup(ch);
        nrf24_hw_ce(true);
        esp_rom_delay_us(130); /* Tstby2a -> RX */
        for(int t = 0; t < 40 && !ctx->stop && ctx->desired_running; t++) { /* ~2ms window */
            if(nrf24_hw_read_reg(NRF_REG_RPD) & 0x01) {
                nrf24_hw_ce(false);
                nrf24_hw_react_to_tx();
                nrf24_hw_ce(true);
                esp_rom_delay_us(430); /* 130 settle + ~300 CW pulse */
                nrf24_hw_ce(false);
                nrf24_hw_react_to_rx(ch);
                break;
            }
            esp_rom_delay_us(50);
        }
        nrf24_hw_ce(false);
    }
}
```

Add `#include <esp_rom_sys.h>` at the top of `scene_jam.c` if not present (for `esp_rom_delay_us`), and ensure `NRF_REG_RPD` is visible — if `scene_jam.c` cannot see it, add `#define NRF_REG_RPD 0x09` locally with a comment, or call a driver accessor. Prefer: add `uint8_t nrf24_hw_read_rpd_raw(void);` to the driver returning `nrf24_hw_read_reg(0x09) & 0x01` and call that (keeps register defs private to `nrf24_hw.c`). If you add the accessor, declare it in `nrf24_hw.h` and use it here instead of `nrf24_hw_read_reg(NRF_REG_RPD)`.

Then in `jam_worker`, in the `if(active)` block, branch reactive before the existing flood/CW batch:

```c
        if(active) {
            nrf24_hw_acquire();
            if(nrf24_jam_mode_is_reactive(mi)) {
                jam_reactive_pass(ctx);
            } else {
                uint32_t end = furi_get_tick() + (BATCH_MS * hz) / 1000;
                while(!ctx->stop && ctx->desired_running && ctx->mode == mi &&
                      furi_get_tick() < end) {
                    uint8_t ch = m->count ? m->list[i % m->count] : (uint8_t)(i % 84);
                    if(flood) nrf24_hw_flood_burst(ch, 12, true);
                    else nrf24_hw_jammer_set_channel(ch);
                    i++;
                }
            }
            nrf24_hw_release();
            furi_delay_ms(1);
        } else {
```

The start/stop transitions above already call `nrf24_hw_jammer_start_ex(ch0, PA_MAX)` for non-flood modes; that is harmless for reactive (reactive_setup reconfigures fully on first pass) but to avoid a stray carrier, guard the start: when `nrf24_jam_mode_is_reactive(mi)` is true, skip the `jammer_start_ex`/`flood_start_ex` call in the `want && (!active || mi != cur)` block — just mark `active = true`. On the stop side, reactive needs `nrf24_hw_power_down()`; extend the `!want && active` and final-cleanup paths: if the last mode was reactive, call `nrf24_hw_ce(false); nrf24_hw_power_down();` instead of `jammer_stop()`.

- [ ] **Step 4: Build the firmware (M5Stick S3)**

Run the private build `.bat`. Expected: clean compile; `NRF24_JAM_MODE_COUNT` now 10.

- [ ] **Step 5: On-device checklist (deferred, record in commit body)**

Flash M5Stick S3 (COM10). In nRF24 → Jammer, press Down until "BLE Reactive" shows; OK starts it. With a phone advertising BLE nearby + HackRF watching 2402/2426/2480 MHz, confirm CW bursts correlate with adv packets. Exit must leave no carrier (spectrum shows quiet). Effectiveness tuning (window length, pulse width) is expected.

- [ ] **Step 6: Commit**

```bash
git add applications/main/nrf24/views/nrf24_jam_view.h applications/main/nrf24/views/nrf24_jam_view.c applications/main/nrf24/scenes/scene_jam.c
git commit -m "feat(nrf24): add BLE Reactive jammer mode (RPD-triggered CW)"
```

---

### Task 3: Spectrum pure-logic module (host-tested)

**Files:**
- Create: `applications/main/nrf24/helpers/nrf24_spectrum_calc.c`
- Create: `applications/main/nrf24/helpers/nrf24_spectrum_calc.h`
- Create: `tools/decoder_host_test/test_nrf24_spectrum.c`
- Create: `tools/decoder_host_test/build_test_nrf24_spectrum.sh`

**Interfaces:**
- Produces (used by Tasks 4/5):
  - `#define NRF24_DWELL_PRESET_COUNT 5` and `extern const uint16_t nrf24_dwell_presets[NRF24_DWELL_PRESET_COUNT];` (`{130,250,500,1000,2000}`)
  - `uint8_t nrf24_maxhold_update(uint8_t peak, uint8_t level);` → `level>peak?level:peak`
  - `int nrf24_channel_to_x(uint8_t ch, int width, uint8_t channels);` → `(ch*width)/channels`
  - `bool nrf24_channel_is_wifi_marker(uint8_t ch);` → ch∈{12,37,62}
  - `bool nrf24_channel_is_ble_marker(uint8_t ch);` → ch∈{2,26,80}
  - `uint16_t nrf24_channel_to_mhz(uint8_t ch);` → `2400+ch`
  - `uint8_t nrf24_dwell_next(uint8_t idx);` → `(idx+1)%NRF24_DWELL_PRESET_COUNT`

- [ ] **Step 1: Write the failing host test `test_nrf24_spectrum.c`**

```c
#include <assert.h>
#include <stdio.h>
#include "../../applications/main/nrf24/helpers/nrf24_spectrum_calc.h"

int main(void) {
    /* max-hold keeps the peak */
    assert(nrf24_maxhold_update(10, 40) == 40);
    assert(nrf24_maxhold_update(40, 10) == 40);
    /* channel -> x on a 128px canvas of 80 channels */
    assert(nrf24_channel_to_x(0, 128, 80) == 0);
    assert(nrf24_channel_to_x(40, 128, 80) == 64);
    /* markers */
    assert(nrf24_channel_is_ble_marker(2) && nrf24_channel_is_ble_marker(26) && nrf24_channel_is_ble_marker(80));
    assert(!nrf24_channel_is_ble_marker(3));
    assert(nrf24_channel_is_wifi_marker(12) && nrf24_channel_is_wifi_marker(37) && nrf24_channel_is_wifi_marker(62));
    assert(!nrf24_channel_is_wifi_marker(13));
    /* MHz + dwell cycle */
    assert(nrf24_channel_to_mhz(0) == 2400);
    assert(nrf24_channel_to_mhz(80) == 2480);
    assert(nrf24_dwell_presets[0] == 130 && nrf24_dwell_presets[4] == 2000);
    assert(nrf24_dwell_next(4) == 0 && nrf24_dwell_next(0) == 1);
    printf("nrf24_spectrum_calc: ALL PASS\n");
    return 0;
}
```

- [ ] **Step 2: Write the build script `build_test_nrf24_spectrum.sh`**

```bash
#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
HT=tools/decoder_host_test
gcc -std=gnu11 -g -O1 -Wall -Wextra \
  "$HT/test_nrf24_spectrum.c" \
  applications/main/nrf24/helpers/nrf24_spectrum_calc.c \
  -o "$HT/test_nrf24_spectrum"
echo "BUILD OK"; "$HT/test_nrf24_spectrum"
```

- [ ] **Step 3: Run it — expect FAIL (module missing)**

Run (PowerShell): `wsl bash /mnt/.../src_m5stick_s3/tools/decoder_host_test/build_test_nrf24_spectrum.sh`
Expected: compile error — `nrf24_spectrum_calc.h` not found.

- [ ] **Step 4: Write `nrf24_spectrum_calc.h`**

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define NRF24_DWELL_PRESET_COUNT 5
extern const uint16_t nrf24_dwell_presets[NRF24_DWELL_PRESET_COUNT];

uint8_t nrf24_maxhold_update(uint8_t peak, uint8_t level);
int nrf24_channel_to_x(uint8_t ch, int width, uint8_t channels);
bool nrf24_channel_is_wifi_marker(uint8_t ch);
bool nrf24_channel_is_ble_marker(uint8_t ch);
uint16_t nrf24_channel_to_mhz(uint8_t ch);
uint8_t nrf24_dwell_next(uint8_t idx);
```

- [ ] **Step 5: Write `nrf24_spectrum_calc.c`**

```c
#include "nrf24_spectrum_calc.h"

const uint16_t nrf24_dwell_presets[NRF24_DWELL_PRESET_COUNT] = {130, 250, 500, 1000, 2000};

uint8_t nrf24_maxhold_update(uint8_t peak, uint8_t level) {
    return level > peak ? level : peak;
}
int nrf24_channel_to_x(uint8_t ch, int width, uint8_t channels) {
    if(channels == 0) return 0;
    return ((int)ch * width) / (int)channels;
}
bool nrf24_channel_is_wifi_marker(uint8_t ch) {
    return ch == 12 || ch == 37 || ch == 62; /* WiFi 1/6/11 */
}
bool nrf24_channel_is_ble_marker(uint8_t ch) {
    return ch == 2 || ch == 26 || ch == 80; /* BLE adv 37/38/39 */
}
uint16_t nrf24_channel_to_mhz(uint8_t ch) {
    return (uint16_t)(2400 + ch);
}
uint8_t nrf24_dwell_next(uint8_t idx) {
    return (uint8_t)((idx + 1) % NRF24_DWELL_PRESET_COUNT);
}
```

- [ ] **Step 6: Run the test — expect PASS**

Run the build script again. Expected: `nrf24_spectrum_calc: ALL PASS`.

- [ ] **Step 7: Commit**

```bash
git add applications/main/nrf24/helpers/nrf24_spectrum_calc.c applications/main/nrf24/helpers/nrf24_spectrum_calc.h tools/decoder_host_test/test_nrf24_spectrum.c tools/decoder_host_test/build_test_nrf24_spectrum.sh
git commit -m "feat(nrf24): host-tested spectrum calc helpers (markers, maxhold, dwell)"
```

---

### Task 4: Spectrum view — markers + max-hold rendering

**Files:**
- Modify: `applications/main/nrf24/views/nrf24_spectrum_view.h` (extend model)
- Modify: `applications/main/nrf24/views/nrf24_spectrum_view.c` (draw markers + peaks)
- Modify: `applications/main/nrf24/scenes/scene_spectrum.c` (maintain `peaks[]`)

**Interfaces:**
- Consumes: Task 3 helpers (`nrf24_channel_is_wifi_marker/ble_marker`, `nrf24_channel_to_x`, `nrf24_maxhold_update`), existing `NRF24_SPECTRUM_CHANNELS`, `BAR_X`, `BAR_TOP/BOTTOM`.
- Produces: extended `Nrf24SpectrumModel` fields used by Task 5.

- [ ] **Step 1: Extend the model in `nrf24_spectrum_view.h`**

Add to `Nrf24SpectrumModel` (after `levels`):

```c
    uint8_t peaks[NRF24_SPECTRUM_CHANNELS]; /* max-hold */
    bool max_hold;      /* toggle */
    uint8_t cursor_ch;  /* 0..NRF24_SPECTRUM_CHANNELS-1 (Task 5) */
    bool cursor_on;     /* show cursor readout (Task 5) */
    uint16_t dwell_us;  /* current dwell (Task 5) */
```

- [ ] **Step 2: Draw markers + max-hold in `nrf24_spectrum_view.c`**

Add `#include "../helpers/nrf24_spectrum_calc.h"`. Inside `nrf24_spectrum_draw_callback`, after the existing bar loop and before the scale labels, add:

```c
    /* Band markers: WiFi 1/6/11 (solid tick) + BLE adv 37/38/39 (dotted tick) */
    for(int i = 0; i < NRF24_SPECTRUM_CHANNELS; i++) {
        int x = BAR_X(i);
        if(nrf24_channel_is_wifi_marker((uint8_t)i)) {
            canvas_draw_line(canvas, x, BAR_TOP - 2, x, BAR_TOP - 6);
        } else if(nrf24_channel_is_ble_marker((uint8_t)i)) {
            canvas_draw_dot(canvas, x, BAR_TOP - 3);
            canvas_draw_dot(canvas, x, BAR_TOP - 5);
        }
    }
    /* Max-hold overlay: a 1px cap at each peak */
    if(model->max_hold) {
        const int bar_h = BAR_BOTTOM - BAR_TOP;
        for(int i = 0; i < NRF24_SPECTRUM_CHANNELS; i++) {
            int ph = (model->peaks[i] * bar_h * 3) / (125 * 2);
            if(ph > bar_h) ph = bar_h;
            if(ph > 0) canvas_draw_dot(canvas, BAR_X(i), BAR_BOTTOM - ph);
        }
    }
```

- [ ] **Step 3: Maintain `peaks[]` in `scene_spectrum.c`**

Add `#include "../helpers/nrf24_spectrum_calc.h"`. In the worker's model update block, after `memcpy(model->levels, local_levels, ...)`, add:

```c
                if(model->max_hold) {
                    for(int j = 0; j < NRF24_SPECTRUM_CHANNELS; j++)
                        model->peaks[j] = nrf24_maxhold_update(model->peaks[j], local_levels[j]);
                }
```

In both model-init blocks (`on_enter` and the worker start), zero the new fields: `model->max_hold=false; model->cursor_ch=0; model->cursor_on=false; model->dwell_us=250; for(...) model->peaks[i]=0;`.

- [ ] **Step 4: Build the firmware (M5Stick S3)** — clean compile.

- [ ] **Step 5: On-device (deferred):** markers appear above the bars at the WiFi/BLE positions; nothing else changed yet.

- [ ] **Step 6: Commit**

```bash
git add applications/main/nrf24/views/nrf24_spectrum_view.h applications/main/nrf24/views/nrf24_spectrum_view.c applications/main/nrf24/scenes/scene_spectrum.c
git commit -m "feat(nrf24): spectrum band markers + max-hold overlay"
```

---

### Task 5: Spectrum input — cursor, dwell, max-hold toggle, export trigger

**Files:**
- Modify: `applications/main/nrf24/views/nrf24_spectrum_view.c` (input callback → custom events; cursor readout draw)
- Modify: `applications/main/nrf24/scenes/scene_spectrum.c` (handle events; apply dwell to worker)
- Modify: `applications/main/nrf24/nrf24_app.h` (spectrum custom-event enum, if events live app-wide) OR add a local enum in the view header

**Interfaces:**
- Consumes: Task 3 (`nrf24_dwell_next`, `nrf24_dwell_presets`, `nrf24_channel_to_mhz`), Task 4 model fields, Task 6's `nrf24_export_scan` (forward-declared; wired in Task 6).
- Produces: input behavior; a `volatile uint16_t dwell_us` shared with the worker.

- [ ] **Step 1: Define spectrum events** in `nrf24_spectrum_view.h`:

```c
typedef enum {
    Nrf24SpectrumEventCursorLeft = 1,
    Nrf24SpectrumEventCursorRight,
    Nrf24SpectrumEventToggleMaxHold,
    Nrf24SpectrumEventDwellCycle,
    Nrf24SpectrumEventExport,
} Nrf24SpectrumEvent;
```

- [ ] **Step 2: Wire the view input callback** in `nrf24_spectrum_view.c` (replace the stub that returns false):

```c
static bool nrf24_spectrum_input_callback(InputEvent* event, void* context) {
    View* view = context;
    if(event->type == InputTypeShort) {
        if(event->key == InputKeyLeft) { view_dispatch_custom(view, Nrf24SpectrumEventCursorLeft); return true; }
        if(event->key == InputKeyRight) { view_dispatch_custom(view, Nrf24SpectrumEventCursorRight); return true; }
        if(event->key == InputKeyUp) { view_dispatch_custom(view, Nrf24SpectrumEventToggleMaxHold); return true; }
        if(event->key == InputKeyDown) { view_dispatch_custom(view, Nrf24SpectrumEventDwellCycle); return true; }
    } else if(event->type == InputTypeLong && event->key == InputKeyOk) {
        view_dispatch_custom(view, Nrf24SpectrumEventExport); return true;
    }
    return false;
}
```

Use the app's existing custom-event send mechanism: match how `nrf24_jam_view.c` emits events (it calls a stored `ViewDispatcher*`/callback). Mirror that exact pattern here — define a small `view_dispatch_custom(view,e)` static that does what the jam view does, or reuse the jam view's helper if shared. Set the view context to the dispatcher in `nrf24_spectrum_view_alloc` the same way the jam view does. **Board note:** M5Stick S3 delivers only Ok/Up/Down/Back (no Left/Right) — so on that board Down cycles dwell and Up toggles max-hold (both present), and cursor is advanced by a Down-long or is simply unavailable; keep Left/Right handlers for Cardputer. Do not assume Left/Right exist on M5Stick.

- [ ] **Step 3: Draw the cursor readout** in the draw callback (after markers):

```c
    if(model->cursor_on) {
        int cx = BAR_X(model->cursor_ch);
        canvas_draw_line(canvas, cx, BAR_TOP, cx, BAR_BOTTOM);
        char rd[24];
        snprintf(rd, sizeof(rd), "c%u %uMHz", model->cursor_ch, nrf24_channel_to_mhz(model->cursor_ch));
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 6, AlignCenter, AlignTop, rd);
    }
```

- [ ] **Step 4: Handle events** in `scene_spectrum.c` `on_event` (replace the stub). The worker must read dwell from a shared volatile; add `volatile uint16_t dwell_us;` to `Nrf24SpectrumCtx`, init to 250, and change the worker's per-channel listen to honor it (the current `nrf24_hw_listen_rpd` uses a fixed 140us; add `nrf24_hw_listen_rpd_dwell(ch, us)` to the driver, or keep listen_rpd and add an extra `esp_rom_delay_us(ctx->dwell_us)` before reading RPD — simplest: after `nrf24_hw_listen_rpd(ch)` returns, it already toggles CE; instead inline a dwell-aware listen). Implement:

```c
bool nrf24_app_scene_spectrum_on_event(void* context, SceneManagerEvent event) {
    Nrf24App* app = context;
    if(event.type != SceneManagerEventTypeCustom || !g_ctx) return false;
    switch(event.event) {
    case Nrf24SpectrumEventCursorLeft:
        with_view_model(app->spectrum_view, Nrf24SpectrumModel* m, {
            m->cursor_on = true; if(m->cursor_ch) m->cursor_ch--; }, true);
        return true;
    case Nrf24SpectrumEventCursorRight:
        with_view_model(app->spectrum_view, Nrf24SpectrumModel* m, {
            m->cursor_on = true; if(m->cursor_ch < NRF24_SPECTRUM_CHANNELS-1) m->cursor_ch++; }, true);
        return true;
    case Nrf24SpectrumEventToggleMaxHold:
        with_view_model(app->spectrum_view, Nrf24SpectrumModel* m, {
            m->max_hold = !m->max_hold;
            if(!m->max_hold) for(int i=0;i<NRF24_SPECTRUM_CHANNELS;i++) m->peaks[i]=0; }, true);
        return true;
    case Nrf24SpectrumEventDwellCycle: {
        static uint8_t di = 1; /* 250us */
        di = nrf24_dwell_next(di);
        g_ctx->dwell_us = nrf24_dwell_presets[di];
        with_view_model(app->spectrum_view, Nrf24SpectrumModel* m, { m->dwell_us = g_ctx->dwell_us; }, true);
        return true;
    }
    case Nrf24SpectrumEventExport:
        /* wired in Task 6 */
        return true;
    default: return false;
    }
}
```

Add the dwell delay in the worker sweep loop: after each `nrf24_hw_listen_rpd(ch)` (or its dwell-aware variant), the effective per-channel dwell should approximate `g_ctx->dwell_us`. Simplest correct change: replace the fixed 140us inside a new driver call `uint8_t nrf24_hw_listen_rpd_dwell(uint8_t ch, uint16_t us)` (copy of `listen_rpd` with `esp_rom_delay_us(us)` instead of 140) declared in `nrf24_hw.h`; call it with `g_ctx->dwell_us`.

- [ ] **Step 5: Build (M5Stick S3)** — clean compile.

- [ ] **Step 6: On-device (deferred):** Up toggles max-hold, Down cycles dwell (readout updates), Left/Right move cursor on Cardputer.

- [ ] **Step 7: Commit**

```bash
git add applications/main/nrf24/views/nrf24_spectrum_view.c applications/main/nrf24/views/nrf24_spectrum_view.h applications/main/nrf24/scenes/scene_spectrum.c applications/main/nrf24/nrf24_hw.c applications/main/nrf24/nrf24_hw.h
git commit -m "feat(nrf24): spectrum cursor + dwell + max-hold input"
```

---

### Task 6: CSV export + settings persistence (host-tested logic)

**Files:**
- Create: `applications/main/nrf24/helpers/nrf24_export.c`
- Create: `applications/main/nrf24/helpers/nrf24_export.h`
- Create: `tools/decoder_host_test/test_nrf24_export.c`
- Create: `tools/decoder_host_test/build_test_nrf24_export.sh`
- Modify: `applications/main/nrf24/scenes/scene_spectrum.c` (call export on the Export event)
- Modify: `applications/main/nrf24/scenes/scene_jam.c` (session log on exit)

**Interfaces:**
- Produces:
  - `int nrf24_export_format_csv(const uint8_t* levels, uint8_t n, char* out, size_t cap);` — writes `channel,mhz,level\n` rows; returns bytes written (or -1 on overflow). **Pure, host-tested.**
  - `bool nrf24_export_scan(const uint8_t* levels, uint8_t n);` — mkdir `/ext/apps_data/nrf24`, write `scan_<seq>.csv` via Storage. (firmware-only)
  - `bool nrf24_settings_load(uint8_t* jam_mode, uint16_t* dwell_us);` / `bool nrf24_settings_save(uint8_t jam_mode, uint16_t dwell_us);` — FlipperFormat at `/ext/apps_data/nrf24/rflab.conf`, filetype `"NRF24 RF Lab State"`, range-checked with defaults. (firmware-only; the range-check helper is pure + host-tested)
  - `bool nrf24_settings_validate(uint32_t mode, uint32_t dwell, uint8_t* mode_out, uint16_t* dwell_out);` — clamp/reject; returns false→caller uses defaults. **Pure, host-tested.**

- [ ] **Step 1: Write failing host test `test_nrf24_export.c`**

```c
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "../../applications/main/nrf24/helpers/nrf24_export.h"

int main(void) {
    uint8_t lv[3] = {0, 125, 10};
    char buf[128];
    int n = nrf24_export_format_csv(lv, 3, buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "0,2400,0\n"));
    assert(strstr(buf, "1,2401,125\n"));
    assert(strstr(buf, "2,2402,10\n"));
    /* overflow guarded */
    assert(nrf24_export_format_csv(lv, 3, buf, 4) == -1);
    /* settings validation */
    uint8_t m; uint16_t d;
    assert(nrf24_settings_validate(9, 500, &m, &d) && m == 9 && d == 500);
    assert(!nrf24_settings_validate(10, 500, &m, &d));   /* mode out of range (0..9) */
    assert(!nrf24_settings_validate(0, 99999, &m, &d));  /* dwell out of range */
    printf("nrf24_export: ALL PASS\n");
    return 0;
}
```

- [ ] **Step 2: Write `build_test_nrf24_export.sh`** (compile only the pure TUs — guard firmware-only code with `#ifndef NRF24_EXPORT_HOST_TEST`):

```bash
#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
HT=tools/decoder_host_test
gcc -std=gnu11 -g -O1 -Wall -DNRF24_EXPORT_HOST_TEST \
  "$HT/test_nrf24_export.c" \
  applications/main/nrf24/helpers/nrf24_export.c \
  -o "$HT/test_nrf24_export"
echo "BUILD OK"; "$HT/test_nrf24_export"
```

- [ ] **Step 3: Run — expect FAIL** (module missing).

- [ ] **Step 4: Write `nrf24_export.h`**

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

int nrf24_export_format_csv(const uint8_t* levels, uint8_t n, char* out, size_t cap);
bool nrf24_settings_validate(uint32_t mode, uint32_t dwell, uint8_t* mode_out, uint16_t* dwell_out);

#ifndef NRF24_EXPORT_HOST_TEST
bool nrf24_export_scan(const uint8_t* levels, uint8_t n);
bool nrf24_settings_load(uint8_t* jam_mode, uint16_t* dwell_us);
bool nrf24_settings_save(uint8_t jam_mode, uint16_t dwell_us);
#endif
```

- [ ] **Step 5: Write `nrf24_export.c`** — pure logic always compiled; firmware I/O behind the guard:

```c
#include "nrf24_export.h"
#include <stdio.h>

int nrf24_export_format_csv(const uint8_t* levels, uint8_t n, char* out, size_t cap) {
    size_t used = 0;
    for(uint8_t i = 0; i < n; i++) {
        char row[24];
        int r = snprintf(row, sizeof(row), "%u,%u,%u\n", i, 2400u + i, levels[i]);
        if(r < 0 || used + (size_t)r >= cap) return -1;
        for(int k = 0; k < r; k++) out[used++] = row[k];
    }
    out[used] = 0;
    return (int)used;
}

bool nrf24_settings_validate(uint32_t mode, uint32_t dwell, uint8_t* mode_out, uint16_t* dwell_out) {
    if(mode > 9) return false;                 /* 10 jammer modes: 0..9 */
    if(dwell < 100 || dwell > 4000) return false;
    *mode_out = (uint8_t)mode;
    *dwell_out = (uint16_t)dwell;
    return true;
}

#ifndef NRF24_EXPORT_HOST_TEST
#include <furi.h>
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>
#define NRF24_DIR  "/ext/apps_data/nrf24"
#define NRF24_CONF NRF24_DIR "/rflab.conf"
#define NRF24_FILETYPE "NRF24 RF Lab State"

bool nrf24_export_scan(const uint8_t* levels, uint8_t n) {
    Storage* st = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(st, NRF24_DIR);
    static uint32_t seq = 0; seq++;
    FuriString* path = furi_string_alloc();
    furi_string_printf(path, NRF24_DIR "/scan_%03lu.csv", (unsigned long)seq);
    File* f = storage_file_alloc(st);
    bool ok = false;
    if(storage_file_open(f, furi_string_get_cstr(path), FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(f, "channel,mhz,level\n", 18);
        char row[24];
        for(uint8_t i = 0; i < n; i++) {
            int r = snprintf(row, sizeof(row), "%u,%u,%u\n", i, 2400u + i, levels[i]);
            if(r > 0) storage_file_write(f, row, (size_t)r);
        }
        ok = true;
    }
    storage_file_close(f);
    storage_file_free(f);
    furi_string_free(path);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

bool nrf24_settings_load(uint8_t* jam_mode, uint16_t* dwell_us) {
    Storage* st = furi_record_open(RECORD_STORAGE);
    FlipperFormat* ff = flipper_format_file_alloc(st);
    FuriString* t = furi_string_alloc();
    bool ok = false;
    uint32_t ver = 0, mode = 0, dwell = 0;
    do {
        if(!flipper_format_file_open_existing(ff, NRF24_CONF)) break;
        if(!flipper_format_read_header(ff, t, &ver)) break;
        if(furi_string_cmp_str(t, NRF24_FILETYPE) != 0) break;
        if(!flipper_format_read_uint32(ff, "Mode", &mode, 1)) break;
        if(!flipper_format_read_uint32(ff, "Dwell", &dwell, 1)) break;
        ok = nrf24_settings_validate(mode, dwell, jam_mode, dwell_us);
    } while(0);
    if(!ok) { *jam_mode = 0; *dwell_us = 250; }
    furi_string_free(t);
    flipper_format_free(ff);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

bool nrf24_settings_save(uint8_t jam_mode, uint16_t dwell_us) {
    Storage* st = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(st, NRF24_DIR);
    FlipperFormat* ff = flipper_format_file_alloc(st);
    bool ok = false;
    uint32_t v;
    do {
        if(!flipper_format_file_open_always(ff, NRF24_CONF)) break;
        if(!flipper_format_write_header_cstr(ff, NRF24_FILETYPE, 1)) break;
        v = jam_mode; if(!flipper_format_write_uint32(ff, "Mode", &v, 1)) break;
        v = dwell_us; if(!flipper_format_write_uint32(ff, "Dwell", &v, 1)) break;
        ok = true;
    } while(0);
    flipper_format_free(ff);
    furi_record_close(RECORD_STORAGE);
    return ok;
}
#endif
```

- [ ] **Step 6: Run host test — expect PASS** (`nrf24_export: ALL PASS`).

- [ ] **Step 7: Wire export into `scene_spectrum.c`** — in the `Nrf24SpectrumEventExport` case, snapshot levels and call `nrf24_export_scan`:

```c
    case Nrf24SpectrumEventExport: {
        uint8_t snap[NRF24_SPECTRUM_CHANNELS];
        with_view_model(app->spectrum_view, Nrf24SpectrumModel* m, {
            memcpy(snap, m->levels, NRF24_SPECTRUM_CHANNELS); }, false);
        nrf24_export_scan(snap, NRF24_SPECTRUM_CHANNELS);
        return true;
    }
```
Add `#include "../helpers/nrf24_export.h"`. Load settings in `on_enter` (`nrf24_settings_load(&mode,&dwell)`, apply dwell to `g_ctx->dwell_us`); save on `on_exit`.

- [ ] **Step 8: Wire jammer settings** in `scene_jam.c`: `nrf24_settings_load` in `on_enter` to restore the last mode; `nrf24_settings_save(g_ctx->mode, <dwell unchanged>)` in `on_exit`. (Session-log CSV is optional; if included, append a one-line file the same way as scan export.)

- [ ] **Step 9: Build firmware (M5Stick S3)** — clean compile with the new TU in the app's sources (confirm the app's build picks up `helpers/*.c`; if the build lists sources explicitly, add `nrf24_export.c` and `nrf24_spectrum_calc.c`).

- [ ] **Step 10: Commit**

```bash
git add applications/main/nrf24/helpers/nrf24_export.c applications/main/nrf24/helpers/nrf24_export.h tools/decoder_host_test/test_nrf24_export.c tools/decoder_host_test/build_test_nrf24_export.sh applications/main/nrf24/scenes/scene_spectrum.c applications/main/nrf24/scenes/scene_jam.c
git commit -m "feat(nrf24): CSV scan export + settings persistence (host-tested logic)"
```

---

### Task 7: Cardputer mirror + dual-board verification

**Files:**
- Mirror all Task 1–6 changes into `src_cardputer_adv` (same relative paths).

**Interfaces:** none new — byte-identical logic across trees.

- [ ] **Step 1: Copy each changed/created file** from `src_m5stick_s3` to the matching path in `src_cardputer_adv`. Diff to confirm the only differences are pre-existing board deltas (do NOT overwrite board-specific pin/`board.h` files). List the files: the 2 driver files, 3 jam files, 3 spectrum files, 2 calc helpers, 2 export helpers.

- [ ] **Step 2: Build M5Stick S3 (private env)** — clean, per the build gotcha.

- [ ] **Step 3: Build Cardputer ADV (private env)** — clean.

- [ ] **Step 4: Run both host tests** to confirm the shared logic still passes:

```bash
wsl bash .../tools/decoder_host_test/build_test_nrf24_spectrum.sh
wsl bash .../tools/decoder_host_test/build_test_nrf24_export.sh
```
Expected: both print `ALL PASS`.

- [ ] **Step 5: Public-build guard check** — confirm a PUBLIC build still compiles and the `nrf24` app (if public-excluded) is unaffected; verify no new car/attack literal leaked (the existing `verify_public_no_car` gate stays green).

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(nrf24): mirror PINGEQUA graft to Cardputer + dual-board build"
```

- [ ] **Step 7: On-device HW pass (deferred, user)** — flash M5Stick S3 (COM10) + Cardputer ADV (COM12); verify reactive jam (HackRF), spectrum markers/max-hold/cursor/dwell, CSV export readback, and settings persistence across app restart. Record results; expect reactive-timing tuning.

---

## Self-Review

**Spec coverage:** G1 reactive jammer → Tasks 1–2. G2 spectrum UI (markers/max-hold → Task 4; cursor/dwell → Tasks 3+5) → covered. G3 export+settings → Task 6. Board split (markers/max-hold everywhere; cursor/dwell Cardputer-full/M5Stick-preset) → Task 5 Step 2 board note. Private-gating + superset→Cardputer mirror → Global Constraints + Task 7. Non-goals respected (no new app, no `pq_chip_arbiter`/`pq_nrf24`/CC1101 port). All spec sections map to a task.

**Placeholder scan:** every code step contains real code; build/on-device gates are explicit. The one soft spot — "mirror the jam view's custom-event send mechanism" (Task 5 Step 2) — is deliberate: it points the executor at the exact existing pattern to copy rather than inventing a signature; acceptable because the concrete behavior (which key → which event) is fully specified.

**Type consistency:** `nrf24_hw_reactive_setup/react_to_tx/react_to_rx` used identically in Tasks 1–2. `Nrf24SpectrumModel` fields added in Task 4 and consumed in Task 5 match. `nrf24_export_format_csv`/`nrf24_settings_validate` signatures match between test (Task 6 Step 1), header (Step 4), and impl (Step 5). Mode range 0..9 consistent (Task 2 count=10 ↔ Task 6 validate `mode>9`). Dwell presets `{130,250,500,1000,2000}` consistent (Task 3 ↔ Task 5).
