# Cardputer-ADV Flipper port — on-device verification (after the pin/NVS merge)

Merged firmware: `Flipper-cardputer_adv-merged.bin` (built 2026-07-13 from the unified
`nimble-port` tree = your work tree + adam's pin/NVS + device_name, with the SubGhz
tx_marker fix and jammer-UI fix preserved).

## 0. FIRST: clear the wedged USB-Serial/JTAG
The CDC got wedged during diagnostics (PermissionError 31). **Physically unplug and
replug the Cardputer** before any flash/serial. Confirm it re-enumerates on COM12.

## 1. Confirm what was actually running (root-cause proof)
Before flashing the new build, read the OLD firmware's build date to prove the
"unflashed fixes" theory:
```
python -m esptool --chip esp32s3 --port COM12 --before default-reset --after hard-reset \
  --no-stub -b 115200 read-flash 0x10000 0x120 old_appdesc.bin
```
Parse offset 0x20: magic 0xABCD5432, then version[32]/proj[32]/time[16]/date[16].
Expectation: an OLD build date (before the pin-NVS / tx_marker work) — confirming the
device never got the recent fixes.

## 2. Flash the unified firmware
```
python -m esptool --chip esp32s3 --port COM12 -b 460800 --after hard-reset \
  write_flash 0x0 Flipper-cardputer_adv-merged.bin
```
(If esptool default reset wedges the CDC again, replug and retry. Avoid `--after no-reset`.)

## 3. THE reported bug — pin config persistence
1. Open **Pin Config** app. Note the header shows `CC1101:Y/X  NRF24:Y/X` (live probe).
2. Change **IR TX** to a different GPIO. You should see the value update.
3. Power-cycle the device (full reboot).
4. Re-open Pin Config. **PASS = IR TX still shows your new value.**
   FAIL = reverts to 44 (default).
5. While doing step 2, capture serial to see the new diagnostic line:
   open COM12 @115200, then in the app change a pin and watch for:
   - `I (…) PinConfig: saved+applied: cc1101 csn=… …`  (the save ran)
   - `E (…) modpins: nvs_open(RW) failed: …` or `nvs_commit failed …`  (NEW — only prints on real failure)
   If you see `saved+applied` and NO `modpins` error → NVS write succeeded.

## 4. Re-dump NVS to confirm persistence (definitive)
After changing a pin and rebooting:
```
python -m esptool --chip esp32s3 --port COM12 --before default-reset --after hard-reset \
  --no-stub -b 115200 read-flash 0x9000 0x6000 nvs_after.bin
```
Then check the raw bytes contain `modpins` and `csn`/`gdo0`/`irtx`:
```
grep -a -c modpins nvs_after.bin    # should be >= 1 now
```
BEFORE the merge/flash this was 0 (namespace absent) — that was the smoking gun.

## 5. SubGhz TX (was silently broken by a const RMT payload)
- **Jammer:** start it; the graph should only animate if TX actually started. Verify
  with a HackRF sweep that the band lifts (Turbo/flood mode).
- **.sub replay / brute / playlist:** send a known .sub; confirm it transmits.
- If TX still dead: capture serial for `rmt: … payload not in internal RAM` (should be
  GONE now — the fix makes tx_marker non-const → internal RAM).

## 6. Known caveats to check while testing (design notes, not yet changed)
- Pin Config still keeps TWO stores: SD file (`/ext/pin_config/pins.conf`, what the app
  DISPLAYS) and NVS (`modpins`, what the HARDWARE uses at boot). If they ever diverge
  (one write fails) the UI and hardware disagree. If step 3 shows the value persisting
  in NVS but the UI not matching, that's the dual-store split — say so and I'll collapse
  it to a single NVS source of truth.
- **SPI SCK/MISO/MOSI and I2C SDA/SCL rows are cosmetic** — the SPI3 bus (shared with the
  SD card) and I2C are initialized from compile-time board macros, so changing those rows
  saves a number that has no hardware effect. If you don't want to mislead, those rows
  should be removed or marked read-only. Only IR TX/RX, CC1101 CSN/GDO0, NRF24 CSN/CE
  are genuinely movable.

## Adam-only features NOT merged (decide later)
- `bw16_r4tkn` (BW16/RTL8720 dual-band deauther over UART)
- `desktop_shortcuts` (+ its desktop_settings / notification changes)
- adam's older ProtoPirate (your tree's v3.2 re-port is newer and was kept)
