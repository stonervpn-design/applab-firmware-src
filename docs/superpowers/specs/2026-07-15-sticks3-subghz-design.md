# StickS3 SubGHz (CC1101) — Design

**Date:** 2026-07-15
**Board:** `m5stick_s3` (M5Stack StickS3 + pingequa RF Pack S3 2-in-1)
**Tree:** `_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15`
**Scope:** Core SubGHz only. Extra CC1101-dependent apps are a follow-up.

## Goal

Enable the CC1101 on the pingequa RF Pack S3 so the StickS3 port gets the
SubGHz app family (read, read raw, saved, frequency analyzer, TX).

## Why this is a config change, not new code

The HAL is already parameterized for this:

- `furi_hal_spi.c:36` — `#if defined(BOARD_SUBGHZ_SPI_HOST)` builds the bus
  config directly from the board defines.
- `furi_hal_spi.c:21` — `FURI_HAL_SPI_PIN_UNMAPPED = UINT16_MAX` is the stub
  path currently active ("SPI pins are not mapped yet; using stub responses").
- `furi_hal_subghz.c:415` — the CC1101 probe already exists.

The pin map is proven: it matches Bruce's `m5stack-sticks3` defaults (which work
on this hardware) and the older `_fzs3_work` tree. `m5stack_cardputer_adv` is a
working in-tree example of the same `SPI3_HOST` path.

The new tree has **no SD card** (`/ext` lives on internal flash), so SPI3 is
free. No CS-muxing with SD is needed — unlike the old tree and the ADV.

## Changes

### 1. `components/furi_hal/boards/board_m5stick_s3.h`

| Define | From | To |
|---|---|---|
| `BOARD_PIN_CC1101_SCK` | `UINT16_MAX` | `5` |
| `BOARD_PIN_CC1101_MISO` | `UINT16_MAX` | `4` |
| `BOARD_PIN_CC1101_MOSI` | `UINT16_MAX` | `6` |
| `BOARD_PIN_CC1101_CSN` | `UINT16_MAX` | `2` |
| `BOARD_PIN_CC1101_GDO0` | `UINT16_MAX` | `3` |
| `BOARD_SUBGHZ_SPI_HOST` | (undefined) | `SPI3_HOST` |
| `BOARD_HAS_SUBGHZ` | `0` | `1` |

`GDO2`, `SW0`, `SW1` stay `UINT16_MAX` (not wired).
`BOARD_CC1101_SPI_SHARED` stays `0` — it specifically means "shares the LCD
bus", and the LCD is on SPI2.

### 2. `fam_config.py`

Remove `m5stick_s3` from `_boards_without_subghz`, restoring `subghz`,
`cli_subghz`, `subghz_load_dangerous_settings`, `js_subghz`.

`_stick_drop` is intentionally left untouched: `proto_pirate`, `radio_scanner`,
`can_commander` and `defender` stay dropped until the radio is confirmed on
hardware. (`defender` was dropped *because* subghz was excluded, so it becomes
re-includable in the follow-up.)

### 3. Comments

Three sites assert "NO onboard CC1101 / NOT PRESENT" (header banner line ~13,
the SubGHz block ~line 130, and the `fam_config.py` exclusion comments). Correct
them to describe the RF Pack S3 on SPI3. A comment asserting the opposite of
reality is exactly what caused the 2026-07-15 black-screen bug (the M5PM1 LCD
rail); it is not cosmetic.

## Verification

The existing boot probe reads the CC1101 `PARTNUM`/`VERSION` registers.

- **Now (fail):** `CC1101 probe failed: part=0x00 version=0x00 status=0xFF
  reset_status=0xFF (all-zero reply...)`
- **Success:** `version=0x14` (real CC1101 ID), `status` no longer `0xFF`.

This is read from the boot log over COM10 — a hardware fact, not an inference.
The RF Pack S3 2-in-1 is attached, so this is testable immediately.

**If the probe still returns all-zero: STOP and investigate. Do not tune
settings.** Suspect #1 is a M5PM1 PMIC rail gating the RF Pack, mirroring the
LCD rail (GPIO2) fixed earlier today. Bruce drives these exact pins
successfully, so a rail is not expected — but that class of bug just bit us
once on this board.

## Risks

- **GPIO3 (GDO0)** is an ESP32-S3 strapping pin (JTAG select). Bruce uses it, so
  no issue expected; flag if boot behaviour changes.
- **Size:** app is 36% under the partition limit and PSRAM is live (~5.9MB
  free). Not a concern.

## Out of scope

nRF24 (CS=8/CE=1, also on SPI3, also excluded), and the CC1101-dependent apps.
Both are follow-ups gated on a passing probe.
