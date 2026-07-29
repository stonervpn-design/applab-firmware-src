# Firmware RAM Shrink — On-Demand Buffers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reclaim ~45 KB of always-resident internal RAM by converting large per-scene static buffers in the WiFi/BLE tools to on-demand heap allocation, freeing DMA-capable RAM and lowering baseline pressure.

**Architecture:** Each target `static Big s_buf[N];` becomes `static Big* s_buf;`, allocated in the scene/app `on_enter` (or worker-start), freed in `on_exit` after the worker/callbacks stop, null-guarded on use, with graceful back-out on allocation failure. Monolithic firmware → removing a buffer from `.bss` returns that RAM to the heap permanently; the scene only occupies it while open.

**Tech Stack:** ESP-IDF v5.4.1, C, FreeRTOS, Flipper `SceneManager`/`View`. Build: `build_fix.bat` (incremental). Measure: `scratchpad/audit2.py` against `build_cardputer_adv/furi_esp32.map`. Flash: `flash_rs.bat write COM12`.

## Global Constraints

- Board `m5stack_cardputer_adv`, ESP32-S3, NO PSRAM. Verify `grep -c "CONFIG_SPIRAM is not set" sdkconfig` == 1 before every flash.
- Invoke cmd.exe batches from Bash with `export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'` and absolute paths.
- Free ordering is mandatory where a worker/callback touches the buffer: **stop worker → unregister promiscuous/event cb → free → NULL**. Never free while a worker can run.
- On alloc failure: set a visible status/popup and return to the previous scene; do not proceed with a NULL buffer.
- Do NOT touch infra statics: net80211 (`g_cnxMgr`, `s_wifi_nvs`), `fatfs_object`, tinyusb `_cdcd_itf`, lwIP `dns_table`, `furi_hal_infrared ir_rx`.
- Device is on COM12; a running WiFi/BLE feature locks the USB port — stop it before flashing.

## Conversion Pattern (apply per buffer)

Reference transform. For a buffer `static T s_buf[N];` used across a scene:

1. Change declaration to `static T* s_buf;` (pointer, initialised NULL by BSS).
2. In `on_enter` (before first use / before starting any worker):
   ```c
   s_buf = malloc(sizeof(T) * N);   // or calloc if code assumes zeroed
   if(!s_buf) {
       // graceful failure — show error, leave scene
       // (use the app's existing status/popup mechanism)
       scene_manager_previous_scene(app->scene_manager);
       return;
   }
   ```
3. Replace every `s_buf[i]` / `&s_buf[i]` use unchanged (array indexing works on the pointer). Confirm no `sizeof(s_buf)` remains (would now be pointer size) — replace with `sizeof(T) * N`.
4. In `on_exit`: stop the worker and unregister callbacks FIRST (existing teardown), then:
   ```c
   free(s_buf);
   s_buf = NULL;
   ```
5. Null-guard any access reachable outside on_enter..on_exit (tick handlers, callbacks): `if(!s_buf) return;`.

Per-task verification cycle (the "test"):
- Build clean (`build_fix.bat` → `SENTINEL_OK`, no `error:`).
- Run `python scratchpad/audit2.py` — confirm the buffer's object file dropped by ~the expected size (buffer moved out of `.bss`).
- Flash, open the touched app on device, exercise its main flow, exit — no crash, and (spot check) free RAM returns on exit.

---

### Task 1: Baseline measurement + shared free-RAM boot probe

**Files:**
- Modify: `applications/main/wlan_app/wlan_hal.c` — no; instead add a one-shot boot log in the main app-init path. Use existing `Main:` logging in `components/furi`/startup.
- Measure: `build_cardputer_adv/furi_esp32.map` via `scratchpad/audit2.py`.

- [ ] **Step 1:** Record current baseline: `python scratchpad/audit2.py > scratchpad/baseline_bss.txt`; note total loaded `.bss` KB.
- [ ] **Step 2:** Add a boot-time free-RAM log (INTERNAL + DMA) at the end of core init so idle baseline is observable on serial. Place near the existing `Main: Started` log.
  ```c
  ESP_LOGW("Main", "boot free: int=%u dma=%u",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
  ```
- [ ] **Step 3:** Build, flash, capture boot serial, record `int=` / `dma=` idle baseline in the plan notes.
- [ ] **Step 4:** Commit the probe (`git add -A && git commit -m "chore(ram): boot free-RAM probe + baseline"`).

### Task 2 (Batch 1): loot_entries `s_entry_path`

**Files:** Modify `applications/main/wlan_app/scenes/scene_loot_entries.c` (buffer `s_entry_path`, ~4.6 KB).

- [ ] **Step 1:** Read the file; map every use of `s_entry_path` and confirm it is GUI-thread-only (no worker/callback).
- [ ] **Step 2:** Apply the Conversion Pattern (declaration → pointer; alloc in on_enter with graceful fail; free+NULL in on_exit; fix any `sizeof`).
- [ ] **Step 3:** Build → `SENTINEL_OK`; `python scratchpad/audit2.py` shows `scene_loot_entries.c.obj` dropped ~4.6 KB.
- [ ] **Step 4:** Flash; open the Loot list on device, scroll, exit — no crash.
- [ ] **Step 5:** Commit (`feat(ram): loot_entries buffer on-demand`).

### Task 3 (Batch 1): mesh_clients `s_state`

**Files:** Modify `applications/main/desktop/scenes/desktop_scene_mesh_clients.c` (buffer `s_state`, ~4.7 KB).

- [ ] **Step 1:** Read the file; confirm access threads (mesh clients may use a worker — if so, follow the stop-worker-before-free ordering).
- [ ] **Step 2:** Apply the Conversion Pattern.
- [ ] **Step 3:** Build; audit2.py shows `desktop_scene_mesh_clients.c.obj` dropped ~4.7 KB.
- [ ] **Step 4:** Flash; open the mesh clients view, exit — no crash.
- [ ] **Step 5:** Commit (`feat(ram): mesh_clients state on-demand`).

### Task 4 (Batch 1): port_scanner `s_open_ports`

**Files:** Modify `applications/main/wlan_app/scenes/scene_port_scanner.c` (buffer `s_open_ports`, ~2.5 KB).

- [ ] **Step 1:** Read the file; port scanner uses a worker — note it; free after worker stop.
- [ ] **Step 2:** Apply the Conversion Pattern with worker-stop-before-free.
- [ ] **Step 3:** Build; audit2.py shows drop ~2.5 KB.
- [ ] **Step 4:** Flash; run a port scan, exit mid-scan and after — no crash/UAF.
- [ ] **Step 5:** Commit. Then re-measure boot free RAM; record Batch 1 delta.

### Task 5 (Batch 2): probe_sniffer `s_table` + `s_ring`

**Files:** Modify `applications/main/wlan_app/scenes/scene_probe_sniffer.c` (`s_table` ~4.6 KB, `s_ring` ~1.2 KB). **Has a promiscuous RX callback.**

- [ ] **Step 1:** Read the file; identify the promiscuous cb and worker. The cb writes into `s_table`/`s_ring`.
- [ ] **Step 2:** Apply the pattern to BOTH buffers. In on_exit: `esp_wifi_set_promiscuous(false)` / unregister cb FIRST, join/stop the worker, THEN free both + NULL. Null-guard the cb (`if(!s_table) return;`).
- [ ] **Step 3:** Build; audit2.py shows `scene_probe_sniffer.c.obj` dropped ~5.9 KB.
- [ ] **Step 4:** Flash; run probe sniffer near active WiFi, let it capture, exit while capturing — no crash/UAF.
- [ ] **Step 5:** Commit.

### Task 6 (Batch 2): netscan `s_hosts`, html_inject `s_flow_cache`, netcut `s_fwd_buf`

**Files:** `scenes/`+hal for `wlan_netscan.c` (`s_hosts` ~2.3 KB), `wlan_html_inject.c` (`s_flow_cache` ~2.4 KB), `wlan_netcut.c` (`s_fwd_buf` ~1.6 KB). Each worker/callback-backed.

- [ ] **Step 1:** For each file: read, identify worker/cb lifecycle.
- [ ] **Step 2:** Apply the pattern to each, stop-worker/cb-before-free. (One commit per file is fine; grouped here as they share the pattern.)
- [ ] **Step 3:** Build after each; audit2.py confirms each object dropped.
- [ ] **Step 4:** Flash after the group; smoke-test netscan, MITM/html-inject, netcut — enter, run, exit — no crash.
- [ ] **Step 5:** Commit per file.

### Task 7 (Batch 2): mitm_server `rendered`, attack_detector `s_rx`, station_scanner `s_map`

**Files:** `wlan_mitm_server.c` (`rendered` ~1.3 KB), `scene_attack_detector.c` (`s_rx` ~1.5 KB, RX cb), `scene_station_scanner.c` (`s_map` ~1.4 KB, worker).

- [ ] **Step 1:** Read each; note attack_detector has an RX/promiscuous cb, station_scanner a worker.
- [ ] **Step 2:** Apply pattern; cb/worker teardown before free; null-guard cbs.
- [ ] **Step 3:** Build; audit2.py confirms drops.
- [ ] **Step 4:** Flash; smoke-test attack detector + station scanner + MITM — no crash. Record Batch 2 delta.
- [ ] **Step 5:** Commit per file.

### Task 8 (Batch 3): ble_walk `s_devices`, ble_tracker `s_devices`, defender_wifi_tap `s_bring`, auto_walk `s_seen`

**Files:** `ble_walk_hal.c` (~3.6 KB), `ble_tracker_hal.c` (~1.9 KB), `defender_wifi_tap.c` (~3.1 KB), `scene_auto_walk.c` (~1.5 KB). BLE scan/tap callbacks.

- [ ] **Step 1:** Read each; BLE HAL buffers are written by scan/GAP callbacks — unregister the scan cb / stop scanning before free.
- [ ] **Step 2:** Apply pattern; cb teardown before free; null-guard cbs.
- [ ] **Step 3:** Build after each; audit2.py confirms drops.
- [ ] **Step 4:** Flash; smoke-test BLE walk, BLE tracker, defender wifi-tap, auto-walk — no crash. Record Batch 3 delta.
- [ ] **Step 5:** Commit per file.

### Task 9 (Batch 4): evil_portal `s_dns_cache` shrink + acceptance test

**Files:** `applications/main/wlan_app/wlan_evil_portal.c` (`s_dns_cache` ~2.8 KB — used DURING the portal, so shrink not remove).

- [ ] **Step 1:** Read the DNS cache usage; reduce its entry count to the smallest that keeps captive-portal detection working (halve first).
- [ ] **Step 2:** Build; audit2.py confirms `wlan_evil_portal.c.obj` dropped.
- [ ] **Step 3:** Flash; start Evil Portal; capture steady-state `free_dma` (re-add the temporary DNS-idle probe from the design session or a boot probe). Compare to the prior ~20 bytes.
- [ ] **Step 4:** Connect a phone. Acceptance: association succeeds (`AP_STACONNECTED` fires) and the portal page loads. If `free_dma` improved but is still < ~1.6 KB, record the number — that quantifies remaining gap.
- [ ] **Step 5:** Remove the temporary probe; commit.

### Task 10: Final measurement + cleanup

- [ ] **Step 1:** Run `python scratchpad/audit2.py`; record total loaded `.bss` vs baseline; confirm ≥ ~30 KB reduction.
- [ ] **Step 2:** Remove the Task 1 boot free-RAM probe (or keep behind a debug flag).
- [ ] **Step 3:** Full clean build; flash; general smoke test (spam, deauth, a few converted apps).
- [ ] **Step 4:** Commit; update the design spec's success-criteria section with actual numbers.

## Self-Review

- **Spec coverage:** Batches 1-4 from the spec map to Tasks 2-9; verification/measurement to Tasks 1 & 10; success criteria to Task 10. Covered.
- **Placeholder scan:** Per-task code is the shared Conversion Pattern applied to a named buffer; exact code is finalised at implementation after reading each file (unavoidable — each buffer's worker/cb lifecycle differs). The pattern itself is complete, not a placeholder.
- **Consistency:** Free ordering (stop worker → unregister cb → free → NULL) and null-guard rule are stated once in Global Constraints and referenced by every worker/cb task.
