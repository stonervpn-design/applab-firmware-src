#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Low-level NRF24L01(+) helper.
 * Bus + CS are shared with the CC1101 / LCD / SD-Card on T-Embed -- callers must
 * surround SPI access with nrf24_hw_acquire() / nrf24_hw_release(). The CE pin
 * is handled exclusively by this module.
 */

void nrf24_hw_init(void);
void nrf24_hw_deinit(void);

void nrf24_hw_acquire(void);
void nrf24_hw_release(void);

void nrf24_hw_write_reg(uint8_t reg, uint8_t value);
uint8_t nrf24_hw_read_reg(uint8_t reg);

/* Multi-byte register write (e.g. RX_ADDR_Px, TX_ADDR). */
void nrf24_hw_write_buf(uint8_t reg, const uint8_t* data, uint8_t len);

/* Issue a single-byte command (FLUSH_RX / FLUSH_TX / NOP / REUSE). */
void nrf24_hw_cmd(uint8_t cmd);

void nrf24_hw_ce(bool high);

/* Power up the chip and run a register round-trip test.
 * Returns true if a module is present and responsive. Also detects the chip
 * variant (nRF24L01+ "P" vs non-P clone) — see nrf24_hw_is_p_variant().
 * Must be wrapped in acquire/release. */
bool nrf24_hw_probe(void);

/* True if the last probe identified a genuine nRF24L01+ ("P" variant), which
 * supports 250 kbps and needs the dummy-payload + REUSE_TX_PL constant-carrier
 * scheme. A non-P clone radiates CONT_WAVE with just CE high (no payload).
 * Only meaningful after a successful nrf24_hw_probe(). */
bool nrf24_hw_is_p_variant(void);

/* Data-rate code (0=1M, 1=2M, 2=250k) that the last jammer/flood start actually
 * negotiated and verified via RF_SETUP readback. Lets callers report what the
 * radio really locked onto instead of what was requested. */
uint8_t nrf24_hw_last_data_rate(void);

/* Raw readback of RF_SETUP / CONFIG — for instrumentation / diagnostics.
 * Must be wrapped in acquire/release. */
uint8_t nrf24_hw_read_rf_setup(void);
uint8_t nrf24_hw_read_config(void);

/* Power down (CONFIG = 0). Must be wrapped in acquire/release. */
void nrf24_hw_power_down(void);

/* Set RF channel (0..125). Must be wrapped in acquire/release. */
void nrf24_hw_set_channel(uint8_t channel);

/* Spectrum helper: tune to channel, listen briefly in RX, return RPD bit.
 * Must be wrapped in acquire/release. */
uint8_t nrf24_hw_listen_rpd(uint8_t channel);

/* Constant-carrier jammer: enable continuous wave on the given channel
 * at maximum PA power, 2 Mbps data rate, PLL locked.
 * Must be wrapped in acquire/release. CE is left HIGH on return. */
void nrf24_hw_jammer_start(uint8_t channel);

/* As nrf24_hw_jammer_start but with a selectable PA level (0..3 =
 * MIN/LOW/HIGH/MAX). Must be wrapped in acquire/release. */
void nrf24_hw_jammer_start_ex(uint8_t channel, uint8_t pa_level);

/* When true, the USB PA-safety clamp (nrf24_jam_pa_for_supply) is bypassed, so
 * the requested PA level is used verbatim even on USB. Default false (clamp on).
 * The Hydra RF board has an on-board 100µF array + dedicated 5V LDO, so full PA
 * on USB should hold — the Bruce jammer app sets this true (faithful PA_MAX +
 * real-world brownout test). Set false again on stop. */
void nrf24_hw_pa_unclamped(bool on);

/* Retune the running carrier to a new channel without reconfiguring RF_SETUP.
 * Must be wrapped in acquire/release. */
void nrf24_hw_jammer_set_channel(uint8_t channel);

/* Stop the constant carrier and put the chip back into standby.
 * Must be wrapped in acquire/release. */
void nrf24_hw_jammer_stop(void);

/* --- RPD reactive jammer (listen on RPD, fire CW only when a carrier appears).
 * All three run inside acquire()/release(). CE is the caller's responsibility.
 * Typical loop: reactive_setup(ch); ce(true); wait 130us; poll RPD; on hit ->
 * ce(false); react_to_tx(); ce(true); wait ~430us; ce(false); react_to_rx(next). */
void nrf24_hw_reactive_setup(uint8_t channel);
void nrf24_hw_react_to_tx(void);
void nrf24_hw_react_to_rx(uint8_t channel);

/* ---- Data-flooding jammer ----
 * Alternative strategy to the constant carrier: instead of an unmodulated
 * tone this floods the channel with full 32-byte ESB packets (AutoAck off,
 * CRC off, fixed address 0xE7E7E7) to cause packet collisions / CRC
 * corruption on the victim. Best for channel-specific protocols
 * (WiFi, BLE, Zigbee). Mirrors Bruce's writeFast() flooding mode. */

/* Configure the chip for TX flooding on the given channel.
 * low_rate selects 250 kbps (longer packets → block bigger time slices,
 * better against slow protocols) instead of the default 2 Mbps.
 * Must be wrapped in acquire/release. CE is left LOW. */
void nrf24_hw_flood_start(uint8_t channel, bool low_rate);

/* As nrf24_hw_flood_start but with explicit data rate (0=1M, 1=2M, 2=250k) and
 * PA level (0..3). Must be wrapped in acquire/release. CE is left LOW. */
void nrf24_hw_flood_start_ex(uint8_t channel, uint8_t data_rate, uint8_t pa_level);

/* Retune to a channel and fire one burst of random garbage packets (fills the
 * 3-deep TX FIFO and pulses CE). Used for channel-hopping flooding.
 * Must be wrapped in acquire/release. */
void nrf24_hw_flood_channel(uint8_t channel);

/* Retune to a channel and stream burst_count packets back-to-back (CE held
 * HIGH, FIFO topped up as it drains). When turbo is true three spectral
 * patterns are cycled (LFSR-varied random, all-ones, alternating 0x55/0xAA) —
 * burst_count packets per pattern — to defeat CRC filters and spread energy.
 * When false a single random-garbage burst is sent. Mirrors Bruce's
 * turboFloodChannel / floodChannel. Must be wrapped in acquire/release. */
void nrf24_hw_flood_burst(uint8_t channel, uint8_t burst_count, bool turbo);

/* Continuous single-channel flooding: top up the TX FIFO with random garbage
 * while it has room and hold CE HIGH, so the radio transmits back-to-back for
 * ~100% duty cycle. Call repeatedly in a tight loop (no channel change).
 * Must be wrapped in acquire/release. */
void nrf24_hw_flood_pump(void);

/* Stop flooding: power down, drop CE, flush TX FIFO.
 * Must be wrapped in acquire/release. */
void nrf24_hw_flood_stop(void);

/* ---- MouseJack: Promiscuous RX ---- */

/* Configure the chip for promiscuous reception:
 *   - 2-byte address width (illegal SETUP_AW=0 trick)
 *   - AutoAck off, CRC off, 2 Mbps, 32-byte fixed payload
 *   - All 6 RX pipes opened with noise-addresses to maximize hit chance.
 * After this call CE is HIGH and the chip is listening on the given channel.
 * Must be wrapped in acquire/release. */
void nrf24_hw_rx_start_promiscuous(uint8_t channel);

/* True if the RX FIFO has at least one packet ready.
 * Must be wrapped in acquire/release. */
bool nrf24_hw_rx_data_ready(void);

/* Read up to maxlen bytes from the RX FIFO. Returns the number of bytes read
 * (always equal to the configured payload size, typically 32).
 * Must be wrapped in acquire/release. */
uint8_t nrf24_hw_rx_read_payload(uint8_t* buf, uint8_t maxlen);

/* Stop listening, drop CE, leave chip in PWR_UP standby.
 * Must be wrapped in acquire/release. */
void nrf24_hw_rx_stop(void);

/* ---- MouseJack: Targeted TX ---- */

/* Configure for unicast TX to a given target address (5-byte ESB):
 *   - AutoAck off, no retries, 2 Mbps, max PA, CRC off
 *   - addr_len bytes copied to TX_ADDR and RX_ADDR_P0
 *   - Sets RX_PW_P0 to payload_len.
 * After this call the chip is in TX-standby (CE low, ready for write+pulse).
 * Must be wrapped in acquire/release. */
void nrf24_hw_tx_setup(
    const uint8_t* addr,
    uint8_t addr_len,
    uint8_t channel,
    uint8_t payload_len);

#ifndef PUBLIC_BUILD
/* As nrf24_hw_tx_setup, but with an explicit data rate and CRC mode instead of
 * the fixed 2 Mbps/CRC-off used by the public MouseJack path. For protocols
 * that require a specific PHY (e.g. Bayang-protocol toy drones: 1 Mbps,
 * 16-bit CRC on):
 *   - data_rate: 0=1 Mbps, 1=2 Mbps, 2=250 kbps (same encoding as elsewhere
 *     in this module).
 *   - crc16: true enables CRC (EN_CRC) in 16-bit mode (CRCO=1); false leaves
 *     CRC off entirely, matching nrf24_hw_tx_setup's default.
 * PA is left at max, same as nrf24_hw_tx_setup. Everything else (address,
 * RX_PW_P0, PWR_UP, PRIM_RX=0 for TX, CE handling) is identical.
 * Must be wrapped in acquire/release. PRIVATE-only: not present in public
 * builds. */
void nrf24_hw_tx_setup_ex(
    const uint8_t* addr,
    uint8_t addr_len,
    uint8_t channel,
    uint8_t payload_len,
    uint8_t data_rate,
    bool crc16);
#endif /* !PUBLIC_BUILD */

/* Send a single payload (no ACK, no retransmit). Caller is responsible for
 * vendor-specific retransmit looping. Returns true if the TX FIFO accepted
 * the write and the PLL settled.
 * Must be wrapped in acquire/release. */
bool nrf24_hw_tx_send(const uint8_t* data, uint8_t len);

/* Power down, drop CE.
 * Must be wrapped in acquire/release. */
void nrf24_hw_tx_stop(void);

/* Flush helpers — useful between TX bursts. */
void nrf24_hw_flush_rx(void);
void nrf24_hw_flush_tx(void);
