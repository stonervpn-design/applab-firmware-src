# On-Device Modulation Auto-Detect — Design Spec

**Date:** 2026-08-06
**Status:** approved (design), pending spec review
**Scope:** MVP — detect OOK vs FSK, auto-select AM650/FM476 preset, feed `SignalObservation.mod` into `signal_fingerprint`.

## Goal

After the Sub-GHz frequency analyzer locks a frequency, classify the signal's modulation (OOK/ASK vs 2‑FSK/GFSK) **on-device**, display it, auto-select the matching CC1101 preset for the subsequent Read, and populate `SignalObservation.mod` so the signal-fingerprint classifier's modulation hard-filter becomes effective. This removes the single biggest capture-quality friction: capturing in the wrong preset yields garbage (the documented "FSK-as-OOK garbage" failures).

## Non-goals (explicitly deferred)

- **Full identify pipeline** (estimate TE short/long + bit count in the correct preset, run `signal_fingerprint_classify()`, and name the protocol/make on-screen). Deferred — this MVP only fills `mod`.
- **Bandwidth refinement** (distinguishing AM270 from AM650, or FM238 from FM476). MVP selects only the two workhorse presets: AM650 for OOK, FM476 for FSK.
- Any change to the decoders themselves or to the RSSI-sweep frequency-locking logic.

## Context (verified in the codebase)

- `applications/main/subghz/helpers/subghz_frequency_analyzer_worker.c` already sweeps RSSI across the frequency list **in the OOK‑650 wide-filter config** and locks the peak frequency (coarse + fine), gated by a `trigger_level`. The worker owns the radio while running.
- `applications/main/subghz/views/subghz_frequency_analyzer.c` has a view model with `locked`, `rssi`, `frequency`, `trigger` — the place to add a modulation readout.
- `applications/main/subghz/helpers/signal_fingerprint.h` defines `SignalMod { SIG_MOD_UNK, SIG_MOD_AM, SIG_MOD_FM }` and `SignalObservation.mod`. `classify()` uses `mod` as a **hard filter** (`obs->mod != e->mod` → reject), but the observation is currently built with `mod = SIG_MOD_UNK`, so the filter is inert.
- `subghz_txrx_set_preset(instance, preset_name, freq, ...)` sets the active preset; `subghz_txrx_get_preset_name()` maps to `"AM270"/"AM650"/"FM238"/"FM12K"/"FM476"/"CUSTOM"`.
- `furi_hal_subghz_get_rssi()` returns instantaneous RSSI (float dBm); `furi_hal_subghz_start_async_rx()` / device-layer `get_rssi` are available. RSSI reads are single status-register SPI transactions (fast; a tight loop samples the amplitude envelope).

## Why RSSI-dynamics (detection method)

OOK/ASK keys the **carrier on/off** → RSSI swings bimodally between noise floor and peak. FSK/GFSK keeps the **carrier always on**, shifting only frequency → in a wide RX filter RSSI stays steady near peak. Sampling instantaneous RSSI over a short window and analysing the distribution therefore separates the two **without** demodulating (which would require already knowing the modulation). The wide OOK‑650 config the analyzer already uses is the correct RX config for this envelope sampling.

## Architecture

One new isolated unit + three small integration points. No new scenes or views.

```
[analyzer worker] --lock(freq)--> subghz_mod_detect_run(freq) --> {mod, confidence}
                                          |                              |
                                   (HW: sample RSSI)             (B) view model -> draw "OOK"/"FSK"
                                          |                              |
                                   subghz_mod_classify()          (C) on Read/save: map mod -> "AM650"/"FM476"
                                   (PURE, host-tested)                   |
                                                                 (D) analyze: obs.mod = from active preset -> classify()
```

## Component A — the detector unit (new)

**Three files**, so the pure classifier compiles standalone on the host (the HW
sampler and the classifier must NOT share a translation unit, or the host test
would drag in `furi_hal`). This mirrors the existing `hitag2_cipher.c` +
`test_hitag2_cipher.c` pattern.

- `helpers/subghz_mod_detect.h` — the shared struct + both function declarations.
- `helpers/subghz_mod_classify.c` — the **PURE** classifier (no `furi_hal`, no
  globals). The unit under host test.
- `helpers/subghz_mod_detect.c` — the **HW** sampler; `#include`s only the header
  and calls `subghz_mod_classify()`.

```c
// subghz_mod_detect.h
#include "signal_fingerprint.h"   // SignalMod
#include <stdint.h>
#include <stddef.h>

typedef struct {
    SignalMod mod;        // SIG_MOD_AM (OOK) | SIG_MOD_FM (FSK) | SIG_MOD_UNK
    uint8_t   confidence; // 0..100
    int8_t    floor_dbm;  // estimated noise floor
    int8_t    peak_dbm;   // estimated peak
} SubGhzModDetect;

// PURE (subghz_mod_classify.c) — classify a window of RSSI samples (dBm as signed
// ints). No hardware or globals. This is the unit under test.
SubGhzModDetect subghz_mod_classify(const int8_t* rssi_dbm, size_t count);

// HW (subghz_mod_detect.c) — tune to freq (wide OOK-650 config), sample RSSI over
// the detection window into a stack buffer, then call subghz_mod_classify().
// Assumes the caller owns the radio (the analyzer worker context). Does not
// restore radio state.
SubGhzModDetect subghz_mod_detect_run(uint32_t frequency_hz);
```

**Classifier logic (`subghz_mod_classify`)** — all thresholds are named constants (starting values, tuned on HW):
1. Estimate `floor` and `peak` robustly (e.g. low/high percentile after trimming outliers, not raw min/max).
2. `swing = peak - floor`; `floor_frac` = fraction of samples within `FLOOR_MARGIN_DB` of `floor`; `active_frac` = fraction meaningfully above `floor`.
3. If `active_frac < SIGNAL_PRESENT_MIN` → `SIG_MOD_UNK` (no real signal to judge).
4. Else if `swing >= SWING_MIN_DB` **and** `floor_frac >= FLOOR_FRACTION_OOK` → `SIG_MOD_AM` (energy toggles on/off; real mass sits at the floor).
5. Else if `swing <= FM_SWING_MAX_DB` (carrier steady, little floor mass) → `SIG_MOD_FM`.
6. Else → `SIG_MOD_UNK`.
7. `confidence` derived from how cleanly the chosen branch separates (e.g. distance of `swing`/`floor_frac` from the decision thresholds), 0..100.

**Sampler (`subghz_mod_detect_run`)**: ensure the OOK‑650 wide config is loaded, `set_frequency(freq)`, enter RX, then tight-loop `furi_hal_subghz_get_rssi()` for `WINDOW_MS`, clamping each reading to `int8_t` into a `MAX_SAMPLES` buffer; call `subghz_mod_classify`.

**Named tunables (in the .c):** `SUBGHZ_MOD_DETECT_WINDOW_MS` (~30), `SUBGHZ_MOD_DETECT_MAX_SAMPLES` (~256), `FLOOR_MARGIN_DB`, `SWING_MIN_DB`, `FLOOR_FRACTION_OOK`, `FM_SWING_MAX_DB`, `SIGNAL_PRESENT_MIN`, confidence mapping constants.

## Component B — Frequency-analyzer integration

- In the worker thread, immediately after a frequency locks (RSSI above `trigger_level`), call `subghz_mod_detect_run(locked_freq)` (the radio is already tuned to that frequency in OOK‑650) and carry `mod` + `confidence` out through the pair result alongside the existing frequency/RSSI.
- Extend the analyzer **view model** with `SignalMod mod` + `uint8_t mod_confidence`.
- The view draws `OOK` / `FSK` next to the locked frequency, and `OOK?` / `FSK?` when `confidence < CONF_SHOW_THRESHOLD`; nothing when `SIG_MOD_UNK`.

## Component C — Preset auto-select

- When the user acts on a lock (save frequency / tune into Read), map: `SIG_MOD_AM → "AM650"`, `SIG_MOD_FM → "FM476"`, and write it into `last_settings` (the same place the analyzer already writes `last_settings->frequency`) so the Read scene opens in the matching demodulator via `subghz_txrx_set_preset`.
- On `SIG_MOD_UNK` **or** `confidence < CONF_AUTOSELECT_THRESHOLD`: do **not** override — the user's current preset stands. (A wrong read must never silently force the wrong preset.)

## Component D — Feed `signal_fingerprint`

- Add a tiny mapping helper `SignalMod subghz_mod_from_preset_name(const char* name)` (`"AM*" → SIG_MOD_AM`, `"FM*" → SIG_MOD_FM`, else `SIG_MOD_UNK`).
- In the analyze/observation-building path (`subghz_scene_analyze.c`), set `obs.mod` from the active preset name instead of leaving `SIG_MOD_UNK`. Modulation now flows to the classifier via the (now correctly auto-selected) preset, activating the `mod` hard-filter.

## Error handling

- Weak/absent signal → `SIG_MOD_UNK` → display `?`, keep the user's preset, leave `obs.mod` untouched.
- Two confidence gates: `CONF_SHOW_THRESHOLD` (display `?` below it) and `CONF_AUTOSELECT_THRESHOLD` (don't auto-override below it). Auto-select is always safe-by-default.

## Testing

- **Host unit test** `tests/test_subghz_mod_classify.c` that `#include`s the real `../applications/main/subghz/helpers/subghz_mod_classify.c` (the pure file — no `furi_hal`), exactly like `tests/test_hitag2_cipher.c` includes `hitag2_cipher.c`. Cases: clean bimodal OOK → `SIG_MOD_AM`; steady-high FSK → `SIG_MOD_FM`; noise-only/low-active → `SIG_MOD_UNK`; a marginal/ambiguous case → `SIG_MOD_UNK` or low confidence. Assert `mod` and sane `confidence` bounds.
- **On-hardware tuning pass** (required, flagged): capture real OOK and FSK fob transmissions, log the RSSI windows, tune the named thresholds so real signals classify correctly. Same HW-validation boundary as the port's other RF features.

## Constraints

- **General feature, ships in both variants** — this improves capture of *all* signals, is not car-specific, and carries **no `#ifndef PUBLIC_BUILD` guard**.
- **Cross-tree parity** — the change must land in both `src_m5stick_s3` and `src_cardputer_adv` (the documented drift risk); the implementation plan must include mirroring or note it explicitly.
- Runs only while the analyzer already owns the radio; adds ~30 ms per lock — negligible, no new RAM of note (one small stack buffer).
