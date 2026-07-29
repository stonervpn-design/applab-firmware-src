# P1 OPSEC Pass Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Two P1 OPSEC fixes from the readiness audit: (1) stop the BadUSB HID descriptor from identifying itself as a Flipper (it ships "Flipper" strings under a Logitech VID); (2) add an opt-in **BLE Stealth** lock-menu toggle that randomizes the BLE advertising MAC and uses a generic name — leaving the default `"mob"` identity and BLE bonding intact when OFF.

**Architecture:** Fix 1 is a default-string change in the HID descriptor. Fix 2 is a new persistent RTC flag (`FuriHalRtcFlagBleStealth`) read at `furi_hal_version_init` (which runs after `furi_hal_rtc_init`) to override the default BLE `ble_mac`+name that every BLE profile falls back to, plus a lock-menu toggle that sets/clears the flag (applies on next boot).

**Tech Stack:** ESP-IDF v5.4.1 (ESP32-S3), C. Firmware build only (no host tests).

## Global Constraints

- Build via `build_ci.bat` from Git Bash, SYNCHRONOUS (never backgrounded), timeout 600000 ms:
  `MSYS_NO_PATHCONV=1 cmd.exe /c "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_ci.bat" > build_<tag>.log 2>&1; echo "EXIT=$?"`
  Success = tail `Project build complete` + `=== BUILD OK ===`, no `FAILED ===`. `build_cardputer_adv/` is validly configured → incremental; do NOT delete it. `build_*.log` gitignored.
- Random static BLE address format (match `furi_hal_bt.c:318`): after filling 6 random bytes, `mac[0] = (mac[0] & 0x3F) | 0xC0`.
- The BLE name default is `"mob"` (the user's rebrand) — it MUST remain the default when stealth is OFF. Stealth ON replaces it with a generic name only for that boot.
- Do NOT touch the qFlipper CDC bridge identity or `furi_hal_version_get_model_name()` ("Flipper Zero") — that identity is required for qFlipper to connect and is out of scope.
- Subagents on opus/haiku (Sonnet-5 cyber gate). Repo is git on `master`; commit per task.

---

### Task 1: De-Flipper the BadUSB HID default descriptor

**Files:**
- Modify: `components/furi_hal/furi_hal_usb_hid_tinyusb.c` (default strings ~line 185-187)

**Interfaces:** none. Behavior: when a BadUSB payload does not set custom USB identity (the app leaves it empty), the HID descriptor now presents a Logitech-consistent identity instead of "Flipper".

- [ ] **Step 1: Change the three default strings**

In `components/furi_hal/furi_hal_usb_hid_tinyusb.c`, the defaults (VID/PID are already Logitech `0x046D/0xC529`):
```c
    const char* manuf = (cfg && cfg->manuf[0]) ? cfg->manuf : "Flipper Devices Inc.";
    const char* product = (cfg && cfg->product[0]) ? cfg->product : "Flipper Zero";
```
become:
```c
    const char* manuf = (cfg && cfg->manuf[0]) ? cfg->manuf : "Logitech";
    const char* product = (cfg && cfg->product[0]) ? cfg->product : "USB Receiver";
```
And the serial a few lines below:
```c
    snprintf(s_serial, sizeof(s_serial), "FZESP32");
```
becomes a neutral, non-identifying serial:
```c
    snprintf(s_serial, sizeof(s_serial), "A1B2C3D4");
```
Change nothing else (the `static const langid_desc` fix and lazy-descriptor handling stay).

- [ ] **Step 2: Build** — run the standard build (`build_hid.log`). Expected `=== BUILD OK ===`.

- [ ] **Step 3: Commit**
```bash
git add components/furi_hal/furi_hal_usb_hid_tinyusb.c
git commit -m "fix(usb): de-Flipper BadUSB HID default descriptor (Logitech identity)"
```

---

### Task 2: BLE Stealth RTC flag + default-identity override

**Files:**
- Modify: `components/furi_hal/furi_hal_rtc.h` (add the flag)
- Modify: `components/furi_hal/furi_hal_version.c` (override ble_mac + name when the flag is set)

**Interfaces:**
- Produces: `FuriHalRtcFlagBleStealth = (1 << 8)` (consumed by Task 3 and by `furi_hal_version_init`).

- [ ] **Step 1: Add the RTC flag**

In `components/furi_hal/furi_hal_rtc.h`, extend the `FuriHalRtcFlag` enum (bits 0-7 are used; add bit 8 after `FuriHalRtcFlagDetailedFilename = (1 << 7),`):
```c
    FuriHalRtcFlagBleStealth = (1 << 8),
```

- [ ] **Step 2: Override the default BLE identity at init**

In `components/furi_hal/furi_hal_version.c`, ensure these includes are present near the top (add any missing): `#include "furi_hal_rtc.h"` and `#include <stdlib.h>` (for `rand`).

In `furi_hal_version_init`, the current tail is:
```c
    const char* custom = version_get_custom_name(NULL);
    const char* effective;
    if(custom && custom[0]) {
        effective = custom;
    } else {
        effective = "mob";
    }
    furi_hal_version_refresh_names(effective);
```
Insert the stealth override immediately before `furi_hal_version_refresh_names(effective);`:
```c
    /* BLE Stealth (opt-in, RTC flag): replace the default advertising identity so
     * the device isn't a constant trackable BLE beacon. Fresh random static MAC +
     * generic name per boot; sacrifices bonding persistence, which is the point. */
    if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagBleStealth)) {
        for(int i = 0; i < 6; i++) furi_hal_version.ble_mac[i] = rand() & 0xFF;
        furi_hal_version.ble_mac[0] = (furi_hal_version.ble_mac[0] & 0x3F) | 0xC0; /* static random */
        effective = "BLE Device";
    }
    furi_hal_version_refresh_names(effective);
```
(This runs after `furi_hal_rtc_init()` per the `furi_hal_init` order, so the flag read is valid. `furi_hal_version.ble_mac` was set to the factory MAC just above; the override only fires when the flag is set.)

- [ ] **Step 3: Build** — run the standard build (`build_blestealth.log`). Expected `=== BUILD OK ===`, no undefined `furi_hal_rtc_is_flag_set` / `rand`.

- [ ] **Step 4: Commit**
```bash
git add components/furi_hal/furi_hal_rtc.h components/furi_hal/furi_hal_version.c
git commit -m "feat(ble): BLE Stealth flag — random adv MAC + generic name when enabled"
```

---

### Task 3: BLE Stealth lock-menu toggle

**Files:**
- Modify: `applications/services/desktop/views/desktop_events.h` (add the event)
- Modify: `applications/services/desktop/views/desktop_view_lock_menu.c` (add the menu item)
- Modify: `applications/services/desktop/scenes/desktop_scene_lock_menu.c` (handle the toggle)

**Interfaces:** consumes `FuriHalRtcFlagBleStealth` (Task 2). Produces a lock-menu "BLE Stealth: On/Off" item.

- [ ] **Step 1: Add the event**

In `applications/services/desktop/views/desktop_events.h`, add to the `DesktopEvent` enum near the other `DesktopLockMenuEvent*` values (e.g. after `DesktopLockMenuEventBigFapToggle,`):
```c
    DesktopLockMenuEventBleStealthToggle,
```

- [ ] **Step 2: Add the menu item**

In `applications/services/desktop/views/desktop_view_lock_menu.c`, ensure `#include <furi_hal_rtc.h>` is present near the top (add if missing). In `lock_menu_build_items`, add an item after the Bluetooth item (the `bt_on ? "Disable Bluetooth" : "Enable Bluetooth"` line):
```c
    s_items[s_item_count++] = (LockMenuItem){
        furi_hal_rtc_is_flag_set(FuriHalRtcFlagBleStealth) ? "BLE Stealth: On" : "BLE Stealth: Off",
        DesktopLockMenuEventBleStealthToggle};
```
Confirm `LOCK_MENU_MAX_ITEMS` (the `s_items` array size) is large enough for one more entry; if it is a tight exact count, bump it by 1.

- [ ] **Step 3: Handle the toggle**

In `applications/services/desktop/scenes/desktop_scene_lock_menu.c`, ensure `#include <furi_hal_rtc.h>` is present. Add a case in the custom-event switch (after the `DesktopLockMenuEventBluetoothToggle` case), mirroring the Bluetooth toggle's "flip state + refresh label" pattern:
```c
        case DesktopLockMenuEventBleStealthToggle:
            /* Toggle the persistent flag; the BLE identity is applied at boot
             * (furi_hal_version_init), so this takes effect after a reboot. */
            if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagBleStealth)) {
                furi_hal_rtc_reset_flag(FuriHalRtcFlagBleStealth);
            } else {
                furi_hal_rtc_set_flag(FuriHalRtcFlagBleStealth);
            }
            desktop_scene_lock_menu_refresh(desktop);
            consumed = true;
            break;
```
(`desktop_scene_lock_menu_refresh` is what the Qflipper/Bluetooth cases already call to rebuild the labels; the item label flips On/Off to confirm. Applies on next boot.)

- [ ] **Step 4: Build** — run the standard build (`build_lockmenu.log`). Expected `=== BUILD OK ===`, and a regenerated `Flipper-cardputer_adv-merged.bin`.

- [ ] **Step 5: Commit**
```bash
git add applications/services/desktop/views/desktop_events.h applications/services/desktop/views/desktop_view_lock_menu.c applications/services/desktop/scenes/desktop_scene_lock_menu.c
git commit -m "feat(desktop): BLE Stealth lock-menu toggle"
```

---

### Task 4: On-hardware validation

**Files:** none (manual; needs the Cardputer ADV on COM12).

- [ ] **Step 1: Flash** — `python -m esptool --chip esp32s3 --port COM12 -b 460800 write_flash 0x0 Flipper-cardputer_adv-merged.bin`
- [ ] **Step 2: BadUSB identity** — run any BadUSB payload; on the host, confirm the injected keyboard enumerates as Manufacturer "Logitech" / Product "USB Receiver" (Windows Device Manager / `dmesg`), NOT "Flipper".
- [ ] **Step 3: BLE Stealth** — lock menu shows "BLE Stealth: Off"; toggle to On (label flips), reboot. With a BLE scanner (nRF Connect), confirm the device now advertises a random static MAC (first octet high bits `C-F`) and the generic name "BLE Device" instead of "mob"; reboot again → MAC changes. Toggle Off + reboot → "mob" identity + factory MAC restored; confirm saved BLE bonds work again.
- [ ] **Step 4: Record** — `git commit --allow-empty -m "test: P1 OPSEC pass validated on Cardputer ADV hardware"`

---

## Self-Review Notes

- **Audit P1 #4 (BadUSB tells):** Task 1 — Logitech-consistent defaults; VID/PID already Logitech; per-payload override preserved. ✓
- **Audit P1 #7 (BLE identity):** Tasks 2+3 — opt-in stealth (random static MAC + generic name) via RTC flag applied at init, toggled from the lock menu; default `"mob"` + bonding intact when OFF. ✓
- **Init order:** `furi_hal_rtc_init` (furi_hal.c:52) precedes `furi_hal_version_init` (:53), so the flag read at init is valid. ✓
- **Scope kept:** qFlipper CDC / model name untouched (needed for the bridge). ✓
- **Type consistency:** `FuriHalRtcFlagBleStealth` defined in Task 2, consumed in Task 2 (version) and Task 3 (view + scene); `DesktopLockMenuEventBleStealthToggle` defined + consumed within Task 3.
