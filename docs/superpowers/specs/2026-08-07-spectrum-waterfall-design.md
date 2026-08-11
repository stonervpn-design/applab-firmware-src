# P3: Spectrum / Waterfall (swept-RSSI panadapter) — design

**Date:** 2026-08-07
**Status:** approved (design), pending plan
**Scope:** PRIVATE builds only. Third PandwaRF-parity roadmap item; adds a live band view the ports lack entirely.

---

## Goal

A **Spectrum** menu entry in the Sub-GHz app that shows a live **swept-RSSI panadapter**: per-bin RSSI bars across a contiguous band, a scrolling thresholded waterfall, and the peak frequency/level. L/R shift the band, OK tunes the receiver to the peak, Back exits.

## What it is / is NOT (honest)

- **IS:** carrier presence + power per frequency (RSSI), swept bin-by-bin on the CC1101. Shows *where* energy is across a band and lets the user pounce (tune-to-peak).
- **IS NOT:** an SDR/FFT spectrum. The CC1101 has no IQ; there is no demod, no signal detail, no sub-bin resolution. This is a panadapter, matching PandwaRF's own ~1.2 MHz RSSI window (not an SDR either).

## Findings that shape this (P3 research workflow)

- HW tune resolution ~397 Hz → arbitrary bin spacing. Bands 300-348/387-464/779-928 MHz (RX-valid wider).
- The FA worker's per-bin cost is ~3.7 ms, dominated by a fixed **2 ms settle** + **~0.8 ms per-bin calibration** — both avoidable. With `FS_AUTOCAL=0` + calibrate-once-per-band + ~200 µs settle: **~1 ms/bin → 128 bins ≈ 8 fps** at the safe 100 kHz SPI. (Raising SPI to 1-2 MHz would give ~30 fps but destabilises the shared bus — **not done**.)
- RSSI: `furi_hal_subghz_get_rssi()`; tune without cal: `cc1101_set_frequency()`; `cc1101_calibrate()`, `cc1101_switch_to_idle/_to_rx`. On M5Stick S3 the band-switch path is a no-op (no 10 ms antenna settle).

---

## GLOBAL CONSTRAINTS

1. **Private-only, gc'd from public.** `ADD_SCENE(subghz, spectrum, Spectrum)` and the Start-menu entry (`submenu_add_item` + the `on_event` case) are `#ifndef PUBLIC_BUILD`; the `subghz_spectrum` view alloc is likewise guarded. With no scene-table entry and no menu reference, the scene/view/worker code is unreferenced in public and gc-sections drops it → **0 spectrum symbols in public** (verified in the gate task; matches the P1 arc precedent).
2. **No-PSRAM discipline:** the view + waterfall buffer are lazy-allocated on scene enter and freed on exit (ProtoView model), zero idle heap.
3. **SPI stays 100 kHz** (shared-bus reliability); accept ~8 fps.
4. Read-only analysis + an explicit user OK to tune; no TX.

---

## Architecture

Three new units + wiring, cloning proven patterns:

1. **Sweep worker** `helpers/subghz_spectrum_worker.c/.h` — cloned from `subghz_frequency_analyzer_worker.c`. Owns a thread that, per frame: (setup once) reset + load a wide-RX preset (OOK-650 kHz) with `FS_AUTOCAL=0`, calibrate once at band start; then steps `NBINS=128` contiguous bins `f = band_start + i*bin_step`, per bin `SIDLE → cc1101_set_frequency(f) → SRX → furi_delay_us(~200) → get_rssi`, quantises to `uint8_t`, tracks the peak; delivers the row via a callback. A `band_start` setter (L/R shift) mirrors `set_trigger_level`.
2. **View** `views/subghz_view_spectrum.c/.h` — a `ViewModelTypeLocking` model `{uint8_t spec[128]; uint8_t wf[WF_ROWS][16]; uint8_t wf_head; uint32_t band_start, bin_step, peak_freq; float peak_rssi;}` (WF_ROWS=24). Draw: header (band + `peak MHz/dBm` right-aligned) · spectrum bars y=11..37 (`canvas_draw_line(i,37,i,37-h)`) · waterfall y=41..62 (`canvas_draw_dot(i,41+row)` per set bit) · peak caret. Input: L/R → band-shift event, OK → tune event, Back → exit event.
3. **Scene** `scenes/subghz_scene_spectrum.c` — lazy `add_view` + worker start on enter (clone `subghz_scene_protoview.c`), `remove_view`+free+worker stop on exit; routes view events (band-shift → worker setter, tune → set `last_settings->frequency` + Receiver).

Wiring: `subghz_scene_config.h` (ADD_SCENE, guarded), `subghz_types.h` (`SubGhzViewIdSpectrum`), `subghz_i.h` (`subghz_spectrum` field + include), `subghz.c` (NULL-init), `subghz_scene_start.h/.c` (`SubmenuIndexSpectrum` + entry, guarded), `subghz_custom_event.h` (Spectrum tune/back/band events).

### Tune-to-peak
On OK: gate `subghz_txrx_radio_device_is_frequency_valid(txrx, peak_freq)`; `last_settings->frequency = peak_freq`; disable hopping; `subghz_last_settings_save`; `scene_manager_next_scene(SubGhzSceneReceiver)` (Receiver reads `last_settings->frequency` on enter → opens tuned to the peak).

---

## Safety

Read-only sweep; OK-to-tune is explicit; no TX. Continuous RX only (no emission). Private-build-gated.

## Testing & verification

1. **Host:** the pure bits are minimal (bin-center + RSSI-quantise math) — a small host test for `spectrum_bin_freq(band_start,bin_step,i)` and the RSSI→height quantiser.
2. **Private build:** green; `nm` shows the spectrum symbols; on-device smoke (deferred): open Spectrum, see live bars + waterfall move on a nearby 433 transmitter, peak label tracks it, OK tunes RX.
3. **Public gate:** green; `nm` public ELF → **0** `subghz_spectrum`/`subghz_view_spectrum`/`spectrum_worker` symbols; `verify_public_no_car` PASS.

## Deferred follow-ons

Band-preset cycle (315/433/868), adjustable span/RBW, SPI-bump 30 fps mode, dBm axis labels, marker readout.
