# Cardputer-ADV — full-firmware audit (pass 2)

Extends the first sweep (`CARDPUTER-BUGFIXES.md`, which covered the freshly-authored
drivers + ESP32 wireless/hardware apps) to the rest of the port: the NFC / SubGHz /
IR / RFID app layers, all remaining `applications_user` apps (protopirate, subhound,
pocketlab, nRF24_jammer, FindMyFlipper, mifare_fuzzer, flippergotchi, …), and the
components/services/startup glue. Five parallel reviews; each finding verified
against the source before fixing. Verbatim-upstream Flipper internals were audited
for port modifications but not rewritten. **Not build-verified beyond compiling** —
smoke-test on hardware.

**21 fixes applied across 13 files.** All three originally-deferred HIGH concurrency
findings (`ble_serial` UAF #12, `mesh_service` start-path UAF #13, protopirate
dangling preset #14) were subsequently fixed. The only remaining documented item is
a MEDIUM cosmetic-only race a fix would risk more than it helps (see last section).

## Critical

1. **Mesh config — stack buffer overflow from SD file**
   `applications/services/desktop/helpers/mesh_config.c` (`mesh_config_load_master`)
   Passed a single `MeshPeer tmp;` to `mesh_load_peer_file()`, which writes up to
   `MESH_CLIENTS_MAX` (16) entries. A `master.txt` with >1 line smashes the caller's
   stack (incl. return address). Now uses `MeshPeer tmp[MESH_CLIENTS_MAX]` and takes
   `tmp[0]`.

2. **EMV — card-controlled `memcpy` past fixed fields**
   `components/nfc/protocols/emv/emv_poller_i.c`
   The port's own EMV hardening guarded most TLV copies but missed `PRIORITY`,
   `LOG_AMOUNT`, `LOG_DATE`, `LOG_TIME` — a crafted/relayed card sets an oversized
   length byte and overwrites adjacent `EmvApplication` fields (forged PAN/expiry,
   corrupted `pan_len`). Added the same `if(tlen > sizeof(field)) break;` guard to all
   four.

3. **flippergotchi — multi-writer corruption of the network table**
   `applications_user/flippergotchi/fg_net_table.c`
   `fg_net_observe`/`set_ssid`/`add_flag` are called from BOTH the WiFi promiscuous
   callback (beacons) and the worker task (EAPOL). Two concurrent inserts interleaved
   struct writes and lost/corrupted entries (misdirecting attacks, corrupting PMKID
   state). Serialized the table mutations with a short `portMUX` spinlock; read-only
   scans stay lock-free (torn single-field reads are cosmetic only).

## High

4. **EMV — oversized transaction-log count → panic (DoS)**
   `emv_poller_i.c` (`emv_poller_read_log_entry`)
   `log_records` (card byte, up to 255) drove the loop past `trans[16]`; the trailing
   `furi_check` panicked on the 17th. One-tap reboot from a hostile card. Now clamps
   `record_end` to the array size and breaks gracefully instead of `furi_check`.

5. **subghz playlist — worker thread + heap leaked every completed run**
   `applications/main/subghz/views/subghz_playlist.c`
   `subghz_playlist_stop()` freed the `FuriThread` only when `is_running` was still
   true, but the worker clears that flag itself on natural completion → the thread
   (stack + buffers, ~2.3 KB) leaked every run. Now guards on the thread pointer, and
   the alloc site frees any leftover before re-allocating.

6. **FindMyFlipper — NULL deref on malformed key import**
   `applications_user/FindMyFlipper/.../findmy_scene_config_import.c`
   `decoded_len` was read uninitialized when `base64_decode` returned NULL; a garbage
   28 fell through to `memcpy` from NULL. Now `decoded_len = 0` and the pointer is
   checked first.

12. **ble_serial — host-callback use-after-free** *(fixed after initial deferral)*
    `components/ble_serial/ble_serial.c`
    The NimBLE host-task GATT/GAP callbacks (`serial_rx_access`/`serial_flow_access`/
    `serial_rpc_access`/`ble_gap_event`) captured the global `serial_state.active`
    with no pin; `ble_serial_free()` nulls it, tears down the host (async), then
    `vSemaphoreDelete`+`free` — an in-flight GATT write could dereference freed
    memory. Ported `ble_hid.c`'s `refs` acquire/release pin-and-drain: callbacks now
    pin the instance under the global lock, the gap handler is wrapped so no return
    path can leak a ref, and `ble_serial_free()` drains `refs` to 0 before freeing.

13. **mesh_service `start()` — sync objects freed under a live worker** *(fixed after
    initial deferral)*
    `applications/services/desktop/helpers/mesh_service.c`
    On `ready_sem` timeout (or the worker-init-failed check), the fail path deleted
    `cmd_q`/`ready_sem`/`done_sem` while the worker task was still alive and still
    references those *global* handles — it would later give a freed (or, after the
    next `start()`, a reused) handle → UAF / cross-session corruption. Split the fail
    path: "xTaskCreate failed" frees directly (no worker); any created-worker failure
    now sends `MeshCmdStop` and waits on `done_sem` (bounded — Stop breaks the command
    loop and init ops terminate) so the worker is confirmed exited before freeing.

14. **protopirate — dangling preset pointer on file switch** *(fixed after initial
    deferral; was latent)*
    `applications_user/protopirate/scenes/protopirate_scene_sub_decode.c`
    `reset_for_file()` freed `ctx->custom_preset_data` while `app->txrx->preset->data`
    still aliased it, leaving a shared struct pointing at freed heap (one new caller
    away from a real UAF). Threaded `app` into `reset_for_file()` and restore the
    previous preset before the free — the exact safe ordering already used in
    `..._impl_on_exit`.

## Medium

7. **nfc_magic scanner — start-before-state race**
   `applications_user/nfc_magic/magic/nfc_magic_scanner.c`
   The worker asserts `session_state == Active` as its first line, but the state was
   set *after* `furi_thread_start()`. Now set before starting the thread.

8. **NFC supported-cards — uninitialized `app` freed**
   `applications/main/nfc/helpers/nfc_supported_cards.c`
   `malloc`'d context never set `->app = NULL`, then `if(instance->app) …free()` ran
   on garbage (PSRAM builds). Now zeroed at alloc.

9. **mifare_fuzzer — two heap leaks per attack cycle**
   `.../scenes/mifare_fuzzer_scene_emulator.c` (`Iso14443_3aData` never freed) and
   `.../views/mifare_fuzzer_emulator.c` (a `FuriString` whose C-string was stashed and
   the object leaked). Both re-fire every tick during a fuzz run → heap exhaustion.
   Now free the temp iso14443_3a, and copy the card name into a persistent buffer.

10. **nRF24_jammer / bad_usb / qflipper_bridge — unchecked allocations**
    `nRF24_jammer.c` (`memset` after unchecked `malloc`), `bad_usb/helpers/bad_usb_hid.c`
    (`calloc`), `desktop/helpers/qflipper_bridge.c` (`malloc`) — each dereferenced an
    allocation before checking it; a crash under OOM on the no-PSRAM board. Guards
    added.

11. **mesh_service `stop()` — free-under-live-worker (partial)**
    `applications/services/desktop/helpers/mesh_service.c`
    Fixed the stop path: the queue/semaphores are now freed only after the worker
    actually signals exit (`done_sem` taken); on timeout they're leaked instead of
    freed under a still-running worker (which would give a freed handle → UAF). The
    matching `start()` timeout path still needs the same treatment — see below.

## Reviewed clean (no confident defects)

subhound, weather_station, flipper_xremote, pocketlab, tamagotchi, the arcade games;
the deep NFC/SubGHz/IR/LFRFID protocol stacks; `ble_hid.c` (a good reference — its
refcount-drain UAF guard is what `ble_serial.c` is missing); mp3 decoder; most of
protopirate (unusually disciplined for a 34k-LOC port).

## Flagged — NOT auto-patched (need a validated refactor)

One item remains documented — a fix would risk more than it helps:

- **`radio_scanner_app.c` — unsynchronized GUI/loop field reads (MEDIUM, from pass 1).**
  Real per C11 but all fields are single-word/aligned = atomic on ESP32-S3; cosmetic
  only. Not worth an unverifiable ViewModel refactor.
