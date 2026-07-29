# QR Code App — On-Hardware Test Checklist

**Feature:** Standalone "QR Code" app in the main menu that generates and displays QR codes on the 128×64 screen.

**Status:** Not yet hardware-validated — requires physical Cardputer ADV and a phone to scan QR codes.

---

## Test Cases

### 1. App Launch
- [ ] Main menu shows "QR Code" app in the list
- [ ] Selecting "QR Code" opens the app menu with four options:
  - Text / URL
  - WiFi
  - From File
  - Contact (vCard)

### 2. Text / URL Generation
- [ ] Select "Text / URL"
- [ ] Type a short URL (e.g., `https://example.com`)
- [ ] QR code renders on screen (black/white grid, scannable pattern)
- [ ] Scan with phone camera:
  - [ ] Code is recognized as valid QR
  - [ ] Phone offers to open the URL
  - [ ] Following the link works correctly
- [ ] Return to menu (Back key) exits QR view cleanly

### 3. WiFi Credentials
- [ ] Select "WiFi"
- [ ] Enter SSID, Password, and Authentication type (WPA/WEP/Open)
- [ ] QR code renders with `WIFI:` protocol format
- [ ] Scan with phone:
  - [ ] Phone recognizes as WiFi join QR
  - [ ] Phone offers to connect to the network with the provided credentials
  - [ ] Attempting to join uses the correct SSID/password/auth
- [ ] Return to menu exits cleanly

### 4. From File (SD Card)
- [ ] Select "From File"
- [ ] Browse to a small `.txt` file on `/ext` (SD card)
- [ ] Select the file
- [ ] QR code renders with the file contents
- [ ] Scan with phone:
  - [ ] Code decodes to the expected text content
- [ ] Supports `.qrcode` files with `Message:` field:
  - [ ] Browse to a `.qrcode` file
  - [ ] Reads and displays the `Message:` field as QR
  - [ ] Scan confirms the correct content
- [ ] Return to menu exits cleanly

### 5. Contact (vCard)
- [ ] Select "Contact"
- [ ] Enter Name, Phone number, and Email
- [ ] QR code renders (should be larger grid due to vCard format)
- [ ] Scan with phone:
  - [ ] Phone recognizes as contact/vCard
  - [ ] Phone offers to add the contact with the provided Name/Phone/Email
  - [ ] Confirmed fields match the input
- [ ] Return to menu exits cleanly

### 6. Over-Long Content (Small Screen Cap)
- [ ] Select "Text / URL"
- [ ] Paste or type content longer than ~152 bytes (e.g., 200+ character URL or long text)
- [ ] Screen shows error message: **"Too long for screen QR"** or similar
- [ ] **No crash or reboot** — app remains stable and responsive
- [ ] Return to menu with Back key works cleanly
- [ ] Repeat test with each input method (WiFi, From File, Contact) to confirm graceful handling

### 7. Navigation & Stability
- [ ] QR view → press Back → returns to app menu
- [ ] App menu → press Back → returns to main menu
- [ ] All transitions are clean (no visual glitches, no freezing)
- [ ] Repeated entry/exit cycles show no degradation or crashes

---

## Notes

- **Small-screen limitation:** The 128×64 display caps QR payload at approximately 152 bytes of data before requiring error-correction truncation or overflow rejection.
- **Encoder:** Uses MIT encoder from [bmatcuk/flipperzero-qrcode](https://github.com/bmatcuk/flipperzero-qrcode).
- **Generation-only:** No camera or QR scanning on the device itself.
- **File support:** Reads plaintext `.txt` files and extracts the `Message:` field from `.qrcode` files.

---

## Pass Criteria

- ✅ All four input methods generate scannable QR codes
- ✅ Phone can decode and act on each QR (URL, WiFi join, contact add)
- ✅ Over-long content shows error message without crash
- ✅ Navigation is clean and responsive
- ✅ No unexpected reboots or display corruption
