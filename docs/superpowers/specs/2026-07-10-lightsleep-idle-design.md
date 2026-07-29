# Light-Sleep Idle Manager — Design Spec

**Date:** 2026-07-10
**Status:** Approved (design confirmed). Source-informed by ElicoftZ upstream v1.16P1.

## Goal

Add configurable idle power-saving to the port: after inactivity on the
desktop/main-menu, **dim** the backlight, then enter real ESP32-S3 **light
sleep** (CPU paused, RAM retained, radios in modem-sleep) — waking instantly on
any keypress. Battery win for field use. Today `furi_hal_power_sleep()` is a
no-op stub and there is no idle light-sleep at all.

## Non-Goals (YAGNI)

- No sleep during any app/operation — light sleep is armed ONLY on the
  desktop/main-menu (see Arming Gate). This inherently protects every WiFi /
  BLE / Sub-GHz / Evil-Portal / NFC capture, which all run inside their apps.
- No deep sleep changes — the existing `furi_hal_power_shutdown()` deep-sleep
  path (power-off) is untouched.
- No per-app "allow sleep" flags, no motion/IMU wake, no auto-dim by ambient
  light. Just idle-timeout dim + light-sleep.

## Arming Gate (the safety mechanism)

Idle power-saving runs ONLY while the **desktop/main-menu view is foreground**.
Launching any application disarms it; returning to the desktop re-arms it.
Because every radio operation lives inside its own app, this single gate
guarantees light sleep can never interrupt a capture — no per-operation
checks needed. Additionally disarmed when **USB data is present**
(`furi_hal_power_is_usb_present()`) so qFlipper / USB-mass-storage / the
serial-JTAG console are never dropped by a sleep/wake cycle.

## Behavior

Two independent, configurable idle thresholds (both selectable
**Off / 30s / 1m / 2m / 5m**; defaults **dim = 30s**, **light-sleep = 2m**),
persisted in Power settings. Any key resets the idle timer.

1. **Dim stage** — at the dim threshold, lower the backlight to a low readable
   level. Any input restores the user's configured brightness and resets the
   timer.
2. **Light-sleep stage** — at the light-sleep threshold, call
   `furi_hal_power_light_sleep()` (below). It turns the panel off (SLPIN),
   backlight + RGB off, then loops `esp_light_sleep_start()` waking on the
   keyboard INT pin, the BOOT/power button, or a 15 s watchdog timer; a real
   GPIO wake (keypress/button) exits the loop, restores panel + backlight, and
   the wake keypress reaches the input stack normally. If `light-sleep = Off`,
   only dimming applies.

## Components

### 1. `furi_hal_power_light_sleep(void) -> bool`  (HAL — port from upstream)
New function in `components/furi_hal/furi_hal_power.c` (+ decl in the header),
ported near-verbatim from upstream v1.16 (validated there). Behavior:
- Return `false` immediately if `furi_hal_power_is_usb_present()`.
- Backlight → 0 (bypassing the readable-idle floor), RGB → off, panel → SLPIN.
- Enable GPIO wake on **`KB_PIN_INT`** (GPIO11, TCA8418 INT, active-low) *and*
  `BOARD_PIN_BUTTON_BOOT` (active-low) via `gpio_wakeup_enable` +
  `esp_sleep_enable_gpio_wakeup()`. (Upstream enables the BOOT button in the
  function and relies on KB_PIN_INT being wake-armed; our port will explicitly
  arm BOTH inside the function, `#ifdef KB_PIN_INT`, for self-containment.)
- Loop: `esp_sleep_enable_timer_wakeup(15s)` → `esp_light_sleep_start()`; break
  on non-timer (GPIO) wake or if start fails or USB appears; the 15 s watchdog
  bounds each chunk so the device can never get stuck asleep.
- Tear down wake sources; `furi_hal_display_wake()` (SLPOUT) +
  restore backlight. Return `true`.
Also port the `esp_timer_get_time()`-based monotonic uptime helper upstream uses
(the FreeRTOS tick freezes during light sleep; the idle manager needs a clock
that advances through sleep).

Existing `furi_hal_power_sleep()` stays (kernel idle hook) — out of scope; this
feature calls the new `furi_hal_power_light_sleep()` explicitly from the idle
manager, not the kernel path.

### 2. Idle manager (desktop integration)
Tracks time since last input while the desktop/menu is foreground and drives
the dim → light-sleep state machine, honoring the arming gate and the two
configured thresholds. Lives with the desktop app (the natural owner of the
desktop/menu foreground state and the existing idle/animation timers). It reads
the two timeout settings, watches an activity timestamp (reset on input),
applies dim, and calls `furi_hal_power_light_sleep()` at the sleep threshold;
on return, restores brightness and resets state. Uses the sleep-surviving
uptime clock for its timing.

### 3. Power-settings rows
Two rows in the existing Power settings app (`applications/settings/
power_settings_app`): **Idle Dim** and **Light Sleep**, each cycling
Off/30s/1m/2m/5m, persisted with the app's existing settings-save mechanism.
The idle manager reads these values.

## Data Flow

Input event → idle manager resets activity timestamp (and restores brightness if
dimmed/waking). Desktop foreground + no USB + elapsed ≥ dim threshold → dim.
Elapsed ≥ sleep threshold → `furi_hal_power_light_sleep()` (blocks until a
wake) → restore. Leaving the desktop (launch app) or USB-present → disarm,
restore brightness.

## Safety Invariants

- Light sleep only on desktop/menu foreground; never inside an app → no capture
  interrupted.
- Never while USB data present (console/flashing/qFlipper/mass-storage safe).
- Watchdog timer guarantees no permanent sleep even if every GPIO wake fails.
- Wake restores panel + backlight + full RAM state (light sleep, not deep
  sleep) — the device resumes exactly where it was.
- Deep-sleep power-off path unchanged.

## Testing

- **Compile gate:** `build_ci.bat` (`Project build complete` + `=== BUILD OK ===`).
- **On-hardware checklist** (GPIO-style, not host-testable): idle on desktop →
  dims at the set time; → light-sleeps at the set time (current draw drops);
  any key wakes instantly and restores brightness; launching an app disables
  idle sleep (verify a WiFi/BLE capture left running past the timeout is NOT
  interrupted); USB-connected disables sleep; "Off" disables each stage;
  settings persist across reboot.

## Board Notes

- ADV: `KB_PIN_INT = 11` (TCA8418 INT) confirmed present — GPIO-INT wake works.
- Standard board (74HC138 matrix) may lack a single keyboard-INT wake line; the
  BOOT-button + 15 s watchdog still wake it, and the plan will confirm the
  Standard path (dim always works; light-sleep degrades gracefully to
  watchdog-bounded if no KB INT). The ADV is the primary target.

## Open Items (resolved during planning)

1. Exact desktop-app hook point for the idle manager (existing idle/animation
   timer vs a dedicated timer) — pick during planning by reading the desktop
   app's current idle handling.
2. Confirm our `furi_hal_light`/`furi_hal_display` expose the symbols upstream's
   function uses (`furi_hal_light_set`, `furi_hal_light_set_rgb_all`,
   `furi_hal_display_sleep/wake`, `furi_hal_display_set_backlight`); adapt names
   if they differ.
3. Standard-board light-sleep wake path (KB INT availability) — confirm from
   `board_m5stack_cardputer.h`.
