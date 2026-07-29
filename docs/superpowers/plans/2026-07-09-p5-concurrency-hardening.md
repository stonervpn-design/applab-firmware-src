# P5 Concurrency Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the accepted concurrency residuals from the Red-Team Readiness audit item #5 — two capture-pool teardown use-after-free windows and three cross-task shared-state races — so a scene exit or a busy scan can no longer corrupt memory or hang the firmware mid-op.

**Architecture:** Five independent, per-file fixes. The two promiscuous-capture pools (PMKID, handshake) get a `portMUX` critical section that makes "free the pool" mutually exclusive with "the RX-task callback writes into the pool", plus the free is done under the lock after NULLing the pointer. The two BLE HAL device tables get an acquire/release publish barrier on the element count so a reader never iterates past a half-written entry. The Evil Portal bridge gate flags get a single `portMUX` guarding every check-then-set and clear so the STA state machine can't be driven into an inconsistent state by the concurrent WiFi-event task.

**Tech Stack:** ESP-IDF v5.4.1 (ESP32-S3), C. Firmware build only — concurrency fixes are not host-unit-testable; each task's verification is a clean build plus the stated reasoning that the race window is closed.

## Global Constraints

- **Build** via `build_ci.bat` from Git Bash, SYNCHRONOUS (never backgrounded), timeout 600000 ms:
  `MSYS_NO_PATHCONV=1 cmd.exe /c "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_ci.bat" > build_<tag>.log 2>&1; echo "EXIT=$?"`
  Success = tail shows `Project build complete` + `=== BUILD OK ===`, no `FAILED ===`. `build_cardputer_adv/` is validly configured → incremental; do NOT delete it. `build_*.log` is gitignored.
- **Match existing idioms.** The codebase already uses both mechanisms this plan needs:
  - `portMUX_TYPE s_x_mux = portMUX_INITIALIZER_UNLOCKED;` + `portENTER_CRITICAL(&s_x_mux)/portEXIT_CRITICAL(&s_x_mux)` — see `applications/main/wlan_app/wlan_evil_portal.c:880`.
  - `__atomic_store_n(&x, v, __ATOMIC_RELEASE)` / `__atomic_load_n(&x, __ATOMIC_ACQUIRE)` publish pattern — see `applications/main/wlan_app/wlan_cred_sniff.c:305-323,898-908`.
- **Critical-section rule:** code inside `portENTER_CRITICAL`/`portEXIT_CRITICAL` MUST NOT block, yield, log, or call anything that might (no `wlan_hal_run_in_worker`, no `ESP_LOGx`, no `free` of unbounded work, no queue sends). Keep the locked region to flag reads/writes and the bounded pool `memcpy` only. Capture pointers under the lock and `free()` after `portEXIT_CRITICAL`.
- **Bounded memcpy is OK under the lock.** The pool `memcpy` is ≤ `HS_PKT_MAX_LEN` / `PMK_PKT_MAX` bytes (a few hundred), a few microseconds — same order as the karma mux's list ops. Acceptable.
- **Scope discipline:** these are *residual* hardening fixes. Remove the crash class (torn count, write-into-freed-pool, inconsistent gate). Do NOT redesign the ring buffers, the GATT client, or the bridge state machine. Cosmetic residuals (a torn RSSI/name field on an already-published BLE entry) are explicitly accepted and out of scope.
- Repo is git on `master`. Commit per task with the message shown. Do not touch unrelated files. The four untracked helper scripts (`build_manual.sh`, `build_manual2.sh`, `run_build.bat`, `test_build.bat`) are pre-existing — leave them untracked, do not add them.

---

### Task 1: Close the PMKID capture-pool teardown UAF

**Files:**
- Modify: `applications/main/wlan_app/scenes/scene_pmkid.c` (callback `pmk_rx_cb` ~line 66-91; teardown `wlan_app_scene_pmkid_on_exit` ~line 238-255; add a file-scope mux near the other `s_*` statics ~line 40-50)

**The race:** `pmk_rx_cb` runs on the WiFi RX task. It does `if(!s_pool) return;` (line 68) then later `memcpy(s_pool[s_w].data, ...)` (line 88). `wlan_app_scene_pmkid_on_exit` runs on the UI task and does `free(s_pool); s_pool = NULL;` (line 247). `wlan_hal_set_promiscuous(false, NULL)` (line 240) synchronizes with the *HAL worker* task, not the WiFi RX task, so an RX callback that already passed the NULL check can be mid-`memcpy` when the pool is freed → write into freed heap.

**Interfaces:**
- Consumes: nothing new.
- Produces: nothing new (file-internal fix only).

- [ ] **Step 1: Add the file-scope critical-section mutex**

In `scene_pmkid.c`, immediately after the pool statics (near line 40, after `s_read_status`/`s_pool` declarations — place it with the other `s_*` file statics), add:

```c
/* Guards the capture pool against a concurrent free() in on_exit while the
 * WiFi RX task is inside pmk_rx_cb. See docs/.../p5-concurrency-hardening. */
static portMUX_TYPE s_pmk_pool_mux = portMUX_INITIALIZER_UNLOCKED;
```

If `<freertos/FreeRTOS.h>` / the portMUX type is not already visible, add `#include <freertos/FreeRTOS.h>` with the other includes at the top of the file. (`furi.h` usually pulls it in transitively; confirm during build — if it compiles, the include is already present.)

- [ ] **Step 2: Guard the pool write in `pmk_rx_cb`**

Replace the tail of `pmk_rx_cb` (current lines 86-90):

```c
    uint32_t next = (s_w + 1) % PMK_POOL_SIZE;
    if(next == s_r) return;
    memcpy(s_pool[s_w].data, payload, len);
    s_pool[s_w].len = (uint16_t)len;
    s_w = next;
}
```

with:

```c
    portENTER_CRITICAL(&s_pmk_pool_mux);
    if(s_pool) {                       /* authoritative re-check under the lock */
        uint32_t next = (s_w + 1) % PMK_POOL_SIZE;
        if(next != s_r) {
            memcpy(s_pool[s_w].data, payload, len);
            s_pool[s_w].len = (uint16_t)len;
            s_w = next;
        }
    }
    portEXIT_CRITICAL(&s_pmk_pool_mux);
}
```

Leave the early `if(!s_pool) return;` at line 68 in place — it is a cheap fast-path; the authoritative guard is now the locked re-check.

- [ ] **Step 3: Free the pool under the lock in `wlan_app_scene_pmkid_on_exit`**

Replace the free block (current lines 246-249):

```c
    if(s_pool) {
        free(s_pool);
        s_pool = NULL;
    }
```

with:

```c
    /* NULL the pointer under the lock so any RX callback that acquires the mux
     * after this point sees s_pool == NULL and skips; a callback already inside
     * the critical section completes before we capture+free here. */
    PmkPkt* pool_to_free;
    portENTER_CRITICAL(&s_pmk_pool_mux);
    pool_to_free = s_pool;
    s_pool = NULL;
    portEXIT_CRITICAL(&s_pmk_pool_mux);
    if(pool_to_free) free(pool_to_free);
```

Keep the preceding `wlan_hal_set_promiscuous(false, NULL);` (line 240) and `pmk_drain();` (line 241) exactly where they are — drain still runs on the UI task before the free, unchanged.

- [ ] **Step 4: Build**

Run: `MSYS_NO_PATHCONV=1 cmd.exe /c "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_ci.bat" > build_p5t1.log 2>&1; echo "EXIT=$?"`
Expected: tail of `build_p5t1.log` shows `Project build complete` and `=== BUILD OK ===`, EXIT=0. If `PmkPkt` is not the pool element type, use the actual element type from the `s_pool` declaration (grep `s_pool` near line 40 to confirm).

- [ ] **Step 5: Commit**

```bash
git add applications/main/wlan_app/scenes/scene_pmkid.c
git commit -m "fix(wlan): close PMKID capture-pool teardown UAF (portMUX guard)"
```

---

### Task 2: Close the handshake capture-pool teardown UAF

**Files:**
- Modify: `applications/main/wlan_app/scenes/scene_handshake.c` (callbacks `hs_rx_callback_single` ~line 301-338 and `hs_rx_callback_channel` ~line 472+; teardown `wlan_app_scene_handshake_on_exit` free block ~line 902-905; add a file-scope mux near `s_pkt_pool` ~line 33)

**The race:** identical shape to Task 1 but for `s_pkt_pool`. Both `hs_rx_callback_single` (line 306 NULL-check, line 334 `memcpy(slot->data,...)`) and `hs_rx_callback_channel` (line 472 NULL-check, symmetric write) run on the WiFi RX task; `wlan_app_scene_handshake_on_exit` frees `s_pkt_pool` (line 903) on the UI task. Same free-vs-write window.

**Interfaces:**
- Consumes: nothing new.
- Produces: nothing new.

- [ ] **Step 1: Add the file-scope critical-section mutex**

In `scene_handshake.c`, right after the `s_pkt_pool` declaration (line 33 `static HsPkt* s_pkt_pool = NULL;`), add:

```c
/* Guards s_pkt_pool against free() in on_exit while the WiFi RX task is inside
 * an hs_rx_callback_*. See docs/.../p5-concurrency-hardening. */
static portMUX_TYPE s_hs_pool_mux = portMUX_INITIALIZER_UNLOCKED;
```

Add `#include <freertos/FreeRTOS.h>` at the top only if the build reports the portMUX type is undeclared.

- [ ] **Step 2: Guard the write in `hs_rx_callback_single`**

Replace the tail (current lines 331-337):

```c
    uint32_t next = (s_write_idx + 1) % HS_PKT_POOL_SIZE;
    if(next == s_read_idx) return;
    HsPkt* slot = &s_pkt_pool[s_write_idx];
    memcpy(slot->data, payload, len);
    slot->len = len;
    slot->timestamp_us = pkt->rx_ctrl.timestamp;
    s_write_idx = next;
}
```

with:

```c
    portENTER_CRITICAL(&s_hs_pool_mux);
    if(s_pkt_pool) {
        uint32_t next = (s_write_idx + 1) % HS_PKT_POOL_SIZE;
        if(next != s_read_idx) {
            HsPkt* slot = &s_pkt_pool[s_write_idx];
            memcpy(slot->data, payload, len);
            slot->len = len;
            slot->timestamp_us = pkt->rx_ctrl.timestamp;
            s_write_idx = next;
        }
    }
    portEXIT_CRITICAL(&s_hs_pool_mux);
}
```

- [ ] **Step 3: Guard the write in `hs_rx_callback_channel`**

Read `hs_rx_callback_channel` (starts ~line 472). It has the same `if(!s_pkt_pool) return;` fast-path and a `memcpy(slot->data, ...)`-style pool write near its end. Apply the identical transformation: wrap the `next`/`s_read_idx` check + `s_pkt_pool[...]` write + index advance in `portENTER_CRITICAL(&s_hs_pool_mux)` / `portEXIT_CRITICAL(&s_hs_pool_mux)` with an inner `if(s_pkt_pool)` re-check, exactly as Step 2. Preserve whatever per-target fields that callback writes (it may set a target index in addition to `len`/`timestamp_us`) — move the entire slot-population block inside the lock unchanged, only adding the lock + re-check around it.

- [ ] **Step 4: Free the pool under the lock in `wlan_app_scene_handshake_on_exit`**

Replace the free block (current lines 902-905):

```c
    if(s_pkt_pool) {
        free(s_pkt_pool);
        s_pkt_pool = NULL;
    }
```

with:

```c
    HsPkt* pool_to_free;
    portENTER_CRITICAL(&s_hs_pool_mux);
    pool_to_free = s_pkt_pool;
    s_pkt_pool = NULL;
    portEXIT_CRITICAL(&s_hs_pool_mux);
    if(pool_to_free) free(pool_to_free);
```

Leave the `wlan_hal_set_promiscuous(false, NULL);` and the drain/close-pcap calls above it (lines 877-900) unchanged — they run on the UI task before the free.

- [ ] **Step 5: Build**

Run: `MSYS_NO_PATHCONV=1 cmd.exe /c "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_ci.bat" > build_p5t2.log 2>&1; echo "EXIT=$?"`
Expected: `Project build complete` + `=== BUILD OK ===`, EXIT=0.

- [ ] **Step 6: Commit**

```bash
git add applications/main/wlan_app/scenes/scene_handshake.c
git commit -m "fix(wlan): close handshake capture-pool teardown UAF (portMUX guard)"
```

---

### Task 3: Publish-barrier the BLE walk device count

**Files:**
- Modify: `applications/main/ble_spam/ble_walk_hal.c` (append site ~line 116-137, count read in `ble_walk_hal_get_devices` ~line 406-408, reset ~line 300)

**The race:** the GAP callback `walk_gap_event_handler` (Bluedroid BT task) appends a device: it populates `s_devices[s_device_count]` fully (lines 117-136) *then* does `s_device_count++` (line 137) — the ordering is already correct. But `s_device_count` is a plain `uint16_t`. `ble_walk_hal_get_devices` (UI task, line 407) reads it with no barrier, so (a) the compiler may cache/hoist the read, and (b) on the dual-core S3 the reader can observe the incremented count *before* the entry writes land, then iterate over a half-written `s_devices[]` entry. Fix: release-store the count after the populate, acquire-load it in the reader. This is the exact `__atomic` publish idiom used in `wlan_cred_sniff.c`.

**Interfaces:**
- Consumes: nothing new.
- Produces: nothing new — `ble_walk_hal_get_devices(uint16_t*)` signature and semantics unchanged.

- [ ] **Step 1: Release-store the count on append**

At line 137, replace:

```c
                s_device_count++;
```

with:

```c
                /* Publish: all s_devices[count] writes above must be visible
                 * before the reader observes the new count. */
                __atomic_store_n(&s_device_count, (uint16_t)(s_device_count + 1), __ATOMIC_RELEASE);
```

(The read of `s_device_count` for the `+ 1` is safe — the GAP callback is the only writer.)

- [ ] **Step 2: Acquire-load the count in the reader**

In `ble_walk_hal_get_devices` (line 406-408), replace:

```c
BleWalkDevice* ble_walk_hal_get_devices(uint16_t* count) {
    *count = s_device_count;
```

with:

```c
BleWalkDevice* ble_walk_hal_get_devices(uint16_t* count) {
    *count = __atomic_load_n(&s_device_count, __ATOMIC_ACQUIRE);
```

Leave the rest of the function (the `return s_devices;`) unchanged.

- [ ] **Step 3: Make the reset consistent**

At line 300 (`s_device_count = 0;`, in the scan-start/reset path), replace with:

```c
    __atomic_store_n(&s_device_count, 0, __ATOMIC_RELEASE);
```

Leave the writer's own internal reads of `s_device_count` in the scan loop (lines 99, 116) as plain reads — the callback is the sole writer, so it always sees its own latest value; no atomic needed there.

- [ ] **Step 4: Build**

Run: `MSYS_NO_PATHCONV=1 cmd.exe /c "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_ci.bat" > build_p5t3.log 2>&1; echo "EXIT=$?"`
Expected: `Project build complete` + `=== BUILD OK ===`, EXIT=0.

- [ ] **Step 5: Commit**

```bash
git add applications/main/ble_spam/ble_walk_hal.c
git commit -m "fix(ble): publish-barrier walk device count (acquire/release)"
```

---

### Task 4: Publish-barrier the BLE tracker device count

**Files:**
- Modify: `applications/main/ble_spam/ble_tracker_hal.c` (append site ~line 234-244, count read in `ble_tracker_hal_get_devices` ~line 298-300, resets ~line 263 and ~line 306)

**The race:** identical to Task 3 for `s_device_count` in the tracker. Callback populates `s_devices[s_device_count]` (lines 234-243) then `s_device_count++` (line 244); `ble_tracker_hal_get_devices` (line 299) reads it barrier-free.

**Interfaces:**
- Consumes: nothing new.
- Produces: nothing new — `ble_tracker_hal_get_devices(uint16_t*)` unchanged.

- [ ] **Step 1: Release-store the count on append**

At line 244, replace `s_device_count++;` with:

```c
                __atomic_store_n(&s_device_count, (uint16_t)(s_device_count + 1), __ATOMIC_RELEASE);
```

- [ ] **Step 2: Acquire-load the count in the reader**

In `ble_tracker_hal_get_devices` (line 298-300), replace `*count = s_device_count;` with:

```c
    *count = __atomic_load_n(&s_device_count, __ATOMIC_ACQUIRE);
```

- [ ] **Step 3: Make the resets consistent**

At line 263 and line 306, replace each `s_device_count = 0;` with:

```c
    __atomic_store_n(&s_device_count, 0, __ATOMIC_RELEASE);
```

Leave the writer's internal reads (lines 213, 233) as plain reads.

- [ ] **Step 4: Build**

Run: `MSYS_NO_PATHCONV=1 cmd.exe /c "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_ci.bat" > build_p5t4.log 2>&1; echo "EXIT=$?"`
Expected: `Project build complete` + `=== BUILD OK ===`, EXIT=0.

- [ ] **Step 5: Commit**

```bash
git add applications/main/ble_spam/ble_tracker_hal.c
git commit -m "fix(ble): publish-barrier tracker device count (acquire/release)"
```

---

### Task 5: Serialize the Evil Portal bridge gate flags

**Files:**
- Modify: `applications/main/wlan_app/wlan_evil_portal_bridge.c` (flag decls ~line 16-19; `bridge_sta_event_handler` disconnect clear ~line 127-129; `wlan_evil_portal_bridge_prewarm` gate ~line 307-316; `wlan_evil_portal_bridge_start` gate ~line 334-363; `wlan_evil_portal_bridge_stop` clear ~line 377-380; `wlan_evil_portal_bridge_is_active` ~line 387-388)

**The race:** `s_bridge_active`, `s_bridge_attempting`, `s_bridge_prewarmed`, `s_pending_activate` are plain `bool`s read/written from three task contexts — the httpd/UI task (`prewarm`/`start`/`stop`/`is_active`), the WiFi-event task (`bridge_sta_event_handler` clears them on disconnect, line 127-129), and indirectly the HAL worker. The compound check-then-set gates (e.g. `if(active||attempting||prewarmed) return; ... attempting=true`, lines 307/315) are not atomic against a concurrent event-task clear, and no barrier guarantees cross-task visibility. Fix: one `portMUX` guarding every semantically-meaningful read/modify of these four flags. Worker dispatch and logging stay OUTSIDE the lock.

**Interfaces:**
- Consumes: nothing new.
- Produces: nothing new — all four public function signatures unchanged.

- [ ] **Step 1: Add the gate mutex**

After the four flag declarations (line 19), add:

```c
/* Serializes the four bridge gate flags across the httpd/UI task, the WiFi-event
 * task, and the HAL worker. See docs/.../p5-concurrency-hardening. */
static portMUX_TYPE s_bridge_mux = portMUX_INITIALIZER_UNLOCKED;
```

`<freertos/FreeRTOS.h>` is already included at line 12 (`freertos/task.h`) region — confirm portMUX resolves at build; add `#include <freertos/FreeRTOS.h>` if not.

- [ ] **Step 2: Atomic gate in `wlan_evil_portal_bridge_prewarm`**

Replace lines 307-316 (from the gate check through the two flag sets):

```c
    if(s_bridge_active || s_bridge_attempting || s_bridge_prewarmed) return;

    strncpy(s_last_ssid, real_ssid, sizeof(s_last_ssid) - 1);
    s_last_ssid[sizeof(s_last_ssid) - 1] = '\0';
    strncpy(s_last_password, password, sizeof(s_last_password) - 1);
    s_last_password[sizeof(s_last_password) - 1] = '\0';
    s_fallback_tried = false;

    s_bridge_attempting = true;
    s_pending_activate = false;
```

with:

```c
    bool proceed = false;
    portENTER_CRITICAL(&s_bridge_mux);
    if(!(s_bridge_active || s_bridge_attempting || s_bridge_prewarmed)) {
        s_bridge_attempting = true;
        s_pending_activate = false;
        proceed = true;
    }
    portEXIT_CRITICAL(&s_bridge_mux);
    if(!proceed) return;

    strncpy(s_last_ssid, real_ssid, sizeof(s_last_ssid) - 1);
    s_last_ssid[sizeof(s_last_ssid) - 1] = '\0';
    strncpy(s_last_password, password, sizeof(s_last_password) - 1);
    s_last_password[sizeof(s_last_password) - 1] = '\0';
    s_fallback_tried = false;
```

(The string copies stay outside the lock — they are only read by `bridge_start_worker` after the dispatch below, which is fine; setting `attempting=true` under the lock already claims the gate.)

- [ ] **Step 3: Atomic gates in `wlan_evil_portal_bridge_start`**

This function has four checks. Convert them to a single locked decision that yields an action, then act (dispatch/log) outside the lock. Replace lines 334-363:

```c
    if(s_bridge_active) {
        ESP_LOGI(BRIDGE_TAG, "bridge already active; ignoring new attempt");
        return;
    }

    // Fast path: STA already connected via prewarm. Just activate NAPT + DNS forward.
    if(s_bridge_prewarmed) {
        ESP_LOGI(BRIDGE_TAG, "prewarmed path: activating NAPT now");
        wlan_hal_run_in_worker(bridge_activate_worker, NULL);
        return;
    }

    // Prewarm in flight: mark to activate as soon as IP arrives.
    if(s_bridge_attempting) {
        ESP_LOGI(BRIDGE_TAG, "bridge attempt in flight; will activate on IP arrival");
        s_pending_activate = true;
        return;
    }

    // Cold path: no prewarm happened. Original flow.
    if(strcmp(real_ssid, s_last_ssid) != 0 || strcmp(password, s_last_password) != 0) {
        s_fallback_tried = false;
    }
    strncpy(s_last_ssid, real_ssid, sizeof(s_last_ssid) - 1);
    s_last_ssid[sizeof(s_last_ssid) - 1] = '\0';
    strncpy(s_last_password, password, sizeof(s_last_password) - 1);
    s_last_password[sizeof(s_last_password) - 1] = '\0';

    s_bridge_attempting = true;
    s_pending_activate = true;

    BridgeConnectArgs args = {0};
    strncpy(args.ssid, real_ssid, sizeof(args.ssid) - 1);
    strncpy(args.password, password, sizeof(args.password) - 1);
    wlan_hal_run_in_worker(bridge_start_worker, &args);
```

with:

```c
    /* Decide the action atomically over the gate flags, then act outside the lock. */
    enum { ACT_NONE, ACT_ACTIVATE, ACT_PENDING, ACT_COLD_START } action = ACT_NONE;
    portENTER_CRITICAL(&s_bridge_mux);
    if(s_bridge_active) {
        action = ACT_NONE;
    } else if(s_bridge_prewarmed) {
        action = ACT_ACTIVATE;
    } else if(s_bridge_attempting) {
        s_pending_activate = true;
        action = ACT_PENDING;
    } else {
        s_bridge_attempting = true;
        s_pending_activate = true;
        action = ACT_COLD_START;
    }
    portEXIT_CRITICAL(&s_bridge_mux);

    if(action == ACT_NONE) {
        ESP_LOGI(BRIDGE_TAG, "bridge already active; ignoring new attempt");
        return;
    }
    if(action == ACT_ACTIVATE) {
        ESP_LOGI(BRIDGE_TAG, "prewarmed path: activating NAPT now");
        wlan_hal_run_in_worker(bridge_activate_worker, NULL);
        return;
    }
    if(action == ACT_PENDING) {
        ESP_LOGI(BRIDGE_TAG, "bridge attempt in flight; will activate on IP arrival");
        return;
    }

    // Cold path: no prewarm happened. Original flow.
    if(strcmp(real_ssid, s_last_ssid) != 0 || strcmp(password, s_last_password) != 0) {
        s_fallback_tried = false;
    }
    strncpy(s_last_ssid, real_ssid, sizeof(s_last_ssid) - 1);
    s_last_ssid[sizeof(s_last_ssid) - 1] = '\0';
    strncpy(s_last_password, password, sizeof(s_last_password) - 1);
    s_last_password[sizeof(s_last_password) - 1] = '\0';

    BridgeConnectArgs args = {0};
    strncpy(args.ssid, real_ssid, sizeof(args.ssid) - 1);
    strncpy(args.password, password, sizeof(args.password) - 1);
    wlan_hal_run_in_worker(bridge_start_worker, &args);
```

- [ ] **Step 4: Guard the disconnect clear in `bridge_sta_event_handler`**

At lines 127-129 (WiFi-event task), replace:

```c
        s_bridge_active = false;
        s_bridge_attempting = false;
        s_bridge_prewarmed = false;
```

with:

```c
        portENTER_CRITICAL(&s_bridge_mux);
        s_bridge_active = false;
        s_bridge_attempting = false;
        s_bridge_prewarmed = false;
        portEXIT_CRITICAL(&s_bridge_mux);
```

(Leave the surrounding `esp_netif_napt_disable` / `wlan_hal_evil_portal_set_dns_upstream` and the fallback-reason logic outside the lock — they must not run inside a critical section.)

- [ ] **Step 5: Guard the clear in `wlan_evil_portal_bridge_stop`**

At lines 377-380, replace:

```c
    s_bridge_active = false;
    s_bridge_attempting = false;
    s_bridge_prewarmed = false;
    s_pending_activate = false;
```

with:

```c
    portENTER_CRITICAL(&s_bridge_mux);
    s_bridge_active = false;
    s_bridge_attempting = false;
    s_bridge_prewarmed = false;
    s_pending_activate = false;
    portEXIT_CRITICAL(&s_bridge_mux);
```

Keep the `esp_event_handler_unregister` / `wlan_hal_run_in_worker(bridge_disconnect_worker...)` calls (lines 372-376) and the `s_last_ssid`/`s_ap_netif` resets (lines 381-384) outside the lock.

- [ ] **Step 6: Guard the read in `wlan_evil_portal_bridge_is_active`**

Replace line 388 `return s_bridge_active;` with:

```c
    portENTER_CRITICAL(&s_bridge_mux);
    bool active = s_bridge_active;
    portEXIT_CRITICAL(&s_bridge_mux);
    return active;
```

Note: the diagnostic `ESP_LOGI` reads of the flags at lines 302-305 and 326-329 are left unlocked on purpose — a slightly-stale value in a log line is harmless, and `ESP_LOGI` must not be called under the lock anyway. `bridge_ip_event_handler` sets `s_bridge_prewarmed`/`s_bridge_active` (line 178/204) — wrap only the flag assignments there in the same `portENTER_CRITICAL(&s_bridge_mux)/portEXIT_CRITICAL` if they are standalone assignments; if they are interleaved with logging or worker calls, lock only the assignment lines. Verify lines 178 and 204 during implementation and apply the minimal lock around the assignment(s).

- [ ] **Step 7: Build**

Run: `MSYS_NO_PATHCONV=1 cmd.exe /c "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_ci.bat" > build_p5t5.log 2>&1; echo "EXIT=$?"`
Expected: `Project build complete` + `=== BUILD OK ===`, EXIT=0.

- [ ] **Step 8: Commit**

```bash
git add applications/main/wlan_app/wlan_evil_portal_bridge.c
git commit -m "fix(wlan): serialize Evil Portal bridge gate flags (portMUX)"
```

---

### Task 6: Mark P5 done in the readiness roadmap

**Files:**
- Modify: `docs/RED_TEAM_READINESS.md` (item #5, lines 43-46)

- [ ] **Step 1: Strike through and annotate item #5**

Change the item #5 heading (line 43) from:

```markdown
5. **Harden the accepted concurrency residuals** — Reliability / Medium / M
```

to:

```markdown
5. ~~**Harden the accepted concurrency residuals**~~ ✅ DONE (P5 pass) — Reliability / Medium / M
```

and append to the item's body (after line 46) a one-line note:

```markdown
   Fixed: PMKID + handshake capture-pool teardown UAF (portMUX free-vs-write guard); BLE walk/tracker device count now acquire/release published; Evil Portal bridge gate flags serialized under one portMUX. Cosmetic torn-field residual on already-published BLE entries accepted. **Not yet hardware-validated.**
```

- [ ] **Step 2: Commit**

```bash
git add docs/RED_TEAM_READINESS.md
git commit -m "docs: mark P5 concurrency hardening done in readiness roadmap"
```

---

## Self-Review

**Spec coverage** (audit item #5 lists three residual classes):
- "PMKID/handshake teardown UAF window" → Task 1 (PMKID) + Task 2 (handshake). ✅
- "unsynchronized device counts (`ble_walk_hal.c:28`, `ble_tracker_hal.c:16`)" → Task 3 + Task 4. ✅
- "Evil Portal bridge flags (`wlan_evil_portal_bridge.c:16`)" → Task 5. ✅
- Roadmap bookkeeping → Task 6. ✅

**Placeholder scan:** every code step shows concrete before/after. The two "verify the exact line during implementation" notes (Task 2 Step 3 `hs_rx_callback_channel`, Task 5 Step 6 `bridge_ip_event_handler` lines 178/204) are genuine "apply the identical, already-shown transformation at the sibling site" instructions, not deferred design — the transformation itself is fully specified. Acceptable.

**Type/name consistency:** `portMUX_TYPE` + `portENTER_CRITICAL`/`portEXIT_CRITICAL` and `__atomic_store_n`/`__atomic_load_n` used verbatim as in the cited existing files. Mutex names are unique per file (`s_pmk_pool_mux`, `s_hs_pool_mux`, `s_bridge_mux`). Public signatures (`ble_walk_hal_get_devices`, `ble_tracker_hal_get_devices`, the four bridge functions) unchanged. Pool element types (`PmkPkt`, `HsPkt`) referenced from their existing declarations.

**Ordering note:** Tasks 1-5 are fully independent (different files, no shared symbols) and may be implemented/committed in any order or in parallel. Task 6 must run last.
