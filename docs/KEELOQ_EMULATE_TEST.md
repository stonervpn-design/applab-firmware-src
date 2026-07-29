# KeeLoq Direct-Emulate Test Checklist

## Overview
Hardware validation checklist for the KeeLoq "By Manufacturer" direct-emulate feature on the Cardputer ADV. This feature adds an **Emulate** option (in addition to Save) when creating KeeLoq signals via the by-manufacturer generator path, allowing immediate transmission and counter-advancing without saving to disk first.

## Test Environment
- **Hardware:** M5Stack Cardputer ADV with CC1101 (433 MHz)
- **Firmware:** Latest build with KeeLoq Emulate feature
- **Receiver:** Physical KeeLoq receiver or RF analyzer (e.g., SDR, analyzer handheld) to verify transmitted codes
- **Status:** Not yet hardware-validated — requires physical Cardputer ADV + a KeeLoq receiver

## Test Scenarios

### 1. KeeLoq By-Manufacturer Path → Emulate/Save Menu
**Steps:**
1. Open **Sub-GHz** app
2. Select **Add Manually**
3. Select **"KL: By Manufacturer"**
4. Pick a manufacturer from the list (e.g., DoorHan, Came, Nice, Marantec, etc.)
5. Enter **Serial** (e.g., `0x12345678`)
6. Enter **Button** (e.g., `0x1` for lock)
7. Enter **Counter** (e.g., `0x0001`)

**Expected:**
- After entering all three fields, an **Emulate / Save** menu choice appears
- Navigation: Up/Down to select, OK to confirm choice

**Result:** ✓ PASS / ✗ FAIL

---

### 2. Emulate Path → Transmitter Opens
**Steps:**
1. Complete Test 1 (reach the Emulate/Save menu)
2. Select **Emulate**

**Expected:**
- The Sub-GHz **Transmitter** app opens immediately
- Display shows the fob information (manufacturer, serial, button, frequency 433 MHz)
- Fob is ready to transmit

**Result:** ✓ PASS / ✗ FAIL

---

### 3. Transmit & Counter Advancement
**Steps:**
1. Open Emulate path (Test 2)
2. Point Cardputer at a KeeLoq receiver or RF analyzer (close range, ~1 m)
3. Press **OK** (Enter key)
4. Check receiver: verify a valid KeeLoq signal is received with:
   - Matching manufacturer
   - Matching serial
   - Matching button
   - Counter = `0x0001` (first press)
5. Press **OK** again
6. Check receiver: verify counter advanced to `0x0002`

**Expected:**
- Each OK press transmits a valid rolling code
- Counter increments by 1 each press
- Transmitted signal is decodable by a real KeeLoq receiver (or RF analyzer confirms frequency/modulation)

**Result:** ✓ PASS / ✗ FAIL

---

### 4. Button Variants (Arrow Keys)
**Steps:**
1. Open Emulate path (Test 2)
2. Press **Up** arrow (Fn + `;`)
3. Check receiver: verify signal sent with button = up/trunk (or appropriate code)
4. Press **Down** arrow (Fn + `.`)
5. Check receiver: verify signal sent with button = down/open (or appropriate code)
6. Press **Left** arrow (Fn + `,`)
7. Press **Right** arrow (Fn + `/`)

**Expected:**
- Each arrow sends a different button command (varies by fob type: lock/unlock/trunk/etc.)
- All transmissions are valid rolling codes with correct counter values

**Result:** ✓ PASS / ✗ FAIL

---

### 5. Save Path (Original Behavior)
**Steps:**
1. Complete Test 1 (reach the Emulate/Save menu)
2. Select **Save**

**Expected:**
- Standard file-save dialog appears (SaveName flow)
- File is saved as `.sub` in the usual location
- File can be reopened later and transmitted normally

**Result:** ✓ PASS / ✗ FAIL

---

### 6. Regression: Hardcoded KeeLoq Entries (No Emulate Choice)
**Steps:**
1. Open **Sub-GHz** → **Add Manually**
2. Select a **hardcoded KeeLoq entry** (e.g., **"KL: DoorHan 433MHz"** or similar)
3. Enter signal parameters as before

**Expected:**
- NO **Emulate / Save** menu appears
- Goes DIRECTLY to Save dialog (original behavior)
- Transmission still works normally

**Result:** ✓ PASS / ✗ FAIL

---

### 7. Regression: Other Protocols (No Emulate Choice)
**Steps:**
1. Open **Sub-GHz** → **Add Manually**
2. Select a NON-KeeLoq-by-manufacturer protocol (e.g., **Nice RollingCode** or **CAME**)
3. Enter required parameters

**Expected:**
- NO **Emulate / Save** menu appears
- Goes DIRECTLY to Save dialog (original behavior)

**Result:** ✓ PASS / ✗ FAIL

---

### 8. Back Navigation from Emulate/Save Menu
**Steps:**
1. Reach the **Emulate / Save** menu (Test 1)
2. Press **Back** (Esc or Backspace)

**Expected:**
- Returns cleanly to the previous parameter-entry screen
- Can re-enter values or navigate back further without crashes

**Result:** ✓ PASS / ✗ FAIL

---

### 9. Back Navigation from Transmitter (Emulate)
**Steps:**
1. Open Emulate path (Test 2)
2. Press **Back** (Esc or Backspace)

**Expected:**
- Transmitter closes cleanly
- Returns to the Emulate/Save menu (or previous screen)

**Result:** ✓ PASS / ✗ FAIL

---

## Summary

| Test # | Description | Result |
|--------|-------------|--------|
| 1 | By-Mfg → Emulate/Save menu appears | ✓/✗ |
| 2 | Emulate → Transmitter opens | ✓/✗ |
| 3 | Transmit & counter advances | ✓/✗ |
| 4 | Arrow keys send button variants | ✓/✗ |
| 5 | Save path (original behavior) | ✓/✗ |
| 6 | Hardcoded entries skip Emulate/Save | ✓/✗ |
| 7 | Other protocols skip Emulate/Save | ✓/✗ |
| 8 | Back from Emulate/Save menu | ✓/✗ |
| 9 | Back from Transmitter | ✓/✗ |

---

## Notes
- This checklist requires **physical hardware validation** on a real Cardputer ADV with a functional CC1101.
- Receiver validation (Test 3, 4) is strongly recommended to confirm rolling-code correctness; if no receiver is available, at minimum verify the Transmitter app opens and counter increments on-screen.
- Counter state is ephemeral; closing the Transmitter and re-opening will reset the counter to the initially entered value.
