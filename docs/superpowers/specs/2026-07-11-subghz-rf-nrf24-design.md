# Sub-GHz / RF + nRF24 Red-Team Features — Design Spec

**Date:** 2026-07-11
**Status:** Draft (design only, no implementation). Authorized security-research
tooling for the port owner (Cardputer ADV, ESP32-S3, **no PSRAM**).
**Target HW:** ESP32-S3, 8 MB flash, no PSRAM, CC1101 Sub-GHz, nRF24L01+ 2.4 GHz,
SD, 128x64 canvas, QWERTY.

## Goal

Add the remaining red-team RF capabilities to the port **that are not already
shipping**, and formally spec the two attack flows that are genuinely missing:

1. **RollBack / rolling-code capture-then-replay** (desync attack on HCS/rolling
   receivers) — a *transmitting* extension of the existing read-only analyzer.
2. **ProtoView-style universal OOK/2-FSK signal decoder/editor** — real-time
   visualize + decode + edit + replay of arbitrary bitstreams beyond the built-in
   protocol registry.
3. **Optional / flag value:** POCSAG pager decoder, Radio Scanner (RSSI channel
   scan), rtl_433-style weather/sensor decoders, and a dedicated nRF24
   promiscuous **sniffer list** (recon view distinct from the Mousejack scan).

## Context — what ALREADY exists (do NOT re-spec)

Grounding read of `applications/main/subghz/` and `applications/main/nrf24/`
shows **most of the originally-requested scope is already implemented and wired**.
This is the headline feasibility finding: the task brief under-counts the port.
The following are DONE and only *cited* here as the substrate the new work reuses:

- **Fixed-code brute-forcer — DONE.** Full SubBrute engine, on-device generation
  (no pre-made `.sub`): `applications/main/subghz/helpers/subbrute_protocols.{c,h}`
  (CAME 12-bit 303/307/315/330/433/868, NICE, Ansonic, Holtek 12-bit AM/FM,
  Chamberlain 7/8/9-bit, Linear/Linear-Delta, UNILARM/SMC5326 24-bit, PT2260
  24-bit), `subbrute_device.{c,h}`, `subbrute_worker.{c,h}`. UI is the
  **"Bruteforce"** start-menu item → `subghz_scene_bf_start` →
  `bf_setup_attack`/`bf_setup_extra`/`bf_run_attack` (protocol + freq + bit-length
  + delay + progress/stop) plus `bf_load_*`/`bf_save_*`. Registered in
  `subghz_scene_config.h` (lines 28-35). **No new brute-forcer needed.**
- **TPMS sniffer/decoder — DONE.** Protocol-layer decoders in
  `lib/subghz/protocols/`: `schrader_gg4.{c,h}`, `tpms_ford.{c,h}`,
  `tpms_renault.{c,h}`, `tpms_citroen.{c,h}`, `tpms_pmv107j.{c,h}` (Toyota PMV-107J),
  `tpms_generic.{c,h}` (`TPMSBlockGeneric` = sensor ID + pressure + temp). Registered
  in `lib/subghz/protocols/protocol_items.{c,h}`. Display via
  `applications/main/subghz/views/subghz_view_tpms_info.{c,h}` and editing via
  `subghz_scene_tpms_edit.c`. Manchester/FSK/OOK decode already runs off the CC1101
  FIFO through the standard decoder step machines. **No new TPMS decoder needed.**
- **nRF24 Mousejack — DONE.** `applications/main/nrf24/helpers/nrf24_mj_core.{c,h}`
  (pseudo-promiscuous fingerprint + CRC16-CCITT validate + vendor detect
  Logitech/Microsoft/MS-crypt, `mj_inject_keystroke`, `mj_inject_string`,
  `mj_logitech_wake`, `mj_ascii_to_hid`, US-layout HID table) and
  `nrf24_mj_ducky.{c,h}` (streaming DuckyScript-subset interpreter reading
  `/ext/badusb/*.txt` line-by-line — already the BadUSB-format reuse the brief
  asked for). UI: **"MouseJacker"** menu → `scene_mj_menu`/`mj_scan`/
  `mj_target_list`/`mj_script_select`/`mj_attack`/`mj_auto`. **No new Mousejack
  needed.**
- **nRF24 recon — largely DONE.** `nrf24_hw_rx_start_promiscuous()`,
  `nrf24_hw_rx_data_ready()`, `nrf24_hw_rx_read_payload()` in `nrf24_hw.{c,h}`;
  **Spectrum Analyzer** (`scene_spectrum` + `nrf24_spectrum_view`) does RSSI/RPD
  channel scan; `scene_mj_scan` does address discovery. A dedicated *promiscuous
  address/sniffer list* is the only recon gap (below, optional).
- **Rolling-code analyzer — DONE but READ-ONLY.**
  `subghz_scene_rolling_analyzer.c` parses an already-decoded DYNAMIC capture
  (protocol/mfr/serial/button/counter/verdict). Its own header states it
  "does NOT transmit, re-decode, or mutate the capture." **This is the hook for
  RollBack — the replay half does not exist yet.**

Shared substrate the new features reuse:

- RX/TX: `applications/main/subghz/helpers/subghz_txrx.{c,h}` —
  `subghz_txrx_rx_start()`, `subghz_txrx_tx_start(instance, FlipperFormat*)`,
  `subghz_txrx_stop()`, `subghz_txrx_set_preset(...)`,
  `subghz_txrx_get_decoder()` → `SubGhzProtocolDecoderBase*`.
- Raw async capture: `lib/subghz/subghz_worker.h` —
  `subghz_worker_set_pair_callback(worker, cb)` where
  `cb(void* ctx, bool level, uint32_t duration)` streams `LevelDuration` pairs.
  The RAW read view (`views/subghz_read_raw.{c,h}`, `subghz_read_raw_alloc`,
  `..._update_sample_write`) already consumes this stream — the ProtoView sample
  ring buffer mirrors it.
- History: `subghz_history.{c,h}` holds decoded captures (RollBack needs ≥2).
- Registration: scenes via `subghz_scene_config.h` (`ADD_SCENE(subghz, x, X)`);
  nRF24 scenes via `applications/main/nrf24/scenes/scene_config.h`; both apps are
  already in `fam_config.py::APPS` (`subghz`, `nrf24`).

## Design

### 1. RollBack — rolling-code capture-then-replay (desync)

**RF mechanism.** Many HCS200/HCS300/rolling receivers accept any code whose
counter is within a forward "window" (typ. 16) of the last-seen counter, and a
larger "resync" window (typ. 32k) that requires two consecutive valid codes.
Capturing ≥2 consecutive presses from a fob (which the legitimate receiver never
saw, because they were out of RF range or you captured while the owner was away)
lets you replay them **in the original order** to satisfy the resync path, or
replay a single not-yet-used code to actuate. Certain receivers can be pushed
out of sync (denial) by replaying an old lower-counter pair. This is the
**capture-then-replay** variant only.

**Flow / decode/encode.**
1. New **"RollBack"** start-menu item → `subghz_scene_rollback_capture`: reuses
   `subghz_txrx_rx_start()` + the existing dynamic decoder; on each decoded
   DYNAMIC frame push `{data, data_count_bit, protocol, counter}` into a small
   fixed slot array (`RollbackSlot slots[8]`) held in `SubGhzTxRx`/app context —
   NOT a growing buffer. Show a live list "Captured N/2+ (Cnt: …)".
2. When ≥2 slots with the *same serial* and *consecutive-ish counters* exist,
   enable **Replay**. `subghz_scene_rollback_replay` re-serializes each slot to a
   `FlipperFormat` (same path the emulate flow already uses) and calls
   `subghz_txrx_tx_start()` per slot, in a chosen order (Ascending / Descending /
   As-captured), with a UI-set inter-frame delay.
3. Each captured slot can also be **Saved** to `/ext/subghz/rollback/*.sub` via
   the existing save-name scene so it survives a reboot (no-PSRAM: never hold
   more than the 8 in-RAM slots; everything else streams to SD).

**Honesty / non-goal (see Non-Goals):** true **RollJam** (jam the receiver while
simultaneously capturing the fob, so the owner's press never lands) needs a
*second radio* to jam and receive at once. With one CC1101 this is impossible;
only capture-then-replay is speced.

**Reuse:** decoder + history + emulate/TX serialization already present; only the
slot store + ordered-replay driver + two scenes are new.

### 2. ProtoView-style universal signal decoder/editor

Reference: **antirez/protoview** (MIT, constrained-device C). Real-time
visualize + decode + edit + replay of arbitrary OOK / 2-FSK bitstreams that the
built-in registry does not cover (Oregon V2/V3 weather, raw PT2262, unknown
fixed codes).

**RF mechanism / capture.** Reuse `subghz_worker_set_pair_callback` to receive
`(level, duration)` pairs straight off the CC1101 async RX — the same feed the
RAW view uses. Write pairs into a **fixed-size ring buffer**
`typedef struct { uint32_t dur; uint8_t level; } PvSample;`
`PvSample ring[PV_RING_LEN]` with `PV_RING_LEN = 2048` (~10 KB — fits SRAM, no
PSRAM). When the ring is full, oldest samples are overwritten; a **freeze** key
stops capture so the frozen window can be scrolled/decoded/exported. Long
captures are *not* kept in RAM — an optional "dump to SD" writes the frozen ring
to `/ext/subghz/protoview/*.pvr` as it scrolls.

**Decode.** Port ProtoView's low-level classifier set (each is a small pure
function over the frozen sample window; no allocation):
- pulse-width / gap classifier → short/long symbol thresholds (auto from
  histogram of durations in the window).
- Manchester, PWM (PT2262-style), and pulse-position (Oregon) line-code decoders
  → bitstring + field guesses (bit count, inferred sync, candidate protocol name).
- Render: 128x64 scope-style waveform (one column per sample, level = row) with a
  cursor; footer shows decoded bits + best-guess label.

**Edit / replay.** The frozen bitstring is editable (QWERTY: move cursor, flip
bit, insert/delete symbol) in a `variable_item_list`-style hex/bit editor, then
re-encoded to raw timings and TX'd via `subghz_txrx_tx_start()` using a RAW
`FlipperFormat` (the RAW send path already exists — `subghz_read_raw_alloc`'s
send-only mode is the template).

**UI flow.** New **"Signal Decoder"** start-menu item →
`subghz_scene_protoview` (freq/preset pick reuses `receiver_config`) → live
scope; keys: Freeze/Resume, scroll, Decode (cycle decoders), Edit, Replay, Save.

**No-PSRAM discipline (explicit):** single 2048-sample ring (~10 KB); decode
windows are slices of the ring, never copies; edit buffer is one bitstring
(≤256 symbols); anything larger streams to SD. No dynamic capture growth.

### 3. Optional / flag value

- **POCSAG pager decoder** — FSK 512/1200/2400 baud, 32-bit codewords, BCH(31,21)
  + parity. Decode off the same worker pair stream into address/function/message
  (numeric + alphanumeric). New `lib/subghz/protocols/pocsag.{c,h}` +
  `subghz_scene_pocsag` (scroll view). Medium effort; robust for 1200-baud FLEX-free
  POCSAG only.
- **Radio Scanner (RSSI channel scan)** — sweep a freq list, park on the strongest
  channel above a threshold (like Flipper's stock scanner). Reuses
  `subghz_threshold_rssi.{c,h}` + frequency-analyzer worker. New
  `subghz_scene_scanner`. Low effort.
- **rtl_433-style sensor decoders** — Oregon/Nexus/Acurite temp-humidity, etc.
  Best delivered *through ProtoView's* decoder plug-ins rather than as a separate
  app; each is a small classifier. Low-medium effort, incremental.
- **nRF24 promiscuous sniffer list** — a recon view that runs
  `nrf24_hw_rx_start_promiscuous` across a channel range and lists distinct
  discovered addresses + hit counts (superset of `mj_scan`, not attack-bound).
  New `scene_sniff` + `nrf24_sniff_view` reusing `MjTarget`-style rows. Low effort.

## Components (concrete files)

New scenes/views live **inside the existing apps** (matches how Toolkit / Vehicle
Vulns / TPMS were added — no separate `.fap`). Group the new Sub-GHz RF tools that
don't belong to an existing menu under a lightweight home for cohesion, but do NOT
duplicate the brute-forcer.

Sub-GHz (`applications/main/subghz/`):
- `scenes/subghz_scene_rollback_capture.c` — RX + slot capture list.
- `scenes/subghz_scene_rollback_replay.c` — ordered replay + delay/order config.
- `helpers/subghz_rollback.{c,h}` — `RollbackSlot`, slot store,
  `subghz_rollback_add(SubGhzTxRx*, decoder)`, `subghz_rollback_replay(order,
  delay_ms, volatile bool* abort)`.
- `scenes/subghz_scene_protoview.c` — live scope + freeze + decode/edit/replay.
- `views/subghz_view_protoview.{c,h}` — 128x64 waveform + bit/edit render.
- `helpers/protoview_decode.{c,h}` — pure classifiers:
  `pv_classify_symbols(const PvSample*, size_t, PvThresholds*)`,
  `pv_decode_manchester(...)`, `pv_decode_pwm(...)`, `pv_decode_ppm(...)`,
  `pv_bits_to_raw(const char* bits, LevelDuration* out, size_t cap)`.
- `helpers/protoview_ring.{c,h}` — fixed `PvSample ring[2048]`, write cursor,
  freeze flag, SD spill.
- (optional) `scenes/subghz_scene_scanner.c`, `scenes/subghz_scene_pocsag.c`,
  `lib/subghz/protocols/pocsag.{c,h}`.
- Modify: `scenes/subghz_scene_config.h` (register new scenes),
  `scenes/subghz_scene_start.{c,h}` (menu items "RollBack", "Signal Decoder", opt.
  "Radio Scanner"/"POCSAG" + `SubmenuIndex*`).

nRF24 (`applications/main/nrf24/`) — optional recon only:
- `scenes/scene_sniff.c`, `views/nrf24_sniff_view.{c,h}`.
- Modify: `scenes/scene_config.h`, `scenes/scene_menu.c` (add "Sniffer" item +
  `SubmenuIndexSniffer`).

`fam_config.py`: **no change** — `subghz` and `nrf24` already in `APPS`; all new
work is scenes/helpers compiled into those existing apps. (Only touch it if a
POCSAG CLI plugin is later split out.)

### Interface signatures (design intent)

```c
// subghz_rollback.h
typedef struct { uint64_t data; uint8_t bits; uint32_t serial, counter;
                 uint8_t protocol_id; } RollbackSlot;
typedef enum { RollbackOrderAsCaptured, RollbackOrderAsc, RollbackOrderDesc }
    RollbackOrder;
bool     subghz_rollback_add(void* app, SubGhzProtocolDecoderBase* decoder);
uint8_t  subghz_rollback_count(void* app);
bool     subghz_rollback_replay(void* app, RollbackOrder order,
                                uint32_t delay_ms, volatile bool* abort);

// protoview_ring.h
typedef struct { uint32_t dur; uint8_t level; } PvSample;
void   pv_ring_reset(void);
void   pv_ring_push(bool level, uint32_t duration);   // worker pair_callback body
size_t pv_ring_snapshot(const PvSample** out);        // frozen window

// protoview_decode.h
typedef struct { uint32_t t_short, t_long, t_sync; } PvThresholds;
bool pv_classify_symbols(const PvSample* s, size_t n, PvThresholds* out);
int  pv_decode_manchester(const PvSample* s, size_t n, char* bits, size_t cap);
int  pv_decode_pwm(const PvSample* s, size_t n, char* bits, size_t cap);
size_t pv_bits_to_raw(const char* bits, uint32_t te, LevelDuration* out, size_t cap);
```

## Non-Goals

- **RollJam (simultaneous jam + capture).** Requires a 2nd radio (jam while
  receiving); impossible with one CC1101. Only capture-then-replay is speced.
- **Any feature needing a second radio** (concurrent TX+RX on the same band).
- **Re-implementing the fixed-code brute-forcer, TPMS decoder, or Mousejack** —
  all already ship (see Context). RollBack must not duplicate the read-only
  rolling analyzer's parsing; it *calls* the same decoder and adds TX.
- No new `.fap`; no crypto/enclave work (KeeLoq decrypt stays out — enclave is a
  stub on this port).
- ProtoView will not attempt full protocol auto-ID for every device — it decodes
  line-codes + best-guess labels; unknown stays "unknown/editable".

## No-PSRAM caveats

- ProtoView: one static `PvSample ring[2048]` (~10 KB) is the *only* sample store;
  decode windows are slices, edit buffer ≤256 symbols; larger dumps stream to SD.
- RollBack: ≤8 in-RAM slots; saved codes go to SD, never held in bulk.
- POCSAG/scanner: line-rate decode, no full-message RAM accumulation beyond one
  message buffer; long logs append to SD.
- Keep the subghz app `stack_size=8*1024` (unchanged); nRF24 is `4*1024` — verify
  the sniffer view fits or bump to `6*1024` if the row array pushes it.

## Legal / authorization note

Brute-force TX, rolling-code replay, jamming, and HID injection are gated
capabilities for **authorized security research on the owner's own equipment**.
- Reuse the existing dangerous-frequency gate (`subghz_dangerous_freq.c` /
  `subghz_load_dangerous_settings`) so region-illegal TX stays behind the same
  opt-in already in the port.
- RollBack replay and ProtoView Replay show a one-time confirm ("Transmit —
  authorized use only?") before first TX, mirroring the brute-forcer's run gate.
- nRF24 sniffer is RX-only (no gate); it lists addresses, does not inject.
- Ship no pre-baked target codes; everything is captured/generated on-device.

## Testing

**Compile gate:** `build_ci.bat` (cmd.exe + `IDF_TOOLS_PATH=~/.espressif`, per the
port's build note). Pure-logic helpers (`protoview_decode.*`, `subghz_rollback`
ordering, `pv_bits_to_raw`) are host-testable — guard any `main()` and unit-test
the classifiers against captured sample vectors.

**On-hardware checklist:**
- RollBack: capture ≥2 presses from an HCS/rolling fob → both land in slots with
  same serial + rising counter; Replay (each order) TXs; a bench receiver in
  learn/window mode actuates; Save writes `/ext/subghz/rollback/*.sub`; nothing in
  the read-only analyzer regressed.
- ProtoView: tune 433.92 OOK, capture a PT2262 remote → waveform renders, Decode
  yields the expected bitstring/bit-count, Freeze holds, Edit flips a bit, Replay
  re-TXs the edited frame; capture an Oregon sensor → PPM decode shows plausible
  temp bits; ring never grows (watch free heap); SD spill writes `.pvr`.
- Optional: Radio Scanner parks on an active channel; POCSAG decodes a known
  1200-baud page; nRF24 Sniffer lists a Logitech dongle address without injecting.
- Regression: Bruteforce, TPMS, MouseJacker, Jammer, Spectrum all still work.

## Open Items

1. Confirm the exact fields `subghz_protocol_decoder_base_get_string()` exposes
   for the rolling family so RollBack reads authoritative serial/counter (the
   analyzer already parses "Fix:/Cnt:/MF:" — reuse that parser, don't re-derive).
2. Does the port's `subghz_txrx_tx_start` accept a RAW `FlipperFormat` for
   ProtoView edited-frame replay, or must edited frames route through the
   `subghz_read_raw` send-only path? (Read `subghz_read_raw.c` send flow.)
3. RollBack receiver-window behavior varies by chip (HCS200/300/360/500,
   AN-Motors, etc.) — replay-order success is receiver-specific; document as
   best-effort, not guaranteed.
4. ProtoView threshold auto-detection robustness on noisy 433 — may need a manual
   TE override in the edit view (add a `variable_item` for `te`).
5. nRF24 pseudo-promiscuous timing: address discovery already works via `mj_scan`;
   confirm the standalone sniffer's dwell-per-channel is long enough to catch
   bursty HID without missing frames (tune against Spectrum's timing).
6. POCSAG BCH correction cost on ESP32-S3 — verify it decodes in real time at
   1200 baud before committing 512/2400 support.
```
