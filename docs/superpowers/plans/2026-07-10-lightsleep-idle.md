# Light-Sleep Idle Manager Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Idle power-saving on the desktop/menu: dim the backlight then enter ESP32-S3 light sleep after configurable timeouts, waking instantly on any key — never during an app/operation, never while USB is connected.

**Architecture:** Port upstream v1.16's proven `furi_hal_power_light_sleep()` (GPIO-INT + BOOT + watchdog wake) plus a new `furi_hal_display_wake()` into the HAL; add two timeout fields to the shared `power_service` `PowerSettings` (edited by the Power-settings app UI); add an idle manager to the desktop app that reads those settings and drives dim → light-sleep, gated to desktop-foreground + no-USB.

**Tech Stack:** ESP-IDF v5.4.1 (ESP32-S3), C. Flipper app/service framework. Reference source: `<scratchpad>/upstream-116/components/furi_hal/furi_hal_power.c` (upstream light_sleep, already cloned). Firmware build only.

## Global Constraints

- **Build** (synchronous, never backgrounded, timeout 600000 ms):
  `MSYS_NO_PATHCONV=1 cmd.exe /c "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_ci.bat" > build_<tag>.log 2>&1; echo "EXIT=$?"`
  Success = `Project build complete` + `=== BUILD OK ===`, EXIT=0. Don't delete `build_cardputer_adv/`. `build_*.log` gitignored.
- **Upstream reference (read-only):** `C:\Users\stone\AppData\Local\Temp\claude\C--Windows-System32\32a55737-1566-4bde-8884-a4ddaad22ce0\scratchpad\upstream-116` — the cloned ElicoftZ v1.16. Read `components/furi_hal/furi_hal_power.c` (`furi_hal_power_light_sleep` ~line 465-505, and the `esp_timer_get_time`-based uptime helper ~line 460) as the port source. Do NOT copy upstream files wholesale — our tree is heavily customized; port only the named function bodies and adapt symbol names to ours.
- **Confirmed available in our port:** `furi_hal_display_set_backlight(uint8_t)`, `furi_hal_display_sleep(void)`, `furi_hal_light_set(Light, uint8_t)`, `LightBacklight`, `furi_hal_light_set_rgb_all(r,g,b)`, `furi_hal_power_is_usb_present()` + `furi_hal_power_refresh_sample()` (both `static` in `furi_hal_power.c`), `KB_PIN_INT`=11 and `BOARD_PIN_BUTTON_BOOT`=0 (ADV board.h). **`furi_hal_display_wake()` does NOT exist — Task 1 adds it.**
- **Safety (non-negotiable):** light sleep is only ever invoked by the desktop idle manager while the desktop/menu is foreground AND `!furi_hal_power_is_usb_present()`. Never call it from an app or the kernel idle hook. The existing `furi_hal_power_sleep()` no-op stub stays untouched. The deep-sleep `furi_hal_power_shutdown()` path stays untouched.
- **Timeout values:** Off / 30s / 1m / 2m / 5m, stored as milliseconds (0 = Off). Defaults: dim=30000, light_sleep=120000.
- Repo is git on `master`; commit per task. Leave the untracked helper scripts alone.

---

### Task 1: HAL — `furi_hal_display_wake()` + `furi_hal_power_light_sleep()`

**Files:**
- Modify: `components/furi_hal/furi_hal_display.c` / `.h` (add `furi_hal_display_wake`)
- Modify: `components/furi_hal/furi_hal_power.c` / `.h` (add `furi_hal_power_light_sleep` + uptime helper)

**Interfaces:**
- Produces: `void furi_hal_display_wake(void);` and `bool furi_hal_power_light_sleep(void);` (declared in the respective headers), plus an internal monotonic-ms uptime helper usable by the idle manager (expose as `uint32_t furi_hal_power_uptime_sec(void)` in `furi_hal_power.h` — the FreeRTOS tick freezes during light sleep, so the idle manager needs a clock that advances through sleep).

- [ ] **Step 1: Add `furi_hal_display_wake()`.** Read `furi_hal_display_sleep()` in `components/furi_hal/furi_hal_display.c` (it uses `esp_lcd_panel_*`; it likely calls `esp_lcd_panel_disp_on_off(panel_handle, false)` and/or a SLPIN command). Add a symmetric `furi_hal_display_wake(void)` that reverses it (`esp_lcd_panel_disp_on_off(panel_handle, true)` / SLPOUT, plus any re-init the sleep path dropped). Declare it in `furi_hal_display.h` next to `furi_hal_display_sleep`.

- [ ] **Step 2: Add the sleep-surviving uptime helper.** In `furi_hal_power.c` add:
  ```c
  uint32_t furi_hal_power_uptime_sec(void) {
      /* esp_timer advances by the measured sleep duration on wake, so this
       * keeps counting through light sleep (unlike the frozen FreeRTOS tick). */
      return (uint32_t)(esp_timer_get_time() / 1000000LL);
  }
  ```
  Declare it in `furi_hal_power.h`. Ensure `#include <esp_timer.h>` is present.

- [ ] **Step 3: Port `furi_hal_power_light_sleep()`.** Transcribe upstream's function (from the reference `furi_hal_power.c` ~465-505) into our `furi_hal_power.c`, adapting to our symbols. The function:
  ```c
  bool furi_hal_power_light_sleep(void) {
      furi_hal_power_refresh_sample();
      if(furi_hal_power_is_usb_present()) return false;

      furi_hal_light_set(LightBacklight, 0);
      furi_hal_light_set_rgb_all(0, 0, 0);
      furi_hal_display_sleep();               /* panel SLPIN */

  #ifdef KB_PIN_INT
      gpio_wakeup_enable((gpio_num_t)KB_PIN_INT, GPIO_INTR_LOW_LEVEL);
  #endif
      gpio_wakeup_enable((gpio_num_t)BOARD_PIN_BUTTON_BOOT, GPIO_INTR_LOW_LEVEL);
      esp_sleep_enable_gpio_wakeup();

      const uint64_t LS_WATCHDOG_US = 15ULL * 1000000ULL;
      esp_sleep_wakeup_cause_t cause = ESP_SLEEP_WAKEUP_UNDEFINED;
      do {
          esp_sleep_enable_timer_wakeup(LS_WATCHDOG_US);
          if(esp_light_sleep_start() != ESP_OK) break;
          cause = esp_sleep_get_wakeup_cause();
          if(cause != ESP_SLEEP_WAKEUP_TIMER) break;   /* GPIO wake → real wake */
          furi_hal_power_refresh_sample();
          if(furi_hal_power_is_usb_present()) break;
      } while(1);

      esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  #ifdef KB_PIN_INT
      gpio_wakeup_disable((gpio_num_t)KB_PIN_INT);
  #endif
      gpio_wakeup_disable((gpio_num_t)BOARD_PIN_BUTTON_BOOT);

      furi_hal_display_wake();                /* panel SLPOUT */
      furi_hal_display_set_backlight(255);
      return true;
  }
  ```
  Note vs upstream: we explicitly `gpio_wakeup_enable(KB_PIN_INT, ...)` inside the function (upstream armed it elsewhere) for self-containment. Add includes as needed: `<esp_sleep.h>` (already present per line 15), `<driver/gpio.h>`, `<esp_timer.h>`. Declare `bool furi_hal_power_light_sleep(void);` in `furi_hal_power.h`.

- [ ] **Step 4: Build** → `build_ls1.log`, expect `=== BUILD OK ===`, EXIT=0. If `furi_hal_light_set`/`LightBacklight` or the display/panel symbols differ from the names above, grep the actual signatures in our headers and adapt (the Global Constraints list the confirmed ones).

- [ ] **Step 5: Commit**
```bash
git add components/furi_hal/furi_hal_display.c components/furi_hal/furi_hal_display.h components/furi_hal/furi_hal_power.c components/furi_hal/furi_hal_power.h
git commit -m "feat(hal): light-sleep primitive + display wake (idle power-saving)"
```

---

### Task 2: Settings — two idle timeouts in PowerSettings + Power-settings UI rows

**Files:**
- Modify: the canonical shared power settings struct — determine which of `components/power/power_service/power_settings.h` vs `applications/services/power/power_service/power_settings.h` is the real one (grep which is `#include`d by `power_settings.c`; the other may be a stale copy). Add fields to the real one + its load/save + defaults in `power_settings.c`.
- Modify: `applications/settings/power_settings_app/scenes/power_settings_scene_start.c` (add two VariableItemList rows)

**Interfaces:**
- Produces: `PowerSettings.idle_dim_delay_ms` and `PowerSettings.light_sleep_delay_ms` (uint32, ms; 0 = Off), readable by the desktop idle manager (Task 3) via the existing power-settings load path.

- [ ] **Step 1: Add the fields.** In the canonical `PowerSettings` struct add:
  ```c
  uint32_t idle_dim_delay_ms;     /* 0 = off */
  uint32_t light_sleep_delay_ms;  /* 0 = off */
  ```
  In `power_settings.c` load/save (and default init) set defaults `idle_dim_delay_ms = 30000`, `light_sleep_delay_ms = 120000` when absent (follow the existing pattern used for `auto_poweroff_delay`-style fields; ensure backward-compatible load — missing fields default, don't corrupt).

- [ ] **Step 2: Add the two UI rows.** In `power_settings_scene_start.c`, mirror the existing `auto_poweroff_delay` VariableItemList row (see `power_settings_scene_start_auto_poweroff_delay_changed`). Add a `Idle Dim` row and a `Light Sleep` row, each cycling the value list `{Off, 30s, 1m, 2m, 5m}` ↔ `{0, 30000, 60000, 120000, 300000}` ms, with a `..._changed` callback that writes the chosen ms into the PowerSettings field and saves. Add a small `const uint32_t idle_delay_values[]` + matching `const char* idle_delay_text[]` for the label mapping, and a helper to map a stored ms back to the nearest index on enter.

- [ ] **Step 3: Build** → `build_ls2.log`, expect `=== BUILD OK ===`, EXIT=0.

- [ ] **Step 4: Commit**
```bash
git add applications/services/power/ components/power/ applications/settings/power_settings_app/
git commit -m "feat(power): idle-dim + light-sleep timeout settings"
```

---

### Task 3: Desktop idle manager (dim → light-sleep, gated)

**Files:**
- Modify: `applications/services/desktop/desktop.c` / the desktop struct header
- (Read: `power_settings.h` to load the timeouts.)

**Interfaces:**
- Consumes: `furi_hal_power_light_sleep()`, `furi_hal_power_uptime_sec()`, `furi_hal_display_set_backlight()` (Task 1); `PowerSettings.idle_dim_delay_ms` / `light_sleep_delay_ms` (Task 2).

**Design:** Mirror the existing `auto_lock_timer` pattern (`desktop_start_auto_lock_timer`, reset in `desktop_input_event_callback` ~line 315-318). Add a periodic idle-check timer (e.g. 1 s tick) plus an activity timestamp taken from `furi_hal_power_uptime_sec()`.

- [ ] **Step 1: Desktop struct additions.** Add to the `Desktop` struct: `FuriTimer* idle_timer;`, `uint32_t last_activity_sec;`, `bool idle_dimmed;`, `uint32_t idle_dim_delay_ms;`, `uint32_t light_sleep_delay_ms;` (the last two cached from PowerSettings). Load the two delays from PowerSettings in `desktop` alloc/init (via the power-settings load path), alongside the existing `desktop_settings_load`.

- [ ] **Step 2: Activity reset.** In `desktop_input_event_callback` (the input hook that already resets auto-lock), also set `desktop->last_activity_sec = furi_hal_power_uptime_sec();` and, if `desktop->idle_dimmed`, restore the backlight (`furi_hal_display_set_backlight(<configured brightness / 255>)`) and clear `idle_dimmed`.

- [ ] **Step 3: Idle tick + state machine.** Start a 1 s `idle_timer` in desktop alloc; its callback (must run on the desktop thread — post a message/flag if the timer fires on the timer task, matching how `auto_lock_timer_callback` hands off) evaluates:
  - **Arming gate:** proceed only if the desktop/main-menu is the foreground view (reuse the desktop's own notion of "is the desktop showing" — the auto-lock logic already predicates on desktop being active; mirror that condition) AND `!furi_hal_power_is_usb_present()`. If not armed, restore brightness if dimmed and return.
  - `elapsed_ms = (furi_hal_power_uptime_sec() - last_activity_sec) * 1000`.
  - If `light_sleep_delay_ms != 0 && elapsed_ms >= light_sleep_delay_ms`: call `furi_hal_power_light_sleep()` (blocks until wake); on return set `last_activity_sec = uptime`, clear `idle_dimmed`, restore backlight. (The wake keypress also flows to input normally.)
  - Else if `idle_dim_delay_ms != 0 && elapsed_ms >= idle_dim_delay_ms && !idle_dimmed`: `furi_hal_display_set_backlight(<~15% e.g. 38>)`, set `idle_dimmed = true`.
  - Re-check the light-sleep condition each tick even while dimmed.

- [ ] **Step 4: Re-load settings on change / desktop enter.** When the desktop regains foreground (returning from an app) reload the two delays from PowerSettings (cheap) so a settings change takes effect without reboot; and reset `last_activity_sec`. Stop/disarm the idle behavior (and restore brightness) whenever leaving the desktop.

- [ ] **Step 5: Teardown.** Stop + free `idle_timer` in `desktop_free`; ensure brightness is restored if dimmed at teardown.

- [ ] **Step 6: Build** → `build_ls3.log`, expect `=== BUILD OK ===`, EXIT=0.

- [ ] **Step 7: Commit**
```bash
git add applications/services/desktop/
git commit -m "feat(desktop): idle manager — dim then light-sleep on desktop, gated by app/USB"
```

---

### Task 4: Docs — on-hardware test checklist + README note

**Files:**
- Create: `docs/LIGHTSLEEP_TEST.md`
- Modify: `README.md`

- [ ] **Step 1: Test checklist** `docs/LIGHTSLEEP_TEST.md`: idle on desktop dims at the set time; light-sleeps at the set time (measure current drop); ANY key wakes instantly and restores brightness; BOOT button wakes; launching an app (e.g. WiFi handshake capture) and leaving it idle past the timeout does NOT sleep/interrupt the capture; USB-connected disables sleep entirely; "Off" disables each stage independently; settings persist across reboot; on the Standard board dim works and light-sleep at least watchdog-wakes. Mark "not yet hardware-validated — requires physical Cardputer ADV."

- [ ] **Step 2: README note** under Settings/Power: configurable idle dim + light-sleep (desktop/menu only, wakes on any key, disabled during apps and while USB-connected).

- [ ] **Step 3: Commit**
```bash
git add docs/LIGHTSLEEP_TEST.md README.md
git commit -m "docs: light-sleep idle test checklist + README note"
```

---

## Self-Review

**Spec coverage:** dim + light-sleep with configurable timeouts → Tasks 1-3; desktop/menu-only arming + USB gate → Task 3 Step 3 + Task 1 (USB check inside `light_sleep`); wake on key/BOOT/watchdog → Task 1; settings in Power app → Task 2; deep-sleep/kernel-idle untouched → Global Constraints. ✅

**Placeholder scan:** the "determine canonical power_settings.h" (Task 2) and "mirror the desktop's is-foreground condition" / "hand off timer to desktop thread like auto_lock" (Task 3) are concrete read-then-adapt instructions against named existing code, not vague TODOs. The `furi_hal_power_light_sleep` body is given verbatim.

**Type consistency:** `idle_dim_delay_ms`/`light_sleep_delay_ms` (uint32 ms, 0=off) defined in Task 2, consumed in Task 3; `furi_hal_power_light_sleep`/`furi_hal_display_wake`/`furi_hal_power_uptime_sec` signatures defined in Task 1, used in Task 3.

**Ordering:** Task 1 (HAL) → Task 2 (settings) → Task 3 (integration, depends on 1+2) → Task 4 (docs). Tasks 1 and 2 are independent of each other.

**Risk to surface:** light-sleep with WiFi/BLE radios enabled on the desktop relies on ESP-IDF modem-sleep; if a background radio is active on the desktop (e.g. BLE for a lock-menu feature) `esp_light_sleep_start()` may return non-OK — the loop's `break` on non-OK handles this gracefully (no sleep, no hang). Confirm on hardware.
