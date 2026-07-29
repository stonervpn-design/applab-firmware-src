# GPIO Manual Control — On-Hardware Test Checklist

**Status:** NOT yet hardware-validated — requires a physical Cardputer ADV.

This checklist verifies the GPIO Manual Control feature as implemented across Tasks 1–5. Each numbered check corresponds to a specific capability; **mark PASS or FAIL** after exercising the feature on a live device.

---

## 1. SAFE Tier Pin List (Locked by Default)

**Test:** Open GPIO → Manual Control. Press OK to view the pin list.

- [ ] **PASS** — Only **GPIO2 (Grove SDA)** and **GPIO1 (Grove SCL)** are visible in the initial list.
- [ ] **PASS** — No peripheral pins (CC1101, NRF24, IR TX, RGB) are shown.
- [ ] **PASS** — The list is labeled or clearly indicates these are Grove pins only.

---

## 2. Advanced Unlock with Confirm Gesture & Warnings

**Test:** From the pin list, navigate to the `Advanced: [OFF]` row.

- [ ] **PASS** — Pressing OK once highlights `Advanced: [ON]` (toggle state changes).
- [ ] **PASS** — The list does NOT reveal new pins after the first OK press.
- [ ] **PASS** — Pressing OK **again** (confirm) reveals additional pins: **GPIO44 (IR TX)**, **GPIO13/GPIO5 (CC1101)**, **GPIO4/GPIO6 (NRF24)**, **GPIO21 (RGB)**.
- [ ] **PASS** — The newly revealed pins each display a **visual warning** (e.g., a warning icon or text banner) indicating they are shared with RF/IR peripherals.
- [ ] **PASS** — Toggling `Advanced: [OFF]` hides the shared pins again.

---

## 3. Output Mode — LED Control

**Test:** Set GPIO2 to Output mode, then toggle HIGH and LOW.

- [ ] **PASS** — When Output is set to **HIGH**, an LED connected to GPIO2 lights up (or logic analyzer shows 3.3 V).
- [ ] **PASS** — When Output is set to **LOW**, the LED turns off (or logic analyzer shows 0 V).
- [ ] **PASS** — The mode row clearly displays the current output state (HIGH or LOW).

---

## 4. Input Mode — Pull-Up and Pull-Down Reads

**Test A: Pull-Up**  
Set GPIO2 to Input mode with pull-up enabled. Idle (pin floating), the read should show **1**. Ground the pin, the read should show **0**.

- [ ] **PASS** — Pin idles at **1** (pulled high internally).
- [ ] **PASS** — When externally grounded, read shows **0**.

**Test B: Pull-Down**  
Set GPIO2 to Input mode with pull-down enabled. Idle (pin floating), the read should show **0**. Apply 3.3 V to the pin, the read should show **1**.

- [ ] **PASS** — Pin idles at **0** (pulled low internally).
- [ ] **PASS** — When externally driven to 3.3 V, read shows **1**.

---

## 5. PWM Mode — Frequency & Duty Cycle

**Test:** Set GPIO2 (or GPIO1) to PWM mode. Set frequency to **10 Hz** and duty cycle to **50%**. Use a scope or observe LED brightness.

- [ ] **PASS** — Scope measures ~10 Hz, ~50% duty cycle (pulse width visible).
- [ ] **PASS** — LED brightness is moderate (not fully on, not fully off).

**Edge Case: Low Frequency (1 Hz)**  
Set frequency to **1 Hz** (or the lowest available). The test may show **"No free ch"** instead of outputting a waveform; this is documented graceful degradation (ESP32-S3 LEDC low-speed timer divider limitation) and **is not a crash**.

- [ ] **PASS** — Either: (a) 1 Hz outputs successfully on a scope, OR (b) the UI shows "No free ch" and the app does not crash.
- [ ] **PASS** — Higher frequencies (10 Hz, 100 Hz, 1 kHz) produce valid output signals.

**Channel Limit (8th PWM Pin)**  
With 7 PWM pins already active, attempt to set an 8th pin to PWM.

- [ ] **PASS** — The UI displays **"No free ch"** (no free LEDC channel).
- [ ] **PASS** — The app does not crash; the pin remains in the previous mode or stays disabled.
- [ ] **PASS** — Channels 1–7 are used by manual PWM; channel 0 is reserved for the LCD backlight.

---

## 6. PWM ↔ ADC Transitions on the Same Pin

**Test:** Set GPIO2 (or GPIO1) to PWM (10 Hz, 50% duty), then switch to ADC mode, back to PWM, back to ADC.

- [ ] **PASS** — After each transition, the mode indicator updates correctly.
- [ ] **PASS** — ADC reads show real analog voltage each time (not stale cached values).
- [ ] **PASS** — PWM resumes with the same frequency/duty after returning from ADC mode.
- [ ] **PASS** — No crashes or stuck states during mode switching.

---

## 7. ADC Mode — Voltage Reading Accuracy

**Test:** Set GPIO2 (or GPIO1) to ADC mode. Apply a known voltage divider (e.g., 3.3 V through a 1 kΩ / 1 kΩ divider to read ~1.65 V).

- [ ] **PASS** — ADC reads the expected voltage within **±5%** accuracy (e.g., 1.65 V → read 1.57–1.73 V).
- [ ] **PASS** — The read value updates when voltage changes.
- [ ] **PASS** — *Note: A digital "Read" row may appear alongside the ADC voltage; this is a harmless UI artifact and does not affect functionality.*

---

## 8. Pin Reset to High-Z on Exit

**Test:** Actively drive GPIO2 to HIGH or LOW (or PWM). Navigate back to the start menu (press Back / Escape).

- [ ] **PASS** — After leaving Manual Control, GPIO2 returns to high-impedance (no residual drive).
- [ ] **PASS** — Using a scope, pin floats; using an external pull resistor, it responds as if the MCU is not driving it.
- [ ] **PASS** — All PWM/ADC resources are cleaned up (LEDC channels freed, ADC disabled).

---

## 9. RF Peripheral Bus Release — Sub-GHz After Advanced CC1101 Use

**Test:** In Manual Control, enable Advanced unlock. Set GPIO13 or GPIO5 (CC1101 SPI pins) to Output or PWM, drive them, then return to the start menu. Launch Sub-GHz app.

- [ ] **PASS** — Sub-GHz app launches and functions normally (no SPI bus contention or crashes).
- [ ] **PASS** — CC1101 receiver/transmitter is responsive; frequency analyzer and receiver work.
- [ ] **PASS** — The manual-control teardown correctly released the shared SPI bus on exit.

---

## Summary

**Total Checks:** 16 (across 9 test groups)  
**Passing:** _____ / 16  
**Hardware:** Cardputer ADV (ESP32-S3 with CC1101, NRF24, IR TX, RGB)  

**Tester Notes:**
```
[Record observations, pin voltages, edge cases, or issues here]
```

---

**Document Version:** 1.0  
**Last Updated:** Task 6, GPIO Manual Control Feature  
