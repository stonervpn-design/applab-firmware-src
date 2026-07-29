# P0 Hardening Pass Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Land the three agreed P0 hardening fixes from the red-team readiness audit: (1) lock the shared SPI3 bus around `sd_release_host()`, (2) randomize the transmit MAC in PMKID injection and the Evil Portal SoftAP so the operator's real MAC never goes on the air, (3) preserve LAN host scans as timestamped snapshots instead of overwriting.

**Architecture:** Three independent, surgical changes. A new shared `wlan_hal_random_mac()` helper (lifted from the existing beacon LAA generator) is consumed by both the PMKID scene and the Evil Portal AP path. The LAN cache keeps its canonical `<ssid>.csv` (read back by load/exists) and additionally writes a dated snapshot. No behavior change to capture/parse logic.

**Tech Stack:** ESP-IDF v5.4.1 (ESP32-S3), C. Firmware build only (no host tests — these are firmware-integration fixes verified by build + on-hardware).

## Global Constraints

- Target board `m5stack_cardputer_adv`. Build via `build_ci.bat`, invoked from Git Bash SYNCHRONOUSLY (never backgrounded), timeout 600000 ms:
  `MSYS_NO_PATHCONV=1 cmd.exe /c "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_ci.bat" > build_<tag>.log 2>&1; echo "EXIT=$?"`
  Success = tail has `Project build complete` and `=== BUILD OK ===`, no `FAILED ===`. `build_cardputer_adv/` is validly configured → incremental; do NOT delete it. `build_*.log` is gitignored.
- Random MAC format is a locally-administered UNICAST MAC: 6 random bytes with `out[0] = (out[0] & 0xFE) | 0x02` (bit0=0 unicast, bit1=1 LAA). This exact form is already used by the beacon path (`wlan_hal.c:653-654`) and satisfies `esp_wifi_set_mac(WIFI_IF_AP,…)` requirements.
- `esp_wifi_set_mac()` MUST be called after `esp_wifi_set_mode()` and BEFORE `esp_wifi_start()`.
- The PMKID capture path filters M1 by target BSSID (not by our MAC), and records the M1's destination as the STA — so swapping `s_own_mac` to a random LAA changes nothing in capture; the loot simply records the spoofed MAC. Do not touch the capture/drain logic.
- The LAN cache `<ssid>.csv` is read back by `wlan_lan_cache_load`/`wlan_lan_cache_exists` — it MUST keep being written unchanged. The timestamped snapshot is ADDITIVE and best-effort (a snapshot write failure must not fail the cache save).
- Repo is a git repo on `master`; commit per task. Subagents run on opus/haiku (Sonnet-5 trips a cyber gate on this code).

---

### Task 1: Lock the shared SPI3 bus around `sd_release_host()`

**Files:**
- Modify: `components/furi_hal/furi_hal_sd.c` (two call sites: ~line 573 and ~line 757)

**Interfaces:** none exported. Behavior: the two unlocked `sd_release_host()` calls become lock-wrapped, matching the existing correct site at ~line 662.

- [ ] **Step 1: Confirm the reference pattern**

Read `components/furi_hal/furi_hal_sd.c` around line 660. The correct, already-locked pattern is:
```c
        furi_hal_spi_bus_lock();
        sd_release_host();
        furi_hal_spi_bus_unlock();
```
`sd_release_host()` (defined ~line 532) does NOT take the lock itself (line 662 wraps it), so wrapping the other two sites cannot double-lock/deadlock. Verify by reading `sd_release_host`'s body that it contains no `furi_hal_spi_bus_lock()` call.

- [ ] **Step 2: Wrap the init-path call (~line 573)**

In `sd_prepare_card()`, the standalone call:
```c
    sd_release_host();
```
(the one at ~line 573, immediately after the `ESP_LOGW ... "Skipping SD init"` block and before the `"Initializing SD card"` log) becomes:
```c
    furi_hal_spi_bus_lock();
    sd_release_host();
    furi_hal_spi_bus_unlock();
```

- [ ] **Step 3: Wrap the unmount-path call (~line 757)**

In `furi_hal_sd_unmount()`, after the `f_mount(NULL, …)` success path:
```c
    ESP_LOGI(TAG, "SD card unmounted");
    sd_release_host();
    return true;
```
becomes:
```c
    ESP_LOGI(TAG, "SD card unmounted");
    furi_hal_spi_bus_lock();
    sd_release_host();
    furi_hal_spi_bus_unlock();
    return true;
```

Note: there are exactly TWO unwrapped `sd_release_host()` calls to fix. Do NOT touch the one at ~line 662 (already wrapped) or the definition at ~line 532.

- [ ] **Step 4: Build**

Run the standard build (`build_sd.log`). Expected `=== BUILD OK ===`.

- [ ] **Step 5: Commit**

```bash
git add components/furi_hal/furi_hal_sd.c
git commit -m "fix(sd): lock shared SPI3 bus around sd_release_host (prevents CC1101 race)"
```

---

### Task 2: Randomize the transmit MAC (shared helper + PMKID + SoftAP)

**Files:**
- Modify: `applications/main/wlan_app/wlan_hal.h` (add helper prototype)
- Modify: `applications/main/wlan_app/wlan_hal.c` (add helper; refactor the beacon LAA gen to use it)
- Modify: `applications/main/wlan_app/scenes/scene_pmkid.c` (use random MAC instead of the real STA MAC)
- Modify: `applications/main/wlan_app/wlan_evil_portal.c` (set a random AP MAC before `esp_wifi_start`)

**Interfaces:**
- Produces: `void wlan_hal_random_mac(uint8_t out[6]);` — fills a random locally-administered unicast MAC.

- [ ] **Step 1: Add the helper prototype**

In `applications/main/wlan_app/wlan_hal.h`, near the other utility prototypes (e.g. after `wlan_hal_get_own_mac`):
```c
/** Fill out[6] with a random locally-administered UNICAST MAC (bit0=0, bit1=1).
 *  Used to spoof the source of injected frames and the SoftAP identity so the
 *  device's real factory MAC never goes on the air. */
void wlan_hal_random_mac(uint8_t out[6]);
```

- [ ] **Step 2: Implement the helper + refactor the beacon path**

In `applications/main/wlan_app/wlan_hal.c`, add the implementation (near the other helpers such as `wlan_hal_get_own_mac`):
```c
void wlan_hal_random_mac(uint8_t out[6]) {
    for(int i = 0; i < 6; i++) out[i] = rand() & 0xFF;
    out[0] = (out[0] & 0xFE) | 0x02; /* unicast + locally administered */
}
```
Then in the beacon-spam loop, replace the inline generator (currently ~line 653-654):
```c
        for(int i = 0; i < 6; i++) mac[i] = rand() & 0xFF;
        mac[0] = (mac[0] & 0xFE) | 0x02;
```
with:
```c
        wlan_hal_random_mac(mac);
```

- [ ] **Step 3: PMKID scene — spoof the injection MAC**

In `applications/main/wlan_app/scenes/scene_pmkid.c`, in `on_enter` (~line 184), replace:
```c
    wlan_hal_get_own_mac(s_own_mac);
```
with:
```c
    wlan_hal_random_mac(s_own_mac); /* spoofed LAA src — never leak the real device MAC */
```
Leave everything else (`pmk_inject_assoc` using `s_own_mac`, the capture filter, and `wlan_pmkid_save(..., station ? station : s_own_mac, ...)`) unchanged — they now naturally use the spoofed identity.

- [ ] **Step 4: Evil Portal — set a random SoftAP MAC before start**

In `applications/main/wlan_app/wlan_evil_portal.c`, first ensure `#include "wlan_hal.h"` is present near the top (add it if missing). Then in the AP-start worker, between the successful `esp_wifi_set_config(WIFI_IF_AP, &ap_cfg)` (~line 1193) and `esp_wifi_start()` (~line 1207-1208), insert:
```c
    /* Spoof the SoftAP MAC with a random LAA so the rogue AP does not broadcast
     * the device's real factory (Espressif-OUI) MAC. Must be set after set_mode
     * and before esp_wifi_start. Non-fatal on error. */
    {
        uint8_t ap_mac[6];
        wlan_hal_random_mac(ap_mac);
        esp_err_t merr = esp_wifi_set_mac(WIFI_IF_AP, ap_mac);
        if(merr != ESP_OK) ESP_LOGW(TAG, "  set_mac(AP): %s", esp_err_to_name(merr));
    }
```
Place it after the `set_config` error-check block and before the event-handler registration / `esp_wifi_start`.

- [ ] **Step 5: Build**

Run the standard build (`build_mac.log`). Expected `=== BUILD OK ===`. Confirm no undefined reference to `wlan_hal_random_mac` (helper links from wlan_hal.c) and no format/type warnings.

- [ ] **Step 6: Commit**

```bash
git add applications/main/wlan_app/wlan_hal.h applications/main/wlan_app/wlan_hal.c applications/main/wlan_app/scenes/scene_pmkid.c applications/main/wlan_app/wlan_evil_portal.c
git commit -m "feat(wlan): randomize TX MAC for PMKID injection + Evil Portal SoftAP (OPSEC)"
```

---

### Task 3: Timestamped LAN host snapshots (preserve re-scans)

**Files:**
- Modify: `applications/main/wlan_app/wlan_lan_cache.c`

**Interfaces:** none changed. `wlan_lan_cache_save` still writes the canonical `<ssid>.csv`; it additionally writes a dated snapshot. `load`/`exists` are untouched.

- [ ] **Step 1: Add the RTC include**

In `applications/main/wlan_app/wlan_lan_cache.c`, add near the top includes:
```c
#include <furi_hal_rtc.h>
```

- [ ] **Step 2: Extract the CSV writer**

Add a static helper above `wlan_lan_cache_save` that contains the existing open+header+row-writing logic (moved verbatim from the current body of `wlan_lan_cache_save`):
```c
static bool lan_write_csv(
    Storage* storage, const char* path, const WlanDeviceRecord* devices, uint16_t count) {
    File* file = storage_file_alloc(storage);
    bool ok = false;
    if(storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        size_t hlen = strlen(LAN_CACHE_HEADER);
        ok = (storage_file_write(file, LAN_CACHE_HEADER, hlen) == hlen);

        char line[96];
        for(uint16_t i = 0; ok && i < count; i++) {
            const WlanDeviceRecord* d = &devices[i];
            uint32_t host = __builtin_bswap32(d->ip);
            uint8_t a = (host >> 24) & 0xFF;
            uint8_t b = (host >> 16) & 0xFF;
            uint8_t c = (host >> 8) & 0xFF;
            uint8_t e = host & 0xFF;

            char hostname_buf[WLAN_APP_HOSTNAME_MAX];
            size_t hi = 0;
            for(size_t j = 0; j < sizeof(d->hostname) && d->hostname[j]; j++) {
                char ch = d->hostname[j];
                if(ch == ',' || ch == '\n' || ch == '\r') ch = '_';
                if(hi < sizeof(hostname_buf) - 1) hostname_buf[hi++] = ch;
            }
            hostname_buf[hi] = '\0';

            int n = snprintf(
                line, sizeof(line),
                "%u.%u.%u.%u,%02x:%02x:%02x:%02x:%02x:%02x,%s\n",
                a, b, c, e,
                d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5],
                hostname_buf);
            if(n <= 0 || n >= (int)sizeof(line)) { ok = false; break; }
            ok = (storage_file_write(file, line, (size_t)n) == (size_t)n);
        }
        storage_file_close(file);
    }
    storage_file_free(file);
    return ok;
}
```

- [ ] **Step 3: Rewrite `wlan_lan_cache_save` to write cache + dated snapshot**

Replace the body of `wlan_lan_cache_save` with:
```c
bool wlan_lan_cache_save(
    const char* ssid,
    const WlanDeviceRecord* devices,
    uint16_t count) {
    if(!ssid || !ssid[0] || (!devices && count > 0)) return false;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, LAN_CACHE_DIR_WIFI);
    storage_simply_mkdir(storage, LAN_CACHE_DIR_LAN);

    char safe_ssid[34];
    sanitize_ssid_for_filename(ssid, safe_ssid, sizeof(safe_ssid));

    /* Canonical cache (latest snapshot, read back by load/exists). */
    char path[80];
    snprintf(path, sizeof(path), "%s/%s.csv", LAN_CACHE_DIR_LAN, safe_ssid);
    bool ok = lan_write_csv(storage, path, devices, count);

    /* Timestamped evidence snapshot — additive, best-effort (never fails the save). */
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    char snap[128];
    snprintf(
        snap, sizeof(snap), "%s/%s_%04u%02u%02u_%02u%02u%02u.csv",
        LAN_CACHE_DIR_LAN, safe_ssid,
        (unsigned)dt.year, (unsigned)dt.month, (unsigned)dt.day,
        (unsigned)dt.hour, (unsigned)dt.minute, (unsigned)dt.second);
    lan_write_csv(storage, snap, devices, count);

    furi_record_close(RECORD_STORAGE);
    return ok;
}
```
Note: `build_lan_cache_path` may now be unused by `save` but is still used by `load`/`exists` — leave it. If the compiler warns `build_lan_cache_path` is unused overall, confirm `load` and `exists` still call it (they do) before touching it.

- [ ] **Step 4: Build**

Run the standard build (`build_lan.log`). Expected `=== BUILD OK ===`, no unused-function or format warnings.

- [ ] **Step 5: Commit**

```bash
git add applications/main/wlan_app/wlan_lan_cache.c
git commit -m "feat(wlan): write timestamped LAN host snapshots alongside the cache"
```

---

### Task 4: On-hardware validation

**Files:** none (manual; requires the physical Cardputer ADV on COM12).

- [ ] **Step 1: Flash**

`python -m esptool --chip esp32s3 --port COM12 -b 460800 write_flash 0x0 Flipper-cardputer_adv-merged.bin`

- [ ] **Step 2: SPI3 stability** — Exercise SD + Sub-GHz together: browse the Archive (SD) while running a Sub-GHz receive/read, mount/unmount cycles. Expect no crash/hang/FS corruption (previously a latent race).

- [ ] **Step 3: MAC randomization** — With a second sniffer (or another device), run the PMKID Attack and the Evil Portal: confirm the injected assoc frames' source MAC and the SoftAP BSSID are locally-administered (`x2/x6/xA/xE` first octet) and NOT the device's factory Espressif-OUI MAC, and that they differ across runs. Confirm `/ext/wifi/pmkid.22000` records the spoofed MAC, not the real one.

- [ ] **Step 4: LAN snapshots** — Run a network host scan twice on the same SSID; confirm `/ext/wifi/lan/<ssid>.csv` holds the latest AND two dated `<ssid>_<YYYYMMDD_HHMMSS>.csv` snapshots exist (re-scan preserved). Confirm the Connect feature's saved-host read-back still works.

- [ ] **Step 5: Record**

```bash
git commit --allow-empty -m "test: P0 hardening pass validated on Cardputer ADV hardware"
```

---

## Self-Review Notes

- **P0 #1 (SPI3 lock):** Task 1 — wraps both unlocked `sd_release_host()` sites, mirroring the proven line-662 pattern; no double-lock (release does not self-lock). ✓
- **P0 #2 (MAC randomization):** Task 2 — shared helper from the existing beacon LAA gen, applied to PMKID injection (`s_own_mac`) and the SoftAP (`esp_wifi_set_mac(WIFI_IF_AP)` between set_mode/set_config and start). Capture path untouched (filters by BSSID). ✓
- **Fix #3 (timestamped LAN snapshots, replacing the non-defect "overwrite" item):** Task 3 — canonical `<ssid>.csv` cache preserved for read-back; additive dated snapshot; snapshot best-effort. ✓
- **Not changed (intended behavior, confirmed during investigation):** `wlan_passwords.c` overwrite (credential store keyed by SSID — latest-wins is correct); handshake PCAP (already auto-numbers); PMKID/creds/BLE-walk (already append). ✓
- **Type consistency:** `wlan_hal_random_mac(uint8_t[6])` defined in Task 2 and consumed in the same task (beacon, PMKID, evil portal); no cross-task new types. `lan_write_csv` is file-local to Task 3.
