# Firmware RAM Shrink — On-Demand Buffer Allocation

**Date:** 2026-07-15
**Board:** m5stack_cardputer_adv (ESP32-S3, NO PSRAM)
**Goal:** Lower always-resident internal-RAM footprint and improve stability by converting large per-app/per-scene static buffers to on-demand heap allocation. Free DMA-capable RAM so WiFi-heavy features (esp. Evil Portal client-connect) have headroom, and reduce baseline pressure that causes low-RAM instability (e.g. ViewPort lockups, allocation failures).

## Problem

The firmware is a single monolithic binary: every app/scene `.bss` buffer is resident at boot regardless of whether that app is open. A map audit of the *loaded* (post-gc-sections) `.bss`/`.data` shows ~109 KB of static data, ~45 KB of which is large per-scene buffers in WiFi/BLE tools that are idle almost all the time. On this no-PSRAM board, after the Evil Portal AP stack initialises, DMA-capable free RAM is ~20 bytes, so a joining station's ~1.6 KB DMA RX buffer can't be allocated and association is rejected. Reclaiming idle buffers returns DMA-capable RAM to the heap.

## Approach

On-demand allocation with a strict safety pattern (per owning app/scene):

- Replace `static Big s_buf[N];` with `static Big* s_buf;`.
- Allocate in `on_enter` / worker-start; **free in `on_exit` only after the worker task is stopped and any promiscuous/event callbacks are unregistered**; then set the pointer to NULL.
- Null-guard every use. On allocation failure, show an error and back out of the scene gracefully (no crash) — this doubles as a stability improvement.

Rejected alternatives: shared scratch arena (too much coupling; cross-scene corruption risk); converting only the top few buffers (fails the "broad" goal).

**Primary risk:** several buffers are touched by worker tasks or RX/event callbacks (wlan/ble scenes). Use-after-free if freed before the worker stops. Every conversion diff is read in full; the free ordering (stop worker → unregister cb → free → null) is mandatory.

## Do NOT touch

Infrastructure statics: net80211 (`g_cnxMgr`, `s_wifi_nvs`), `fatfs_object`, tinyusb `_cdcd_itf`, lwIP `dns_table`, `furi_hal_infrared ir_rx`. These are framework-owned and/or always needed.

## Batches (each: convert → build → measure free heap/DMA → smoke-test touched apps on device → commit)

**Batch 1 — display/scene-local (GUI-thread only, lowest risk):**
`loot_entries s_entry_path` (4.6 KB), `mesh_clients s_state` (4.7 KB), `port_scanner s_open_ports` (2.5 KB).

**Batch 2 — worker/callback-backed WiFi scenes (stop worker/cb before free):**
`probe_sniffer s_table` + `s_ring` (5.9 KB), `netscan s_hosts` (2.3 KB), `html_inject s_flow_cache` (2.4 KB), `netcut s_fwd_buf` (1.6 KB), `mitm_server rendered` (1.3 KB), `attack_detector s_rx` (1.5 KB), `station_scanner s_map` (1.4 KB).

**Batch 3 — BLE + defender scenes:**
`ble_walk s_devices` (3.6 KB), `defender_wifi_tap s_bring` (3.1 KB), `ble_tracker s_devices` (1.9 KB), `auto_walk s_seen` (1.5 KB).

**Batch 4 — targeted shrink (not remove):**
`evil_portal s_dns_cache` (2.8 KB) — used *during* the portal; reduce its entry count rather than make it on-demand. Re-measure Evil Portal steady-state DMA and re-test phone association as the acceptance check for the whole effort.

## Verification

- Per batch: a free-heap/DMA readout (reuse the `heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA)` probe) logged at boot idle, compared before/after.
- Per touched app: open it on-device, exercise its main flow, exit — confirm no crash and RAM is returned on exit.
- Final acceptance: Evil Portal steady-state `free_dma` high enough (>~2 KB) that a phone associates and loads the portal page.

## Success criteria

- Baseline idle free internal RAM increases by a meaningful margin (target ≥ ~30 KB reclaimed-when-idle across batches).
- No regressions in the touched apps.
- Evil Portal accepts a client (stretch goal; gated by whether reclaimed RAM lands in DMA-capable regions — verified empirically in Batch 4).
