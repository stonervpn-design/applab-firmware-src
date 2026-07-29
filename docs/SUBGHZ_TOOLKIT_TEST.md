# SubGhz Toolkit Test Checklist

**Status:** Not yet hardware-validated — requires physical Cardputer ADV.

---

## Overview

Verify the SubGhz Toolkit feature: Sub-GHz → SubGhz Toolkit → menu with two actions:
- **Protocol Analysis** — Lists registered Sub-GHz protocols and exports them with a firmware header to `/ext/subghz/analysis/protocol_analysis.txt`.
- **Export Keystore** — Exports the loaded plaintext KeeLoq keystore (~68 manufacturer keys) to `/ext/subghz/analysis/keeloq_keys.txt`.

Adapted from RocketGods-SubGHz-Toolkit (GPL-3.0); the original's secure-enclave KeeLoq decrypt is not applicable on this port, so the already-plaintext keystore is exported instead.

---

## Part 1: Menu Entry & Toolkit Actions

Verify the SubGhz Toolkit menu item and main action menu.

### 1.1 Sub-GHz → SubGhz Toolkit Entry
- [ ] Navigate to Sub-GHz app
- [ ] Scroll through the main menu
- **Expected:** "SubGhz Toolkit" menu item appears in the Sub-GHz menu
- [ ] Select "SubGhz Toolkit"
- **Expected:** Opens a submenu showing "Protocol Analysis" and "Export Keystore" options

### 1.2 Toolkit Menu Layout
- [ ] Verify the toolkit submenu lists exactly two actions:
  - Protocol Analysis
  - Export Keystore
- **Expected:** Both items are visible and selectable; menu is clean with no duplicate or malformed entries

### 1.3 Back Navigation
- [ ] Press Back from the toolkit menu
- **Expected:** Returns to Sub-GHz main menu; "SubGhz Toolkit" is still selectable

---

## Part 2: Protocol Analysis Action

Verify the protocol analysis feature displays protocols and exports data.

### 2.1 Protocol Analysis Screen
- [ ] Select "Protocol Analysis" from the toolkit menu
- **Expected:** Displays a scrollable screen listing all registered Sub-GHz protocols, showing:
  - Protocol name
  - Type (Static/Dynamic/Unknown)
  - Flags
- [ ] Verify the screen is readable and not truncated

### 2.2 Protocol List Scrolling
- [ ] Navigate through the protocol list using Up/Down arrow keys
- **Expected:** List scrolls smoothly without glitches
- [ ] Confirm that more than one protocol is visible (expect 20+ protocols depending on firmware)

### 2.3 Protocol Analysis Export File
- [ ] After viewing the protocol list, check the SD card for the exported file:
  - **Path:** `/ext/subghz/analysis/protocol_analysis.txt`
  - **Expected:** File exists and is readable
- [ ] Verify the file contains:
  - Firmware header information
  - Total protocol count
  - One entry per protocol with name, type, and flags
  - No truncation or corruption

### 2.4 Back from Protocol Analysis
- [ ] Press Back from the protocol analysis screen
- **Expected:** Returns to the SubGhz Toolkit menu

---

## Part 3: Export Keystore Action

Verify the keystore export feature displays confirmation and exports data.

### 3.1 Export Keystore Selection
- [ ] From the toolkit menu, select "Export Keystore"
- **Expected:** Displays a success popup confirming the export with the path `/ext/subghz/analysis/keeloq_keys.txt`

### 3.2 Export Keystore File
- [ ] After dismissing the success popup, check the SD card for the exported keystore:
  - **Path:** `/ext/subghz/analysis/keeloq_keys.txt`
  - **Expected:** File exists and is readable
- [ ] Verify the file contains approximately 68 manufacturer entries with:
  - Manufacturer name
  - Key in hexadecimal format
  - Key type (e.g., plaintext, standard)
- [ ] Confirm no truncation or malformed entries

### 3.3 Back from Export Keystore
- [ ] Press Back after the success popup
- **Expected:** Returns to the SubGhz Toolkit menu

---

## Part 4: Navigation & Menu Integrity

### 4.1 Back Navigation Chain
- [ ] From the SubGhz Toolkit menu, press Back → Sub-GHz main menu
- [ ] From Sub-GHz main menu, press Back → main home screen
- **Expected:** All navigation is clean with no orphaned screens or crashes

### 4.2 Sub-GHz Menu Integrity
- [ ] Verify the Sub-GHz main menu still shows all original entries:
  - Receive
  - Read RAW
  - Frequency Analyzer
  - Hopper
  - Transmit
  - Add Manually
  - Saved
  - Rolling-Code Analyzer
  - KeeLoq "By Manufacturer" Generator
  - Vehicle Vulns
  - (and any other existing items)
- **Expected:** No menu items removed, reordered, or broken by SubGhz Toolkit integration
- [ ] Verify "SubGhz Toolkit" appears in the correct position

---

## Part 5: Export Directory Structure

### 5.1 Analysis Directory Creation
- [ ] After both exports, check the SD card directory structure:
  - **Path:** `/ext/subghz/analysis/`
  - **Expected:** Directory exists (created automatically on first export if needed)

### 5.2 Both Files Present
- [ ] Verify both files exist in `/ext/subghz/analysis/`:
  - `protocol_analysis.txt`
  - `keeloq_keys.txt`
- **Expected:** Both files are present with recent timestamps

### 5.3 File Accessibility
- [ ] Attempt to read both files via SD card or qFlipper file browser
- **Expected:** Files are readable and display correctly without corruption

---

## Part 6: Edge Cases & Robustness

### 6.1 Repeated Exports
- [ ] Export keystore multiple times in succession
- **Expected:** Each export succeeds; file is overwritten with latest data; no errors or corrupted output

### 6.2 Protocol Analysis Consistency
- [ ] Run Protocol Analysis, then run it again
- **Expected:** Both exports show identical protocol lists with consistent order and data

### 6.3 Toolkit Menu Availability
- [ ] Verify SubGhz Toolkit remains accessible after multiple operations
- **Expected:** Menu responds to all selections without lag or unexpected behavior

---

## Notes

- **Hardware:** Requires a Cardputer ADV with working Sub-GHz firmware and a formatted FAT32 SD card.
- **Source:** Adapted from RocketGods-SubGHz-Toolkit (GPL-3.0).
- **Keystore Origin:** The ~68 manufacturer keys are already plaintext in the firmware (`keeloq_mfcodes_user`); this feature exports them for reference and offline analysis.
- **Limitation:** The original toolkit's secure-enclave KeeLoq decryption is not applicable on this port—only plaintext keystore export is available.
- **Purpose:** Export protocols and keystore for reference, analysis, and documentation; not for key generation or automatic fob matching.
