# nRF24 Fusion Spectrum — Design Spec

**Date:** 2026-08-15
**Status:** Approved design (brainstorming complete), pending implementation plan
**Primary target:** Cardputer ADV (COM12); canonical source `src_m5stick_s3`, mirrored to `src_cardputer_adv`
**Gating:** PRIVATE — entire scene + menu entry wrapped in `#ifndef PUBLIC_BUILD`
**App:** `applications/main/nrf24` (new scene alongside existing `scene_spectrum.c`, which is left untouched)

---

## 1. Goal & the two hardware ceilings

Make the 2.4 GHz spectrum both **the most advanced** and **visually stunning** the hardware allows — a fused, classified, color heat-waterfall SDR-style view. Two hard ceilings govern the work and are accepted as-is:

- **Sensing ceiling:** the nRF24L01+ RPD is a *1-bit* carrier-present flag per channel (threshold ≈ −64 dBm). It yields **occupancy**, never true amplitude/dBm. Real dBm comes only from the ESP32-S3 internal radio (Wi-Fi/BLE), and only where decodable traffic exists.
- **Rendering ceiling:** the panel is a real ST7789 RGB565 color TFT, but the Flipper GUI renders a 128×64 **1-bit** framebuffer upscaled with exactly two colors (`furi_hal_display.c`). Color requires bypassing the mono canvas.

**Approach (chosen):** full-screen **color takeover** (the proven doom/wolf3d `gui_direct_draw_acquire` + `furi_hal_display_get_panel_handle()` path) driving a **fused 126-channel model** from three coordinated producers. Delivered in 4 stages.

Honest framing carried into the UI: the full-band layer is occupancy; only the Wi-Fi/BLE overlays are true power. Classification (`SRC_HOPPER`) is how non-Wi-Fi/BLE emitters visually separate.

---

## 2. Component boundaries (files)

New units, each single-purpose; pure ones are host-tested:

| Unit | Purpose | Pure/testable |
|---|---|---|
| `helpers/nrf24_fusion_model.{h,c}` | `FusedSpectrum` state + `FuriMutex` + snapshot/update | mostly |
| `helpers/nrf24_chanmap.{h,c}` | Wi-Fi ch / BLE adv-ch ↔ nRF24 span; dBm↔percent | **yes** |
| `helpers/nrf24_heatmap.{h,c}` | Heat palette LUT, intensity→RGB565, waterfall ring | **yes** |
| `helpers/nrf24_font5x7.h` | ~300-byte 5×7 ASCII font for takeover text | n/a |
| `nrf24_radio_internal.{h,c}` | Time-shared Wi-Fi+BLE worker | no (radio) |
| `scenes/scene_fusion_spectrum.c` | Takeover, 3 workers, render loop, controls, export | no |

Reused as-is: `nrf24_hw_listen_rpd_dwell` (sweep), `nrf24_export.c` (CSV), `nrf24_wifi_scan()` (AP scan w/ rssi+channel), `ble_tracker_hal_start_scan()`/`get_devices` (BLE rssi). New menu entry in `scene_menu.c`, private-gated; classic `scene_spectrum.c` unchanged (no regression).

---

## 3. Data model

```c
#define NRF_CH 126   // 2400..2525 MHz, channel = freq_MHz - 2400
typedef enum { SRC_NONE, SRC_HOPPER, SRC_WIFI, SRC_BLE } SourceClass;

typedef struct {
    uint8_t     occ[NRF_CH];       // nRF24 occupancy 0..100 (EMA of RPD)
    uint8_t     peak[NRF_CH];      // max-hold
    int8_t      wifi_dbm[NRF_CH];  // -128 = no data; real dBm spread over AP ~22 MHz
    int8_t      ble_dbm[NRF_CH];   // -128 = none; real dBm on adv channels
    SourceClass klass[NRF_CH];     // per-channel classification
    uint16_t    sweep_count;
    uint32_t    wifi_epoch, ble_epoch; // freshness → age overlays out
} FusedSpectrum;                    // guarded by FuriMutex
```

- **Fused intensity:** `inten = max(occ, dbm2pct(wifi_dbm), dbm2pct(ble_dbm))`; `dbm2pct` maps ≈[−95..−35] dBm → [0..100].
- **Channel mapping** (`nrf24_chanmap`, host-tested): Wi-Fi ch1=2412→nRF12, ch6→37, ch11→62, ch13→72; AP floods `[center−11 … center+11]`. BLE adv 37/38/39 → nRF 2/26/80. Occupancy with no Wi-Fi/BLE overlap → `SRC_HOPPER`.
- **Waterfall history:** ring of ~90 rows × `NRF_CH` bytes (intensity low 6 bits, class high 2 bits) ≈ **11 KB**, regular RAM (no-PSRAM-safe).

---

## 4. Workers & radio scheduler

- **nRF24 sweep worker** (FuriThread, continuous): existing `acquire → listen_rpd_dwell per ch → release` loop → `occ[]`/`peak[]`, ~20–30 sweeps/s. External SPI chip, independent.
- **Internal-radio worker** (real FreeRTOS task — `esp_wifi`/NimBLE need proper task/TLS context, same reason `nrf24_wifi_scan` uses `xTaskCreatePinnedToCore`): **time-shares** the single internal radio in a cycle — `WiFi init→AP scan (~3s)→deinit` → `wifi_dbm[]`; then `BLE init→scan (~2s)→deinit` → `ble_dbm[]`; then idle. Per-cycle init/deinit is robust and avoids ESP coexistence config. **Coex optimization deferred (YAGNI).**
- **Render worker** (FuriThread, ~15–20 FPS): snapshot under mutex → push waterfall row → draw unlocked → DMA blit.

**SPI coexistence:** nRF24 and ST7789 serialize on the SPI bus lock; already proven (classic spectrum sweeps while GUI renders). Render worker holds the bus lock only during stripe blits; sweep worker only during RPD reads. No new hazard.

---

## 5. Color render pipeline

Native dims read at runtime (`furi_hal_display_get_h_res/v_res`); Cardputer ~240 wide → ~1.9 px/channel. **Stripe rendering** (reuse one small DMA buffer per horizontal band, like the driver's `STRIPE_HEIGHT`) — a full 240×135×2 = 65 KB frame won't fit DMA RAM; stripes will.

- **Top half — bars:** height ∝ fused intensity, vertical **heat gradient** (blue→cyan→green→yellow→red, 256-entry RGB565 LUT), 1px **glow** halo, **peak-hold** line, labeled ticks at Wi-Fi centers + BLE adv channels.
- **Bottom half — waterfall:** ring buffer through the heat LUT, scrolling down, **source-tinted** (Wi-Fi cool-white cast, BLE magenta cast, hopper pure-heat).
- **Cursor + readout:** vertical line + 5×7-font line, e.g. `2437 MHz · 62% · −41 dBm · WiFi`.

---

## 6. Controls, errors, testing

**Controls (Cardputer keyboard):** ←/→ cursor-inspect · `W` bars/waterfall · `M` max-hold · `D` dwell cycle · `space` freeze · `P` palette cycle · `S` export · `esc` exit.

**Error handling / graceful degradation:** nRF24 probe fail → banner, still run Wi-Fi/BLE-only fused view; Wi-Fi or BLE init fail → that overlay empty (dbm=−128), logged, keep going; direct-draw acquire fail → clean abort to menu. **Exit:** stop+join all 3 workers, release direct-draw, power-down nRF24, ensure `esp_wifi`/BLE deinit'd, free ring+DMA buffers.

**Testing:** extend `tools/decoder_host_test` with `test_nrf24_fusion.c` — chanmap (Wi-Fi/BLE→nRF span), dbm↔pct, palette LUT monotonicity, fusion `max`, ring advance, classification. Mirrors existing `test_nrf24_spectrum.c`. On-device per stage: build+flash COM12, visual check + HackRF ground-truth (`fixedtune.py` duty per [[mob-cardputer-adv-build-workflow]]).

---

## 7. Staging

1. **Color takeover + nRF24 waterfall** — takeover, palette, stripe render, bars+waterfall from occupancy, font readout, cursor. *Stunning win lands here.*
2. **Wi-Fi fusion** — internal-radio worker (Wi-Fi phase), AP→channel spread, real-dBm overlay + markers, `SRC_WIFI`.
3. **BLE fusion** — add BLE scan phase, adv-channel dBm, `SRC_BLE`; leftover carriers → `SRC_HOPPER`.
4. **Polish/export** — palette cycle, freeze, max-hold, CSV (extend `nrf24_export`) + PNG snapshot to `/ext/apps_data/nrf24/`, per-channel stats.

---

## 8. Open items / notes

- Build/flash from `src_cardputer_adv` via `build_pingequa.bat`-style reconfigure (new files added → reconfigure required); mirror from canonical `src_m5stick_s3`.
- Private-gating pattern follows existing nrf24 Sniffer/KeySniffer/Fuzzer/BLE-Reactive (`#ifndef PUBLIC_BUILD`) — verify both preprocessor paths compile (public = feature absent, menu count adjusts).
- M5Stick S3 is *not* a primary target (2-button input, no keyboard); if later mirrored, controls need the KEY1/KEY2 scheme per [[m5sticks3-input-and-open-bugs]] — out of scope for now.
- Related: [[nrf24-pingequa-graft]], [[fzcard-nrf24-jammer-overhaul]], [[hackrf-pc-sdr-decoding]].
