# Cardputer-ADV Flipper port — FULL firmware audit (2026-07-13)

Base: `nimble-port` work tree + adam's pin/NVS merge. Method: hardware NVS dump +
app-descriptor read for the root cause, then **13 independent opus code-review passes**
covering the whole firmware. Severity = user impact.

Legend: ✅ FIXED this session (in the build) · ⚠️ OPEN (documented, not changed).

---
## ROOT CAUSE of "GPIO edits don't save" — SOLVED
Device was running `5fe86ae-dirty` (Jul 12 08:11) = OLD SD-only pin_config, no NVS. A
backlog of fixes was built but never flashed (CDC kept wedging). Merge + clean flash fixes it.

---
## ✅ FIXED (built; latest build pending a replug to flash)

### Reported bug + merge
- ✅ Pin config persists to NVS + boot-apply; UI now loads from NVS (single source of truth)
- ✅ SPI/I2C pin rows made read-only (they had no hardware effect)
- ✅ SubGhz TX un-broken (`tx_marker` internal RAM; adam's zip had reverted it)

### From the sweep
- ✅ **NimBLE `ble_hid_free` use-after-free [HIGH]** — added a per-instance refcount
  (`refs`) taken under the global lock at every host-task `active` capture (GATT report
  read/write + GAP connect/disconnect); `ble_hid_free` drains it to 0 before freeing.
  No lock-order inversion (the instance→global order is preserved).
- ✅ **RAW `.sub` transmit crash** — encoder alloc (`raw.c:328`) left `file_worker_encoder`
  uninitialized → `furi_check`/wild deref on any RAW TX. Now NULL-init.
- ✅ **`subghz_last_settings` uninitialized `frequency_offset` [HIGH]** — malloc wasn't
  zeroed and load() defaulted only 8/13 fields, so `frequency_offset` (added to every
  tuned frequency) was garbage on first boot/no-SD → silently mistuned all RX/TX. memset added.
- ✅ **CAME Atomo TX heap overflow** — `upload[900]` but up to ~994 written; bumped to 1100.
- ✅ **NFC multi-protocol cards unreadable** — `nfc_detected_protocols_set` memcpy'd `count`
  bytes into a 4-byte-enum array; a 2-protocol card (e.g. MIFARE Classic) filled only one
  slot → picker couldn't read it. Now `count * sizeof(NfcProtocol)`.
- ✅ **MF Ultralight FAST_READ emulation stack smash** — `page_cnt` (up to 231 on NTAG216)
  wrote past `pages[64]` from reader-supplied bytes. Clamped to 64.
- ✅ **Infrared app leak** — loading a bad `.ir` file returned without `infrared_free` (4
  records + views + worker + thread leaked per attempt). Freed now.
- ✅ **text_input 1-byte stack overrun** — caret insert wrote one past a VLA on every text
  field draw. Buffer sized +2.
- ✅ Signal-Decoder intermittent-dead (`raw.c` decoder `pause` uninit), ProtoView ring data
  race (mutex), scroll clamp, real I2C-scanner probe, SPI-flash-reader hidden, save-NVS
  error logging, WiFi client-scanner release fence.

---
## ⚠️ OPEN — real bugs, NOT changed (need on-hardware verification / your call)

### High
- ⚠️ **NFC emulation/parse heap+stack overflows from crafted `.nfc` files or malicious
  cards** (`mf_desfire_i.c:469` access-rights, `:293` GetFileSettings; `felica.c:142` blocks,
  `:270` data; `nxp_native_command.c:104` length underflow). Several may exist upstream;
  each is a real overrun. Recommend a focused DESFire/FeliCa/ISO15693 hardening pass.
- ⚠️ **NimBLE (other)**: `ble_serial`/`btshim` HID→serial double host-init, `ble_serial_free`
  deinit race, `btshim.c:262` rx_thread UAF, "forget bonded devices" no-op in serial mode.

### Medium
- ⚠️ **Wiegand app is entirely dead** — submenu emits indices 0/1/2 but scenes expect
  custom events 100/102, and Capture/Craft/Replay views are never registered. Reads no
  cards. (Needs the app wired up.)
- ⚠️ **Mousejack freezes GUI+SD up to 60 s** — holds the shared SPI/LCD/SD lock across a
  DuckyScript `DELAY` (`scene_mj_attack.c` / `scene_mj_auto.c`). Scope the lock to frame TX.
- ⚠️ **MITM live-creds UI freeze** — `scene_live_creds.c:325` unbounded drain loop with
  blocking SD writes in the GUI tick → watchdog on chatty traffic.
- ⚠️ **NFC PN532 large reads fail** — `furi_hal_nfc.c:250` rejects 248–253-byte payloads
  (DESFire/EMV) and desyncs the bus.
- ⚠️ **USB HID vs composite double-install** — 2nd USB personality per boot silently fails
  until reboot (`furi_hal_usb_hid_tinyusb.c` / composite each install their own driver).
- ⚠️ **BadUSB IR pin from Pin Config → RMT `ESP_ERROR_CHECK`** could reboot on an invalid
  GPIO (the UI list is valid pins, so low reach; still unvalidated).

### Feature-dead / lower
- ⚠️ **Geiger tracker audio never plays** — speaker acquired on the scene thread, used on
  the timer thread (`is_mine()` false). BLE tracker `set_sweep` table memset race; race-
  detector abort dead + ~7 s UI freeze.
- ⚠️ LAN recon: teardown UAF of recon state in the lwIP thread (low hit-rate); LLMNR decoder
  can leave a non-terminated name (codec not wired to a scene yet).
- ⚠️ Subghz: keystore lines not NUL-terminated (over-read on corrupt mfcodes); several
  protocol allocs miss NULL checks (OOM); `tx_power` OOB read from a hand-edited settings file.
- ⚠️ BadUSB settings don't persist (empty save stub). Infrared RX-stop TOCTOU (latent).
- ⚠️ furi HAL: WS2812 LED race; speaker busy-spin on OOM; SPI lazy-mutex first-acquire race
  (all latent/low). RTC flags RAM-only (known port gotcha).
- ⚠️ Storage: `furi_hal_sd` check-outside-lock TOCTOU (latent, no card-detect pin); unchecked
  mallocs (OOM). Core storage SPI3 lock balance verified CLEAN.

---
## Verified CLEAN (no in-scope defects)
Core GUI/scene_manager/loader/view_dispatcher/submenu/canvas/notification; WiFi handshake/
PMKID parsers; ProtoPirate v3.2 plugin→static collapse + PSA crypto BF; CAN Commander;
storage SPI3 bus-lock balance; SubGhz async RX worker/teardown; nRF24 SPI arbitration + register
config; BadUSB DuckyScript parser + HID buffers; iButton (not compiled on this board).

---
## Not merged from adam (decide later)
`bw16_r4tkn`, `desktop_shortcuts` + its desktop/notification changes.
