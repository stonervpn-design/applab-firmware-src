# On-Device MIFARE Classic mfkey Recovery Test Checklist

This document describes the hardware and software validation steps for the on-device mfkey32 key-recovery feature.

## Status

- **Crypto**: Host-vector-verified (vendored crapto1, 112 KB RAM bounded)
- **Hardware**: NFC frontend currently **PN532-only**; end-to-end validation **pending ST25R3916 NFC driver** (separate planned effort)

---

## Test Checklist

### Nonce Collection (Detect Reader)

- [ ] Connect PN532 NFC reader (or compatible NFC-A frontend once ST25R3916 driver is available)
- [ ] Navigate to **NFC → Detect Reader** in the app
- [ ] Present a MIFARE Classic card to the reader
- [ ] Confirm reader triggers and detects the card (UID, ATQA, SAK logged)
- [ ] Perform a sector/key authentication attempt → reader logs nonce pair
- [ ] Repeat with a second authentication on **the same sector/key** (mfkey32 requires two nonce samples)
- [ ] Confirm two nonce pairs are collected and visible in the Detect Reader log
- [ ] *(Pending ST25R3916)*: Repeat with M5Stack ST25R3916 unit once driver is available

### Recovery Flow

- [ ] Exit Detect Reader with collected nonces (via **Back**)
- [ ] Navigate to **NFC → (nonce file)** to open the nonce capture
- [ ] Select **Recover Keys** to start on-device recovery
- [ ] Confirm **Progress screen** appears showing:
  - X / Y (current key index / total keys to recover)
  - Running key count and elapsed time
  - **Back** button available to cancel

### Recovery Completion

- [ ] Allow recovery to run to completion (up to ~1 minute per key)
- [ ] Confirm completion message shows **number of keys recovered**
- [ ] If keys recovered, confirm **"Save to user dictionary"** option is available
- [ ] Select save and confirm keys are written to `/ext/nfc/user_dict`
- [ ] Verify saved keys unlock the original card in **NFC → Read** mode
- [ ] Confirm de-duplication: no duplicate keys in the dictionary after save

### Edge Cases & Robustness

- [ ] **Back mid-recovery**: Press Back during the progress screen → confirm recovery is **cleanly cancelled** with no freeze or crash
- [ ] **Low-memory skip**: With free heap < 112 KB, confirm recovery skips the problematic key set and **logs the skip count** in the progress display (no crash or hard failure)
- [ ] **None recovered**: If no keys are recovered (e.g., invalid nonce format), confirm a clear message is shown ("No keys recovered")
- [ ] **Empty nonce file**: Attempt to run recovery on an empty or malformed nonce file → confirm graceful error message
- [ ] **Screen timeout**: Confirm screen stays on during recovery; no idle timeout interruption

### Validation Notes

- The on-device recovery algorithm is **chip-independent** and has been host-vector-verified against known test keys.
- This checklist covers **software correctness** and **user-flow robustness**.
- **End-to-end hardware validation** (collection + recovery + unlock) is **pending ST25R3916 NFC driver** support, as the current firmware is PN532-only.
- Once the ST25R3916 driver is available, repeat the full checklist with that hardware unit.

---

## Known Limitations

- **NFC frontend**: PN532 over I2C is currently supported; ST25R3916 driver is a separate planned effort.
- **Recovery time**: ~1 minute per MIFARE Classic key is expected with the vendored crapto1 algorithm.
- **RAM constraint**: Recovery is bounded to ~112 KB; recovery runs on a background worker thread to prevent UI freeze.

---

## Rollback / Revert

If the recovery feature causes issues:

1. Remove recovered keys from `/ext/nfc/user_dict` manually (via SD card file browser).
2. Restore a backup of the user dictionary if available.
3. Report the issue with nonce file samples and full debug logs.
