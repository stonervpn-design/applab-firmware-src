# NFC Magic-Card Cloning (Gen1a 7-bit frame) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Gen1a magic-card cloning work on the ST25R3916 NFC backend by transmitting the Gen1a 7-bit `0x40` magic-wakeup as a real 7-bit frame instead of a rounded-up 8-bit byte.

**Architecture:** One targeted branch in `furi_hal_nfc_poller_tx` (ST25R3916 backend): frames whose bit count is not a multiple of 8 are sent via the existing exact-bit `st25r3916_hal_transceive_custom_parity` (which writes the raw bits and sets the chip's NTX "bits-in-last-byte" field, parity off) instead of the byte-count transceive that rounds 7→8. Byte-aligned frames are untouched.

**Tech Stack:** C, ESP-IDF 5.4.1, ESP32-S3, ST25R3916 over I2C. No new driver function — reuses proven code.

## Global Constraints

- Board: `m5stick_s3`, ST25R3916 backend (`nfc_backend == NfcBackendSt25r3916`). PN532 backend is out of scope for this step.
- The change must ONLY affect non-byte-aligned frames (which do not work today anyway). Every byte-aligned read/write/auth path must be byte-for-byte unchanged — that is the de-risk.
- No changes to the `nfc_magic` app or the `gen1a` poller — the gap is purely in the HAL frame path.
- Source tree is NOT a git repo. "Checkpoint" = run the stated verification and confirm; there is no `git commit`.
- Firmware build: `cmd /c "<abs>\build_only_public.bat"` (or `build_only_private.bat`), then confirm `SENTINEL_OK` in the log and no `error:` lines. On-device flash: `python -m esptool --chip esp32s3 -p COM10 -b 460800 --before default_reset --after hard_reset write_flash 0x10000 <builddir>\furi_esp32.bin`.
- This feature cannot be unit/host-tested (hardware register + physical card). Verification is: compiles for target, boots clean, byte-aligned NFC still works (boot-log detect + a normal card read), and the tester clones to real magic cards.

---

## File Structure
- **Modify** `components/furi_hal/furi_hal_nfc.c` — add the sub-byte branch in `furi_hal_nfc_poller_tx`'s ST25R3916 section. (Only file changed.)

---

## Task 1: Route sub-byte frames to the exact-bit transceive

**Files:**
- Modify: `components/furi_hal/furi_hal_nfc.c` (function `furi_hal_nfc_poller_tx`, ST25R3916 branch)

**Interfaces:**
- Consumes (both already declared/used in this file): `st25r3916_hal_transceive_custom_parity(const uint8_t* tx, uint16_t tx_bits, uint8_t* rx, uint16_t rx_cap, uint16_t* rx_bits, uint32_t rx_budget_ms)` and `st25r_stage_result_bits(St25rHalStatus, const uint8_t* rx, uint16_t rx_bits)`.
- Produces: no new symbols.

- [ ] **Step 1: Read the current ST25R3916 branch**

Open `furi_hal_nfc.c`, find `FuriHalNfcError furi_hal_nfc_poller_tx(...)`. In the
`if(nfc_backend == NfcBackendSt25r3916)` block, note the structure: first an
`if(nfc_current_tech == FuriHalNfcTechIso15693) { ... }` block (NFC-V), then a
`/* Standard data frame. ... */` comment followed by the byte-count call
`st25r3916_hal_transceive(tx_data, (uint16_t)tx_bytes, rx, sizeof(rx), &rxn, 200);`.
The new branch goes **between** the ISO15693 block's closing `}` and the
`/* Standard data frame` comment.

- [ ] **Step 2: Insert the sub-byte branch**

Immediately after the ISO15693 `if(...) { ... }` block's closing brace, and before the
`/* Standard data frame` comment, insert:

```c
        /* Sub-byte short frame (bit count not a multiple of 8), e.g. the Gen1a 7-bit
         * 0x40 magic wakeup. The byte-count transceive below rounds 7 bits up to a full
         * 8-bit byte, so the wakeup is malformed and the magic card never answers. The
         * custom-parity transceive already transmits the EXACT bit count with hardware
         * parity off (it writes the raw bits and programs the chip's NTX "bits in last
         * byte" field) — which is precisely a short frame — and reports the exact RX bit
         * count (the Gen1a wakeup replies with a 4-bit 0x0A ACK). Byte-aligned frames
         * (every normal read/write/auth) skip this and are unaffected. */
        if((tx_bits % 8) != 0) {
            static uint8_t rx_sb[64];
            uint16_t rx_sb_bits = 0;
            St25rHalStatus s_sb = st25r3916_hal_transceive_custom_parity(
                tx_data, (uint16_t)tx_bits, rx_sb, sizeof(rx_sb), &rx_sb_bits, 20);
            return st25r_stage_result_bits(s_sb, rx_sb, (s_sb == St25rHalOk) ? rx_sb_bits : 0);
        }
```

(`tx_bits` is the function parameter; `tx_bytes` is the existing local used by the
byte-count path below — leave it as-is.)

- [ ] **Step 3: Build**

Run: `cmd /c "C:\Users\stone\Downloads\_fzcard_work\NEW_v2\src_m5stick_s3\build_only_private.bat"`
Expected: `SENTINEL_OK` in `build_only_private.log`, no `error:` lines.

- [ ] **Step 4: Verify isolation by inspection**

Confirm the new branch is guarded by `(tx_bits % 8) != 0` and sits inside the
`nfc_backend == NfcBackendSt25r3916` block after the ISO15693 handling. This guarantees:
byte-aligned frames (all standard NFC traffic) never enter it; the PN532 backend never
enters it; NFC-V is handled before it. So nothing that works today can change behavior.

- [ ] **Step 5: Checkpoint**

Confirm `SENTINEL_OK`. (No git in this tree.)

---

## Task 2: On-device verification (tester)

**Files:** none — flash + physical magic cards.

- [ ] **Step 1: Flash**

Merge + flash to the stick (COM10). App-only is enough:
`python -m esptool --chip esp32s3 -p COM10 -b 460800 --before default_reset --after hard_reset write_flash 0x10000 build_m5stick_s3\furi_esp32.bin`

- [ ] **Step 2: Confirm nothing regressed**

Boot log shows the ST25R3916 detected; open NFC → Read a normal MIFARE/NTAG card and
confirm it still reads (proves the byte-aligned path is unchanged).

- [ ] **Step 3: Phase 1 — Gen2/Gen4 clone (no code, baseline)**

`nfc_magic` app → write a saved `.nfc` dump onto a **Gen2 (CUID)** magic card → re-read
the magic card → UID + blocks match the source. (Expected to already work; this is the
baseline that the byte-aligned magic path is fine.)

- [ ] **Step 4: Phase 2 — Gen1a clone (this fix)**

`nfc_magic` app with a **Gen1a** magic card: it should now **detect** as Gen1a (the 7-bit
wakeup + 4-bit ACK succeed), then write a saved dump (UID / block-0 + blocks), then
re-read → matches the source.

- [ ] **Step 5: If Gen1a detects but writes are flaky**

The wakeup RX budget or the FDT/timers may need a small tune. In the Task-1 branch the
`20` (ms rx budget) passed to `st25r3916_hal_transceive_custom_parity` is the one knob;
try 30–50. Rebuild + re-flash. (Detection working but writes flaky points at timing, not
framing.)

- [ ] **Step 6: If Gen1a does NOT detect at all**

Then the 7-bit frame still isn't landing. Capture the boot/serial log while running the
Gen1a detect and check the `St25r3916` trace lines around the wakeup TX/RX (bit counts).
Report back — the fallback is a dedicated `st25r3916_hal_transceive_bits` with explicit
no-parity short-frame handling (the spec's original §4.1), but the custom-parity reuse
should work since it already sends exact raw bits.

---

## Notes for the implementer
- The whole code change is Task 1's one branch. Everything else is verification.
- Do NOT touch `st25r3916_hal_transceive` (byte path), the custom-parity function, or the
  `gen1a` poller — the fix is purely routing the sub-byte case to existing exact-bit code.
- Gen2 uses the standard MIFARE write (byte-aligned) and is not affected by this change
  either way; it's in the plan only as the "did I break anything" baseline.
