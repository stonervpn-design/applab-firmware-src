# Modulation Auto-Detect Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** After the Sub-GHz frequency analyzer locks a frequency, classify OOK vs FSK from RSSI dynamics, show it, and auto-select the AM650/FM476 preset for the next Read.

**Architecture:** One new isolated detector unit (a pure RSSI-window classifier + a thin hardware sampler) plus three small wirings: the analyzer worker runs the sampler on lock, the analyzer view shows the result, and the analyzer scene stores the matching preset index. `signal_fingerprint` already reads modulation from the preset name (`subghz_scene_analyze_mod_of`), so no fingerprint code changes.

**Tech Stack:** C11, ESP-IDF v5.4.1, the existing Sub-GHz app (`applications/main/subghz`), the CC1101 `furi_hal_subghz` HAL. Host tests are plain C compiled with `gcc` (pattern: `tests/test_hitag2_cipher.c` `#include`s the real `.c`).

## Global Constraints

- **No `#ifndef PUBLIC_BUILD` guard** — this is a general capture-quality feature (helps every signal, not car-specific); it ships in all variants.
- **Land in BOTH source trees** — apply every change in `src_m5stick_s3` (canonical) and mirror to `src_cardputer_adv` (documented drift risk). Task 6 covers this.
- **App helpers auto-compile** — files under `applications/main/subghz/helpers/` are globbed into the `esp32_fam_app_subghz` target; new `.c` files need no CMakeLists edit.
- **Detector thresholds are starting values** — the named constants in `subghz_mod_classify.c` require one on-hardware tuning pass against real OOK and FSK captures. This is expected and flagged; do not treat unlanded HW tuning as a blocker to merging the (host-tested) logic.
- **Radio ownership** — `subghz_mod_detect_run()` only runs inside the analyzer worker thread, which already owns the radio and has it tuned in the OOK-650 wide config.
- **C style** — match surrounding files: 4-space indent, `snake_case`, `furi_*` APIs, no dynamic allocation in the classifier.

---

## File structure

- Create `applications/main/subghz/helpers/subghz_mod_detect.h` — struct `SubGhzModDetect` + two function declarations.
- Create `applications/main/subghz/helpers/subghz_mod_classify.c` — PURE classifier (no `furi_hal`), host-tested.
- Create `applications/main/subghz/helpers/subghz_mod_detect.c` — HW sampler (`furi_hal_subghz` RSSI loop).
- Create `tests/test_subghz_mod_classify.c` — host unit test for the classifier.
- Modify `applications/main/subghz/helpers/subghz_frequency_analyzer_worker.h` / `.c` — extend the pair callback and run detection on lock.
- Modify `applications/main/subghz/views/subghz_frequency_analyzer.h` / `.c` — carry + draw the modulation.
- Modify `applications/main/subghz/scenes/subghz_scene_frequency_analyzer.c` — receive the mod and set `last_settings->preset_index` on tune-to-Read.

No change to `subghz_scene_analyze.c` (its `subghz_scene_analyze_mod_of` already maps `"AM"→SIG_MOD_AM`, `"FM"→SIG_MOD_FM`).

---

### Task 1: Pure RSSI classifier + host test

**Files:**
- Create: `applications/main/subghz/helpers/subghz_mod_detect.h`
- Create: `applications/main/subghz/helpers/subghz_mod_classify.c`
- Test: `tests/test_subghz_mod_classify.c`

**Interfaces:**
- Consumes: `SignalMod` (`SIG_MOD_UNK/AM/FM`) from `signal_fingerprint.h` (pure header, host-safe).
- Produces: `SubGhzModDetect` struct and `SubGhzModDetect subghz_mod_classify(const int8_t* rssi_dbm, size_t count)`.

- [ ] **Step 1: Write the header**

Create `subghz_mod_detect.h`:
```c
#pragma once
#include "signal_fingerprint.h" // SignalMod
#include <stdint.h>
#include <stddef.h>

typedef struct {
    SignalMod mod;        // SIG_MOD_AM (OOK) | SIG_MOD_FM (FSK) | SIG_MOD_UNK
    uint8_t   confidence; // 0..100
    int8_t    floor_dbm;  // low-percentile RSSI of the window
    int8_t    peak_dbm;   // high-percentile RSSI of the window
} SubGhzModDetect;

// PURE (subghz_mod_classify.c): classify a window of RSSI samples in dBm.
SubGhzModDetect subghz_mod_classify(const int8_t* rssi_dbm, size_t count);

// HW (subghz_mod_detect.c): tune to freq, sample RSSI, call subghz_mod_classify().
SubGhzModDetect subghz_mod_detect_run(uint32_t frequency_hz);
```

- [ ] **Step 2: Write the failing test**

Create `tests/test_subghz_mod_classify.c`:
```c
#include "../applications/main/subghz/helpers/subghz_mod_classify.c"
#include <assert.h>
#include <stdio.h>

int main(void) {
    // OOK: bimodal, carrier on/off -> big swing between floor and peak.
    int8_t ook[200];
    for(int i = 0; i < 200; i++) ook[i] = (i % 2) ? -40 : -100;
    SubGhzModDetect a = subghz_mod_classify(ook, 200);
    assert(a.mod == SIG_MOD_AM);
    assert(a.confidence > 0);

    // FSK: steady near peak (carrier always on), tiny swing, high level.
    int8_t fsk[200];
    for(int i = 0; i < 200; i++) fsk[i] = (int8_t)(-45 + (i % 3 - 1)); // -46..-44
    SubGhzModDetect b = subghz_mod_classify(fsk, 200);
    assert(b.mod == SIG_MOD_FM);

    // Noise only: steady but low level -> no real signal.
    int8_t noise[200];
    for(int i = 0; i < 200; i++) noise[i] = (int8_t)(-100 + (i % 2)); // -100..-99
    SubGhzModDetect c = subghz_mod_classify(noise, 200);
    assert(c.mod == SIG_MOD_UNK);

    // Too few samples -> UNK.
    int8_t few[8] = {-40, -100, -40, -100, -40, -100, -40, -100};
    assert(subghz_mod_classify(few, 8).mod == SIG_MOD_UNK);

    printf("test_subghz_mod_classify OK\n");
    return 0;
}
```

- [ ] **Step 3: Run the test, verify it FAILS**

Run: `gcc -Wall -Werror tests/test_subghz_mod_classify.c -o /tmp/tmod && /tmp/tmod`
Expected: FAIL — `subghz_mod_classify.c: No such file` (undefined). (If no `gcc` is available in your environment, verify the logic with an equivalent Python port of the classifier first, then still land the C for the executor's CI.)

- [ ] **Step 4: Write the classifier**

Create `subghz_mod_classify.c`:
```c
#include "subghz_mod_detect.h"

// Starting thresholds — tune on hardware (see Global Constraints).
#define SMD_FLOOR_PCTL      15   // low percentile = window floor
#define SMD_PEAK_PCTL       90   // high percentile = window peak
#define SMD_SWING_MIN_DB    12   // >= this floor->peak swing => OOK (bimodal)
#define SMD_FM_SWING_MAX_DB  8   // <= this swing => steady carrier (FSK)
#define SMD_MIN_PEAK_DBM   (-90) // peak below this => no real signal => UNK
#define SMD_MIN_SAMPLES     16

static int8_t smd_percentile(const uint16_t hist[256], size_t total, uint8_t pctl) {
    size_t target = (total * pctl) / 100;
    size_t cum = 0;
    for(int v = 0; v < 256; v++) {
        cum += hist[v];
        if(cum >= target) return (int8_t)(v - 128);
    }
    return 0;
}

SubGhzModDetect subghz_mod_classify(const int8_t* s, size_t n) {
    SubGhzModDetect r = {SIG_MOD_UNK, 0, 0, 0};
    if(!s || n < SMD_MIN_SAMPLES) return r;

    uint16_t hist[256] = {0};
    for(size_t i = 0; i < n; i++) hist[(uint8_t)((int)s[i] + 128)]++;

    int8_t floor = smd_percentile(hist, n, SMD_FLOOR_PCTL);
    int8_t peak = smd_percentile(hist, n, SMD_PEAK_PCTL);
    r.floor_dbm = floor;
    r.peak_dbm = peak;
    int swing = (int)peak - (int)floor;

    if((int)peak < SMD_MIN_PEAK_DBM) return r; // no real signal present -> UNK

    if(swing >= SMD_SWING_MIN_DB) {
        r.mod = SIG_MOD_AM; // OOK: carrier keyed on/off
        int c = 45 + (swing - SMD_SWING_MIN_DB) * 3;
        r.confidence = (uint8_t)(c > 100 ? 100 : c);
    } else if(swing <= SMD_FM_SWING_MAX_DB) {
        r.mod = SIG_MOD_FM; // FSK: carrier steady, only frequency shifts
        int c = 45 + (SMD_FM_SWING_MAX_DB - swing) * 5 + ((int)peak - SMD_MIN_PEAK_DBM);
        r.confidence = (uint8_t)(c > 100 ? 100 : (c < 0 ? 0 : c));
    } else {
        r.mod = SIG_MOD_UNK; // ambiguous middle
        r.confidence = 20;
    }
    return r;
}
```

- [ ] **Step 5: Run the test, verify it PASSES**

Run: `gcc -Wall -Werror tests/test_subghz_mod_classify.c -o /tmp/tmod && /tmp/tmod`
Expected: PASS — prints `test_subghz_mod_classify OK`.

- [ ] **Step 6: Commit**

```bash
git add applications/main/subghz/helpers/subghz_mod_detect.h \
        applications/main/subghz/helpers/subghz_mod_classify.c \
        tests/test_subghz_mod_classify.c
git commit -m "feat(subghz): pure RSSI OOK/FSK modulation classifier + host test"
```

---

### Task 2: Hardware RSSI sampler

**Files:**
- Create: `applications/main/subghz/helpers/subghz_mod_detect.c`

**Interfaces:**
- Consumes: `subghz_mod_classify()` (Task 1); `furi_hal_subghz_set_frequency()`, `furi_hal_subghz_get_rssi()`, `furi_hal_subghz_rx()`, `furi_hal_subghz_load_custom_preset`/`load_preset` and `furi_delay_us()` from the HAL (confirm exact names in `components/furi_hal/furi_hal_subghz.h`).
- Produces: `SubGhzModDetect subghz_mod_detect_run(uint32_t frequency_hz)`.

- [ ] **Step 1: Write the sampler**

Create `subghz_mod_detect.c`:
```c
#include "subghz_mod_detect.h"
#include <furi_hal.h>

#define SMD_WINDOW_SAMPLES 200
#define SMD_SAMPLE_GAP_US  50   // ~20 kHz sampling of the amplitude envelope

// Caller (analyzer worker) already loaded the OOK-650 wide config and owns the
// radio. We (re)tune to freq, enter RX, sample RSSI, and classify.
SubGhzModDetect subghz_mod_detect_run(uint32_t frequency_hz) {
    int8_t samples[SMD_WINDOW_SAMPLES];
    furi_hal_subghz_set_frequency(frequency_hz);
    furi_hal_subghz_rx();
    for(size_t i = 0; i < SMD_WINDOW_SAMPLES; i++) {
        float rssi = furi_hal_subghz_get_rssi();
        if(rssi > 0.f) rssi = 0.f;
        if(rssi < -128.f) rssi = -128.f;
        samples[i] = (int8_t)rssi;
        furi_delay_us(SMD_SAMPLE_GAP_US);
    }
    return subghz_mod_classify(samples, SMD_WINDOW_SAMPLES);
}
```

- [ ] **Step 2: Verify it compiles (build the app)**

Run: `cmd.exe /c build_m5stick_s3.bat` then confirm `build_m5stick_s3.log` reaches `SENTINEL_OK`.
Expected: builds clean; `subghz_mod_detect.c.obj` appears under `build_m5stick_s3/esp-idf/main/.../helpers/`. If a HAL name differs (e.g. `furi_hal_subghz_rx` vs a device-layer call), fix to the real symbol from `furi_hal_subghz.h` and rebuild.

- [ ] **Step 3: Commit**

```bash
git add applications/main/subghz/helpers/subghz_mod_detect.c
git commit -m "feat(subghz): hardware RSSI sampler for modulation detect"
```

---

### Task 3: Run detection on lock (analyzer worker)

**Files:**
- Modify: `applications/main/subghz/helpers/subghz_frequency_analyzer_worker.h` (pair-callback signature)
- Modify: `applications/main/subghz/helpers/subghz_frequency_analyzer_worker.c` (call `subghz_mod_detect_run` on lock; pass through)

**Interfaces:**
- Consumes: `subghz_mod_detect_run()` (Task 2).
- Produces: extended callback `SubGhzFrequencyAnalyzerWorkerPairCallback = void (*)(void* context, uint32_t frequency, float rssi, bool signal, SignalMod mod, uint8_t confidence)`.

- [ ] **Step 1: Extend the callback typedef**

In `subghz_frequency_analyzer_worker.h`, add `#include "signal_fingerprint.h"` and change the typedef to:
```c
typedef void (*SubGhzFrequencyAnalyzerWorkerPairCallback)(
    void* context,
    uint32_t frequency,
    float rssi,
    bool signal,
    SignalMod mod,
    uint8_t confidence);
```

- [ ] **Step 2: Run detection and pass it through**

In `subghz_frequency_analyzer_worker.c`: add `#include "subghz_mod_detect.h"`. At the point where the worker currently invokes `instance->pair_callback(instance->context, frequency, rssi, signal)` after a lock, first compute:
```c
SubGhzModDetect md = {SIG_MOD_UNK, 0, 0, 0};
if(signal && frequency > 0) {
    md = subghz_mod_detect_run(frequency);
}
```
then call `instance->pair_callback(instance->context, frequency, rssi, signal, md.mod, md.confidence);`. Leave the non-locked / `signal == false` calls passing `SIG_MOD_UNK, 0`.

- [ ] **Step 3: Verify it compiles**

Run: `cmd.exe /c build_m5stick_s3.bat` → expect a **compile error** in `subghz_scene_frequency_analyzer.c` (the callback now takes two more args). That is expected and fixed in Task 5. To verify Task 3 in isolation, confirm the worker `.c`/`.h` themselves compile (no errors originating in the worker file); the scene error is the Task-5 dependency.

- [ ] **Step 4: Commit**

```bash
git add applications/main/subghz/helpers/subghz_frequency_analyzer_worker.h \
        applications/main/subghz/helpers/subghz_frequency_analyzer_worker.c
git commit -m "feat(subghz): analyzer worker runs modulation detect on lock"
```

---

### Task 4: Carry + draw the modulation (analyzer view)

**Files:**
- Modify: `applications/main/subghz/views/subghz_frequency_analyzer.h` (setter decl)
- Modify: `applications/main/subghz/views/subghz_frequency_analyzer.c` (model field, setter, draw)

**Interfaces:**
- Consumes: `SignalMod`.
- Produces: `void subghz_frequency_analyzer_set_mod(SubGhzFrequencyAnalyzer* instance, SignalMod mod, uint8_t confidence)`.

- [ ] **Step 1: Add model fields**

In `subghz_frequency_analyzer.c`, in the `SubGhzFrequencyAnalyzerModel` struct (the `typedef struct { ... }` around line 42), add:
```c
    SignalMod mod;
    uint8_t mod_confidence;
```
Add `#include "../helpers/signal_fingerprint.h"` at the top if not already visible.

- [ ] **Step 2: Add the setter**

Add to `subghz_frequency_analyzer.c` (and declare in the `.h`):
```c
void subghz_frequency_analyzer_set_mod(
    SubGhzFrequencyAnalyzer* instance, SignalMod mod, uint8_t confidence) {
    furi_assert(instance);
    with_view_model(
        instance->view, SubGhzFrequencyAnalyzerModel * model,
        { model->mod = mod; model->mod_confidence = confidence; }, true);
}
```

- [ ] **Step 3: Draw it**

In `subghz_frequency_analyzer_draw()` (around line 156), where the locked frequency is drawn, add a modulation label. Confidence gate `#define`d near the top of the file: `#define SMD_CONF_SHOW 40`. Render:
```c
if(model->locked && model->mod != SIG_MOD_UNK) {
    const char* lbl = model->mod == SIG_MOD_AM
        ? (model->mod_confidence >= SMD_CONF_SHOW ? "OOK" : "OOK?")
        : (model->mod_confidence >= SMD_CONF_SHOW ? "FSK" : "FSK?");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 96, 24, lbl); // place near the frequency readout; adjust x/y to layout
}
```

- [ ] **Step 4: Verify it compiles** (still expect the Task-5 scene error)

Run: `cmd.exe /c build_m5stick_s3.bat` — the view file must compile; the remaining error is only the scene callback arity (Task 5).

- [ ] **Step 5: Commit**

```bash
git add applications/main/subghz/views/subghz_frequency_analyzer.h \
        applications/main/subghz/views/subghz_frequency_analyzer.c
git commit -m "feat(subghz): draw detected modulation on the analyzer"
```

---

### Task 5: Scene wiring + preset auto-select

**Files:**
- Modify: `applications/main/subghz/scenes/subghz_scene_frequency_analyzer.c`

**Interfaces:**
- Consumes: the extended pair callback (Task 3), `subghz_frequency_analyzer_set_mod` (Task 4), `subghz_setting_get_preset_count/_name` (existing), `subghz->last_settings->preset_index` (existing).

- [ ] **Step 1: Update the pair callback**

The scene's worker/pair callback (the function passed via `subghz_frequency_analyzer_worker_set_pair_callback`, or the view's own pair hook) must now accept `SignalMod mod, uint8_t confidence`. Store the latest into scene/subghz state (e.g. `subghz->fa_mod`, `subghz->fa_mod_conf` — add two fields to the analyzer-owned state, or reuse the view via `subghz_frequency_analyzer_set_mod`). Concretely, wherever the frequency/rssi from the worker is pushed into the view, also call:
```c
subghz_frequency_analyzer_set_mod(subghz->subghz_frequency_analyzer, mod, confidence);
```
and stash `mod`/`confidence` for the OK-long handler (a static/scene field or on `SubGhz`).

- [ ] **Step 2: Add a name→index resolver + auto-select on tune-to-Read**

Add a static helper in the scene file:
```c
static void subghz_fa_apply_preset_for_mod(SubGhz* subghz, SignalMod mod, uint8_t confidence) {
    if(mod == SIG_MOD_UNK || confidence < 50) return; // don't override on low confidence
    const char* want = (mod == SIG_MOD_AM) ? "AM650" : "FM476";
    SubGhzSetting* setting = subghz->setting; // confirm the accessor used elsewhere in this file
    for(size_t i = 0; i < subghz_setting_get_preset_count(setting); i++) {
        if(!strcmp(subghz_setting_get_preset_name(setting, i), want)) {
            subghz->last_settings->preset_index = i;
            subghz_last_settings_save(subghz->last_settings);
            break;
        }
    }
}
```
In the `SubGhzCustomEventViewFreqAnalOkLong` branch (currently: `scene_manager_previous_scene` + `next_scene(SubGhzSceneReceiver)`), call `subghz_fa_apply_preset_for_mod(subghz, <stashed mod>, <stashed confidence>);` **before** navigating, so the Receiver opens in the detected preset (it applies `last_settings->preset_index` via `subghz_txrx_set_preset_internal`). Add `#include <string.h>` and `../helpers/subghz_last_settings.h` / `subghz_setting.h` if not already included.

- [ ] **Step 3: Verify the whole app builds clean**

Run: `cmd.exe /c build_m5stick_s3.bat` → confirm `SENTINEL_OK` (no callback-arity errors remain).

- [ ] **Step 4: Commit**

```bash
git add applications/main/subghz/scenes/subghz_scene_frequency_analyzer.c
git commit -m "feat(subghz): analyzer shows modulation + auto-selects AM650/FM476 preset"
```

---

### Task 6: Cross-tree mirror + build both boards

**Files:**
- Mirror all six created/modified paths into `../src_cardputer_adv/` (same relative paths).

- [ ] **Step 1: Copy the new + modified files into the cardputer tree**

Copy these into `../src_cardputer_adv/` at identical relative paths: the 3 new helper files, the test, and the 4 modified files (worker `.h/.c`, view `.h/.c`, scene `.c`). If a target file in the cardputer tree has diverged, port the change by hand rather than overwriting.

- [ ] **Step 2: Build both boards**

Run: `cmd.exe /c build_m5stick_s3.bat` (already green) and the cardputer public/private build (`build_adv_only_private.bat`). Confirm each reaches `SENTINEL_OK`.

- [ ] **Step 3: Commit**

```bash
git add -A
git commit -m "chore(subghz): mirror modulation auto-detect into cardputer tree"
```

- [ ] **Step 4: On-hardware validation (manual, flagged)**

Flash the private M5Stick bin. In Sub-GHz → Frequency Analyzer, transmit a known **OOK** fob (e.g. a KeeLoq gate) and a known **FSK** fob near the device; confirm the readout shows `OOK`/`FSK` correctly and that opening Read auto-selects AM650/FM476. If misclassified, capture the RSSI window (temporary `FURI_LOG_I` of `floor_dbm`/`peak_dbm`/`swing`) and adjust the `SMD_*` thresholds in `subghz_mod_classify.c`; re-run the host test to confirm no regression, rebuild, re-validate.

---

## Self-review

- **Spec coverage:** Component A → Tasks 1–2; Component B → Tasks 3–4; Component C → Task 5; Component D → none needed (`subghz_scene_analyze_mod_of` already maps preset→mod, noted). Constraints: no PUBLIC_BUILD guard (Global Constraints); both trees (Task 6); host test (Task 1); on-HW tuning (Task 6 Step 4). Covered.
- **Type consistency:** `SubGhzModDetect`, `subghz_mod_classify(const int8_t*, size_t)`, `subghz_mod_detect_run(uint32_t)`, the 6-arg `SubGhzFrequencyAnalyzerWorkerPairCallback`, and `subghz_frequency_analyzer_set_mod(instance, SignalMod, uint8_t)` are used identically across tasks.
- **Known confirm-on-implement points** (verify the exact symbol, not placeholders): the RX/preset-load HAL call names in Task 2 (`furi_hal_subghz.h`), the `subghz->setting` accessor name used in Task 5 (match its use elsewhere in the scene/receiver files), and the exact draw x/y in Task 4 (fit the existing layout). All are named lookups, not undefined behavior.
