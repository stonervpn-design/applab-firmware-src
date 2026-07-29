# StickS3 nRF24 — Design

**Date:** 2026-07-15
**Board:** `m5stick_s3` (M5Stack StickS3 + pingequa RF Pack S3 2-in-1)
**Tree:** `_fzcard_work/Flipper-Zero-meets-M5Stack-Cardputer-1.15`
**Follow-up to:** `2026-07-15-sticks3-subghz-design.md`

## Goal

Enable the nRF24L01+ on the pingequa RF Pack S3, alongside the CC1101 already
running on SPI3.

## Why this is a config change, not new code

Everything is already wired, just fed `UINT16_MAX`:

- `furi_hal_resources.c:54` — `gpio_nrf24_cs` ← `BOARD_PIN_NRF24_CSN`
- `furi_hal_resources.c:62` — `furi_hal_nrf24_ce_pin` ← `BOARD_PIN_NRF24_CE`
- `furi_hal.c:118-128` — already drives NRF24 CSN HIGH / CE LOW at boot
- `furi_hal_spi.c:85` — `furi_hal_spi_bus_handle_nrf24` already exists with
  `.bus = &furi_hal_spi_bus_subghz` and `.cs = &gpio_nrf24_cs`

`m5stack_cardputer_adv` is a working in-tree precedent: its nRF24 shares the
CC1101's SPI3 pins and differs only in CSN/CE.

Note the `#ifdef BOARD_PIN_NRF24_*` guards are already true today (the pins are
*defined*, just as `UINT16_MAX`), which is why the boot log currently prints
`NRF24_CSN GPIO65535 set HIGH`. Setting real values makes that code do its job.

## Changes

### 1. `components/furi_hal/boards/board_m5stick_s3.h`

| Define | From | To |
|---|---|---|
| `BOARD_HAS_NRF24` | `0` | `1` |
| `BOARD_PIN_NRF24_CSN` | `UINT16_MAX` | `8` |
| `BOARD_PIN_NRF24_CE` | `UINT16_MAX` | `1` |
| `BOARD_PIN_NRF24_SCK/MISO/MOSI` | `UINT16_MAX` | `5`/`4`/`6` (documentation only) |

`SCK`/`MISO`/`MOSI` are consumed by nothing — `furi_hal_resources.c` builds the
bus pins (`gpio_ext_pb3/pa6/pa7`) from the **CC1101** defines, and the nRF24
handle reuses them. They are set to the true wiring and commented as unused, to
record the physical layout without implying they are live.

### 2. `fam_config.py`

Remove `m5stick_s3` from `_boards_without_nrf24`, restoring the `nrf24` app.

## Bus sharing

Both radios share `furi_hal_spi_bus_subghz`; its recursive mutex
(`furi_hal_spi_acquire`) serializes CC1101 vs nRF24 transactions. Each has its
own CS (`CC1101 CSN=2`, `nRF24 CSN=8`), so they are parallel-attached and
CS-muxed on one host.

`init_early` drives nRF24 CSN HIGH (deselected) and CE LOW (standby) before any
CC1101 traffic. The in-tree comment documents the failure mode this prevents:
the nRF24 otherwise sees CC1101 traffic and corrupts the bus, showing up as a
*stuck ~312 MHz reading in the Frequency Analyzer and total RX failure*.

The nRF24 handle runs at 2 MHz, already tuned down from 4 MHz because pingequa
plug-in modules were marginal at 4 MHz (connector capacitance) — directly
relevant here, as this is a pingequa module.

## Verification

No nRF24 boot probe exists, and we deliberately are not adding one: it would be
new code touching the shared bus during boot, right after the CC1101 was brought
up. The app probes on launch instead.

1. **Open the nRF24 app.** `nrf24_app.c:145` calls `nrf24_hw_probe()`, which
   writes `RF_CH=0x55` then `0x2A` and reads back (`nrf24_hw.c:129-134`).
   - Success: app opens.
   - Failure: "NRF24 Not Available" dialog + `NRF24 not detected, aborting app
     launch` (`FURI_LOG_W`, visible on serial).
2. **Regression-check SubGHz.** Confirm `CC1101 probe OK ... version=0x14` still
   appears at boot and the Frequency Analyzer is not stuck. Adding a second
   device to a shared bus is exactly the change that could break the CC1101.

## Out of scope

NFC. The CC1101-dependent apps (`proto_pirate`, `radio_scanner`,
`can_commander`, `defender`) remain in `_stick_drop` pending their own pass.
