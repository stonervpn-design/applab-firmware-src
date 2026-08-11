# Multi-button rolling fob emulator — design spec

**Date:** 2026-08-11
**Branch:** feat/keeloq-no-desync-clone
**App:** applications_user/proto_pirate (private-only car fob toolkit)

## Goal
After capturing a car fob, save it as a single **identity fob** and present a
**multi-button emulate view** (like the reference Flipper video: title, `SN: CNT:xxxx`,
`F:… <preset>`, and a navigable grid of the protocol's buttons — Trunk/Lock/Panic/Unlock…).
Selecting a button **synthesizes** that button's frame from `serial + counter + button code +
manufacturer key`, transmits it, and **increments + persists the rolling counter** so the
emulator never desyncs. Replay-only protocols fall back to re-emitting the captured button.

## Scope
Every TX-capable private protocol. Two capability classes, determined by audit (Task 1):
- **Field encoder** (encoder `deserialize` accepts a button field, protocol exposes a button
  list) → full multi-button synthesis.
- **Replay encoder** (reproduces the captured frame only, e.g. Ford V3) → captured-button
  fallback (grid shows only the captured button).

Private-only: all new surface `#ifndef PUBLIC_BUILD` (car material stays off public builds).

## Non-goals
- No new radio/TX stack — reuse the existing emulate plugin transmitter.
- No key cracking — manufacturer key comes from the keystore (as today).
- Fixing partial-capture protocols (PSA/VAG serialize↔deserialize field coupling) is out of
  scope; those simply show fewer buttons / captured-only when fields are missing.

## Architecture

### 1. Button catalog (`protocols/protopirate_fob_buttons.{c,h}`)
`typedef struct { uint32_t code; const char* name; } PPFobButton;`
Per-protocol `const PPFobButton* pp_fob_buttons(const char* protocol_name, size_t* n)` +
`bool pp_fob_can_synthesize(const char* protocol_name)`. Backed by per-protocol tables
(reuse existing maps: vag_button_name, honda_v1_button_codes, kia button handling…). The
audit (Task 1) fills the authoritative table and the synth/replay classification.

### 2. Identity save (extend `scene_need_saving` / the save writer)
Saving a captured car fob writes a `.sub` (SubGhz Key File) carrying at minimum:
`Protocol, Frequency, Preset, Bit, Key, Serial, Cnt` (the fields each protocol's decoder
`serialize` already emits; ensure Serial + Cnt are present). The manufacturer key is NOT
stored — it is fetched from the keystore at TX time by protocol. A loaded fob with a
synthesizable protocol opens the multi-button view.

### 3. Multi-button emulate view (`views/protopirate_fob_emulate.{c,h}`)
Renders: header = protocol name (highlighted while transmitting), line `SN: CNT:<counter>`,
line `F:<freq> <preset>`, and the button grid from the catalog. D-pad navigates, OK
transmits the selected button. Shows the live counter.

### 4. Synthesize + TX (extend `scenes/plugins/protopirate_emulate_plugin.c`)
On OK: build a FlipperFormat from `(protocol, serial, current_counter, selected button code)`,
run the protocol's **encoder** `deserialize` (which pulls the key from the keystore), TX via
the existing `SubGhzTransmitter` path. Reuse the plugin's `original_counter`/`current_counter`.
Replay protocols: deserialize the stored captured frame instead.

### 5. Counter persistence
After each successful TX, `current_counter++` and write the new `Cnt` back to the fob's
`.sub` (no-desync, matching [[keeloq-no-desync-clone]]). Guarded so a failed/aborted TX does
not advance the stored counter.

## Data flow
capture → decode (serial, counter, btn, key-id) → **save identity .sub** → open →
**fob emulate view** (buttons from catalog) → select button → **encoder synthesize(serial,
counter, btn, keystore key)** → TX → counter++ → persist Cnt.

## Testing
- Host tests (`tests/`, WSL gcc): button-catalog lookups; a synthesize→re-decode round-trip
  per synth-capable protocol (reuse the `tools/decoder_host_test` harness: for each protocol,
  for each button, encode → decode → assert btn matches + counter advances).
- Build: private m5stick + cardputer green; public build car-free (verify_public_no_car.py);
  nm confirms new symbols absent from public ELF, present in private.
- On-device (owner): match the video — load a VAG fob, press each button, confirm counter
  advances and each button TXs a distinct frame.

## Risks / constraints
- Some protocols need keystore keys that are absent/partial on-device → those buttons TX an
  unvalidated frame (documented; same limitation as decode-side keystore).
- No-PSRAM Cardputer RAM: the view is small; reuse existing buffers, no large allocs.
