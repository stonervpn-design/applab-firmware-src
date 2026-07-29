# GPIO Manual Control — Design Spec

**Date:** 2026-07-09
**Status:** Approved (approach A), pending spec review.

## Goal

Replace the GPIO app's output-only "Manual Control" (the current `gpio_test`
scene, which force-configures every pin as push-pull output and toggles the
selected one) with a proper Flipper-style **per-pin controller**: pick a pin,
choose its **mode** (Output / Input / PWM / ADC), set mode-specific parameters,
and see live readings — scoped safely to pins that are not owned by an active
peripheral, with an explicit "advanced" unlock to reach the shared/peripheral
pins at the user's own risk.

## Non-Goals (YAGNI)

- No peripheral **pin remapping** (assigning which GPIO CC1101/NRF24/IR use) —
  that was explicitly not what was wanted; this is raw pin driving only.
- No persistence of pin state across app exit — pins reset to a safe (high-Z)
  state on leaving Manual Control, matching Flipper behaviour.
- No I2C/SPI/UART protocol tooling on the pins (the USB-UART bridge already
  exists and is untouched).
- No changes to the WiFi/BLE/Sub-GHz/NRF24 apps or any OPSEC/concurrency work.

## Hardware Reality (why the pin list is small)

On the Cardputer **ADV**, almost every GPIO is claimed (from
`components/furi_hal/boards/board_m5stack_cardputer_adv.h`):

- LCD: 33,34,35,36,37,38   SD (SPI3): 12,14,39,40
- CC1101: 5,13,14,39,40    NRF24: 4,6,14,39,40
- IR: 1 (RX), 44 (TX)      I2S/Speaker/Mic: 0,41,42,43,46
- RGB WS2812: 21           Battery ADC: 10   Boot: 0
- Grove / QWIIC: SDA=2, SCL=1  (note: SCL shares GPIO1 with IR-RX)
- USB native: 19,20        Flash/PSRAM region: 26–32 (reserved)

So there is **no clean bank of free header pins** like a real Flipper. The
physically broken-out, user-reachable pins are the **Grove/HY2.0 port** (GPIO 1
and GPIO 2, plus 5V/GND). GPIO 2 (QWIIC SDA) and GPIO 1 (QWIIC SCL / IR-RX) are
"conditionally free" — usable only when I2C/IR-RX are idle. A handful of other
GPIOs (candidates: 3, 7, 8, 9, 11, 15–18, 47, 48) are electrically unclaimed but
**not broken out** on the standard chassis, so they are ADVANCED-tier only.

**Consequence:** the SAFE tier will be short (Grove pins, conditionally). This is
inherent to the hardware, not a design shortcut. The exact SAFE list is finalized
in the implementation plan after confirming physical break-out per board.

## Architecture (Approach A — rework in place)

Keep the GPIO app shell (`gpio_app.c`, `gpio_scene_start`, USB-UART bridge
scenes) unchanged. Replace the `gpio_test` scene/view with a two-view Manual
Control backed by a pin catalog and a driver layer. Five focused units:

### 1. `gpio_pin_catalog.{c,h}` — the exposed-pin table
Board-aware, compile-time table. Each entry:
```c
typedef enum { GpioTierSafe, GpioTierAdvanced } GpioTier;
typedef enum {                 // capability bitmask
    GpioCapOutput = 1<<0, GpioCapInput = 1<<1,
    GpioCapPwm    = 1<<2, GpioCapAdc   = 1<<3,
} GpioCap;
typedef struct {
    const char* name;          // e.g. "GPIO2 (Grove SDA)"
    uint8_t     gpio;          // ESP32-S3 GPIO number
    GpioTier    tier;          // Safe or Advanced
    uint8_t     caps;          // OR of GpioCap
    const char* warn;          // NULL, or "shared with CC1101 — disable Sub-GHz"
} GpioCatalogEntry;
```
Rules baked into the table:
- SAFE tier: only broken-out, non-peripheral pins. Never a pin owned by an
  active peripheral.
- ADC cap set only on **ADC1-capable** GPIOs (1–10) — ADC2 is unusable with
  WiFi and is never offered.
- PWM cap set on any output-capable pin (LEDC can route to any output GPIO).
- Peripheral/shared pins are ADVANCED with a non-NULL `warn`.
- Table selected per board via the existing `BOARD_*` compile macros
  (Standard vs ADV differ).

### 2. `gpio_driver.{c,h}` — hardware apply/read/teardown
The only unit that touches ESP-IDF peripheral APIs. Interface:
```c
bool  gpio_driver_set_output(uint8_t gpio, bool level);
bool  gpio_driver_set_input(uint8_t gpio, GpioDriverPull pull); // up/down/none
bool  gpio_driver_read_digital(uint8_t gpio);                   // live level
bool  gpio_driver_set_pwm(uint8_t gpio, uint32_t freq_hz, uint8_t duty_pct);
bool  gpio_driver_read_adc(uint8_t gpio, uint32_t* out_mv);     // ADC1 oneshot
void  gpio_driver_release(uint8_t gpio);   // this pin -> high-Z, free LEDC/ADC
void  gpio_driver_release_all(void);       // teardown on scene exit
```
- Digital in/out + pull via `furi_hal_gpio`.
- PWM via **LEDC**: allocates one of 8 timer/channel pairs per active PWM pin;
  `set_pwm` returns false if all channels are in use (UI shows "no free PWM
  channel"). `release` frees the channel.
- ADC via **ADC1 oneshot** driver with calibration → millivolts.
- Tracks the set of pins it has touched so `release_all` can restore every one
  to high-Z (`GpioModeAnalog`) and tear down LEDC/ADC handles.

### 3. `views/gpio_manual_list.{c,h}` — pin list
Scrollable list. Row 0 is the **"Advanced: show all pins"** toggle (off by
default; turning it on shows a one-time confirm + reveals ADVANCED entries with a
`[!]`). Remaining rows: one per catalog pin in the active tier set, each showing
name + a one-line live state:
`[OUT] HIGH` · `[IN↑] 1` · `[PWM] 1kHz 50%` · `[ADC] 1.83V` · `[--] off`.
Polls `gpio_driver` on a timer tick to keep readings live. OK on a pin → edit
view.

### 4. `views/gpio_manual_edit.{c,h}` — per-pin editor
A `VariableItemList`. First row: **Mode** selector, filtered to the pin's caps
(so an ADC-incapable pin never shows ADC). Then mode-specific rows:
- Output → **Level** (LOW/HIGH).
- Input → **Pull** (None/Up/Down) + a live **Read** row (0/1).
- PWM → **Freq** (stepped: 1/10/100/1k/5k/10k Hz) + **Duty** (0–100%, 5% steps).
- ADC → live **Voltage** row (mV→V), read-only.
Every change calls the matching `gpio_driver` setter immediately (live control).
A first-entry warning banner appears for ADVANCED pins with a `warn` string.

### 5. `scenes/gpio_scene_manual.c` (+ its edit sub-scene)
Replaces `gpio_scene_test.c`. Reuses the existing `gpio_scene_start` menu entry
("GPIO Manual Control" already dispatches `GpioStartEventManualControl`). Owns
the two views, the poll timer, and calls `gpio_driver_release_all()` in
`on_exit`. The old `gpio_test` view/scene files are removed.

## Data Flow

Start menu → Manual Control → **list view** (timer tick → `gpio_driver` live
reads → redraw) → OK on pin → **edit view** → user changes mode/param →
`gpio_driver` applies to hardware at once → Back → list. Leaving Manual Control →
`on_exit` → `gpio_driver_release_all()` → every touched pin high-Z, LEDC/ADC
freed.

## Safety

- SAFE default cannot select any active-peripheral pin.
- ADVANCED unlock is explicit (toggle + confirm) and every shared pin shows its
  `warn` ("shared with CC1101/SD/… — disable that feature first").
- Guaranteed safe teardown on exit (no pin left driven).
- LEDC channel exhaustion and ADC-on-non-ADC1 are refused gracefully, not
  crashed.
- Driving a pin the user physically shorted is on them; firmware never *forces*
  a peripheral pin without the advanced opt-in.

## Testing

- **Compile gate:** `build_ci.bat` (`Project build complete` + `=== BUILD OK ===`).
- **On-device manual checklist** (in the plan, since GPIO is not host-testable):
  drive an LED HIGH/LOW on a SAFE pin; read a button with pull-up; scope/meter a
  PWM pin at a set freq/duty; read a known voltage divider on an ADC1 pin; verify
  all pins go high-Z after leaving the app; verify SAFE tier hides peripheral
  pins and the advanced toggle reveals them with the warning.

## Open Items (resolved during planning)

1. **Final SAFE pin list** per board — enumerated from the board map and
   confirmed against physical break-out (Grove port + any exposed header).
   Preliminary SAFE candidates (ADV): GPIO2 (Grove SDA), GPIO1 (Grove SCL /
   IR-RX) — both conditionally free; everything else ADVANCED.
2. Whether the **Standard** board exposes any different free pins (its keyboard/
   audio wiring differs) — confirm from `board_m5stack_cardputer.h`.
3. LEDC timer/channel allocation strategy (shared timer vs per-pin) — settle in
   the plan; 8 channels is the hard cap surfaced to the UI.
