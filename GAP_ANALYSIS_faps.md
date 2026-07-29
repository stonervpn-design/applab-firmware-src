# Gap Analysis — lab.flipper.net catalog vs. Cardputer-ADV port ("mob")

> Maps the Flipper Application Catalog (~700 apps) against what this port already
> ships, then filters for what is actually feasible on the ESP32-S3 / Hydra-RF
> hardware. Companion to `AUDIT_FINDINGS.md`. Written 2026-07-14.

## Key reality check

**This port does NOT load `.fap` files.** Every app is static-linked C, registered by
hand in `fam_config.py` `APPS[]`, compiled into one `Flipper-cardputer_adv-merged.bin`.
The lab.flipper.net files are compiled ARM (STM32WB55) `.fap`s — wrong CPU (vs ESP32-S3
Xtensa), wrong loader (no runtime .fap), wrong HAL (`furi_hal_serial_*`/`furi_hal_i2c_*`
are phantom here). So "converting a FAP" = **manually re-porting its C source** into the
monolith, per app. There is no batch transcode and no zip-of-files deliverable.

## 1. Already covered — do not re-port

Sub-GHz (RX/TX/jammer/replay/brute/playlist/ProtoView "Signal Decoder"/KeeLoq),
ProtoPirate v3.2 (car fobs), NFC (PN532 + `nfc_magic`, `mifare_fuzzer`, `rfid2_reader`),
Infrared (TV-B-Gone + Flipper-IRDB), BadUSB, full WiFi/BLE red-team suite (`wlan_app`,
`defender`, `ble_spam`, `flippergotchi`, `findmy`, `radio_scanner`), nRF24
(jammer/mousejack/spectrum/scan), LAN recon (`lan_attack`), wardriver, LoRa/meshtastic,
games (asteroids, game15, pong, snake20, tetris, texas_holdem), bus_tools, qrcode, clock,
power_monitor, key_copier, JS runtime.

## 2. Hardware-blocked — cannot port regardless of effort

- **All 125 kHz LFRFID** (EM4100, HID Prox, Indala, T5577; readers/emulators/fuzzers).
  `BOARD_HAS_RFID=0`. The Hydra RF cap is **CC1101 + nRF24 only** — no LF antenna coil.
  **The M5 Unit RFID2 does NOT change this**: it is a WS1850S = 13.56 MHz MFRC522-clone
  (same band as the PN532), already wired as `rfid2_reader`. It adds a second HF NFC
  frontend, not low-frequency RFID.
- **Picopass / iClass** — needs iClass HF framing the PN532/WS1850S won't do cleanly.
- **iButton / 1-Wire** — `ibutton`/`onewire` code exists but needs a 1-Wire GPIO pin
  wired first; hardware-gated, not a pure software port.

## 3. Missing + feasible — the actual shortlist

### Tier A — pure software, no HW dep, low RAM (best ROI, lowest risk)
Several are **already ported and sitting in `applications_user/`**, just disabled
(icon/global symbol collisions when static-linked) or unregistered:
- Disabled-collision: `blackjack`, `dice`, `tamagotchi_p1`, `t_embed_snake` (redundant
  with snake20), `flipper_xremote` (Infrared; big).
- Present but not in `APPS[]`: `mp3_player` (music), `weather_station` (Tier B),
  `blackjack_split`.
- Not yet in tree (port from scratch): 2048, Minesweeper, Arkanoid/Breakout, Game of
  Life, T-Rex Runner, Solitaire, Chess/Checkers; tools: Calculator, Unit Converter,
  Metronome, Pomodoro; generators: Barcode (UPC/EAN).

### Tier B — reuses backends the port already drives
- TPMS reader + Weather Station decoders (CC1101 / Signal-Decoder path; `weather_station`
  already in tree, unregistered).
- MFKey32 (nonce collection + offline key recovery) — PN532/RFID2; RAM-heavy, needs the
  adaptive-heap treatment (§6 of the briefing).
- NFC Magic write (already have `nfc_magic`), AC universal remote (IR),
  Mouse Jiggler / USB HID remote (BadUSB stack exists).

### Tier C — feasible but heavier / riskier (do last)
Amiibo emulation (NFC + keys + RAM), CC1101 spectrum analyzer (FFT/display RAM),
Sub-GHz signal generator.

## 4. Recommended first move

Enable the **already-ported disabled games** before porting anything new — lowest risk,
highest ROI. Starter batch: **Dice D&D, Tamagotchi, Blackjack** (three distinct games).
De-collision method TBD after confirming this port's icon/asset compile path (no Pillow;
§3 says checked-in XBM). Skip `t_embed_snake` (duplicate of snake20). Defer
`flipper_xremote` (largest, Infrared) to a later batch.
