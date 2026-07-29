# Rolling-Code Analyzer & KeeLoq Generator Test Checklist

**Status:** Not yet hardware-validated — requires physical Cardputer ADV + a dynamic-protocol remote/receiver.

---

## Part 1: Rolling-Code Analyzer

Verify the analyzer UI and decoding for dynamic Sub-GHz captures.

### 1.1 Static Protocol Fob
- [ ] Receive a static-protocol fob (e.g., fixed-code garage remote, non-KeeLoq)
- [ ] Open receiver-info for the capture
- **Expected:** No "Analyze" (Left) button appears; TX/Save buttons remain functional

### 1.2 Dynamic Protocol Fob (KeeLoq Garage Remote)
- [ ] Receive a dynamic-protocol fob (KeeLoq garage remote or equivalent)
- [ ] Open receiver-info for the capture
- **Expected:** "Analyze" (Left) button appears

### 1.3 Analyzer View
- [ ] Press Left (Analyze) button
- **Expected:** Opens Rolling-Code Analyzer with fields displayed:
  - **Protocol** — shows detected protocol (e.g., "KeeLoq")
  - **Manufacturer** — decoded manufacturer name OR "unknown"
  - **Serial** — device serial number
  - **Button** — pressed button code (0–3 typically)
  - **Counter** — rolling counter value
  - **Bits** — frame size in bits
  - **Verdict** — plaintext summary:
    - If a matching plaintext key exists in keystore: `"[Manufacturer] serial [Serial], button [Button], counter [Counter] — key found"`
    - If no key: `"key unknown (append to keeloq_mfcodes_user to decode)"`

### 1.4 Receiver-Info TX/Save Still Work
- [ ] Close Analyzer (Back)
- [ ] From receiver-info, press Right (TX) to transmit the capture (or Left for Save)
- **Expected:** TX sends the raw frame; Save writes to SD without errors

---

## Part 2: KeeLoq Manufacturer Keystore

Verify the plaintext keystore loads and enables decoding.

### 2.1 Keystore Loads
- [ ] Power on Cardputer, navigate to Sub-GHz app
- [ ] Open any saved KeeLoq `.sub` file from a listed manufacturer (e.g., garage remote)
- [ ] Analyzer displays **Manufacturer** name (not "unknown")
- **Expected:** The 68 plaintext keys in `keeloq_mfcodes_user` have loaded; manufacturer lookup succeeds

### 2.2 Counter Decodes with Matching Key
- [ ] Same KeeLoq fob from Step 2.1
- [ ] Analyzer shows **Counter** field populated with a decoded integer (not 0xFFFFFFFF or "error")
- **Expected:** The rolling counter has been decrypted using the matching manufacturer key

### 2.3 Append New Plaintext Key
- [ ] On the Cardputer or via SD card reader, edit `/ext/subghz/assets/keeloq_mfcodes_user`
- [ ] Add a new line: `<manufacturer_name>,<plaintext_hex_key>` (e.g., `TestMfg,0123456789ABCDEF`)
- [ ] Reboot Cardputer
- [ ] Receive or load a frame from "TestMfg" (or any fob matching the new key's manufacturer)
- **Expected:** Analyzer shows the new manufacturer name and decodes its counter correctly

---

## Part 3: KeeLoq "By Manufacturer" Generator

Verify manual signal creation with the keystore.

### 3.1 Generator Entry Point
- [ ] Navigate to Sub-GHz → Add Manually
- [ ] Scroll to "KL: By Manufacturer" option
- **Expected:** Option appears and is selectable

### 3.2 Manufacturer Picker
- [ ] Select "KL: By Manufacturer"
- **Expected:** Picker displays all 68 keystore manufacturers (alphabetically or in keystore order)

### 3.3 Create Signal
- [ ] Pick any manufacturer from the list (e.g., "garage")
- [ ] Enter **Serial** (e.g., `12345`)
- [ ] Enter **Button** (e.g., `0` for first button)
- [ ] Enter **Counter** (e.g., `42`)
- **Expected:** All fields accept numeric input; no validation errors

### 3.4 Transmit & Verify
- [ ] Press TX to transmit the generated KeeLoq signal
- [ ] Receive the frame back (via another receiver or scan immediately after)
- [ ] Open Analyzer on the received frame
- **Expected:**
  - Manufacturer matches the selected manufacturer
  - Serial matches entered value
  - Button matches entered value
  - Counter matches entered value

### 3.5 Empty Keystore Fallback
- [ ] Temporarily remove or rename `/ext/subghz/assets/keeloq_mfcodes_user` to simulate an empty keystore
- [ ] Reboot and navigate to Sub-GHz → Add Manually → "KL: By Manufacturer"
- **Expected:** A message or disabled state indicates "No keys available" or similar user-friendly fallback

---

## Part 4: Cross-Validation (Optional, Requires Second Device)

### 4.1 Interoperability
- [ ] Generate a KeeLoq frame with manufacturer "X" on Cardputer A (Part 3.4)
- [ ] Transmit to a real garage door or capture on Cardputer B
- [ ] On Cardputer B, open Analyzer and verify the frame decodes to manufacturer "X"
- **Expected:** The generated frame is accepted by real hardware or correctly parsed by a second instance

---

## Notes

- **Hardware:** Requires a Cardputer ADV with CC1101 module and working Sub-GHz firmware.
- **Test Remotes:** Recommend using a real garage-door remote (KeeLoq protocol) for static vs. dynamic comparison.
- **Keystore Location:** `/ext/subghz/assets/keeloq_mfcodes_user` (plaintext format, one manufacturer per line).
- **Limitations:** AES-encrypted keystores (`keeloq_mfcodes`, `nice_flor_s`, `alutech_at_4n`) are not supported on this port.
