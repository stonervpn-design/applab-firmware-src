# GPIO Manual Control Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the GPIO app's output-only "Manual Control" (`gpio_test`) with a Flipper-style per-pin controller: choose a pin's mode (Output / Input / PWM / ADC), set params, see live readings — SAFE-tier pins by default with an explicit Advanced unlock for peripheral/shared pins, and all pins reset to high-Z on exit.

**Architecture:** Two new data/driver units in the app dir (`gpio_pin_catalog`, `gpio_driver`) plus two views (`gpio_manual_list`, `gpio_manual_edit`) and a scene pair (`gpio_scene_manual`, `gpio_scene_manual_edit`) that replace `gpio_scene_test` + `views/gpio_test`. The driver is the only unit that touches hardware. Staged so **digital I/O is fully working and shippable after Task 3**; PWM (Task 4) and ADC (Task 5) layer on.

**Tech Stack:** ESP-IDF v5.4.1 (ESP32-S3), C. Flipper app framework (scenes, views, VariableItemList). PWM via ESP-IDF **LEDC** (`driver/ledc.h`); ADC via **ADC1 oneshot** (`esp_adc/adc_oneshot.h` + `esp_adc/adc_cali.h`). Firmware build only.

## Global Constraints

- **Build** (synchronous, never backgrounded, timeout 600000 ms), from Git Bash:
  `MSYS_NO_PATHCONV=1 cmd.exe /c "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_ci.bat" > build_<tag>.log 2>&1; echo "EXIT=$?"`
  Success = tail shows `Project build complete` + `=== BUILD OK ===`, EXIT=0. Do NOT delete `build_cardputer_adv/` (incremental). `build_*.log` is gitignored.
- **Apps compile into the firmware** and MAY include ESP-IDF headers directly (precedent: `applications/main/wlan_app` includes `<esp_wifi.h>`, `<lwip/sockets.h>`). Sources in an app dir are auto-globbed by the FAP build — new `.c` files are picked up with no manifest edit; deleted files drop out automatically.
- **Do NOT use `furi_hal_pwm` or `furi_hal_adc`** — both are unimplemented stubs on this port (`furi_hal_pwm.h` has only legacy STM32 IDs and no `.c`; there is no `furi_hal_adc.c`). Use ESP-IDF LEDC and ADC1-oneshot directly inside `gpio_driver.c`.
- **LEDC ownership:** the LCD backlight owns `LEDC_TIMER_0` + `LEDC_CHANNEL_0` (`components/furi_hal/furi_hal_light.c`). The RGB LED uses RMT, not LEDC. PWM here MUST use `LEDC_TIMER_1` and `LEDC_CHANNEL_1..7` only. Use `LEDC_LOW_SPEED_MODE`, `LEDC_TIMER_10_BIT` duty resolution.
- **ADC:** ADC1 only. On ESP32-S3, ADC1 channel N = GPIO(N+1), i.e. GPIO1→ch0 … GPIO10→ch9. Never ADC2 (unusable with WiFi active).
- **Driving a raw pin:** on this port `GpioPin` is `{ void* port /*NULL*/, uint16_t pin /*gpio number*/ }`. Construct `GpioPin gp = {.port = NULL, .pin = gpio};` and use `furi_hal_gpio_init(&gp, mode, pull, GpioSpeedVeryHigh)`, `furi_hal_gpio_write(&gp, level)`, `furi_hal_gpio_read(&gp)`. Modes: `GpioModeOutputPushPull`, `GpioModeInput`, `GpioModeAnalog` (high-Z). Pulls: `GpioPullNo/Up/Down`.
- **Safety:** SAFE tier never contains an active-peripheral pin. Advanced unlock is explicit. Every touched pin returns to high-Z (`GpioModeAnalog`) on scene exit; LEDC channels stopped, ADC handles released.
- Repo is git on `master`; commit per task with the message shown. Leave the 4 untracked `build_manual*.sh`/`*.bat` helper scripts alone.

---

### Task 1: Pin catalog (`gpio_pin_catalog.{c,h}`)

**Files:**
- Create: `applications/main/gpio/gpio_pin_catalog.h`
- Create: `applications/main/gpio/gpio_pin_catalog.c`

**Interfaces:**
- Produces:
  ```c
  typedef enum { GpioTierSafe, GpioTierAdvanced } GpioTier;
  typedef enum {
      GpioCapOutput = 1 << 0, GpioCapInput = 1 << 1,
      GpioCapPwm    = 1 << 2, GpioCapAdc   = 1 << 3,
  } GpioCap;
  typedef struct {
      const char* name;   // e.g. "GPIO2 (Grove SDA)"
      uint8_t     gpio;   // ESP32-S3 GPIO number
      int8_t      adc_ch; // ADC1 channel, or -1 if not ADC-capable
      GpioTier    tier;
      uint8_t     caps;   // OR of GpioCap
      const char* warn;   // NULL, or shared-peripheral warning
  } GpioCatalogEntry;

  // Count of entries visible for the given advanced state.
  size_t gpio_catalog_count(bool include_advanced);
  // i-th visible entry (SAFE entries first, then ADVANCED when included).
  const GpioCatalogEntry* gpio_catalog_get(size_t i, bool include_advanced);
  ```

- [ ] **Step 1: Write the header** `gpio_pin_catalog.h` with the enums, struct, and the two function declarations above (wrapped in `#pragma once` + `extern "C"` guard matching sibling headers like `gpio_items.h`).

- [ ] **Step 2: Write `gpio_pin_catalog.c`** — a static const array of entries, board-gated with the existing target macros. Read `components/furi_hal/boards/board_m5stack_cardputer_adv.h` and `board_m5stack_cardputer.h` and the board-select macro used elsewhere (grep `#if defined` in `components/furi_hal/boards/board.h` to find the ADV vs Standard switch; mirror that guard). ADV table:

  ```c
  #include "gpio_pin_catalog.h"

  /* SAFE: only broken-out, non-peripheral pins. On the ADV the physically
   * reachable free pins are the Grove/HY2.0 port (GPIO1/GPIO2). Both are
   * ADC1-capable (ch0/ch1). They are also QWIIC I2C / IR-RX when those are in
   * use, so they carry a soft note but remain SAFE (user opts into wiring). */
  static const GpioCatalogEntry k_catalog[] = {
      /* --- SAFE --- */
      { "GPIO2 (Grove SDA)", 2, 1, GpioTierSafe,
        GpioCapOutput | GpioCapInput | GpioCapPwm | GpioCapAdc, NULL },
      { "GPIO1 (Grove SCL)", 1, 0, GpioTierSafe,
        GpioCapOutput | GpioCapInput | GpioCapPwm | GpioCapAdc,
        "also IR-RX / QWIIC SCL" },
      /* --- ADVANCED (shared with peripherals — warn) --- */
      { "GPIO44 (IR TX)",    44, -1, GpioTierAdvanced,
        GpioCapOutput | GpioCapInput | GpioCapPwm, "IR transmitter pin" },
      { "GPIO13 (CC1101 CS)",13, -1, GpioTierAdvanced,
        GpioCapOutput | GpioCapInput | GpioCapPwm, "shared with CC1101 - disable Sub-GHz" },
      { "GPIO5 (CC1101 G0)",  5, -1, GpioTierAdvanced,
        GpioCapOutput | GpioCapInput | GpioCapPwm, "shared with CC1101 - disable Sub-GHz" },
      { "GPIO4 (NRF24 CE)",   4, -1, GpioTierAdvanced,
        GpioCapOutput | GpioCapInput | GpioCapPwm, "shared with NRF24" },
      { "GPIO6 (NRF24 CSN)",  6, -1, GpioTierAdvanced,
        GpioCapOutput | GpioCapInput | GpioCapPwm, "shared with NRF24" },
      { "GPIO21 (RGB LED)",  21, -1, GpioTierAdvanced,
        GpioCapOutput | GpioCapInput | GpioCapPwm, "shared with RGB LED" },
  };
  ```
  Note: keep ADC-capable (`adc_ch >= 0`) only for GPIO1–10. The ADVANCED CC1101/NRF24/IR/RGB pins above are >10 or not ADC1, so `adc_ch = -1` and no `GpioCapAdc`. For the **Standard** board, provide the equivalent table (same Grove pins if present; omit CC1101/NRF24 entries since Standard lacks those modules — verify against `board_m5stack_cardputer.h`).

- [ ] **Step 3: Implement the two accessors.** `gpio_catalog_count(include_advanced)` returns the number of entries whose tier is SAFE, or all entries when `include_advanced`. `gpio_catalog_get(i, include_advanced)` returns the i-th such entry with SAFE entries enumerated before ADVANCED (iterate SAFE first, then ADVANCED if included), so the list view shows SAFE pins on top. Return `NULL` for out-of-range `i`.

- [ ] **Step 4: Build**

Run: `MSYS_NO_PATHCONV=1 cmd.exe /c "C:\Users\stone\Downloads\_fzcard_work\Flipper-Zero-meets-M5Stack-Cardputer-1.15\build_ci.bat" > build_gpiot1.log 2>&1; echo "EXIT=$?"`
Expected: `Project build complete` + `=== BUILD OK ===`, EXIT=0. (The catalog is unused so far; this just verifies it compiles and links.)

- [ ] **Step 5: Commit**

```bash
git add applications/main/gpio/gpio_pin_catalog.h applications/main/gpio/gpio_pin_catalog.c
git commit -m "feat(gpio): board-aware pin catalog (safe/advanced tiers + caps)"
```

---

### Task 2: Driver — digital core (`gpio_driver.{c,h}`)

**Files:**
- Create: `applications/main/gpio/gpio_driver.h`
- Create: `applications/main/gpio/gpio_driver.c`

**Interfaces:**
- Consumes: nothing from Task 1 (keyed by raw GPIO number).
- Produces:
  ```c
  typedef enum { GpioDriverPullNone, GpioDriverPullUp, GpioDriverPullDown } GpioDriverPull;

  void gpio_driver_init(void);          // reset internal tracking
  bool gpio_driver_set_output(uint8_t gpio, bool level);
  bool gpio_driver_set_input(uint8_t gpio, GpioDriverPull pull);
  bool gpio_driver_read_digital(uint8_t gpio);   // live pin level
  void gpio_driver_release(uint8_t gpio);        // this pin -> high-Z (+ stop pwm/adc in later tasks)
  void gpio_driver_release_all(void);            // all touched pins -> high-Z
  ```

- [ ] **Step 1: Write `gpio_driver.h`** with the enum + declarations above, `#pragma once`, `extern "C"` guard, `#include <stdint.h>`/`<stdbool.h>`.

- [ ] **Step 2: Write `gpio_driver.c` digital implementation.** Include `<furi_hal.h>` (or `<furi_hal_gpio.h>` + `<furi_hal_resources.h>`). Maintain a touched-pin set (a `bool touched[GPIO_NUM_MAX]` static array, or a small bitmap) so `release_all` can restore only what was driven.
  ```c
  #include "gpio_driver.h"
  #include <furi_hal.h>
  #include <string.h>

  #define GPIO_MAX_NUM 49   // ESP32-S3 GPIO0..48

  static bool s_touched[GPIO_MAX_NUM];

  static GpioPin pin_of(uint8_t gpio) {
      GpioPin gp = {.port = NULL, .pin = gpio};
      return gp;
  }

  void gpio_driver_init(void) { memset(s_touched, 0, sizeof(s_touched)); }

  bool gpio_driver_set_output(uint8_t gpio, bool level) {
      if(gpio >= GPIO_MAX_NUM) return false;
      GpioPin gp = pin_of(gpio);
      furi_hal_gpio_init(&gp, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);
      furi_hal_gpio_write(&gp, level);
      s_touched[gpio] = true;
      return true;
  }

  bool gpio_driver_set_input(uint8_t gpio, GpioDriverPull pull) {
      if(gpio >= GPIO_MAX_NUM) return false;
      GpioPull p = (pull == GpioDriverPullUp) ? GpioPullUp :
                   (pull == GpioDriverPullDown) ? GpioPullDown : GpioPullNo;
      GpioPin gp = pin_of(gpio);
      furi_hal_gpio_init(&gp, GpioModeInput, p, GpioSpeedVeryHigh);
      s_touched[gpio] = true;
      return true;
  }

  bool gpio_driver_read_digital(uint8_t gpio) {
      if(gpio >= GPIO_MAX_NUM) return false;
      GpioPin gp = pin_of(gpio);
      return furi_hal_gpio_read(&gp);
  }

  void gpio_driver_release(uint8_t gpio) {
      if(gpio >= GPIO_MAX_NUM) return;
      GpioPin gp = pin_of(gpio);
      furi_hal_gpio_init(&gp, GpioModeAnalog, GpioPullNo, GpioSpeedVeryHigh);
      s_touched[gpio] = false;
  }

  void gpio_driver_release_all(void) {
      for(uint8_t g = 0; g < GPIO_MAX_NUM; g++) if(s_touched[g]) gpio_driver_release(g);
  }
  ```
  (Later tasks extend `gpio_driver_release` to also stop PWM/free ADC for that pin. Leave a `/* TASK 4/5: stop pwm / free adc here */` marker comment inside `gpio_driver_release`, above the high-Z init — this is a real extension point, not a placeholder for this task.)

- [ ] **Step 3: Build** → `build_gpiot2.log`, expect `=== BUILD OK ===`, EXIT=0.

- [ ] **Step 4: Commit**

```bash
git add applications/main/gpio/gpio_driver.h applications/main/gpio/gpio_driver.c
git commit -m "feat(gpio): pin driver digital core (in/out/pull/read/release)"
```

---

### Task 3: Manual Control UI — digital (list + edit views, scenes, wire-in, remove old test)

**Files:**
- Create: `applications/main/gpio/views/gpio_manual_list.h` / `.c`
- Create: `applications/main/gpio/views/gpio_manual_edit.h` / `.c`
- Create: `applications/main/gpio/scenes/gpio_scene_manual.c`
- Create: `applications/main/gpio/scenes/gpio_scene_manual_edit.c`
- Modify: `applications/main/gpio/scenes/gpio_scene_config.h`
- Modify: `applications/main/gpio/scenes/gpio_scene_start.c`
- Modify: `applications/main/gpio/gpio_app_i.h`
- Modify: `applications/main/gpio/gpio_app.c`
- Delete: `applications/main/gpio/scenes/gpio_scene_test.c`
- Delete: `applications/main/gpio/views/gpio_test.c` / `gpio_test.h`

**Interfaces:**
- Consumes: `gpio_pin_catalog` (Task 1), `gpio_driver` (Task 2).
- Produces: the working digital Manual Control.

Per-pin runtime state (held in the app, one struct per catalog index so the list can render live rows and the editor can mutate):
```c
typedef enum { GpioPinModeOff, GpioPinModeOutput, GpioPinModeInput,
               GpioPinModePwm, GpioPinModeAdc } GpioPinMode;   // Pwm/Adc used in Tasks 4/5
typedef struct {
    GpioPinMode    mode;      // default GpioPinModeOff
    bool           out_level; // Output
    GpioDriverPull pull;      // Input
    uint32_t       pwm_freq;  // Task 4 (default 1000)
    uint8_t        pwm_duty;  // Task 4 (default 50)
} GpioPinState;
```

- [ ] **Step 1: Scene registration.** In `scenes/gpio_scene_config.h`, replace the `test` line and add the edit scene:
  ```c
  ADD_SCENE(gpio, start, Start)
  ADD_SCENE(gpio, manual, Manual)
  ADD_SCENE(gpio, manual_edit, ManualEdit)
  ADD_SCENE(gpio, usb_uart, UsbUart)
  ADD_SCENE(gpio, usb_uart_cfg, UsbUartCfg)
  ADD_SCENE(gpio, usb_uart_close_rpc, UsbUartCloseRpc)
  ```
  (Removing `ADD_SCENE(gpio, test, Test)` retires `GpioSceneTest`.)

- [ ] **Step 2: App view enum + members.** In `gpio_app_i.h`, in the `GpioAppView` enum replace `GpioAppViewGpioTest` with:
  ```c
      GpioAppViewManualList,
      GpioAppViewManualEdit,
  ```
  Add to `struct GpioApp`: `GpioManualList* manual_list;`, `GpioManualEdit* manual_edit;`, the per-pin state array `GpioPinState pin_state[/* max catalog size */ 16];`, `bool show_advanced;`, and `size_t edit_index;` (which catalog pin the editor is on). Include the new view headers. Remove the `#include` and the `GpioTest* gpio_test;` member.

- [ ] **Step 3: App alloc/free.** In `gpio_app.c`, read the existing `gpio_test` view alloc (`gpio_test_alloc`) + `view_dispatcher_add_view(..., GpioAppViewGpioTest, ...)` and its free in app_free. Replace with allocating the two new views and adding them under `GpioAppViewManualList` / `GpioAppViewManualEdit`; free both in `gpio_app_free`. Call `gpio_driver_init()` in `gpio_app_alloc`.

- [ ] **Step 4: List view (`views/gpio_manual_list.c/.h`).** Model this on the existing `views/gpio_test.c` (same `View*` + `with_view_model` pattern). The model holds a pointer to the app's catalog-iteration context (`show_advanced`, `pin_state` array). Draw a scrollable list: row 0 = `Advanced: [OFF]/[ON]`; then one row per `gpio_catalog_get(i, show_advanced)` showing `entry->name` + a right-aligned live state string from `pin_state[i]`:
  - Off → `--`; Output → `OUT HIGH`/`OUT LOW`; Input → `IN` + live `gpio_driver_read_digital(gpio)` (`1`/`0`) + pull glyph. (PWM/ADC strings added in Tasks 4/5.)
  Up/Down move selection; OK on row 0 toggles `show_advanced` (on first enable to ON, require a second OK as confirm — show a `Reveal shared pins? OK=yes` line); OK on a pin row sets `app->edit_index` and emits a custom event to enter the edit scene. Provide a public setter `gpio_manual_list_set_context(GpioManualList*, GpioApp*)` and an OK/selection callback wired to a custom event enum value (add `GpioCustomEvent` entries in `gpio_custom_event.h`, e.g. `GpioManualEventEditPin`, `GpioManualEventToggleAdvanced`).
  Use a redraw timer (a `FuriTimer` in the scene, ~5 Hz) to refresh live input reads — see the scene step.

- [ ] **Step 5: Edit view (`views/gpio_manual_edit.c/.h`).** Use a `VariableItemList` (see `gpio_scene_usb_uart_config.c` / `gpio_scene_start.c` for the `variable_item_list_add` + change-callback pattern). Rows, filtered by the pin's caps (`app->edit_index` → `gpio_catalog_get`):
  - **Mode**: cycles Output/Input (+ PWM/ADC when those caps present — Tasks 4/5 extend the option set). On change, apply immediately: Output→`gpio_driver_set_output(gpio, level)`, Input→`gpio_driver_set_input(gpio, pull)`.
  - **Output** rows: `Level` (LOW/HIGH) → `gpio_driver_set_output`.
  - **Input** rows: `Pull` (None/Up/Down) → re-`gpio_driver_set_input`; plus a read-only `Read` row updated on the redraw tick via `gpio_driver_read_digital`.
  If the pin has a `warn`, draw it as a header/first line. Provide `gpio_manual_edit_set_context(GpioManualEdit*, GpioApp*)`; the scene refreshes it on enter with `app->edit_index`.

- [ ] **Step 6: Scenes.** `gpio_scene_manual.c`: `on_enter` → `gpio_manual_list_set_context(app->manual_list, app)`, start a ~5 Hz `FuriTimer` that calls `view` redraw (or `with_view_model` commit) so live reads update, `view_dispatcher_switch_to_view(GpioAppViewManualList)`; `on_event` → handle `GpioManualEventToggleAdvanced` (flip `app->show_advanced`) and `GpioManualEventEditPin` (`scene_manager_next_scene(GpioSceneManualEdit)`); `on_exit` → stop+free the timer, `gpio_driver_release_all()`. `gpio_scene_manual_edit.c`: `on_enter` → `gpio_manual_edit_set_context` for `app->edit_index`, switch to `GpioAppViewManualEdit`; `on_exit` → nothing special (pin keeps its state until you leave Manual Control entirely).

- [ ] **Step 7: Wire the start menu.** In `gpio_scene_start.c`, the `GpioStartEventManualControl` handler currently does `scene_manager_next_scene(app->scene_manager, GpioSceneTest)`. Change `GpioSceneTest` → `GpioSceneManual`. (The `scene_manager_set_scene_state(..., GpioSceneStart, GpioItemTest)` line: keep the enum item; it only restores menu cursor position — leave `GpioItemTest` as the index constant or rename to `GpioItemManual` consistently in the `enum GpioItem`.)

- [ ] **Step 8: Delete the old test scene/view.** Remove `scenes/gpio_scene_test.c`, `views/gpio_test.c`, `views/gpio_test.h`. Grep the app dir for any remaining `gpio_test` / `GpioSceneTest` / `GpioAppViewGpioTest` / `gpio_scene_test` references and remove/replace them (there should be none after Steps 1–3,7).

- [ ] **Step 9: Build** → `build_gpiot3.log`. Fix any dangling references until `=== BUILD OK ===`, EXIT=0.

- [ ] **Step 10: Commit**

```bash
git add applications/main/gpio/
git commit -m "feat(gpio): per-pin Manual Control (digital) — list+edit views, safe/advanced tiers"
```

*After this task, Output + Input (with pull + live read) fully work on-device; PWM/ADC are inert modes not yet offered.*

---

### Task 4: PWM mode (LEDC)

**Files:**
- Modify: `applications/main/gpio/gpio_driver.h` / `.c`
- Modify: `applications/main/gpio/views/gpio_manual_edit.c` (+ list state string)
- Modify (if build requires): `main/CMakeLists.txt`

**Interfaces:**
- Produces (added to `gpio_driver.h`):
  ```c
  bool gpio_driver_set_pwm(uint8_t gpio, uint32_t freq_hz, uint8_t duty_pct); // false if no free channel
  void gpio_driver_stop_pwm(uint8_t gpio);
  ```

- [ ] **Step 1: Driver PWM via LEDC.** In `gpio_driver.c` add `#include <driver/ledc.h>`. Maintain a channel-allocation table mapping GPIO→`ledc_channel_t` for active PWM pins, using **only** `LEDC_CHANNEL_1..LEDC_CHANNEL_7` (channel 0 = backlight). Configure `LEDC_TIMER_1` once (`LEDC_LOW_SPEED_MODE`, `LEDC_TIMER_10_BIT`, requested `freq_hz`) via `ledc_timer_config`, then `ledc_channel_config` binding the free channel to the GPIO, and set duty = `duty_pct * 1023 / 100` via `ledc_set_duty` + `ledc_update_duty`. Mirror the config-struct style in `furi_hal_light.c:98-117`. Return false if all of channels 1–7 are in use. `gpio_driver_stop_pwm(gpio)` → `ledc_stop(LEDC_LOW_SPEED_MODE, ch, 0)` and free the channel slot. Extend `gpio_driver_release(gpio)` (at the marker from Task 2) to call `gpio_driver_stop_pwm(gpio)` if that pin holds a channel. Mark the pin touched.
  *Freq note:* one shared `LEDC_TIMER_1` means all active PWM pins share the current freq; setting a new freq re-configs the timer (acceptable — document in a comment). If per-pin freq is needed later, add TIMER_2/3; not now (YAGNI).

- [ ] **Step 2: Edit view PWM rows.** In `gpio_manual_edit.c`, when the pin has `GpioCapPwm`, add `PWM` to the Mode cycle. In PWM mode show `Freq` (stepped list: 1,10,100,1000,5000,10000 Hz) and `Duty` (0–100%, 5% steps); on any change call `gpio_driver_set_pwm(gpio, freq, duty)`. If it returns false, show a `No free PWM channel` text. Switching the pin away from PWM calls `gpio_driver_stop_pwm(gpio)`. Add the list-view state string `PWM <f> <d>%`.

- [ ] **Step 3: Build** → `build_gpiot4.log`. If it fails with `driver/ledc.h` not found, add `driver` to the `REQUIRES` list in `main/CMakeLists.txt:35` (append ` driver`) and rebuild. Expect `=== BUILD OK ===`, EXIT=0.

- [ ] **Step 4: Commit**

```bash
git add applications/main/gpio/ main/CMakeLists.txt
git commit -m "feat(gpio): PWM mode via LEDC (channels 1-7, timer 1)"
```

---

### Task 5: ADC mode (ADC1 oneshot)

**Files:**
- Modify: `applications/main/gpio/gpio_driver.h` / `.c`
- Modify: `applications/main/gpio/views/gpio_manual_edit.c` (+ list state string)
- Modify (if build requires): `main/CMakeLists.txt`

**Interfaces:**
- Produces (added to `gpio_driver.h`):
  ```c
  // Reads ADC1 on the given channel; writes millivolts to *out_mv. false if unavailable.
  bool gpio_driver_read_adc(int8_t adc_ch, uint32_t* out_mv);
  ```

- [ ] **Step 1: Driver ADC via ADC1 oneshot.** In `gpio_driver.c` add `#include <esp_adc/adc_oneshot.h>` and `#include <esp_adc/adc_cali.h>`. Lazily create one `adc_oneshot_unit_handle_t` for `ADC_UNIT_1` (`adc_oneshot_new_unit`), configure the requested channel on demand (`adc_oneshot_config_channel` with `ADC_ATTEN_DB_12` for full-range ~0–3.1 V, `ADC_BITWIDTH_DEFAULT`), read raw via `adc_oneshot_read`, and convert to mV using an `adc_cali` scheme (`adc_cali_create_scheme_curve_fitting` on S3; fall back to raw*3300/4095 if calibration creation fails). Return false on any error. The channel comes from the catalog entry's `adc_ch`. Do not put the pin into a furi_hal GPIO mode for ADC — the oneshot driver owns the pad; but still record the GPIO as touched so `release_all` restores it. Add teardown of the ADC unit/cali handles to `gpio_driver_release_all` (delete unit + cali scheme if created).

- [ ] **Step 2: Edit view ADC rows.** In `gpio_manual_edit.c`, when the pin has `GpioCapAdc`, add `ADC` to the Mode cycle. In ADC mode show a read-only `Voltage` row updated on the redraw tick: call `gpio_driver_read_adc(entry->adc_ch, &mv)` and render `mv/1000 . mv%1000` as `X.XX V`. Add the list state string `ADC <X.XX>V`. ADC mode drives nothing, so switching away just needs the pin released (handled by `release_all` / mode change re-init).

- [ ] **Step 3: Build** → `build_gpiot5.log`. If it fails with `esp_adc/...` not found, add `esp_adc` to `REQUIRES` in `main/CMakeLists.txt:35` and rebuild. Expect `=== BUILD OK ===`, EXIT=0.

- [ ] **Step 4: Commit**

```bash
git add applications/main/gpio/ main/CMakeLists.txt
git commit -m "feat(gpio): ADC mode via ADC1 oneshot (voltage read)"
```

---

### Task 6: On-hardware test checklist + README note

**Files:**
- Create: `applications/main/gpio/MANUAL_CONTROL_TEST.md`
- Modify: `README.md` (the GPIO / System-Tools area)

- [ ] **Step 1: Write the manual on-hardware test checklist** `MANUAL_CONTROL_TEST.md`: (1) SAFE tier lists only Grove GPIO1/GPIO2; (2) Advanced toggle requires confirm and reveals CC1101/NRF24/IR/RGB pins with warnings; (3) Output HIGH lights an LED on GPIO2, LOW turns it off; (4) Input pull-up reads 1 idle, 0 when the pin is grounded; (5) PWM on a SAFE pin produces the set frequency/duty (scope or LED brightness), and requesting an 8th PWM pin shows "No free PWM channel"; (6) ADC on GPIO1/2 reads a known divider voltage within ~±5%; (7) leaving Manual Control returns every pin to high-Z (no residual drive); (8) Sub-GHz still works after using an Advanced CC1101 pin then leaving (i.e., release restored the bus). Mark it "not yet validated — requires physical Cardputer".

- [ ] **Step 2: README note.** Under the GPIO / System Tools section, add a short bullet: per-pin Manual Control (Output/Input+pull/PWM/ADC) with safe-by-default pin list + advanced unlock for shared pins.

- [ ] **Step 3: Commit**

```bash
git add applications/main/gpio/MANUAL_CONTROL_TEST.md README.md
git commit -m "docs(gpio): manual-control on-hardware test checklist + README note"
```

---

## Self-Review

**Spec coverage:**
- Per-pin Output/Input(+pull)/PWM/ADC → Tasks 2 (digital), 4 (PWM), 5 (ADC), surfaced in Task 3 view. ✅
- SAFE default + Advanced unlock with warnings → Task 1 (tiers/warn) + Task 3 (toggle+confirm+warn display). ✅
- Live reads (input digital, ADC voltage) → Task 3 redraw timer + Tasks 2/5 read fns. ✅
- Reset-on-exit to high-Z, LEDC/ADC teardown → Task 2 `release_all`, extended in 4/5, called in Task 3 scene `on_exit`. ✅
- Board-aware catalog, ADC1-only, LEDC-avoids-backlight → Task 1 + Global Constraints. ✅
- Replace old output-only test scene → Task 3 delete + rewire. ✅

**Placeholder scan:** the Task 2 `/* TASK 4/5 ... */` marker is a real, named extension point that Tasks 4 and 5 fill with shown code — not a vague TODO. View boilerplate references concrete sibling templates (`views/gpio_test.c`, `gpio_scene_usb_uart_config.c`) rather than restating framework boilerplate; the pin-specific logic is fully specified. No "add error handling"/"TBD" left.

**Type consistency:** `GpioCatalogEntry`/`GpioCap`/`GpioTier` (Task 1) used by Tasks 3/4/5; `GpioDriverPull`, driver fn signatures (Task 2) used verbatim in Tasks 3/4/5; `GpioPinState`/`GpioPinMode` (Task 3) carry `pwm_freq`/`pwm_duty` consumed in Task 4. ADC channel typed `int8_t adc_ch` in both the catalog and `gpio_driver_read_adc`.

**Staging:** Tasks 1→2→3 are ordered (3 depends on 1+2). Task 3 is the digital shippable milestone. Tasks 4 and 5 are independent of each other (both extend driver+edit view; if done back-to-back, the second rebases trivially — no shared new symbols beyond the Task-2/3 base). Task 6 last.

**Open risk (surface to user, not a blocker):** the SAFE tier is only the two Grove pins — inherent to the ADV hardware. If the user identifies other physically broken-out pins, add them to the Task 1 catalog before/after implementation.
