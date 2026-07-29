# NFC Magic-Card Cloning (Gen1a 7-bit frame) — M5Stick S3 — Design Spec

**Date:** 2026-07-28
**Board:** `m5stick_s3` (ESP32-S3, tree `_fzcard_work/NEW_v2/src_m5stick_s3`), NFC via the
ST25R3916 backend (M5 NFC2 @ Grove I2C 0x50) — and the PN532 backend @ 0x24.
**Status:** Approved design — ready for implementation plan

## 1. Purpose

Enable **magic-card cloning** on the M5Stick: write a saved `.nfc` dump (UID + all
blocks) onto a magic card so a captured access card can be duplicated. This is the first
item of the NFC-attack roadmap. It is **reader-side**, so it is NOT blocked by the
ST25R3916's I2C timing wall that makes crypto card-emulation (mfkey32 etc.) infeasible.

## 2. Current state (already in the tree)

- The **`nfc_magic` app** (`applications_user/nfc_magic/`) with **Gen1a / Gen2 / Gen4 /
  USCUID** pollers already builds and is kept on the StickS3 (`fam_config.py`).
- MIFARE Classic read/write + dictionary attack + NTAG/UL/MIFARE writes are confirmed
  working on the ST25R3916 backend (see `[[fzcard-st25r3916-nfc-backend]]`).

**Feasibility by magic type on the ST25R3916 backend:**
- **Gen2 (CUID):** block-0 written via a standard MIFARE auth+write → uses the
  already-working custom-parity Crypto1 write path → **expected to already work**
  (Phase 1 confirms this).
- **Gen4 / USCUID:** standard CRC-framed config commands → expected to work via the
  normal transceive path.
- **Gen1a:** **BROKEN — the gap this spec fixes** (see §3).

## 3. Root cause of the Gen1a gap

The Gen1a poller (`magic/protocols/gen1a/gen1a_poller_i.c`) starts with a **7-bit magic
wakeup**: `bit_buffer_set_byte(tx, 0, 0x40)` then `nfc_poller_trx(...)` with a 7-bit
frame (then `0x43`, then CRC'd raw block read/write commands). A 7-bit short frame like
this carries **no parity** (same class as REQA).

`nfc_poller_trx` (`components/nfc/nfc.c:487`) passes the frame size **in bits**:
`furi_hal_nfc_poller_tx(bit_buffer_get_data(tx), bit_buffer_get_size(tx))`. But the
ST25R3916 branch of `furi_hal_nfc_poller_tx` (`furi_hal_nfc.c` ~line 1403) does
`tx_bytes = (tx_bits + 7) / 8` and transmits **whole bytes** via
`st25r3916_hal_transceive`. So the 7-bit `0x40` wakeup is sent as a **full 8-bit byte**,
the magic card never recognizes the wakeup, and Gen1a detection/write fails.

(The subsequent CRC'd Gen1a commands go through `iso14443_3a_poller_txrx_custom_parity`
→ `furi_hal_nfc_iso14443a_poller_tx_custom_parity`, which the ST25R3916 backend already
implements — so they are fine once the wakeup works.)

## 4. Fix design

Add **sub-byte (bit-granular) TX** to the ST25R3916 poller path so a frame whose bit
count is not a multiple of 8 is transmitted with the exact number of bits and no parity.
The ST25R3916 already supports this in hardware (it is how the short-frame REQA is sent):
the FIFO holds the byte(s), `NUM_TX_BYTES` sets the byte count, and the **NTX / "bits in
last byte"** field sets how many bits of the final byte are sent.

### 4.1 New driver primitive — `st25r3916_hal.c/.h`
```
St25rHalStatus st25r3916_hal_transceive_bits(
    const uint8_t* tx, uint16_t tx_bits,
    uint8_t* rx, uint16_t rx_cap, uint16_t* rx_len);
```
- Disable parity for TX and RX (`ISO14443A_NFC.no_tx_par | no_rx_par`) — a short frame
  has none. (Restore the register after, mirroring the existing custom-parity path.)
- Load `ceil(tx_bits/8)` bytes into the FIFO; program `NUM_TX_BYTES` with that byte count
  and the last-byte bit count `= tx_bits % 8` (0 ⇒ full byte).
- `TRANSMIT_WITHOUT_CRC` (software owns CRC; the wakeup has none).
- Capture the response with the **exact** RX bit count via `FIFO_STATUS2.fifo_lb` (the
  same mechanism the existing `st25r_transceive` uses to report the 4-bit MIFARE ACK) —
  the Gen1a wakeup answers with a 4-bit ACK (`0x0A`), so exact bit reporting is required.
- Return staged via `st25r_stage_result_bits` so the poller sees the true bit length.

Model it closely on the existing `st25r3916_hal_transceive` /
`st25r3916_hal_transceive_custom_parity` (buffer sizes, TXE/RXE hybrid-poll waits,
FIFO handling) — do not invent a new wait/timeout scheme.

### 4.2 Route sub-byte frames — `furi_hal_nfc.c` `furi_hal_nfc_poller_tx`
At the top of the ST25R3916 branch, before the existing byte-count transceive:
```
if(nfc_current_tech != FuriHalNfcTechIso15693 && (tx_bits % 8) != 0) {
    /* sub-byte short frame (e.g. Gen1a 7-bit 0x40 magic wakeup) */
    ...call st25r3916_hal_transceive_bits, stage exact rx bits...
    return ...;
}
```
This branch triggers **only** for non-byte-aligned frames — which today don't work at
all — so every byte-aligned read/write/auth path is byte-for-byte unchanged. That is the
core de-risk.

### 4.3 No PN532 change needed for this step
Gen1a on PN532 is out of scope here (the PN532's InDataExchange/InCommunicateThru path
handles framing differently; if wanted it's a separate follow-up). This spec targets the
ST25R3916 backend.

## 5. Phased delivery

- **Phase 1 — verify existing (no code):** tester confirms **Gen2 (and Gen4/USCUID)**
  cloning works on the current public build (write a saved dump onto a Gen2/CUID magic
  card, check UID + blocks). If yes, the common-case capability already ships.
- **Phase 2 — Gen1a fix (this spec's code):** implement §4; tester verifies cloning onto
  a **Gen1a** magic card (detect → write UID/block-0 + blocks → re-read matches).

## 6. Testing

- **What I can verify:** it compiles for the target, boots clean (boot log), the sub-byte
  branch is isolated (byte-aligned frames unchanged), and normal NFC read still works on
  the attached ST25R3916 (boot-log detection + a normal read if the tester does one).
- **What only the tester can verify (I can't drive NFC cards):** Phase 1 Gen2/Gen4 clone;
  Phase 2 Gen1a detect + clone + re-read on a real Gen1a magic card. Register values
  (NTX field, no-parity bits) may need one on-device tuning pass — expose them as named
  constants so a tweak is one line.

## 7. Files touched
- **Modify** `components/furi_hal/st25r3916/st25r3916_hal.c` + `.h` — add
  `st25r3916_hal_transceive_bits` (+ no-op stub in the `!BOARD_HAS_ST25R3916` branch).
- **Modify** `components/furi_hal/furi_hal_nfc.c` — route sub-byte frames in
  `furi_hal_nfc_poller_tx` (ST25R3916 branch).
- No changes to `nfc_magic` app code (the gap is purely in the HAL frame path).

## 8. Non-goals
- No PN532 Gen1a path (separate follow-up if wanted).
- No mfkey32 / nested / crypto-emulation (I2C timing wall — separate feasibility work).
- No new UI — the existing `nfc_magic` app drives everything.

## 9. Open items (resolve on-device with the tester)
- The exact ST25R3916 register for the "bits in last byte" and no-parity bits, and
  whether the wakeup RX needs a specific timeout — confirm against the working REQA path
  in the same driver; expose as named constants for a one-line tuning pass.
- Confirm Gen2/Gen4 already work in Phase 1 before investing in Gen1a (they should).
