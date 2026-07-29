# Bug-fix pass (port-specific code)

A parallel audit of the ESP32-S3 port-specific subsystems (not the mature upstream
Flipper internals). 15 confirmed defects fixed across 13 files. **None are
build-verified** — compile on your ESP-IDF toolchain (`build_adv.bat`) and smoke-test
on hardware. Fixes are surgical and each traces to a specific finding below.

## Critical (memory safety / crashes)

1. **ESP-NOW app — use-after-free of GUI ViewPort**
   `applications/main/esp_now/esp_now_app.c`
   `esp_now_app_free()` used raw `free()` on GUI objects instead of the real
   destructors. The missing `view_dispatcher_free()` left a dangling `ViewPort`
   registered in the system Gui service — a deterministic UAF affecting the whole
   firmware's GUI thread after one exit from the app. Now calls
   `submenu_free`/`widget_free`/`scene_manager_free`/`view_dispatcher_free`.

2. **ESP-NOW sniffer — TOCTOU NULL-deref on `g_app`**
   `applications/main/esp_now/scenes/scene_sniff.c`
   The WiFi-task receive callback re-read the global `g_app` after its NULL guard;
   the GUI task could null it in between. Now snapshots the pointer once.

3. **Buddy capture — double-free / task race on stop→start**
   `buddy_firmware/main/features/feat_capture_hs.c`
   `feat_stop()` returns immediately while `capture_task` frees `s_pool` up to
   400 ms later; `feat_start()` only checked `s_run`, so a quick restart spawned a
   second task that double-freed the pool. Added an `s_task_alive` guard cleared
   only after the pool is freed. (Also: capture now reads the master MAC fresh each
   tick instead of a stale cached copy, so delivery survives a re-pair.)

4. **NFC — stack over-read from tag-controlled FeliCa length**
   `components/furi_hal/furi_hal_nfc.c`
   `felica_resp_len` (from `resp[2]`, attacker-controlled) was bounded only against
   the 256-byte destination, not the 64-byte source `resp[]`. A malicious FeliCa
   target could read ~190 bytes of stack into card data. Now also bounded by
   `resp_len - 2`.

5. **BadUSB / USB HID — dangling stack pointer as USB string descriptor**
   `components/furi_hal/furi_hal_usb_hid_tinyusb.c`,
   `components/furi_hal/furi_hal_usb_tinyusb_composite.c`
   The LANGID descriptor was a block-scope compound literal whose storage died when
   the install function returned, but TinyUSB reads it lazily on every enumeration.
   Made it `static const`.

6. **NRF24 WiFi scan — stack-UAF + semaphore double-free on timeout**
   `applications/main/nrf24/nrf24_wifi_scan.c`
   The request struct + semaphore were stack-allocated and destroyed after an 8 s
   timeout while the orphaned scan task could still write through them. Rewritten
   with a reference-counted heap block and an `abandoned` flag so the last side
   frees; the task never touches caller memory after a timeout.

7. **SD — mount-failure diagnostics ran without the shared SPI3 lock**
   `components/furi_hal/furi_hal_sd.c`
   SD and CC1101 share SPI3; the mount-failure diagnostics + host teardown issued
   SPI transactions without `furi_hal_spi_bus_lock()`, unlike every other SD path.
   Wrapped them. (See "Needs review" for two other `sd_release_host()` call sites.)

8. **Cardputer ADV keyboard — debounce swallowed key-release events**
   `targets/m5stack_cardputer_adv/target_input.c`
   One debounce timestamp per key was applied to press *and* release; a fast tap
   dropped the release, sticking Shift/Ctrl/Fn/Alt/Opt on or auto-repeating nav
   keys forever. Releases now always pass (TCA8418 already debounces in hardware).

## High / medium

9. **BadUSB — OOB heap read on `MOUSEMOVE`/`MOUSESCROLL` with missing args**
   `applications/main/bad_usb/helpers/ducky_script_commands.c`
   Raw `strcspn(line," ")` indexed one past the NUL when no argument was present.
   Switched to `ducky_get_command_len()` (returns 0 safely), matching every other
   command handler.

10. **Sub-GHz TPMS edit — corrupted non-Schrader captures**
    `applications/main/subghz/scenes/subghz_scene_tpms_edit.c`
    The Schrader-only packer ran for every TPMS protocol, overwriting
    Citroën/Ford/Renault/PMV107J entries with the wrong bit-count/CRC and breaking
    later reloads. Now gated on the entry's actual protocol.

11. **Sub-GHz — Toyota PMV107J decoder could never match its preamble**
    `lib/subghz/protocols/tpms_pmv107j.c`
    Preamble constant `0xF8` compared against a 6-bit-masked accumulator (max
    `0x3F`) → always false → dead decoder. Corrected to `0x3E` per the code's own
    comment. (Verify with a real sensor.)

12. **Evil Portal — CSV injection / row corruption in captured creds**
    `applications/main/wlan_app/scenes/scene_evil_portal.c`
    Victim-controlled `user`/`pwd` were written to `*_creds.csv` unescaped (a comma
    in a password, or smuggled `%0A`/`%0D`, corrupts rows / injects formulas). Added
    the same `,`/`\r`/`\n` escaping the sibling MiTM logger already uses.

13. **NFC dict attack — Stream + RECORD_STORAGE leak on open failure**
    `applications/main/nfc/scenes/nfc_scene_mf_classic_dict_attack.c`
    The per-CUID dictionary error path closed the file but leaked the `Stream`
    object and a storage reference (reachable on flaky SD). Now mirrors
    `keys_dict_free()`.

14. **Charger — REG04 read return value clobbered the register value**
    `components/furi_hal/furi_hal_bq25896.c`
    `uint8_t reg04 = bq25896_read_reg(REG04, &reg04);` overwrote the read byte with
    the bool success flag, so "preserve EN_PUMPX" always cleared the bit. Split the
    read from the value.

## Needs review / not auto-fixed (verify on hardware)

- **Standard Cardputer keyboard maps only 6 keys** (`targets/m5stack_cardputer/target_input.c`)
  — no text input is possible at all. Needs the 74HC138 matrix map (hardware/schematic)
  to add safely; not blind-patched.
- **Two other `sd_release_host()` call sites** (`furi_hal_sd.c` ~565, ~744) likely also
  need the SPI bus lock — trace their callers first.
- **Evil Portal internet-bridge flags** (`wlan_evil_portal_bridge.c`) are shared between
  the WiFi-event task and httpd task without `volatile`/sync — can wedge bridge state.
- **ESP-NOW sniffer** drops packets under lock contention (non-blocking mutex in the RX
  callback) — a design tradeoff; add a ring buffer or drop counter.
- **PN532 DCS checksum** not validated (`furi_hal_nfc.c`) — defense-in-depth.
- **`get_hash_data()` reads > 8 bytes** for >64-bit TPMS protocols — inherited upstream
  idiom, not a new regression.
- **Backspace/Esc share one nav state slot** on the standard Cardputer — minor
  long-press desync.
- **PMKID scene (and handshake scene) teardown: cross-core promiscuous-RX UAF window**
  (`applications/main/wlan_app/scenes/scene_pmkid.c`, `scene_handshake.c`) — the
  promiscuous RX callback runs on the WiFi RX task while `on_exit` runs on the UI
  task. Teardown disables promiscuous synchronously *then* frees the ring pool, with
  an `if(!pool) return;` guard in the callback, but a callback that already passed
  the guard can `memcpy` into freed heap in the sub-µs window before `free()`
  completes. Accepted residual risk (matches the established codebase pattern);
  hardening would need an in-callback "active" gate + post-disable settle/barrier
  applied to both scenes. Not a new regression — the PMKID scene inherits the
  existing handshake-scene idiom.
- **BLE tracker scan: unsynchronized cross-thread read of the device table**
  (`applications/main/ble_spam/scenes/scene_tracker_scan.c`,
  `ble_tracker_hal.c`) — the scan scene `memcpy`s `s_devices`/`s_device_count`
  from the scene tick (UI task) while the BLE GAP callback (BT host task) mutates
  them with no lock (`s_device_count` is plain, not `volatile`). Pre-existing
  pattern in the original tracker code; the Tracker/Bug-Sweep feature only widened
  `TrackerDevice` (added `first_seen_ms`) and now feeds the snapshot into
  follow-state/alert logic. Worst case is a one-frame inconsistent snapshot (no
  overrun — `count` is clamped to `TRACKER_MAX_DEVICES` before the copy). Accepted
  residual; a mutex or double-buffer would harden it.
