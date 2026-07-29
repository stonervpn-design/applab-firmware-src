# NRF24 Jammer "Detects But No Effect" — Driver Alignment + On-Air Verification

**Date:** 2026-07-11
**Board:** M5Stack Cardputer-ADV (ESP32-S3, `furi_esp32` Flipper port)
**Status:** Design — approved approach, pending implementation

## Problem

On the Cardputer-ADV the NRF24 app **detects** the module (probe passes) but jamming/flooding has **no observable on-air effect** (victim devices keep working). The user runs the same features successfully under Bruce on the T-Embed, and asked to "port the driver/logic" from the working `bruce-nrf-fix` tree.

## Key finding (why this is NOT a wholesale swap)

`applications/main/nrf24/nrf24_hw.c` is already a faithful raw-register port of Bruce/RF24:

- `nrf24_hw_jammer_start_ex` matches `RF24::startConstCarrier` almost line-for-line: CRC off (CONFIG=0x02), AutoAck off, retries off, 5-byte 0xFF TX addr, 32-byte dummy payload, `reUseTX` keep-alive, CE high after 1 ms.
- `nrf24_jam_presets.c` already has every Bruce band (WiFi/BLE/BLE-Adv/BT/USB/Video/RC/Zigbee/Full) **plus** extras Bruce lacks (Drone FHSS, per-preset CW-vs-Turbo strategy, dwell tuning).

A verbatim re-port would re-implement existing code and *lose* the extras. The real gap is narrower.

## Root-cause hypotheses (ranked)

1. **P-variant assumption.** RF24 selects its carrier path at runtime via `isPVariant()`. The nRF24L01**+** ("P") path uses the dummy-payload + `reUseTX` scheme (which `nrf24_hw.c` hard-codes for all modules); a **non-P clone** should instead use a plain `CONT_WAVE|PLL_LOCK` + CE-high carrier. If the Cardputer's module is a non-P clone, the hard-coded P path may not radiate.
2. **No data-rate negotiation.** Bruce calls `setDataRate(2M → 1M → 250k)` with fallback and re-reads `RF_SETUP`; `nrf24_hw.c` hard-codes 2 Mbps. A module that won't lock 2 Mbps CONT_WAVE emits nothing.
3. **Hardware** (antenna / PA / wiring on the specific module). Not code-fixable, but falsifiable by measurement.

## Approach (approved): instrument + measure

Do **not** rip out the existing driver. Make targeted, Bruce-faithful additions and verify on-air before/after.

### Components to change

1. **`nrf24_hw.c` — add `isPVariant()` detection.**
   Mirror RF24: attempt to set `RF_DR_LOW`, read back; if it sticks → P variant. Cache the result in `nrf24_hw_probe()`. Add a non-P CW path (`CONT_WAVE|PLL_LOCK` + CE high, no dummy/reUseTX) selected when the module reads as non-P.

2. **`nrf24_hw.c` — data-rate negotiation.**
   New helper `nrf24_hw_set_data_rate(rate)` that writes `RF_SETUP`, reads it back, and returns success. Jammer/flood start tries 2M → 1M → 250k, using the first that verifies. Expose the chosen rate for logging.

3. **`nrf24_hw.c` — `RF_SETUP`/`CONFIG` readback accessors** (or reuse `nrf24_hw_read_reg`) so the scene can log actual register state after setup.

4. **Jam scene instrumentation** (`scenes/scene_jam.c` + helpers): `FURI_LOG_I` on jam start — preset, strategy (CW/Turbo), chosen data rate, `isPVariant` result, `RF_SETUP`/`CONFIG` readback, and a throttled channel-hop heartbeat. Removed or gated behind a debug flag once fixed.

### Verification (the "measure" half)

- Recover the Cardputer USB (currently wedged — needs physical replug) and flash.
- Watch 2.4 GHz with the HackRF/PortaPack (or T-Embed spectrum) while running each jam mode; confirm whether a carrier/energy actually appears on the swept channels.
- Evidence decides the fix:
  - Carrier appears after adding non-P path / rate fallback → software fix confirmed.
  - No carrier under any config → hardware (antenna/PA/wiring), report back, no further code churn.

## Out of scope

- Rewriting the jam presets/scenes/UI (already superior to Bruce).
- MouseJack / promiscuous RX / targeted TX paths (not the reported symptom).
- The unrelated boot-log fixes (NFC GPIO sentinel, BQ I2C skip) — already implemented in this tree, pending flash.

## Success criteria

- `isPVariant` correctly identifies the Cardputer's module; the matching CW path is used.
- Jammer selects a data rate that verifies via `RF_SETUP` readback.
- HackRF/PortaPack shows real 2.4 GHz energy on the target channels while jamming — or, if not, the cause is positively identified as hardware.
