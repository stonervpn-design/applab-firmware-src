# WiFi CSI Radar v1 — Design + Plan

**Date:** 2026-07-13
**App:** `applications_user/wifi_radar/` (MENUEXTERNAL)
**Status:** Approved scope (v1 = 1D motion radar). Clean-room build — the reference
(Zeloksa/Cardputer-ADV-CSI-Radar) is proprietary/binary-only; we use only the public
ESP-IDF CSI API.

## 1. What it does (v1)
A WiFi **Channel State Information** motion detector. The ESP32-S3 radio reports the
per-subcarrier channel response of received frames; a human moving in the RF path
perturbs those subcarriers. We lock onto one nearby transmitter, learn a baseline of the
empty channel, then flag deviations from it as motion — sensitive enough for gross motion
and, in a quiet RF environment, breathing. Output on the 128×64 mono canvas: a scrolling
"radar" trace of the motion level, a big meter, MOTION/CLEAR status, packet rate + locked
BSSID, and an adjustable sensitivity threshold.

Out of scope for v1 (noted as later stretch): 2D trained zones; BMI270 IMU room-mapping.

## 2. Enabling CSI
`CONFIG_SOC_WIFI_CSI_SUPPORT=y` already; `CONFIG_ESP_WIFI_CSI_ENABLED` is unset. Set it to
`y` in `sdkconfig` (present-but-"not set" key → defaults won't flip it, edit directly) and
add it to `sdkconfig.defaults.m5stack_cardputer_adv` so clean builds keep it. CSI API used:
`esp_wifi_set_csi_config`, `esp_wifi_set_csi_rx_cb`, `esp_wifi_set_csi(true)`;
`wifi_csi_info_t{ mac[6], buf(int8 im,re pairs), len }`.

## 3. Signal source
Promiscuous mode on a chosen channel (credential-free, uses ambient WiFi). The CSI callback
fires for received frames. To keep the channel path stable, we **lock to a single source
MAC**: a 2 s LOCK phase counts CSI packets per source and picks the dominant transmitter;
only that MAC's CSI is processed thereafter (mixing sources = path noise). (v1.1 stretch:
associate + ping the gateway for a higher, steadier packet rate.)

## 4. DSP (pure, `csi_dsp`)
- `int csi_amplitudes(const int8_t* buf, int len, float* amp, int max_sc)` — amp[k] =
  sqrt(im^2+re^2) per subcarrier, skipping the invalid first pair; returns subcarrier count.
- baseline: `csi_baseline_acc(float* sum, const float* amp, int n)` + finalize by /count →
  mean amplitude per subcarrier over the ~8 s CALIBRATE window.
- `float csi_disturbance(const float* amp, const float* base, int n)` — mean over
  subcarriers of `|amp-base|/max(base,eps)`, i.e. normalized fractional deviation.
- `float csi_ema(float prev, float x, float a)` — smoothing (a≈0.2).
- `uint8_t csi_motion_level(float dist, float sensitivity)` — map smoothed distance to 0..100.
All pure → validated by `tests/csi_oracle.py`.

## 5. State machine (worker + CSI cb)
Phases: `LOCK (2 s)` → `CALIBRATE (8 s)` → `RUN`. The CSI cb (WiFi task) extracts amplitudes
and, by phase: LOCK → bump per-MAC counters; CALIBRATE → (locked MAC only) accumulate
baseline; RUN → (locked MAC only) compute disturbance → EMA → publish `motion_level` +
packet counter. The worker owns phase timing/transitions and the UI feeds off the published
level. Baseline is only written in CALIBRATE and only read in RUN (phase-sequenced, no lock
needed on the hot path). `OK` = recalibrate; `Up/Down` = sensitivity; `Left/Right` = channel;
`Back` = exit.

## 6. Modules / files
- `csi_dsp.{c,h}` — pure DSP (above).
- `wifi_radar.c` — CSI setup + cb + worker + UI + lifecycle (single app file; smaller than
  flippergotchi, one screen).
- `application.fam` — MENUEXTERNAL, requires ["gui","wlan"], category GPIO.
- `tests/csi_oracle.py` — Python mirror of csi_dsp.
- `sdkconfig` + `sdkconfig.defaults.m5stack_cardputer_adv` — enable CSI.

## 7. Plan (build order)
1. Enable CSI in sdkconfig + defaults; confirm `esp_wifi_set_csi*` compiles in a stub build.
2. `csi_dsp.{c,h}` + oracle (amplitudes/baseline/disturbance/ema/level). Validate oracle.
3. `wifi_radar.c`: CSI HAL setup (config+cb+enable) via the existing wlan_hal start +
   promiscuous; the per-MAC lock table; the LOCK→CALIBRATE→RUN worker; publish motion_level.
4. UI: radar trace ring (last 100 levels), meter bar, status line, sensitivity/channel.
5. `application.fam` + register in `fam_config.py` APPS. Build green.
6. Flash; on-device: stand still → CLEAR + low trace; walk through the RF path → MOTION +
   trace spike; recalibrate; change channel/sensitivity.

## 8. Threading/safety
CSI cb = WiFi task (not ISR) — amplitude math + a fenced publish of a float level + counter;
no heavy locks on the hot path. Baseline arrays are fixed-size (max 128 subcarriers). Per-MAC
lock table bounded (16). No attacker-length writes (CSI len clamped to the amp buffer).
