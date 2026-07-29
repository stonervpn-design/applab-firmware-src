# Vehicle Vulnerability (CVE) Lookup Test Checklist

**Status:** Not yet hardware-validated — requires physical Cardputer ADV.

---

## Overview

Verify the Vehicle Vulns reference browser: Sub-GHz → Vehicle Vulns → make picker → per-make CVE list. The feature displays known keyless-entry and rolling-code CVEs indexed by vehicle manufacturer, with year ranges, CVE IDs, and descriptions (replay, RollBack, rolling-code reuse attacks).

---

## Part 1: Menu Entry & Make Picker

Verify the Vehicle Vulns menu item and manufacturer selector.

### 1.1 Sub-GHz → Vehicle Vulns Entry
- [ ] Navigate to Sub-GHz app
- [ ] Scroll through the main menu
- **Expected:** "Vehicle Vulns" menu item appears between "Saved" and "Add Manually" (or similar position)
- [ ] Select "Vehicle Vulns"
- **Expected:** Opens the manufacturer picker

### 1.2 Make Picker Lists All Manufacturers
- [ ] On the picker menu, verify all 7 manufacturers are listed:
  - Renault
  - Honda
  - Hyundai
  - Kia
  - Mazda
  - Nissan
  - Acura
- **Expected:** All 7 makes are present; menu scrollable if needed

### 1.3 Back Navigation
- [ ] Press Back from the picker
- **Expected:** Returns to Sub-GHz main menu; "Vehicle Vulns" is still selectable

---

## Part 2: Make-Specific CVE Lists

Verify each make displays its known CVEs with correct details.

### 2.1 Honda Entries
- [ ] From the picker, select "Honda"
- **Expected:** Opens a scrollable list with Honda CVE entries:
  - **Civic** — CVE-2022-27254 (replay vulnerability)
  - **Fit / City / Vezel / HR-V** — CVE-2022-37418 (RollBack vulnerability)
  - Static-code vulnerability — CVE-2019-20626
- [ ] Verify each entry includes:
  - Model(s)
  - Year range (e.g., "2015–2019")
  - CVE ID
  - Region/scope
  - Description (attack type: replay, RollBack, rolling-code reuse)
- [ ] Test scrolling (Up/Down) through all entries without truncation

### 2.2 Renault Entries
- [ ] Press Back to return to the picker
- [ ] Select "Renault"
- **Expected:** Opens a scrollable list with Renault CVE entries:
  - **ZOE** — CVE-2022-38766 (replay vulnerability)
- [ ] Verify year range, CVE ID, region, and full description

### 2.3 Mazda Entries
- [ ] Press Back to return to the picker
- [ ] Select "Mazda"
- **Expected:** Opens a scrollable list with Mazda CVE entries:
  - RollBack vulnerabilities — CVE-2022-36945
  - Verify all models and year ranges are displayed
- [ ] Confirm text scrolls without truncation

### 2.4 Hyundai, Kia, Nissan, Acura (Spot Checks)
- [ ] Pick any of these four remaining makes
- **Expected:** Each displays at least one CVE entry with model, year, CVE ID, and description
- [ ] Verify no entries are missing or malformed

---

## Part 3: Navigation & Exit Behavior

### 3.1 Back from CVE List
- [ ] While viewing any make's CVE list, press Back
- **Expected:** Returns to the manufacturer picker (selection resets or remains on the previously selected make)

### 3.2 Back from Picker
- [ ] From the picker, press Back
- **Expected:** Returns to Sub-GHz main menu

### 3.3 Sub-GHz Menu Integrity
- [ ] Verify the Sub-GHz main menu still shows all original entries:
  - Receive
  - Read RAW
  - Frequency Analyzer
  - Hopper
  - Transmit
  - Add Manually
  - Saved
  - (and any other existing items)
- **Expected:** No menu items removed, reordered, or broken by Vehicle Vulns integration

---

## Part 4: Edge Cases

### 4.1 No Entries for Invalid Make (if applicable)
- [ ] If the picker accidentally includes a manufacturer with no CVEs, select it
- **Expected:** Displays "No entries" or an appropriate empty-state message

### 4.2 Text Overflow on Long Descriptions
- [ ] Pick any CVE entry with a long description text
- **Expected:** Text scrolls smoothly (Up/Down); no truncation or visual glitches

---

## Notes

- **Hardware:** Requires a Cardputer ADV with working Sub-GHz firmware.
- **CVE Data Source:** NVD via the KAT project (21 total entries across 7 manufacturers).
- **Purpose:** Reference browser for known vulnerabilities—NOT automatic capture-to-CVE matching; use with the Rolling-Code Analyzer for fob analysis.
- **Limitation:** This is a read-only lookup; no data modification or capture integration in this version.
