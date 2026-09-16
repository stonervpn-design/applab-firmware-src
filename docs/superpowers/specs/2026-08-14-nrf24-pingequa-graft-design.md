# Design: PINGEQUA RF Lab feature graft into the ESP32 nRF24 app

- **Date:** 2026-08-14
- **Status:** Approved design, pre-implementation
- **Scope owner:** private builds only (`#ifndef PUBLIC_BUILD`)
- **Targets:** M5Stick S3 (superset `src_m5stick_s3`) → mirrored to Cardputer ADV (`src_cardputer_adv`)
- **Source of features:** PINGEQUA RF Lab / `pingequa_rf_toolkit` v0.5.4 FAP (`Downloads/rf-lab-0.5.4.zip`, MIT)

## 1. Motivation & framing

The user asked to "add PINGEQUA RF Lab to the private builds." PINGEQUA is a **real-Flipper STM32 FAP** (2.4 GHz spectrum analyzer + 7-mode nRF24 jammer) for the pingequa nRF24+CC1101 devboard. The chosen target is the **ESP32** M5Stick S3 / Cardputer private builds, which cannot run a `.fap` — so this is a feature port, not a drop-in.

**Key finding from exploring both codebases:** the existing ESP32 `applications/main/nrf24` app is already **at or beyond** PINGEQUA on most axes:

- Jammer: **9 modes** (WiFi/BT/BLE-adv/BLE-data/Zigbee/RC/USB/Video/Full-band) with per-protocol strategy and PA-max unclamped — broader than PINGEQUA's 7.
- Driver `nrf24_hw.c`: RPD read (`nrf24_hw_listen_rpd`), CONT_WAVE jammer, flood/payload-spam, PA-unclamped, RX-promiscuous, RF_SETUP verify — equivalent to `pq_nrf24`.
- Spectrum analyzer exists (`scene_spectrum` + `nrf24_spectrum_view`) — but bare RPD bars only.

Therefore a wholesale port would **duplicate a superset**. Only three PINGEQUA features are genuinely net-new; those are the entire scope.

## 2. Scope — the three grafts

### G1. RPD reactive BLE jammer (highest value)
Listen on RPD; fire a CW pulse only when a target carrier appears, then return to listening. PINGEQUA's differentiator ("first-on-Flipper NRF24 reactive", claimed ~70–100 % hit vs ~4 % blind). **Confirmed absent** from the existing app — every current mode is blind broadcast.

### G2. Spectrum UI upgrade
- **Band markers** (WiFi ch 1/6/11 → nRF 12/37/62; BLE adv 37/38/39 → nRF 2/26/80) — display-only.
- **Max-hold** peak retention (toggle).
- **Cursor** channel inspection + **dwell** adjust (130–2000 µs).

### G3. Export & persistence
- Spectrum: long-press OK → hits CSV to `/ext/apps_data/nrf24/scan_*.csv`.
- Jammer: session log on exit (mode/channel/duration).
- Persist jammer mode + spectrum dwell via FlipperFormat (PINGEQUA's `pq_*_config` pattern).

## 3. Non-goals (YAGNI)

- No standalone "PINGEQUA" app, no About/QR/branding screens, no splash.
- No port of `pq_nrf24.c` register driver — reuse `nrf24_hw.c`.
- No port of `pq_chip_arbiter.c` — the STM32 shared-bus arbiter (PA4/PC3/PB2) does not map to ESP32 pins; the ESP32 driver owns its own bus/CE.
- No CC1101 features (`pq_cc1101`) — out of scope for this graft.
- No new jammer modes beyond reactive (the 9 existing stay as-is).

## 4. Architecture & components

Everything lands inside the existing `applications/main/nrf24` app; no new app, no new menu tree.

### 4.1 Driver — `nrf24_hw.c` / `nrf24_hw.h` (extend, ~40 LOC)
Add three reactive helpers built from existing primitives (`nrf24_hw_write_reg`, `_ce`, `_set_channel`, `_flush_tx`, `_listen_rpd`, the `nrf24_rf_setup_byte`/CONT_WAVE path):

- `void nrf24_hw_reactive_setup(uint8_t ch)` — PRIM_RX + PWR_UP, pre-load TX FIFO 32×0xFF, set channel, CE handling left to caller.
- `void nrf24_hw_react_to_tx(void)` — RF_SETUP = CONT_WAVE|PLL_LOCK|PA_MAX, clear PRIM_RX (enter TX/CW). Caller raises CE.
- `void nrf24_hw_react_to_rx(uint8_t ch)` — RF_SETUP = normal 2 Mbps, set PRIM_RX, set channel (re-enter RX). Caller raises CE.

Reuse verbatim: `nrf24_hw_listen_rpd`, `nrf24_hw_ce`, `nrf24_hw_flush_tx`, `nrf24_hw_power_down`, acquire/release.

### 4.2 Reactive mode — `scenes/scene_jam.c` (extend)
- Extend `NRF24_JAM_MODE_COUNT` +1; add label "BLE Reactive" (mode index appended after Full Band) in `nrf24_jam_view` mode list.
- Worker branch when `mode == reactive`: rotate `CH_BLE_ADV` {2,26,80}; per channel: `reactive_setup(ch)`, CE high, wait Tstby2a (~130 µs), poll `listen_rpd` every ~50 µs across a dwell window; on RPD=1 → CE low, `react_to_tx`, CE high, hold CW ~300 µs, CE low, advance channel via `react_to_rx(next)`. Honour the existing `stop`/`desired_running`/`mode` volatile flags for clean exit.
- Reuse the existing `JamCtx`, worker-thread lifecycle, and `nrf24_jam_view` rendering.

### 4.3 Spectrum — `views/nrf24_spectrum_view.c` + `scenes/scene_spectrum.c` (extend)
- View model gains: `bool max_hold`, `uint8_t peaks[NRF24_CH_COUNT]`, `uint8_t cursor_ch`, `uint16_t dwell_us`.
- Draw: existing bars + marker ticks/labels at the WiFi/BLE nRF channels + peak overlay when max-hold on + cursor highlight with a readout line (channel N, ~MHz, hit-count).
- Worker keeps `peaks[ch] = max(peaks[ch], hits[ch])` when max-hold; dwell read from model.
- Input adaptation:
  - Cardputer (full keyboard): Left/Right move cursor; Up/Down (or keys) adjust dwell; a key toggles max-hold; long-press OK export.
  - M5Stick S3 (2-button: KEY1=Ok, KEY2 tap=Down / dbl=Up / long=Back — no L/R): markers + max-hold always on/toggle via OK; cursor advances on KEY2-tap; dwell cycles through presets {130,250,500,1000,2000} on a dedicated gesture; long-press export. Markers + max-hold need **no input** and are identical on both boards.

### 4.4 Export/persist — new `helpers/nrf24_export.c` / `.h` + config
- `bool nrf24_export_scan(const uint8_t* hits, size_t n)` → CSV `channel,mhz,hits` to `/ext/apps_data/nrf24/scan_<seq>.csv` (RTC wall-clock name if the board exposes RTC; else a monotonic sequence).
- `bool nrf24_export_jam_log(mode, channel, duration_s)` → append session line.
- `nrf24_settings_load/save` (FlipperFormat, filetype `"NRF24 RF Lab State"`): jammer mode + spectrum dwell; range-checked with defaults on any mismatch (PINGEQUA's load/save template).
- Storage via `RECORD_STORAGE`; `/ext` maps to internal flash on M5Stick S3 (no SD) and to SD on Cardputer. `storage_simply_mkdir` the target dir first.

## 5. Data flow

```
Spectrum:  worker sweep 0..125  -> hits[]/peaks[] -> view model -> canvas (bars+markers+cursor)
                                              \-> long-press OK -> nrf24_export_scan -> /ext CSV
Reactive:  worker: for ch in {2,26,80}: setup_reactive -> RX -> poll RPD
                     RPD=1 -> react_to_tx -> CW 300us -> react_to_rx(next)
           exit -> nrf24_export_jam_log
Settings:  scene enter -> nrf24_settings_load ; on change/exit -> nrf24_settings_save
```

## 6. Error handling

- Driver probe (`nrf24_hw_probe`) already gates hardware-absent with an on-screen "hardware not detected" state; reactive/spectrum reuse it. No reactive setup on probe failure.
- Export: any storage failure logs `FURI_LOG_E` and shows a brief on-screen "export failed" toast; never crashes the scene.
- Settings: load failure → defaults (mode 0 / dwell 250 µs), matching PINGEQUA's fall-through.
- Reactive worker must always `nrf24_hw_ce(false)` + `power_down` on exit path (no stuck carrier), mirroring existing jammer stop.

## 7. Testing & verification

No host harness exists for nRF24 (hardware-bound radio). Verification tiers:

1. **Build-clean, both boards** — compile `src_m5stick_s3` (M5Stick S3 private env) and mirror-build Cardputer ADV. Follow the documented build gotchas: delete build dir, absolute-path invoke via PowerShell (not Bash), verify fresh log mtime, never trust a stale SENTINEL_OK.
2. **Boot + navigate** — app launches, jammer list shows "BLE Reactive", spectrum shows markers/max-hold, long-press writes a CSV that reads back.
3. **On-device RF (user, HackRF)** — reactive jam produces CW bursts correlated to a live BLE advertiser; spectrum markers align with known WiFi APs. Expect reactive-timing tuning (dwell/settle) as with prior nRF24 work.

## 8. Risks (honest)

- **Reactive timing on ESP32:** the technique was written for STM32 furi_hal SPI; the ESP32 driver uses software-CS with settle delays, so RX→TX flip is slower (~µs-scale overhead). Reactive mode will build and run but effectiveness needs on-device tuning; it may not reach PINGEQUA's STM32 hit-rate. Highest value, least guaranteed until HackRF-verified.
- **M5Stick 2-button UX:** cursor/dwell are preset-cycled, not free; markers/max-hold unaffected. Cardputer gets the full experience.
- **Screen real estate:** M5Stick S3 TFT is small; markers/cursor labels must stay terse.
- **NOT HW-tested** until flashed — consistent with all prior nRF24 work in this tree.

## 9. File manifest

Extend:
- `applications/main/nrf24/nrf24_hw.c` / `.h` — reactive primitives
- `applications/main/nrf24/scenes/scene_jam.c` — reactive mode
- `applications/main/nrf24/views/nrf24_jam_view.c/.h` — mode label
- `applications/main/nrf24/views/nrf24_spectrum_view.c/.h` — markers/maxhold/cursor/dwell
- `applications/main/nrf24/scenes/scene_spectrum.c` — dwell/cursor input + export trigger

New:
- `applications/main/nrf24/helpers/nrf24_export.c` / `.h` — CSV export + settings persistence

All new/changed code gated `#ifndef PUBLIC_BUILD` (verify the app is already private-gated; if not, gate the new offensive surface). Mirror all edits from the `src_m5stick_s3` superset into `src_cardputer_adv`.
