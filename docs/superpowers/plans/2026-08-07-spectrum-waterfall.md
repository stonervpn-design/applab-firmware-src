# P3: Spectrum / Waterfall — Implementation Plan

> Implement task-by-task. Private-only; each task ends buildable. Clone the named reference files rather than inventing patterns.

**Goal:** A private "Spectrum" Sub-GHz menu entry → a live swept-RSSI panadapter (128-bin bars + scrolling waterfall + peak label; L/R band, OK tune-to-peak, Back exit) at ~8 fps.

**Architecture:** sweep worker (clone of `subghz_frequency_analyzer_worker.c`) → row callback → locking view model → `subghz_view_spectrum` draw; `subghz_scene_spectrum` lazy-loads the view + runs the worker.

## Global Constraints

- **Private-only:** `ADD_SCENE`, the Start-menu entry, and the view alloc are `#ifndef PUBLIC_BUILD`; feature gc's out of public (0 symbols, verified in Task 5).
- **No-PSRAM:** lazy alloc on scene enter / free on exit (clone `subghz_scene_protoview.c`).
- **SPI stays 100 kHz.** Worker skips per-bin cal (`FS_AUTOCAL=0`, calibrate once per band) + ~200 µs settle (not 2 ms).
- `NBINS=128`, `WF_ROWS=24`. Build private: `build_m5stick_s3.bat`; public gate: `build_m5stick_s3_public.bat`. nm at the usual xtensa path.

---

### Task 1: Scaffolding + wiring (menu → placeholder view)

**Files:** Create `views/subghz_view_spectrum.h/.c`, `scenes/subghz_scene_spectrum.c`; Modify `scenes/subghz_scene_config.h`, `helpers/subghz_types.h`, `subghz_i.h`, `subghz.c`, `scenes/subghz_scene_start.h`, `scenes/subghz_scene_start.c`, `helpers/subghz_custom_event.h`.

- [ ] **Step 1 — View skeleton.** `subghz_view_spectrum.h`: opaque `SubGhzViewSpectrum`, callback typedef `SubGhzViewSpectrumCallback(SubGhzCustomEvent, void*)`, `alloc/free/get_view/set_callback`. `.c`: `ViewModelTypeLocking` model with the fields from the spec; `alloc` (clone `subghz_view_protoview` alloc), draw = just `canvas_draw_str(canvas, 2, 8, "Spectrum")` for now; input returns false.
- [ ] **Step 2 — View ID + slot.** Add `SubGhzViewIdSpectrum` to `subghz_types.h` `SubGhzViewId` (near `SubGhzViewIdProtoview`). Add `SubGhzViewSpectrum* subghz_spectrum;` to `SubGhz` in `subghz_i.h` (+ `#include "views/subghz_view_spectrum.h"`). In `subghz.c:subghz_alloc` set `subghz->subghz_spectrum = NULL;` (mirror protoview line ~219); in free, `if(subghz->subghz_spectrum) subghz_view_spectrum_free(...)`.
- [ ] **Step 3 — Scene.** `subghz_scene_spectrum.c`: `on_enter` lazy-adds the view (`if(!subghz->subghz_spectrum){ alloc; view_dispatcher_add_view(vd, SubGhzViewIdSpectrum, get_view); }`), sets the callback, `view_dispatcher_switch_to_view(SubGhzViewIdSpectrum)` (clone `subghz_scene_protoview.c` on_enter 90-101); `on_exit` frees + `view_dispatcher_remove_view` + NULL (clone protoview on_exit); `on_event` returns false for now.
- [ ] **Step 4 — Register scene (guarded).** In `subghz_scene_config.h`, after the FrequencyAnalyzer line: `#ifndef PUBLIC_BUILD` / `ADD_SCENE(subghz, spectrum, Spectrum)` / `#endif`.
- [ ] **Step 5 — Menu entry (guarded).** In `subghz_scene_start.h` add `SubmenuIndexSpectrum` to `enum SubmenuIndex`. In `subghz_scene_start.c` `on_enter`, under `#ifndef PUBLIC_BUILD`, `submenu_add_item(subghz->submenu, "Spectrum", SubmenuIndexSpectrum, subghz_scene_start_submenu_callback, subghz)` (clone the Frequency Analyzer block); in `on_event`, under `#ifndef PUBLIC_BUILD`, `else if(event.event == SubmenuIndexSpectrum){ scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSpectrum); return true; }`.
- [ ] **Step 6 — Events.** Add `SubGhzCustomEventViewSpectrumTune`, `...SpectrumBandUp`, `...SpectrumBandDown`, `...SpectrumBack` to `subghz_custom_event.h` (guard block optional; inert in public).
- [ ] **Step 7 — Build + smoke.** `build_m5stick_s3.bat` green; `nm` shows `subghz_view_spectrum_alloc` PRESENT. (On device: "Spectrum" appears in the menu and opens the placeholder.)
- [ ] **Step 8 — Commit.** `feat(subghz): Spectrum scene/view scaffolding + menu wiring (private-only)`

---

### Task 2: Sweep worker + spectrum bars

**Files:** Create `helpers/subghz_spectrum_worker.h/.c`, `tests/test_subghz_spectrum.c`, `tools/spectrum/spectrum_replica.py`; Modify `subghz_view_spectrum.c`, `subghz_scene_spectrum.c`.

**Interfaces produced:** `SubGhzSpectrumWorker`, `SubGhzSpectrumWorkerRowCallback(void* ctx, uint32_t band_start, uint32_t bin_step, const uint8_t* bins, size_t n, uint32_t peak_freq, float peak_rssi)`, `subghz_spectrum_worker_{alloc,free,start,stop,set_row_callback,set_band_start}`, pure `uint32_t spectrum_bin_freq(uint32_t band_start, uint32_t bin_step, size_t i)` + `uint8_t spectrum_rssi_q(float dbm)`.

- [ ] **Step 1 — Pure helpers + host test.** In `subghz_spectrum_worker.c` (or a pure `subghz_spectrum_util.c`), `spectrum_bin_freq` = `band_start + bin_step*i`; `spectrum_rssi_q` = clamp `(dbm - FLOOR)/(CEIL-FLOOR)*height` with FLOOR=-97, CEIL=-60, height=26 → 0..26. Host test `test_subghz_spectrum.c` + `spectrum_replica.py` (Python cross-check, no local gcc); validate + `xtensa -fsyntax-only`.
- [ ] **Step 2 — Worker.** Clone `subghz_frequency_analyzer_worker.c`: thread setup once (reset + wide OOK-650 preset, `FS_AUTOCAL=0`, one `cc1101_calibrate` at band start); loop `for(i<NBINS) f=band_start+i*bin_step`: `SIDLE → cc1101_set_frequency(f) → SRX → furi_delay_us(200) → get_rssi`, `bins[i]=spectrum_rssi_q(rssi)`, track peak; after the pass call the row callback; honour `worker_running`. `set_band_start` setter (mirror `set_trigger_level`).
- [ ] **Step 3 — Wire worker → view.** In `subghz_scene_spectrum.c`, alloc+start the worker on enter, set its row callback to a fn that `with_view_model(subghz->subghz_spectrum->view, model, {memcpy spec; model->band_start=...; model->bin_step=...; model->peak_freq=...; model->peak_rssi=...; append waterfall row (Task 3)}, true)`; stop+free on exit.
- [ ] **Step 4 — Spectrum bars.** In `subghz_view_spectrum.c` draw: header band text + peak label `canvas_draw_str_aligned(126,0,AlignRight,AlignTop,"433.92 -58")`; bars `for(i<128) canvas_draw_line(i,37,i,37-model->spec[i]*26/... )` (spec already 0..26 → `canvas_draw_line(i,37,i,37-model->spec[i])`); separators.
- [ ] **Step 5 — Build + verify.** private green; `nm` shows `subghz_spectrum_worker_thread`. (On device: bars move with a nearby 433 signal.)
- [ ] **Step 6 — Commit.** `feat(subghz): contiguous-band RSSI sweep worker + live spectrum bars (private)`

---

### Task 3: Waterfall + peak marker

**Files:** Modify `subghz_view_spectrum.c`, `subghz_scene_spectrum.c`.

- [ ] **Step 1 — Waterfall append.** In the row callback, before/after copying spec, compute the thresholded row: for each bin, set packed bit if `spec[i]` above a floor+margin; write into `model->wf[model->wf_head]`, advance `wf_head` mod WF_ROWS.
- [ ] **Step 2 — Waterfall draw.** y=41..62: for display row `r`, source `wf[(wf_head - 1 - r + WF_ROWS) % WF_ROWS]`; for each set bit `i`, `canvas_draw_dot(i, 41+r)`. Separator line at y=39.
- [ ] **Step 3 — Peak caret.** Draw a 3px down-caret (two `canvas_draw_line`) just above the spectrum baseline at `x = peak_bin`, and a faint vertical marker across the waterfall at the same x.
- [ ] **Step 4 — Build + verify.** private green; on-device: waterfall scrolls, a held transmitter draws a vertical streak.
- [ ] **Step 5 — Commit.** `feat(subghz): thresholded scrolling waterfall + peak marker (private)`

---

### Task 4: Input — band shift, tune-to-peak, exit

**Files:** Modify `subghz_view_spectrum.c`, `subghz_scene_spectrum.c`.

- [ ] **Step 1 — View input.** In `subghz_view_spectrum` input callback: Left → `SpectrumBandDown`, Right → `SpectrumBandUp`, OK(short) → `SpectrumTune`, Back → `SpectrumBack` custom events via the callback.
- [ ] **Step 2 — Band shift.** In `subghz_scene_spectrum_on_event`: on BandUp/Down, `subghz_spectrum_worker_set_band_start(worker, band_start ± span/4)` clamped to the RX-valid range; the next sweep re-centers.
- [ ] **Step 3 — Tune-to-peak.** On `SpectrumTune`: read `peak_freq` (getter on the view/worker); if `subghz_txrx_radio_device_is_frequency_valid(subghz->txrx, peak_freq)` → `subghz->last_settings->frequency = peak_freq`; `subghz->last_settings->enable_hopping = false`; `subghz_last_settings_save(subghz->last_settings)`; `scene_manager_next_scene(subghz->scene_manager, SubGhzSceneReceiver)`.
- [ ] **Step 4 — Back.** On `SpectrumBack`: `scene_manager_previous_scene` (stops worker via on_exit).
- [ ] **Step 5 — Build + verify.** private green; on-device: L/R moves the band, OK opens RX tuned to the peak, Back exits cleanly.
- [ ] **Step 6 — Commit.** `feat(subghz): Spectrum input - band shift + tune-to-peak + exit (private)`

---

### Task 5: Public exclusion gate

**Files:** none (verification).

- [ ] **Step 1 — Build public.** `build_m5stick_s3_public.bat` → SENTINEL_OK.
- [ ] **Step 2 — Symbol gate.** `nm build_m5stick_s3_public/furi_esp32.elf | grep -cE 'subghz_view_spectrum|subghz_spectrum_worker|scene_spectrum'` → **0**.
- [ ] **Step 3 — String gate.** `grep -a` the public merged bin for `"Spectrum"` → 0 (menu entry guarded out).
- [ ] **Step 4 — Car gate.** `verify_public_no_car.py` → PASS.
- [ ] **Step 5 — Commit.** `chore(subghz): verify Spectrum is private-only (public gate clean)`

---

## Self-review

- **Coverage:** Task 1 wiring (spec §Architecture/§Global 1), Task 2 worker+bars (§Architecture 1-2, §Findings), Task 3 waterfall (§Architecture 2), Task 4 input+tune (§Tune-to-peak), Task 5 gate (§Global 1). Mapped.
- **Type consistency:** `SubGhzViewSpectrum`, `SubGhzSpectrumWorker`, `SubGhzViewIdSpectrum`, `SubmenuIndexSpectrum`, `spectrum_bin_freq/_rssi_q`, `NBINS`/`WF_ROWS` used identically.
- **No placeholders:** clone-sources named for each new file; exact wiring anchors + the tune-to-peak code given; the write-time work is transcribing the FA-worker/protoview-scene patterns.
