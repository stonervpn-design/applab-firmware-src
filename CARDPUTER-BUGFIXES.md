# Cardputer-ADV port — defect + quality sweep

A focused audit of the **port-specific** code (freshly-authored drivers + ESP32
apps written for this board), not the mature upstream Flipper internals. Findings
came from a 4-way parallel code review; each was verified against the source
before the fix was applied. **None are build-verified** — compile on your ESP-IDF
toolchain (`build_ci.bat` / `build_adv.bat`) and smoke-test on hardware.

13 fixes across 12 files. Surgical; each traces to a specific finding below.

## Critical (memory safety / crashes)

1. **wardriver — OOB stack read persisted to the SD card**
   `applications_user/wardriver/wardriver.c` (`wd_log_ap`)
   `snprintf` returns the length it *would* have written; that value was passed
   straight to `storage_file_write`. A truncated row (>220 B — reachable via a
   garbled GPS coordinate, see #2) writes bytes past the stack buffer into the
   CSV. Now clamped to the real buffer length.

2. **GNSS — undefined `(int)` cast of an unbounded coordinate**
   `components/gnss/nmea.c` (`nmea_coord`)
   `nmea_checksum_ok()` accepts checksum-less sentences, so a UART glitch can feed
   `atof()` a huge value; casting it to `int` is UB and produces a giant lat/lon
   that overflows the CSV formatter (#1). Now rejects non-finite / out-of-range
   (`|value| > 18000.0`) fields. Added `<math.h>`.

3. **wlan_app — RX-callback vs `free()` use-after-free (3 scenes)**
   `scene_probe_sniffer.c`, `scene_attack_detector.c`, `scene_station_scanner.c`
   The promiscuous callback runs on the WiFi RX task, guarded only by a plain
   `if(!s_ring) return;`. `on_exit` called `wlan_hal_set_promiscuous(false, NULL)`
   — which syncs the HAL worker, **not** the WiFi RX task — then immediately
   `free()`d the ring, so a callback already past the NULL check writes into freed
   heap. Fixed with the same `portMUX_TYPE` pattern already shipped in
   `scene_pmkid.c`/`scene_handshake.c`: ring writes re-checked under the critical
   section, and a capture-NULL-free in `on_exit`.

4. **reverse_shell — worker freed while blocked in `recv()`**
   `applications_user/reverse_shell/reverse_shell.c`
   `rs_recv_line` blocked in `recv()` with no timeout; the serve loop only checks
   `running` between reads. On Back with a connected-but-idle listener the worker
   parked forever, teardown freed `app`/`mutex` after a fixed 5 s cap, and the
   worker later dereferenced freed memory. Fixed three ways: `SO_RCVTIMEO` (500 ms)
   on the session socket, `recv` timeout treated as "re-check `running` and keep
   waiting" (not a session drop), and an **unconditional** join before free.

## High

5. **lora — worker teardown UAF**
   `applications_user/lora/lora.c`
   Exit waited at most 1 s, but the worker can block in `sx1262_tx(…, 2000)` for
   ~2 s; free ran anyway. Now waits unconditionally for `worker_alive` to clear.

6. **rfid2_reader — worker teardown UAF**
   `applications_user/rfid2_reader/rfid2_reader.c`
   Same shape: 1 s cap vs a worker that can be mid-I2C in `rfid2_read_uid()`
   longer than that. Now an unconditional join.

## Medium

7. **wardriver — worker teardown UAF**
   `applications_user/wardriver/wardriver.c`
   `esp_wifi_scan_start(…, true)` is synchronous with no cap that reliably bounds
   it; the 2.5 s exit wait could free `app`/`mutex`/file mid-scan. Unconditional
   join now.

8. **Unchecked `malloc` on app entry (4 apps)**
   `lora.c`, `meshtastic.c`, `wardriver.c`, `reverse_shell.c`
   `memset(app, 0, …)` ran before any NULL check — a crash on allocation failure
   (plausible on the no-PSRAM board under WiFi load). Each now bails out cleanly.

9. **st25r3916 NFC — swallowed I2C driver-install error**
   `components/furi_hal/st25r3916/st25r3916_i2c.c` (`st25r3916_i2c_init`)
   Any non-OK `i2c_driver_install` result was logged as "already initialized" and
   returned `ESP_OK`, masking real failures (`ESP_ERR_NO_MEM`, bad pins). Now only
   `ESP_ERR_INVALID_STATE` is treated as "already installed"; genuine errors are
   logged and returned.

## Low

10. **NFC Type-4 emu — one-byte over-read on a minimal APDU**
    `components/furi_hal/furi_hal_nfc.c` (PN532 `pn532_type4_ndef_emulate`)
    `uint8_t lc = a[4];` read one byte past a 5-byte header-only APDU (stale stack,
    in-array). Now `(alen >= 5) ? a[4] : 0`, matching the ST25R3916 path.

## Behavior fix

11. **radio_scanner — signal lock did nothing in release builds**
    `applications_user/radio_scanner/radio_scanner_app.c`
    (`radio_scanner_process_scanning`)
    The `return;` that stops frequency-hopping once a signal is found sat **inside**
    `#ifdef FURI_DEBUG`, so non-debug builds kept hopping while the UI showed
    "Locked." Moved the `return` out of the guard.

## Reviewed, not changed (documented)

- **radio_scanner shared fields unsynchronized** (`frequency`/`rssi`/`sensitivity`/
  `scanning` read by the GUI draw callback, written by the main loop). Real per
  C11, but every field is single-word and naturally aligned, so loads/stores are
  atomic on the ESP32-S3 — worst case is a one-frame stale value, no memory
  unsafety. Not worth an unverifiable ViewModel refactor.
- **furi_hal board glue** (`furi_hal_nfc.c` tag-length clamps, `furi_hal_sd.c`
  SPI3 lock pairing, USB string-descriptor lifetime) — already correctly hardened
  with explicit guards; no critical/high issues found.
- **esp_now / defender / nrf24 / ble_spam / lan_attack**, and the bus/CAN/GPIO/
  wiegand/power apps — already hardened (snapshot-once callbacks, join-before-
  mutate, bounds-checked parsers); no additional confident defects.

## Files touched

```
applications_user/wardriver/wardriver.c
applications_user/lora/lora.c
applications_user/meshtastic/meshtastic.c
applications_user/reverse_shell/reverse_shell.c
applications_user/rfid2_reader/rfid2_reader.c
applications_user/radio_scanner/radio_scanner_app.c
applications/main/wlan_app/scenes/scene_probe_sniffer.c
applications/main/wlan_app/scenes/scene_attack_detector.c
applications/main/wlan_app/scenes/scene_station_scanner.c
components/gnss/nmea.c
components/furi_hal/st25r3916/st25r3916_i2c.c
components/furi_hal/furi_hal_nfc.c
```

## Smoke-test checklist (after a clean build + flash)

- **WiFi App → Probe Sniffer / Attack Detector / Station Scanner**: enter, let
  traffic flow, press Back — repeat several times fast. No crash = UAF fix holds.
- **Reverse Shell**: connect to an idle `nc -lvnp 4444`, then press Back with no
  keystrokes sent — should exit within ~1 s (previously could hang/crash).
- **RFID2 Reader** and **LoRa Monitor**: open/close repeatedly, including with no
  module attached.
- **Wardriver**: drive/walk a loop, then confirm the CSV imports cleanly into
  Wigle with no malformed rows.
- **Radio Scanner**: on a real signal it should now show "Locked" *and* stop
  hopping (release build).
- **NFC**: read a card / EMV to confirm the I2C-init and Type-4 changes didn't
  regress detection.
