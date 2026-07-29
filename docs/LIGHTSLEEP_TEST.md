# Light Sleep Idle-Power Feature: Hardware Test Checklist

**Status:** NOT yet hardware-validated — requires a physical Cardputer ADV.

This checklist covers end-to-end testing of the idle backlight dim and light-sleep power-saving features on the Flipper Zero M5Stack Cardputer port.

## Basic Behavior

### Idle Backlight Dimming
- [ ] **Desktop Idle Dimming:** On the desktop (lock menu / home screen), leave the device idle without pressing any key. Verify that the backlight dims at the configured Idle-Dim time (default 30 seconds).
- [ ] **Dim Persistence:** The dimmed state persists until a key is pressed.
- [ ] **Key Wake Restores Brightness:** Any key press instantly wakes the device and restores full backlight brightness.

### Light Sleep Mode
- [ ] **Desktop Light Sleep:** On the desktop, leave the device idle without pressing any key past the configured Light-Sleep timeout (default 2 minutes). Verify the device enters light sleep.
- [ ] **Current Draw Drop:** Using a current meter or power analyzer on the Cardputer's power rails, confirm a measurable drop in current draw when light sleep is active (device should draw significantly less power than full operation).
- [ ] **Instant Wake on Key:** Press any key (including letter keys, arrow keys, spacebar, etc.). The device must wake instantly and restore full brightness and operation.
- [ ] **BOOT Button Wake:** Press the physical BOOT/power button (or ADV reset button if mapped). The device must wake and return to the desktop/previous state.

## Settings Persistence

### Power Settings App Configuration
- [ ] **Idle Dim Option:** Open Settings → Power and verify an "Idle Dim" row is present.
- [ ] **Light Sleep Option:** Verify a "Light Sleep" row is present.
- [ ] **Idle Dim Cycling:** Select the Idle Dim row and cycle through options: OFF → 30s → 1m → 2m → 5m → OFF. Verify each option displays and can be selected.
- [ ] **Light Sleep Cycling:** Select the Light Sleep row and cycle through options: OFF → 30s → 1m → 2m → 5m → OFF. Verify each option displays and can be selected.
- [ ] **Idle Dim OFF Disables Dimming:** Set Idle Dim to OFF. Leave the device idle on the desktop past the normal dim time. Verify the backlight does NOT dim.
- [ ] **Light Sleep OFF Disables Sleep:** Set Light Sleep to OFF (with Idle Dim enabled). Leave the device idle past the normal light-sleep time. Verify the device enters idle dim but NOT light sleep (backlight dims but device remains fully powered).
- [ ] **Settings Persist Across Reboot:** Change Idle Dim and Light Sleep to custom values (e.g., Idle Dim=1m, Light Sleep=5m). Power off and reboot the device. Verify that the settings are restored to their saved values.

## Safety Gates

### App Launch Disables Idle Sleep
- [ ] **WiFi Handshake Capture:** Open the WiFi app and start a handshake capture (e.g., connect to a WiFi network and run the handshake sniffer). Leave the device idle with the capture running past the configured Light-Sleep timeout. Verify that the device does NOT dim, does NOT enter light sleep, and the WiFi capture continues uninterrupted.
- [ ] **Any App Launch Disables Sleep:** Launch another app (e.g., Sub-GHz Receiver, NFC reader, Bad USB). Leave it idle past the light-sleep timeout. Verify no dimming or sleep occurs and the app continues to function normally.

### Mesh / Buddy Capture Disables Idle Sleep
- [ ] **Mesh Clients Lock Menu:** On the desktop, open the Lock Menu and navigate to "Mesh Clients" (ESP-NOW master mode, if buddies are configured). Leave this menu idle past the configured Light-Sleep timeout. Verify the device does NOT dim, does NOT enter light sleep, and any incoming buddy handshakes or mesh activity still arrives and is processed normally (confirm with a "Handshake received" overlay or buddy status indicator).
- [ ] **Passive Mesh Activity:** With a paired buddy actively sending WiFi handshakes or mesh data, confirm that the master device remains awake and does not light-sleep, even when the UI is idle.

### Bluetooth Disables Idle Sleep
- [ ] **Bluetooth Enabled + Phone Connected:** Enable Bluetooth (Lock Menu BT toggle or Settings → Bluetooth), connect a phone / BLE central, then leave the device idle on the desktop past the configured Light-Sleep timeout. Verify the device does NOT dim, does NOT enter light sleep, and the BLE link stays connected (no drop, RPC-over-BLE session survives).
- [ ] **Bluetooth Enabled (Advertising Only):** Enable Bluetooth but leave it advertising (no central connected). Leave the device idle past the timeout. Verify it still does NOT light-sleep (the gate is conservative: BT enabled at all disarms sleep). Disable Bluetooth and confirm idle sleep resumes normally.

### USB Connection Disables Idle Sleep
- [ ] **USB Power / qFlipper:** Connect the device to a PC via USB (either qFlipper connection or USB charging cable with data lines). Leave the device idle on the desktop past the configured Light-Sleep timeout. Verify the device does NOT enter light sleep (USB prevents sleep, though dimming may still occur depending on configuration).
- [ ] **USB Charging Cable:** Disconnect and reconnect a USB charging cable (with data conductors), ensuring the device recognizes the USB connection. Verify that idle sleep is disabled as long as USB is connected.

## Wake Behavior & State Preservation

### Wake Restores Full State
- [ ] **Desktop Wake:** Leave the device on the desktop in light sleep (waiting in dimmed/low-power state). Press a key to wake. Verify the device returns immediately with full brightness, and the desktop is exactly as it was (no reboot, no state loss).
- [ ] **App State Wake:** While an idle-sleep-incompatible app (e.g., a menu scene) was running before sleep, confirm the device wakes in the exact same app/menu.
- [ ] **RAM Retained (Not Deep Sleep):** Verify wake is instantaneous (sub-second) and the device is not rebooting. Light sleep should retain RAM; deep sleep would show a boot animation.

## Standard (Non-ADV) Board Specific Tests

### Dimming Works on Standard
- [ ] **Standard Cardputer Dimming:** On a Standard M5Stack Cardputer (no ADV features), verify that idle backlight dimming functions correctly at the configured Idle-Dim timeout.

### Light Sleep Wake via BOOT and Watchdog
- [ ] **BOOT Button Wake on Standard:** On the Standard Cardputer, enter light sleep and press the BOOT button. Verify the device wakes.
- [ ] **Watchdog Wake Fallback:** If the Standard Cardputer's keyboard-INT wake line does not function as expected (hardware variation), confirm that the 15-second watchdog timer wakes the device even without a key press (watchdog serves as a safety fallback).

## Edge Cases & Cleanup

### Settings Reset
- [ ] **Factory Reset:** Perform a factory reset from Settings → System Info → Factory Reset. After reboot, verify that Idle Dim and Light Sleep are restored to their default values.

### Power Cycling
- [ ] **Power Off While Idle:** Leave the device idle and enter light sleep. While in light sleep, perform a power cycle (toggle power switch). Verify the device boots normally and no corruption occurs.

### Rapid Key Presses
- [ ] **Multiple Rapid Wakes:** While the device is in light sleep, press a key, then immediately sleep again by going idle. Repeat 10 times. Verify no hangs, crashes, or stuck states.

---

## Test Results Summary

| Test Case | Status | Notes |
|-----------|--------|-------|
| Idle dimming at configured time | ☐ PASS ☐ FAIL | |
| Backlight dim measurable current drop | ☐ PASS ☐ FAIL | |
| Any key wakes and restores brightness | ☐ PASS ☐ FAIL | |
| BOOT button wakes | ☐ PASS ☐ FAIL | |
| Settings: Idle Dim cycles OFF/30s/1m/2m/5m | ☐ PASS ☐ FAIL | |
| Settings: Light Sleep cycles OFF/30s/1m/2m/5m | ☐ PASS ☐ FAIL | |
| Settings persist across reboot | ☐ PASS ☐ FAIL | |
| WiFi capture disables idle sleep | ☐ PASS ☐ FAIL | |
| Mesh Clients menu disables idle sleep | ☐ PASS ☐ FAIL | |
| Bluetooth enabled disables idle sleep (link not dropped) | ☐ PASS ☐ FAIL | |
| USB connection disables idle sleep | ☐ PASS ☐ FAIL | |
| Wake preserves full state (no reboot) | ☐ PASS ☐ FAIL | |
| Standard board dimming works | ☐ PASS ☐ FAIL | |
| Standard board BOOT button wake works | ☐ PASS ☐ FAIL | |

---

**Notes:**
- Idle sleep is only active on the desktop/lock menu. Apps, mesh captures, an enabled Bluetooth radio, and USB connections prevent sleep.
- Light sleep uses ESP-IDF modem sleep to suspend the CPU while maintaining RAM and basic wake capability.
- Current measurements should ideally be taken at the device's power input (e.g., USB 5V line or battery).
- Watchdog timer (15s default) serves as a safety fallback on boards with limited wake-line support.
