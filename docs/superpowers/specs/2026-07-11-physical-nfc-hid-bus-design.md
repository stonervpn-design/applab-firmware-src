# Physical, NFC, HID & Bus Tooling Suite — Design Spec

**Date:** 2026-07-11  
**Status:** Architecture approved. **Planning phase** — scoped feature set for Cardputer ADV (ESP32-S3, 8MB flash, NO PSRAM, native USB-OTG, PN532+ST25R3916 NFC, 125kHz LF RFID, IR RMT, GPIO, I2C/SPI/UART).

---

## Goal

Extend the Flipper port with a cohesive **tooling suite for authorized penetration testing & security research** on physical interfaces: Wiegand card readers (capture/replay), NFC key recovery & magic-card writing, MIFARE password checking, EMV card reconnaissance, BadBLE keyboard delivery, BadUSB enhancements (mouse, OS detection), hardware bus scanning (I2C/SPI/UART), and IR universal device control. Each tool is **independently useful** but designed to work within a unified security model: safety gates, advanced-unlock confirmation (reusing GPIO app patterns), and minimal-PSRAM constraints.

---

## Context: What Already Exists (Reuse, Don't Rebuild)

### NFC & MIFARE existing implementations

1. **Nonce collection (`nfc/helpers/mfkey32_logger.*`):** Detect-Reader emulates, collects auth nonces
2. **On-device mfkey32 recovery (per spec `2026-07-10-nfc-mfkey-magic-design.md`):** crapto1 + lfsr_recovery32 recovery; adds keys to user dict
3. **Magic-card writing phase 2:** Gen2 block-0 writes + Gen1a backdoor (7-bit 0x40) via raw-framing
4. **MIFARE Fuzzer:** existing tag/key brute-force on known weak keys
5. **ST25R3916 driver (per spec `2026-07-10-st25r3916-driver-design.md`):** PN532 fallback or native SPI/I2C; implements furi_hal_nfc API
6. **EMV parser plugin (nfc/helpers/protocol_support/emv/):** already decodes contactless card data
7. **Existing NFC protocol stack:** ISO14443-3a/4a, NTAG, Ultralight, MF Plus, FeliCa, ISO15693 supported via plugin architecture

### GPIO & hardware control patterns

1. **GPIO Manual Control app:** safe-pin / advanced-unlock pattern in `gpio/gpio_scene_manual.{c,h}`, `gpio_pin_catalog.{c,h}`, `gpio_driver.{c,h}`
2. **Pin catalog model:** compile-time table with tier (Safe/Advanced), capabilities, warnings
3. **Driver abstraction:** hardware apply/read/teardown via `gpio_driver_*` functions

### USB & BLE HID Infrastructure

1. **Bad USB app (`bad_usb/`):** DuckyScript parser, USB HID keystroke delivery via `furi_hal_usb_hid`
2. **BLE HID component (`components/ble_hid/`):** BLE keyboard support
3. **USB HID API (`furi_hal_usb_hid.h`):** keyboard + consumer control + modifiers; ASCII-to-keycode map; tinyUSB composite backend
4. **BLE profile library (`components/ble_profile/`):** Bluetooth stack integration
5. **HID ASCII map & keycode tables:** mature 16-bit keycode + modifier encoding

### Infrared & 125kHz RFID

1. **Infrared app (`infrared/`):** IR TX/RX via RMT, remote DB, scene/view infrastructure
2. **125kHz RFID app (`lfrfid/`):** modulation capture/playback

### Existing safe harness patterns

- Advanced-unlock gate in GPIO app: toggle + confirmation + per-pin warn strings
- Scene/view dispatch in Flipper framework: `view_dispatcher`, `submenu`, `variable_item_list`, `widget`

---

## Design

### 1. Wiegand Interface Tool

**Goal:** capture 26-bit and 34-bit Wiegand pulse sequences from a card reader's two data lines (D0/D1) via GPIO timing; decode & store credentials; replay/craft new credentials.

**Module: `applications/main/wiegand/`**

```
wiegand/
  application.fam
  wiegand_app.c / wiegand_app.h / wiegand_app_i.h
  wiegand_driver.{c,h}           // GPIO capture, pulse timing
  wiegand_decoder.{c,h}          // 26/34-bit decode, checksum
  wiegand_encoder.{c,h}          // craft credentials, encode to pulse sequence
  scenes/
    wiegand_scene_start.c         // Main menu
    wiegand_scene_capture.c       // Real-time capture (lists D0/D1 GPIO select, auto-detect rate)
    wiegand_scene_captured_list.c // Saved captures: view, replay, delete
    wiegand_scene_craft.c         // Manual credential entry → encode
    wiegand_scene_replay.c        // Send capture or crafted sequence
  views/
    wiegand_view_capture.{c,h}    // Live pulse display
    wiegand_view_decode.{c,h}     // Show parsed facility/card ID + checksum
  wiegand_catalog.{c,h}           // Pin catalog (GPIO1/2 Grove safe + ADVANCEDtier others)
  wiegand_files.c                 // SD: /ext/wiegand/captures/
```

**Hardware interface:**
- Reuse GPIO driver model from GPIO app (`gpio_driver.c` interface style) — read D0/D1 digital levels in a tight loop with timer-based capture (10µs resolution on LEDC timer or FreeRTOS tick ISR)
- Wiegand typically 1–2kHz pulse rate; capture for 2–5 seconds per swipe
- Timer precision: **ESP32-S3 ISR latency ~10–50µs** is sufficient for Wiegand (bits are ~125µs pulses)

**UI flow:**
```
Start menu → Wiegand
  ├─ Capture: Select D0 & D1 GPIO (Grove + Advanced unlock)
  │  └─ Real-time: "Ready (26/34 auto-detect)…" → capture on button press
  │     └─ Show live pulse train (ASCII graph or LED-style bars)
  │        └─ Decoded: "Facility: 123, Card: 456789, Checksum: OK"
  │           → Save to /ext/wiegand/captures/TIMESTAMP.wg
  ├─ Saved captures
  │  └─ List (file→date→decoded) → View details / Replay / Delete
  └─ Craft: Input facility ID + card ID
     → Compute 26-bit Wiegand format (example: 1-bit odd parity + 8-bit facility + 16-bit card + 1-bit even)
     → Encode pulse sequence
     → Replay option
```

**SD artifacts:**
- `ext/wiegand/captures/`: JSON or binary captures (timestamp, D0/D1 pulse timings, decoded result)
- Format: `[{ts_us: 0, d0: 1, d1: 0}, {ts_us: 125, d0: 0, d1: 1}, …]`

**Safety:** Gate D0/D1 selection behind advanced-unlock (same confirm pattern as GPIO Manual Control advanced pins).

---

### 2. MIFARE Classic Nested Key Recovery (On-Device)

**Goal:** given one known sector key (obtained via mfkey32 or brute-force), recover remaining sector keys via the PN532/ST25R3916 timing channel without external tools or PSRAM-heavy tables. Complements existing mfkey32 recovery.

**Constraint:** **NO hardnested** (requires ~700MB state table; device has 8MB flash, NO PSRAM). **Nested only** (~100KB state tables fit in flash).

**Module: extend `applications/main/nfc/helpers/` + new `nfc/scenes/nfc_scene_mf_classic_nested_recover.c`**

```
nfc/helpers/
  nested_recover.{c,h}            // NEW: nested recovery (smaller state table)
  nested_state.{h}                // Struct for nested recover context
nfc/scenes/
  nfc_scene_mf_classic_nested_recover.c  // NEW: UI for nested recovery workflow
```

**Algorithm (nested only):**
- **Input:** known sector key (from mfkey32 or user), Mifare card
- **Process:**
  1. Enumerate PN532 distance-vs-nonce observations (use ST25R's built-in `rf_timing` if available, else estimate from poller latency)
  2. Filter nested_state candidates that produce the observed distance distribution
  3. Recover key candidate via LFSR rollback (similar to mfkey32 but tighter timing loop)
- **Reference:** AloneLiberty/FlipperNested implementation (which targets ~50ms per key on Flipper F7)

**Feasibility:**
- PN532 over I2C: **timing latency ±5–10ms** (less precise than ST25R's edge-triggered IRQ, but usable for nested detection distance)
- ST25R over SPI: **timing latency ±1µs** (full precision; makes nested robust)
- **Decision:** spec both paths; ST25R preferred once that driver lands

**UI flow:**
```
NFC app → Saved card → "Recover Keys (Advanced)"
  → Gate: advanced-unlock confirm + "Nested recovery requires 5–30 sec per key"
  → Start with: select known key (from dictionary or cache)
  → Progress: "Recovering sector 1…" (worker thread)
  → Result: "Key sector 1 [AB12CD34EF56] recovered" → Add to dictionary
  → Try next unknown sector or done
```

**Safety:** Worker thread prevents UI freeze; can cancel mid-recovery.

---

### 3. MIFARE Ultralight / NTAG Password Checking

**Goal:** test 32-bit passwords for Ultralight EV1 / NTAG4xx / NTAG I2C with known/derived wordlists; read-only verification (no writes).

**Module: `applications/main/nfc/scenes/nfc_scene_mf_ultralight_pwd_check.c` (extend existing MFU support)**

**Wordlist sources (SD card):**
- `ext/nfc/ul_passwords.txt`: common defaults (Mifare, NTAG, manufacturer defaults)
- User-supplied: `ext/nfc/wordlist_custom.txt`

**Algorithm:**
- For each password candidate (32-bit), issue `VERIFY_PWD` APDU
- Time the response: OK = valid, timeout/NACK = invalid
- Cache results per UID (prevents duplicate attempts)

**UI flow:**
```
NFC app → Saved Ultralight card → "Check Password"
  → Select wordlist (built-in defaults + custom files from SD)
  → Progress: "Trying 1234… Testing NTAG default list" (non-blocking worker)
  → Result: "Password matched: 0xABCD1234" or "No match in wordlist"
  → Option: export test results to SD
```

**SD artifacts:**
- `ext/nfc/ul_passwords.txt` (shipped defaults)
- `ext/nfc/ul_pwd_results_TIMESTAMP.csv` (test results: UID, attempts, found/notfound)

---

### 4. EMV Contactless Card Reader

**Goal:** read cardholder-visible data from contactless EMV cards (Visa, Mastercard, AMEX) for audit/reconnaissance: PAN (partial), expiry, transaction log. **Read-only** — no cloning, no crypto attacks.

**Module: extend NFC EMV plugin (`nfc/helpers/protocol_support/emv/nfc_emv_parser.c` + new scene)**

**Existing support:** NFC app already parses EMV; we add a dedicated **Read Contactless EMV Card** scene with cleaner output.

**What we read (public data channels):**
- Track 2 equivalent (MSD mode): PAN (full or partial per card), expiry, service code
- PDOL responses: transaction data (amount, currency, date, terminal ID)
- **Not touched:** CVM list, issuer scripts, cryptography (genuinely read-only)

**UI flow:**
```
NFC app → "Read Contactless Card"
  → Hold device over card
  → Auto-detect EMV (tap IC if NFC fails, or check bounds)
  → Display:
     ├─ Cardholder Name (if present)
     ├─ PAN: ●●●●●●●●●●●1234 (masked for safety)
     ├─ Expiry: MM/YY
     ├─ Card Brand: Visa / Mastercard / AMEX
     ├─ Last 5 transactions: amount, date, merchant, approval/decline
  → Save to /ext/nfc/emv_reads/TIMESTAMP.txt
```

**Safety:** UI **always masks PAN** (last 4 digits only) to prevent accidental exposure on-device.

---

### 5. BadBLE — HID Keyboard over BLE

**Goal:** deliver DuckyScript payloads over Bluetooth HID (device acts as a wireless keyboard), reusing the Bad USB DuckyScript parser + native ESP32 BLE HID.

**Module: new `applications/main/badbletool/` (parallels bad_usb)**

```
badbletool/
  application.fam
  badbletool_app.c / badbletool_app.h / badbletool_app_i.h
  badbletool_ducky_parser.c       // Reuse bad_usb's parser logic
  badbletool_ble_hid.{c,h}        // Wrapper around components/ble_hid
  scenes/
    badbletool_scene_start.c
    badbletool_scene_file_select.c
    badbletool_scene_pair_mode.c  // "Enable BLE pairing, scan for devices"
    badbletool_scene_execute.c    // Play script with device connection status
  views/
    badbletool_view_script_editor.{c,h}  // Text input for DuckyScript
    badbletool_view_ble_status.{c,h}     // Connection status, pairing info
  badbletool_files.c
```

**DuckyScript parser reuse:**
- Copy `bad_usb/helpers/ducky_script_interpreter.{c,h}` or link as a shared library
- Extend to support BLE-specific timing (slightly looser than USB, ~10ms inter-keystroke delays)

**BLE pairing flow:**
1. User enables "BLE Keyboard Mode" → device advertises as "Flipper BadBLE Keyboard"
2. Host scans, finds device, initiates pairing (no PIN or 0000 default)
3. Device enters HID keyboard profile (via `components/ble_hid`)
4. Script execution sends HID key codes over BLE

**Timing challenges:**
- BLE has ~10–50ms latency per HID report (vs USB ~1ms)
- DELAY command in DuckyScript should default to 100ms (vs 50ms for USB)
- Connection drop → graceful error (show "BLE disconnected" on UI)

**UI flow:**
```
Bad Bluetooth Tool
  ├─ File Select: pick DuckyScript file from SD
  │  → Preview script
  ├─ Pairing: Show "Awaiting pairing from host…" (BLE advertisement active)
  │  → Visual: blinking indicator until paired
  │  → Timeout: 60sec, then exit
  └─ Execute: Show key deliveries in real-time
     └─ Status line: "Connected [Device Name]" or "Connection lost!"
```

**SD artifacts:** Same as Bad USB (DuckyScript files in `ext/badusb/`).

---

### 6. BadUSB Enhancements

**Goal:** extend the existing Bad USB app with:
1. Richer DuckyScript coverage (STRING with non-ASCII, multiple payloads)
2. OS-detection conditional payloads (if-Windows, if-macOS, if-Linux blocks)
3. Mouse movement & clicks via USB HID composite
4. Mouse jiggler (idle detection evasion)

**Module: extend `applications/main/bad_usb/`**

**DuckyScript extensions:**

```
// Existing:
STRING hello world
DELAY 1000
KEY_PRESS ENTER

// NEW: Mouse support
MOUSE_MOVE 100 50      // Δx Δy (relative movement)
MOUSE_CLICK LEFT       // Button: LEFT / RIGHT / MIDDLE
MOUSE_DRAG_TO 200 150  // Absolute drag

// NEW: OS detection
OS_DETECT
  IF_WIN
    STRING powershell -c "..."
  ENDIF
  IF_MAC
    STRING open -a Terminal
  ENDIF
  IF_LINUX
    KEY_PRESS ALT_F2
  ENDIF

// NEW: Mouse jiggler (runs continuously until STOP_JIGGLE)
START_JIGGLE 2000 500  // interval_ms, distance_px
// ... execute other commands ...
STOP_JIGGLE
```

**Implementation:**
- Extend `bad_usb/helpers/ducky_script_interpreter.c` to parse new commands
- Add mouse handling: USB HID composite (keyboard + mouse descriptor)
- OS detection: query host via FEATURE_REPORT or sample first keypress timing (heuristic)
- Jiggler: spawn a worker thread that sends micro-moves every N ms

**Constraints:**
- USB composite descriptor must include both keyboard AND mouse (tinyUSB config)
- OS detection is **best-effort heuristic** (no 100% reliable host fingerprint via HID); document limitations

---

### 7. Hardware Bus Tools

**Goal:** I2C/SPI/UART reconnaissance tools: scanner (I2C address enumeration), EEPROM/flash read, UART console with baud auto-detect.

**Module: new `applications/main/bus_tools/`**

```
bus_tools/
  application.fam
  bus_tools_app.c / bus_tools_app.h / bus_tools_app_i.h
  bus_tools_driver.{c,h}          // I2C/SPI/UART abstraction over furi_hal
  bus_tools_i2c_scanner.c         // Enumerate 0x00–0x7F
  bus_tools_spi_flash_reader.c    // Read + dump to SD
  bus_tools_uart_terminal.c       // Console, baud auto-detect
  scenes/
    bus_tools_scene_main_menu.c
    bus_tools_scene_i2c.c          // Select SDA/SCL pins, run scan
    bus_tools_scene_spi.c          // Select MOSI/MISO/CLK/CS, specify chip size
    bus_tools_scene_uart.c         // Select TX/RX pins, auto-baud or manual, terminal view
  views/
    bus_tools_view_i2c_results.{c,h}
    bus_tools_view_uart_terminal.{c,h}
  bus_tools_pin_catalog.{c,h}     // SDA/SCL/TX/RX options (reuse GPIO safe/advanced tiers)
```

**I2C Scanner:**
- Enumerate addresses 0x00–0x7F
- Attempt 1-byte read; if ACK, device present
- Report: address, optional device name from a small DB (common chips: 0x50 EEPROM, 0x68 RTC, etc.)
- Save results to `ext/bus/i2c_scan_TIMESTAMP.txt`

**SPI Flash Reader:**
- Auto-detect JEDEC ID (0x9F command)
- Dump full flash to `ext/bus/spi_flash_TIMESTAMP.bin`
- Progress bar (reads can be slow on 1-byte-at-a-time poller)

**UART Terminal:**
- Pin selection (TX/RX + optional CTS/RTS from Grove or Advanced pins)
- Baud rate: manual selection or auto-detect (send a known sync pattern `\r`, measure baud via timing)
- Console: receive + display (scrollable), send via on-device keyboard input
- Logging: save session to `ext/bus/uart_session_TIMESTAMP.log`

**Safety:** I2C/SPI/UART pin selection gated behind advanced-unlock (same as GPIO).

---

### 8. TV-B-Gone — Universal IR Device Control

**Goal:** blast universal power-off IR codes to any TV, projector, or AC unit. Reuse existing IR infrastructure; add a large code DB on SD.

**Module: extend `applications/main/infrared/`**

**New scene:** `infrared_scene_universal_blaster.c`

```
infrared_scene_universal_blaster.c   // NEW: universal codes menu
```

**Code database (on SD, JSON format):**
```
ext/infrared/tvbgone_db.json
[
  {
    "brand": "Sony",
    "model": "Generic TV",
    "code_type": "NEC",  // or RC5, RC6, SIRC
    "frequency": 38000,
    "pattern": [9000, 4500, 560, 560, ...]  // durations in µs
  },
  ...
]
```

**UI flow:**
```
Infrared app → "Universal Blaster"
  ├─ Browse by Brand (Sony, LG, Samsung, Philips, Panasonic, …)
  │  └─ Select model (Generic TV, Projector, AC Unit, Soundbar, …)
  │     └─ Select code (Power Off, Mute, Volume Up, …)
  │        → "Send in 3… 2… 1…" (countdown)
  │        → "Sent!" (LED feedback)
  └─ Repeat code: hold OK to re-send (useful for AC units needing multiple blasts)
```

**Code sourcing & format:**
- Curate initial DB from public sources (shirriff/Arduino-IRremote database, universal remote codes)
- Format: gzip-compressed JSON on SD to save space (~500 codes ≈ 50KB compressed)
- Fallback: if no code found, offer "Custom IR" option (load .ir file user saved manually)

**Safety:** No custom code injection into DB; only shipped + user's own .ir files.

---

### 9. Optional/Lower-Priority Features (Flagged as Hardware-Dependent)

#### U2F/FIDO2 USB Authenticator
- **Module:** Extend existing `applications/main/u2f/`
- **Current status:** Already present in port; add USB HID attestation flow (generate key pairs on-device, store as CTAP2 resident credential)
- **Prerequisite:** USB HID working (already is)
- **Lower priority:** Requires user pairing workflow (not as critical as BadBLE); deferred to Phase 2

#### FeliCa Read/Emulate
- **Module:** Extend NFC app plugin system
- **Current support:** Already in `nfc_felica` plugin
- **Enhancement:** Add user-friendly "FeliCa Emulate" scene (simpler than Mifare emulation; fewer key varieties)
- **Priority:** Lower (Japan-centric; not all devices have FeliCa readers)

#### MagSpoof (Magnetic Stripe Cloning)
- **Status:** **NOT IMPLEMENTED** — requires external hardware (coil, H-bridge circuit, GPIO PWM at specific frequencies)
- **Flag:** Hardware dependency; document as future add-on only
- **Scope:** Out of this spec

---

## Components (File Layout & Registrations)

### New Applications

| App | Path | fam Entry | Depends On |
|-----|------|-----------|-----------|
| **Wiegand** | `applications/main/wiegand/` | `appid="wiegand"` | GPIO driver, FreeRTOS timer |
| **BadBLE Tool** | `applications/main/badbletool/` | `appid="badbletool"` | ble_hid, ble_profile, ducky parser |
| **Bus Tools** | `applications/main/bus_tools/` | `appid="bus_tools"` | furi_hal_i2c, furi_hal_spi, furi_hal_uart |

### Extended Applications (Reuse Existing)

| App | New Scenes/Views | Depends On |
|-----|------------------|-----------|
| **NFC** | `nfc_scene_mf_classic_nested_recover.c`, `nfc_scene_mf_ultralight_pwd_check.c`, `nfc_scene_emv_read.c` | nested_recover.c, mfkey32_logger, NFC stack |
| **Infrared** | `infrared_scene_universal_blaster.c` | Existing IR driver, SD JSON loader |
| **Bad USB** | Extend `ducky_script_interpreter.c` with mouse + OS-detect + JIGGLE | Existing USB HID |

### Shared Libraries

```
// Existing (link to Bad USB)
bad_usb/helpers/ducky_script_interpreter.{c,h}

// NEW or EXTEND
components/
  wiegand_driver/
    wiegand_gpio_capture.c        // Tight GPIO timing loop
    wiegand_gpio_capture.h
  bus_tools_driver/
    bus_tools_i2c_scanner.c
    bus_tools_spi_flash_reader.c
    bus_tools_uart_auto_baud.c
  nested_recovery/
    nested_recover.c              // Nested recovery (NOT hardnested)
    nested_recover.h
```

### fam_config.py Registration

```python
# applications/main/application.fam (master app list)

App(
    appid="wiegand",
    name="Wiegand Reader",
    apptype=FlipperAppType.MENUEXTERNAL,
    entry_point="wiegand_app",
    icon="A_GPIO_14",  # Reuse GPIO icon
    stack_size=3 * 1024,
    order=52,
    fap_libs=["assets"],
    fap_icon="icon.png",
    fap_category="GPIO",
)

App(
    appid="badbletool",
    name="Bad Bluetooth",
    apptype=FlipperAppType.MENUEXTERNAL,
    entry_point="badbletool_app",
    icon="A_Bluetooth_14",
    stack_size=4 * 1024,
    order=71,
    fap_libs=["assets", "ble_profile"],
    fap_icon="icon.png",
    fap_category="HID",
)

App(
    appid="bus_tools",
    name="Bus Tools",
    apptype=FlipperAppType.MENUEXTERNAL,
    entry_point="bus_tools_app",
    icon="A_GPIO_14",
    stack_size=3 * 1024,
    order=53,
    fap_libs=["assets"],
    fap_icon="icon.png",
    fap_category="GPIO",
)
```

---

## Non-Goals

- **Hardnested key recovery** — impossible without PSRAM state tables (~700MB)
- **DESFire crypto attacks** — read-only structure recon only
- **Live-payment card cloning** — EMV reading stays read-only; no cryptogram generation
- **Magspoof without external hardware** — flag as future add-on, out of this release
- **Flipper app plugin loader** — all new features are built-in (no external FAP support for Wiegand/Bus tools)
- **Multi-protocol HID (keyboard+mouse) SIMULTANEOUSLY over Bad USB** — composite descriptor supports both, but DuckyScript executes sequentially; parallel delivery deferred

---

## No-PSRAM Constraints (Baked Into Design)

1. **Nested recovery state tables:** sized at ~100KB (fits in flash); enumerate candidates in memory-efficient streaming passes
2. **Bus tools UART/SPI dumps:** streamed directly to SD card (no buffering in RAM beyond 4KB circular buffer)
3. **Wiegand pulse capture:** fixed ring buffer (1000 events × 8 bytes = 8KB)
4. **IR code database:** gzip-compressed JSON on SD; loaded into RAM piecemeal (one record at a time for UI iteration)
5. **EMV transaction log:** read and display only (no caching multi-record logs; show last 5 in UI only)
6. **BadBLE/BadUSB DuckyScript:** already proven to work with bad_usb's parser; no change to memory model

---

## Safety & Authorization Gating

### 1. Advanced-Unlock Pattern (Reuse GPIO Model)

All intrusive tools (pin selection, nested recovery, bus scanning) are gated behind a **two-step unlock:**

```c
// In each app's scene_main or scene_start:
if (!app->advanced_unlocked) {
    view_dispatcher_send_custom_event(
        app->view_dispatcher,
        APP_EVENT_REQUEST_ADVANCED_UNLOCK
    );
    // Show "Advanced Mode — Confirm to enable?" + countdown
    // User must hold OK for 2 seconds or press OK twice
    return;
}
// Only now allow pin/key/bus selection
```

### 2. Per-Feature Warnings

- **Wiegand capture:** "Capture may disrupt reader operation for 5 seconds"
- **Nested recovery:** "Recovery may take 30 seconds per key. Card must not be removed."
- **Bus scanning:** "I2C scan may trigger unintended device responses"
- **UART terminal:** "Sending commands to unknown devices can damage hardware"
- **BadBLE/BadUSB:** No additional gate (reuses existing app's caution text)

### 3. GPIO Pin Warnings

Wiegand & Bus Tools reuse `gpio_pin_catalog.c` pattern:
- **SAFE tier:** only free, non-peripheral pins (Grove ports)
- **ADVANCED tier:** shared pins (CC1101, SPI, etc.) with per-pin `warn` string

Example Wiegand Catalog:
```c
{
    .name = "GPIO1 (Grove SCL / IR-RX)",
    .gpio = 1,
    .tier = GpioTierAdvanced,
    .warn = "Shared with IR-RX — disable IR to use",
    .caps = 0  // Read-only on Wiegand (D1 line)
},
{
    .name = "GPIO2 (Grove SDA)",
    .gpio = 2,
    .tier = GpioTierSafe,
    .warn = NULL,
    .caps = GpioCapInput
},
```

### 4. Mandatory User Confirmation for Risky Operations

- **Writing to magic cards:** "Overwrite card UID/block 0 — IRREVERSIBLE"
- **Exporting dumps:** "Contains sector keys — store securely"
- **Nested recovery start:** "Requires 5+ minutes. Continue?"

---

## Testing

### Compile Gate
```bash
cd /path/to/firmware
build_ci.bat
# Expect: "Project build complete" + "=== BUILD OK ===" (no errors in new modules)
```

### Unit Tests (Host)

For crypto/parsing logic (no hardware needed):
- **Nested recovery:** feed known test vector (Flipper mfkey test data) → assert recovers correct key
- **Wiegand decoder:** test 26/34-bit checksums against reference implementations
- **EMV parser:** feed known card APDUs → assert correct PAN/expiry extraction

Test harness: `pytest` on host, or Flipper's built-in unit test framework.

### On-Hardware Checklist

1. **Wiegand:**
   - [ ] Compile gate passes
   - [ ] App launches; "Capture" scene selects GPIO1 and GPIO2 (SAFE tier)
   - [ ] Advanced unlock + confirm reveals ADVANCED pins with warnings
   - [ ] Capture real Wiegand reader pulses (use waveform generator if reader unavailable)
   - [ ] Decode shows correct facility/card ID
   - [ ] Replay pulses appear on oscilloscope or to test reader

2. **Nested Recovery:**
   - [ ] Compile gate passes
   - [ ] NFC app → Saved MIFARE Classic → "Recover Keys (Advanced)" appears
   - [ ] Select known key, start recovery
   - [ ] Progress shows "Recovering sector X…"
   - [ ] Recovered key added to dictionary
   - [ ] Re-read card with new key succeeds

3. **BadBLE:**
   - [ ] Compile gate passes
   - [ ] App launches; DuckyScript file picker works
   - [ ] "Enable Pairing" shows BLE advertisement
   - [ ] Host scans, finds "Flipper BadBLE Keyboard", pairs (no PIN or 0000)
   - [ ] Execute script; keys appear on host
   - [ ] Test MOUSE commands (if implemented)
   - [ ] Disconnect handling doesn't crash app

4. **Bus Tools:**
   - [ ] Compile gate passes
   - [ ] I2C Scanner: connect known device (e.g., EEPROM on 0x50), scan, detect address
   - [ ] UART: select TX/RX pins, auto-detect baud of a known serial device, show received data
   - [ ] SPI: detect JEDEC ID of a known flash chip

5. **EMV Read:**
   - [ ] Compile gate passes
   - [ ] NFC app → "Read Contactless Card" scene exists
   - [ ] Tap contactless EMV card, display PAN (masked), expiry, transactions
   - [ ] Save to SD; file readable on host

6. **Universal Blaster:**
   - [ ] Compile gate passes
   - [ ] Infrared app → "Universal Blaster" scene
   - [ ] Browse Sony → Generic TV → Power Off
   - [ ] Send code; IR LED pulses visible on scope or IR receiver

---

## Open Items (Planning Phase)

1. **Nested recovery timing precision:** Decide ST25R3916 vs PN532 timing strategy once driver lands. PN532 over I2C may need heuristic tuning of distance thresholds; ST25R will be more robust.

2. **Wiegand auto-rate detection:** Implement heuristic (measure pulse width distribution) to auto-select 26 vs 34-bit decode. Fallback: manual selection in UI.

3. **BadBLE pairing security:** Determine if 0000 PIN or no PIN (Just Works) is acceptable. Consider bonding (persistent pairing) vs temporary pairing per session.

4. **Mouse jiggler pattern:** Finalize micro-move algorithm (random walk vs sine wave) to avoid being too obvious to anti-cheat software.

5. **EMV transaction log parsing:** Confirm which fields (amount, date, merchant) are reliably present in all card brands (Visa/MC/AMEX). May need per-brand parser variants.

6. **IR code database format:** Finalize JSON schema + compression strategy (gzip vs DEFLATE). Curate initial ~500-code set from shirriff/open sources.

7. **DESFire "read-only" scope:** Clarify which commands (GetFileSettings, GetKeySettings, GetAppIDs) are considered recon vs attack. Document hard boundary.

8. **U2F/FIDO2 integration:** If time permits, add "Generate Resident Credential on-device" workflow. Otherwise, defer to Phase 2.

---

## Architecture Summary (10-Line)

1. **Wiegand Interface:** Tight GPIO ISR captures D0/D1 pulse trains; decoder extracts 26/34-bit credentials; replay via GPIO driver; integrated into GPIO app framework
2. **Nested Recovery:** Streaming LFSR enumeration (no PSRAM tables); PN532 timing loop collects distance observations; state filtering reduces candidate space; fits in 8MB flash
3. **Ultralight Password Check:** Wordlist-based VERIFY_PWD APDU loop; caches per-UID results; non-blocking worker thread; results exported to SD
4. **EMV Read:** Reuse existing NFC EMV plugin; dedicated "Read Contactless" scene; always-masked PAN display; transaction log harvested via APDU CDOL response
5. **BadBLE:** DuckyScript parser reused from Bad USB; wraps BLE HID component (`components/ble_hid`); supports MOUSE_MOVE/CLICK; advertises as "Flipper Keyboard"
6. **BadUSB Mouse+Jiggle Extensions:** USB composite descriptor with keyboard+mouse; DuckyScript interpreter extended with MOUSE_* and START_JIGGLE commands; OS heuristics via keystroke timing
7. **Bus Tools:** I2C scanner (0x00–0x7F enumeration); SPI flash JEDEC read + dump to SD; UART auto-baud via sync pattern; all via pin catalog + advanced-unlock gate
8. **Universal Blaster:** SD-based compressed JSON IR code database; brand/model/function tree UI; reuses infrared app driver; sends patterns via IR RMT
9. **Safety Model:** Extends GPIO app's advanced-unlock pattern (2-step confirm); per-tool warnings + per-pin warnings for shared GPIOs; no crypto attacks (read-only where applicable)
10. **PSRAM-Free:** Nested tables <100KB (flash-resident); bus dumps stream to SD; IR DB compressed; DuckyScript/EMV caching minimal; all fits in 8MB with -Os optimization

---

## Feasibility Flags

| Feature | Risk | Mitigation |
|---------|------|-----------|
| **Nested via PN532 I2C timing** | ±5–10ms latency may not detect distance precisely | ST25R driver provides ±1µs; use PN532 as fallback only |
| **BadBLE pairing flow** | Android/iOS pairing UX varies; no PIN = "Just Works" may be rejected on some hosts | Document as "Best-effort"; provide PIN entry option if pairing fails |
| **UART auto-baud at low speeds (<1200)** | Single sync pattern insufficient; ambiguous timing | Support manual baud selection; auto-baud for ≥9600 only |
| **EMV transaction log parsing** | Not all cards expose log via ISO7816 APDUs; some use proprietary schemes | Read what's available; handle parse failures gracefully; document card-specific quirks |
| **IR DB curation** | Maintaining 500+ codes is labor-intensive; coverage gaps inevitable | Start with popular brands (Sony, LG, Samsung, Philips); accept community PRs for additions |
| **Wiegand auto-detection (26 vs 34-bit)** | Pulse timing alone may be ambiguous (overlap in some weak implementations) | Provide manual override in UI; log raw pulse train for debugging |

---

## Related / Future Specs

- **Blue Defender Mode (Drone Remote-ID Detector):** Separate spec (TBD). Cross-link once published.
- **ST25R3916 Driver:** Spec `2026-07-10-st25r3916-driver-design.md` — prerequisite for ST25R-based nested + Gen1a magic writes.
- **On-device mfkey32 Recovery:** Spec `2026-07-10-nfc-mfkey-magic-design.md` — already built; nested + magic-write complement this.

---

## References

- **Wiegand:** OSDP 2.3 spec (public), AloneLiberty/FlipperNested (nested algorithm ref)
- **MIFARE Nested:** AloneLiberty/FlipperNested, darkside/nested detection literature
- **EMV:** EMV Book 3 (public), ISO 14443-3a, NXP PN532 datasheet
- **Flipper GPIO app:** `applications/main/gpio/` (safe-pin model)
- **Bad USB:** `applications/main/bad_usb/` (DuckyScript parser, USB HID)
- **IR Database:** shirriff/Arduino-IRremote, universal remote code archives
- **BLE HID:** T-vK/ESP32-BLE-Keyboard (reference), ESP-IDF ble_hid component
- **Bus Tools:** standard i2ctools, picocom (UART terminal), standard SPI flash JEDEC specs

