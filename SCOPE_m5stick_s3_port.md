# Port Scope — "mob" firmware → M5Stack **StickS3**

> Feasibility + safe, phased bring-up plan for porting this ESP32-S3 Flipper port to the
> M5Stack **StickS3** (ESP32-S3-PICO-1-N8R8). Written 2026-07-14. Companion to
> GAP_ANALYSIS_faps.md / SCOPE_claude_buddy.md. Research-only; nothing built yet.

## TL;DR verdict
**Feasible, and in several ways EASIER + better than the Cardputer ADV.** The StickS3 is
an ESP32-S3 with **8 MB PSRAM** — so the whole toolchain/furi/NimBLE stack applies
directly, and PSRAM **removes the RAM ceiling** that dominated recent work (BLE OOM, the
Subhound 36 KB fight). Its display (ST7789 135×240), audio (ES8311), and IMU (BMI270)
are **already supported** by the Cardputer-ADV board profile. Only **three** items are
real new engineering: **input** (2 buttons vs Flipper's 6 keys), **storage** (LittleFS on
internal flash — no SD; clean because the port routes all `/ext` through one mount point),
and the **M5PM1 PMIC**. Radios (Sub-GHz/nRF24/NFC) are external add-ons via the HAT header.

## 1. Target hardware — M5StickS3 (verified from m5-docs)
- **SoC:** ESP32-S3-PICO-1-N8R8 (Xtensa LX7 dual-core, 240 MHz), **8 MB flash + 8 MB octal PSRAM**
- **Display:** ST7789**P3**, **135×240 portrait**, pins MOSI=G39 SCK=G40 RS/DC=G45 CS=G41 RST=G21 **BL=G38 (plain GPIO)**
- **Buttons:** only **2** — KEY1=**G11**, KEY2=**G12** (+ power button w/ single=reset, double=off, long=download)
- **IMU:** BMI270 6-axis, I2C SDA=G47 SCL=G48
- **Audio:** ES8311 codec (I2S: MCLK=G18 BCLK=G17 LRCK=G15 DOUT=G14 DIN=G16) + AW8737 amp + MEMS mic
- **IR:** native **TX=G46, RX=G42**
- **PMIC:** **M5PM1** (I2C 0x6e) — *not* AXP192
- **Battery:** 250 mAh
- **Storage:** **NO microSD/TF slot**
- **Expansion:** Grove Port.A (G9/G10, HY2.0-4P) + **HAT2-Bus 16-pin** header exposing G5,G4,G6,G1,G7,G8,G43,G44,G2,G3 (+Boot,BAT,5V,3V3) — where an external RF module would attach (no SD; storage is internal LittleFS)
- **Radio:** WiFi 2.4 GHz + BLE 5 (ESP32-S3 built-in). **No onboard CC1101 / nRF24 / NFC.**

## 2. How this port adds a board (from board-abstraction analysis)
`FLIPPER_BOARD` (CMake var + env + `BOARD_INCLUDE` macro) drives everything. Adding a
board is **5 artifacts**, very little core C:
1. `components/furi_hal/boards/board_<board>.h` — the pin/feature source of truth (`#include`d by `boards/board.h`, which `#error`s without `BOARD_NAME`/`BOARD_TARGET`/`BOARD_PIN_LCD_MOSI`).
2. `components/input/targets/<board>/target_input.c` — implements `target_input_init()` + `target_input_poll()`. **Missing → FATAL_ERROR at configure.**
3. `sdkconfig.defaults.<board>` — layered over `sdkconfig.defaults` + auto `sdkconfig.defaults.esp32s3`.
4. `fam_config.py` — add the board string to `_boards_without_{subghz,nrf24,lfrfid,wolf3d,ir,nfc}` sets for whatever hardware it lacks (app-list gating is Python, not C).
5. A build script (copy `build_adv.bat`; change `-DFLIPPER_BOARD`, `-B` dir, `SDKCONFIG_DEFAULTS`, merge-bin name).

The shared HALs (`furi_hal_display.c`, `_resources.c`, `_spi.c`, `_speaker.c`, `_power.c`,
`_subghz.c`, `input.c`) read board macros generically and **do not** need edits unless the
hardware needs a code path that doesn't exist yet.

## 3. What's already handled (StickS3 ≈ Cardputer ADV)
Start by copying the **`m5stack_cardputer_adv`** header + sdkconfig (closest match), then:
- **Display:** ST7789 driver is generic; set `BOARD_LCD_H_RES=135 V_RES=240`, `GAP_X=40 GAP_Y=53`, `SWAP_XY/MIRROR_*` for portrait, `BOARD_PIN_LCD_*` to the StickS3 pins, `BOARD_PIN_LCD_BL=G38` (active-high GPIO). The 128×64 canvas is aspect-fit scaled automatically.
- **Audio:** set `BOARD_HAS_SPEAKER 1` + `BOARD_HAS_ES8311 1` and the I2S pins (G14-18). The ES8311 path already exists in `furi_hal_speaker.c`.
- **IMU:** BMI270 already supported (Cardputer ADV) — I2C on G47/G48.
- **IR:** `BOARD_HAS_IR/IR_TX/IR_RX 1`, `BOARD_PIN_IR_TX=46 BOARD_PIN_IR_RX=42`.
- **BLE/WiFi:** ESP32-S3 built-in; keep NimBLE. **Enable PSRAM** (pull `CONFIG_SPIRAM=y` etc. from the T-Embed `sdkconfig.defaults.esp32s3` profile rather than the Cardputer's `SPIRAM=n`).

## 4. The three real problems + safe solutions

### A. Input — 2 buttons for 6 Flipper keys (the #1 UX problem)
Flipper needs Up/Down/Left/Right/OK/Back. StickS3 has KEY1(G11)+KEY2(G12).
**Recommended scheme (write in `targets/m5stick_s3/target_input.c`):**
- **BMI270 tilt → directional** Up/Down/Left/Right (poll accel, threshold + debounce). The IMU driver already exists.
- **KEY1 short = OK**, **KEY1 long = Back**, **KEY2 = context/menu** (or KEY2 short = Back, long = app-exit).
- Fallback if tilt feels bad: KEY1 = cycle/next, KEY2 = select, both-long = back (Bruce's 2-button model).
- Copy the publish/long-press scaffolding from `targets/m5stack_cardputer_adv/target_input.c` (`publish_nav`, long-press logic). Text-input-heavy apps (BadUSB script editing, etc.) will be awkward — acceptable for v1.
**Prior art:** Bruce firmware runs on the StickC form with 2-button nav — reference its input handling.

### B. Storage — LittleFS on internal flash (NO SD card — confirmed by user)
The StickS3 has **no microSD and none is wanted — storage is LittleFS-only on internal
flash.** This is architecturally clean here because of how the port already works:
- **Single mount point.** `components/storage/storage.c` routes *everything* through
  `SD_MOUNT_POINT "/sdcard"`: `/ext/foo → /sdcard/foo`, `/int → /sdcard/.int`, and all file
  ops gate on one `sd_mounted` bool. So the backend swap is localized.
- **`/int` is already NVS, not the card.** SavedStruct maps `/int/*` (notification/bt/
  desktop/power settings, modpins) to NVS keys (seen in boot log:
  `/int/.notification.settings -> NVS key`). So **settings persist via NVS regardless** —
  LittleFS only has to serve `/ext` bulk assets (IR DBs, subghz, apps_assets, NFC dicts).
- **The swap:** on StickS3, replace `furi_hal_sd_mount()`'s FATFS-on-SD (`f_mount`, lines
  ~738) with `esp_vfs_littlefs_register()` mounting a flash data partition at base path
  `/sdcard`. `storage_sd_mount()` / `sd_mounted` / the `/ext` mapping all stay unchanged.
  Add the **`joltwallet/esp_littlefs`** managed component (Bruce uses it; not yet in this
  port's build). Since internal flash is always present, storage **always mounts** →
  `sd_mounted` true after boot, **no removable-card UX, no "SD Bad" path**.
- **Partition:** the tree's `partitions_multiboot.csv` already declares a `spiffs`/LittleFS
  data partition (the Bruce concept) — reuse that layout. On 8 MB: ~3 MB `factory` app +
  bootloader/nvs/coredump (~0.2 MB) leaves **~4.5 MB for a `littlefs` data partition.**
- **Capacity:** full apps_assets (~7 MB) + full 985-file IRDB won't fit. But a base StickS3
  build **gates off subghz/nrf24/nfc** (no onboard radio), which drops most of those assets,
  and PocketLab content is compiled into flash (not assets). Ship a **trimmed** set:
  universal IR (`tv.ir/ac.ir/...` + a few brands, ~1 MB) + essential apps_assets → fits ~4 MB.
- **Pre-populate:** build a LittleFS image with `mklittlefs`/`littlefs-python` from a staging
  dir at build time, and `esptool write_flash <littlefs_offset> littlefs.bin` alongside the
  app (internal-flash analogue of the SD image). Or ship empty + first-boot defaults.

### C. Power — M5PM1 PMIC (I2C 0x6e)
`furi_hal_power.c` knows BQ25896/BQ27220-over-I2C or `BOARD_POWER_SKIP_I2C`+ADC. M5PM1 is
neither. **Safe v1:** `#define BOARD_POWER_SKIP_I2C` + a fixed/fake battery capacity (the
backlight is a plain GPIO here, so — unlike AXP192 StickCs — the PMIC is NOT needed to
light the display or bring up rails). **Later:** write a small M5PM1 read driver for real
battery %/charging.

## 5. Radios (Sub-GHz / nRF24 / NFC) — external only
No onboard RF. Two options:
- **v1: gate them off** — add `m5stick_s3` to `_boards_without_{subghz,nrf24,lfrfid,nfc}`
  in `fam_config.py`; set the `BOARD_PIN_CC1101_*`/`NRF24_*` to `UINT16_MAX`. You get
  WiFi/BLE + IR + games/tools/JS — a legit "Flipper-lite."
- **later: external CC1101 + nRF24 on the HAT2-Bus header** (SPI on G5/G4/G6/G1/G7/G8…),
  define the pins + `BOARD_HAS_SUBGHZ/NRF24 1` + `BOARD_CC1101_SPI_SHARED` or a dedicated
  host, and remove from the exclusion sets. NFC via an I2C unit reuses the existing
  `rfid2_reader` (WS1850S) / PN532 paths. 125 kHz LFRFID + iCLASS stay impossible (no LF).

## 6. Safe, phased bring-up (each phase independently testable)
- **Phase 0 — boots + display + minimal input + storage.** board header (all pins), sdkconfig
  (PSRAM on, 8 MB, custom partition w/ LittleFS), `target_input.c` (2 buttons only), storage
  remap to flash FS, `BOARD_POWER_SKIP_I2C`. Radios/NFC gated off. **Goal:** reaches the
  desktop, screen renders, 2-button nav works, a game launches. This is the risky part;
  once it boots you have a device.
- **Phase 1 — input polish:** add BMI270 tilt directions.
- **Phase 2 — IR:** enable native TX/RX (a real Flipper feature, onboard). Trim IR DB into flash.
- **Phase 3 — BLE + WiFi:** enable (PSRAM makes it comfortable) — BLE spam/walk, wlan_app, Evil Portal.
- **Phase 4 — audio:** ES8311 notification tones.
- **Phase 5 — external RF (optional):** CC1101+nRF24 on HAT header; NFC via I2C unit.
- **Phase 6 — PMIC:** real M5PM1 battery/charge driver.

## 7. Risks / unknowns
- **ST7789P3 vs ST7789/V2 init:** P3 may need a slightly different init sequence/offsets;
  verify against an M5 StickS3 example (Bruce/UIFlow) if the panel is garbled.
- **LittleFS-as-/ext:** `storage.c` currently assumes the `/sdcard` VFS mount; the remap +
  a few apps that hardcode SD assumptions need testing (archive, subghz save).
- **Tilt input UX:** may feel awkward; keep the 2-button fallback ready.
- **PSRAM cache + shared SPI:** if an external RF module is added later, mind SPI/PSRAM
  bus timing (the Cardputer's shared-SPI CS discipline applies).
- **Flash budget:** app (~3 MB) + LittleFS data (~3 MB) is tight on 8 MB; keep the IR/subghz
  DBs trimmed.

## 8. Prior art / references
- **Bruce firmware** (BruceDevices/firmware) runs on M5StickC-Plus2 / Sticks: LittleFS
  storage, 2-button nav, ES8311, M5 PMIC handling — the reference for storage + input +
  power on this exact form factor. This port already dual-boots with Bruce on the T-Embed
  (`patchBruce.py`), so Bruce board files are familiar territory.
- In-tree template: **`m5stack_cardputer_adv`** board (ST7789 135×240 + ES8311 + BMI270)
  is the closest existing profile — copy it and swap pins/input/storage/PMIC.

## 9. Effort estimate
- Phase 0 (boot/display/input/storage): the bulk — a few days incl. LittleFS remap + input driver.
- Phases 1–4: ~1–2 days each, independently.
- External RF (Phase 5): depends on wiring an add-on; the HAL supports it.
Net: a **real board bring-up (~1–2 weeks)**, not a config tweak — but low architectural
risk because the SoC/display/audio/IMU already match an existing profile and the two hard
problems (input, storage) have proven solutions (Bruce).

---
### Parting note (unrelated bug) — DIAGNOSED + FIXED 2026-07-14
The beta tester's **"changing GPIO pins crashes the device"**. My first guess (unguarded
`furi_hal_spi_bus_handle_deinit`) was **WRONG** — that deinit, plus the whole SPI acquire/
init/tx path and every `furi_hal_gpio_*` call, are already NULL/validity-guarded. Real
root cause: the Pin Config app lets you remap a movable RF/IR pin onto a GPIO already owned
by a **live bus line** (SPI SCK/MISO/MOSI — shared with the SD on SPI3) or another function;
`pin_config_item_changed` then immediately re-probes, driving that line and hanging/
corrupting the shared bus → watchdog reboot. **Fix:** added `pin_config_pin_conflict()`
(store) + a guard in `pin_config_item_changed` that rejects the collision (reverts the UI,
skips apply) and logs every apply as `[I][PinConfig] apply <func>=GPIO<n>`. Known remaining
gap: the SD **chip-select** pin isn't a tracked PinFunc, so remapping specifically onto it
isn't blocked yet — the apply-log will reveal it if that's the tester's exact case (add SD
CS to the reserved set then).
