# BMI270 Tilt-Nav (Left/Right) — M5Stick S3 — Design Spec

**Date:** 2026-07-28
**Board:** `m5stick_s3` (ESP32-S3, tree `_fzcard_work/NEW_v2/src_m5stick_s3`)
**Status:** Approved design — ready for implementation plan

## 1. Purpose

The M5Stick S3 has only two physical buttons (KEY1=GPIO11, KEY2=GPIO12). The current
input driver (`targets/m5stick_s3/target_input.c`) maps Down/Ok/Up/Back onto them via
press-duration gestures, but **Left and Right are unreachable** — which breaks
horizontal menu navigation, dialog button rows (worked around by a Down/Ok remap), the
file browser's "up a folder", and any app needing Left/Right.

The board's **BMI270 IMU is wired but unused**. This feature adds a minimal accel-only
driver and maps **rolling the stick left/right into `InputKeyLeft`/`InputKeyRight`**,
retiring the two-button limitation system-wide. Benefits both public and private builds.

## 2. Requirements

**Functional**
- Rolling the stick left past a threshold emits `InputKeyLeft`; right emits `InputKeyRight`.
- **Hold-repeat:** while held past the threshold the key auto-repeats (mirrors the
  existing KEY1-hold = Up-repeat), so lists/menus scroll. Returning to the neutral
  (upright) zone stops it.
- **Deadzone + hysteresis** (~35 deg to fire, ~25 deg to release) so only a deliberate
  roll fires and there is no chatter at the boundary.
- **Settings toggle** "Tilt Nav" (On/Off) under System settings, persisted, **default ON**.
- If the BMI270 is absent or init fails, tilt is silently skipped; **buttons keep working**.
- Left/Right only. Buttons continue to provide Down/Ok/Up/Back unchanged.

**Non-goals (YAGNI)**
- No tilt-driven Up/Down (buttons cover them).
- No flick/gesture shortcuts (back, etc.) in v1.
- No gyro use (accel only).
- No per-app tilt gating beyond the global toggle.

## 3. Architecture / Components

Three additions; nothing existing is restructured.

### 3.1 `targets/m5stick_s3/bmi270.{c,h}` — accel-only IMU driver
- Runs on the internal I2C bus `I2C_NUM_0` (SDA=47, SCL=48, 400 kHz), which is already
  installed at boot by the M5PM1 PMIC init — the driver reuses it (no install).
- I2C address **0x68** (BMI270 default, SDO low). Confirm on-device via CHIP_ID.
- API:
  - `bool bmi270_init(void)` — full bring-up (see 3.2). Returns false if absent/failed.
  - `bool bmi270_is_present(void)`
  - `bool bmi270_read_accel(int16_t* x, int16_t* y, int16_t* z)` — signed 16-bit raw.
- Ships the ~8 KB Bosch config blob as a `static const uint8_t bmi270_config_file[]`
  (from Bosch's open BMI270-Sensor-API / Arduino BMI270 lib; BSD-3). Kept in its own
  header `bmi270_config.h` so the driver body stays readable.

### 3.2 BMI270 bring-up sequence (in `bmi270_init`)
Standard Bosch init — BMI270 will not leave suspend without the config upload:
1. Read `CHIP_ID` (0x00); expect `0x24`. If not, return false (absent).
2. Soft reset: write `CMD`(0x7E)=`0xB6`; wait ~5 ms.
3. Disable advanced power save: `PWR_CONF`(0x7C)=`0x00`; wait ~450 us.
4. Config upload: `INIT_CTRL`(0x59)=`0x00`; set `INIT_ADDR_0`(0x5B)=0, `INIT_ADDR_1`(0x5C)=0;
   burst-write the 8 KB blob to `INIT_DATA`(0x5E) in chunks; `INIT_CTRL`(0x59)=`0x01`.
5. Wait ~150 ms; poll `INTERNAL_STATUS`(0x21) low nibble == `0x01` (init_ok). Timeout → false.
6. Enable accel: `PWR_CTRL`(0x7D)=`0x04` (acc_en); `ACC_CONF`(0x40)=`0xA8`
   (100 Hz ODR, normal filter); `ACC_RANGE`(0x41)=`0x00` (+/-2g).
7. Accel data at `ACC_X_LSB`(0x0C): 6 bytes X/Y/Z, little-endian signed 16-bit.

Init is called from `target_input_init()`. Total init time (~1 s worst case) is fine at
boot alongside the other init.

### 3.3 Tilt logic in `target_input.c`
- Add a **pure, host-testable** evaluator that owns all the math and state:
  ```
  typedef enum { TiltNone, TiltLeft, TiltRight } TiltDir;
  typedef struct { bool engaged; TiltDir dir; uint32_t last_repeat_at; } TiltState;
  TiltDir tilt_eval(int16_t a_roll, int16_t a_up, uint32_t now,
                    TiltState* st);   // returns a key to emit THIS tick, or TiltNone
  ```
  - Roll angle from the two relevant axes (`a_roll` = the axis that swings with L/R roll,
    `a_up` = the axis reading ~-1g upright): `roll_deg = atan2(a_roll, a_up)`.
  - **Not engaged** and `|roll| >= FIRE_DEG (35)` -> engage, set dir, emit that dir now,
    stamp `last_repeat_at`.
  - **Engaged** and still past `RELEASE_DEG (25)` in the same dir and
    `now - last_repeat_at >= REPEAT_MS (150)` -> emit dir again, restamp.
  - `|roll| < RELEASE_DEG` (or dir flipped) -> disengage (`TiltNone`).
  - Mirrors the KEY1 `up_fired`/`last_repeat_at` structure already in the file.
- In `target_input_poll()`: if tilt enabled and `bmi270_is_present()`, read accel, call
  `tilt_eval`, and on a non-None result `publish_tap(pubsub, InputKeyLeft/Right, seq)`.
- Sampling piggybacks the existing input poll (~10-20 ms), no extra timer/thread.
- **Axis/sign:** the physical mapping (which raw axis is `a_roll`/`a_up`, and which sign
  is Left vs Right) depends on how the BMI270 is mounted. Pick the likely mapping; expose
  the axis pick + a sign flip as named constants so on-device confirmation is a one-line
  change, not a rewrite.

### 3.4 Settings toggle
- Add a "Tilt Nav" On/Off item to **System settings**
  (`applications/settings/system/system_settings.c`, `VariableItemList`).
- Persist with the same mechanism the other system settings use (saved_struct /
  the existing settings store). Default **ON** on first boot / missing value.
- Expose a shared flag the input driver reads:
  `void target_input_set_tilt_enabled(bool)` / read a `volatile bool` in target_input.c.
  Loaded at boot from the persisted value; updated when the setting changes.

## 4. Data flow

```
BMI270 (I2C_NUM_0) --accel--> bmi270_read_accel()
   -> target_input_poll(): if tilt_enabled && present
        -> tilt_eval(a_roll, a_up, now, &state) -> TiltDir
        -> publish_tap(InputKeyLeft/Right) -> RECORD_INPUT_EVENTS pubsub -> GUI
System Settings "Tilt Nav" -> persisted store + target_input_set_tilt_enabled(flag)
```

## 5. Error handling
- BMI270 absent / wrong CHIP_ID / config upload timeout -> `bmi270_init` returns false,
  `bmi270_is_present()` stays false, tilt path is skipped every poll. No log spam beyond a
  single boot line. Buttons unaffected.
- I2C read failure mid-run -> treat as neutral (no key) that tick; do not disengage a
  held repeat on a single dropped sample (require N consecutive failures before giving up).

## 6. Testing

**Host unit tests (pure logic):**
- `tilt_eval`: upright -> None; roll past FIRE -> one key then repeats at REPEAT_MS;
  hysteresis (fires at 35, does not release until <25); direction flip disengages;
  neutral resets. Synthetic `a_roll`/`a_up` + a fake `now` clock. No hardware.

**On-device checklist:**
- Boot log shows BMI270 CHIP_ID 0x24 + init_ok.
- Tilt left/right scrolls a horizontal menu / moves dialog focus; hold auto-repeats.
- Correct direction mapping (flip the sign constant if reversed).
- No accidental Left/Right when held upright / during normal handling.
- Settings toggle Off -> no tilt input; On -> restored; persists across reboot.
- Buttons (Down/Ok/Up/Back) unchanged.

## 7. Files touched
- **New:** `targets/m5stick_s3/bmi270.c`, `bmi270.h`, `bmi270_config.h` (8 KB blob).
- **New:** host test for `tilt_eval` (test harness / scratch, per repo convention).
- **Edit:** `targets/m5stick_s3/target_input.c` (init call, poll-time tilt, enable flag,
  `tilt_eval`), `target_input.h` (setter decl).
- **Edit:** `targets/m5stick_s3/CMakeLists.txt` (add `bmi270.c`).
- **Edit:** `applications/settings/system/system_settings.c` (+ persistence) — add toggle.

## 8. Open items (resolve on-device, not blocking design)
- BMI270 I2C address (0x68 vs 0x69) — confirm via CHIP_ID scan.
- Which raw axis is roll vs up, and Left/Right sign — confirm by tilting; one-line consts.
- Final FIRE/RELEASE angles + REPEAT_MS — tune for feel (start 35/25/150).
