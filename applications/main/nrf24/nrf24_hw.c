#include "nrf24_hw.h"

#include <furi_hal_gpio.h>
#include <furi_hal_resources.h>
#include <furi_hal_light.h>
#include <furi_hal_power.h>
#include <furi_hal_spi.h>
#include <furi_hal_spi_bus.h>
#include <esp_rom_sys.h>
#include <string.h>
#include <stdlib.h>
#include "boards/board.h"

#define NRF_CMD_R_REGISTER  0x00
#define NRF_CMD_W_REGISTER  0x20
#define NRF_CMD_R_RX_PAYLOAD 0x61
#define NRF_CMD_W_TX_PAYLOAD 0xA0
#define NRF_CMD_FLUSH_TX     0xE1
#define NRF_CMD_FLUSH_RX     0xE2
#define NRF_CMD_REUSE_TX_PL  0xE3
#define NRF_CMD_NOP          0xFF

#define NRF_REG_CONFIG     0x00
#define NRF_REG_EN_AA      0x01
#define NRF_REG_EN_RXADDR  0x02
#define NRF_REG_SETUP_AW   0x03
#define NRF_REG_SETUP_RETR 0x04
#define NRF_REG_RF_CH      0x05
#define NRF_REG_RF_SETUP   0x06
#define NRF_REG_STATUS     0x07
#define NRF_REG_RPD        0x09
#define NRF_REG_RX_ADDR_P0 0x0A
#define NRF_REG_RX_ADDR_P1 0x0B
#define NRF_REG_RX_ADDR_P2 0x0C
#define NRF_REG_RX_ADDR_P3 0x0D
#define NRF_REG_RX_ADDR_P4 0x0E
#define NRF_REG_RX_ADDR_P5 0x0F
#define NRF_REG_TX_ADDR    0x10
#define NRF_REG_RX_PW_P0   0x11
#define NRF_REG_RX_PW_P1   0x12
#define NRF_REG_FIFO_STATUS 0x17
#define NRF_REG_DYNPD      0x1C
#define NRF_REG_FEATURE    0x1D

#define NRF_CONFIG_PWR_UP  0x02
#define NRF_CONFIG_PRIM_RX 0x01
#define NRF_CONFIG_EN_CRC  0x08
#define NRF_CONFIG_CRCO    0x04 /* 1 = 16-bit CRC, 0 = 8-bit (only meaningful with EN_CRC) */

#define NRF_STATUS_RX_DR    0x40
#define NRF_STATUS_TX_DS    0x20
#define NRF_STATUS_MAX_RT   0x10

#define NRF_FIFO_RX_EMPTY   0x01
#define NRF_FIFO_TX_FULL    0x20

/* RF_SETUP for constant-carrier jammer:
 *   bit7 CONT_WAVE | bit4 PLL_LOCK | bit3 RF_DR_HIGH (2 Mbps) | bits2-1 RF_PWR=3
 *   (max) | bit0 LNA-enable = 0x80|0x10|0x08|0x06|0x01 = 0x9F (matches RF24) */
#define NRF_RF_SETUP_JAMMER 0x9F
/* Normal 2 Mbps, max PA, LNA enable (bit0) — RF24 setPALevel default */
#define NRF_RF_SETUP_2M_MAX 0x0F

static const GpioPin nrf24_ce = {.port = NULL, .pin = BOARD_PIN_NRF24_CE};

/* Chip variant + last negotiated data rate (see header). Set by probe / the
 * jammer+flood start paths respectively. */
static bool nrf24_p_variant = false;
static uint8_t nrf24_data_rate_last = 1; /* 0=1M, 1=2M, 2=250k */

void nrf24_hw_init(void) {
    furi_hal_gpio_init_simple(&nrf24_ce, GpioModeOutputPushPull);
    furi_hal_gpio_write(&nrf24_ce, false);
}

void nrf24_hw_deinit(void) {
    furi_hal_gpio_write(&nrf24_ce, false);
    furi_hal_gpio_init_simple(&nrf24_ce, GpioModeAnalog);
}

void nrf24_hw_acquire(void) {
    furi_hal_spi_acquire(&furi_hal_spi_bus_handle_nrf24);
}

void nrf24_hw_release(void) {
    furi_hal_spi_release(&furi_hal_spi_bus_handle_nrf24);
}

void nrf24_hw_write_reg(uint8_t reg, uint8_t value) {
    uint8_t tx[2] = {NRF_CMD_W_REGISTER | (reg & 0x1F), value};
    furi_hal_spi_bus_tx(&furi_hal_spi_bus_handle_nrf24, tx, sizeof(tx), 100);
}

uint8_t nrf24_hw_read_reg(uint8_t reg) {
    uint8_t tx[2] = {NRF_CMD_R_REGISTER | (reg & 0x1F), 0xFF};
    uint8_t rx[2] = {0};
    furi_hal_spi_bus_trx(&furi_hal_spi_bus_handle_nrf24, tx, rx, sizeof(tx), 100);
    return rx[1];
}

void nrf24_hw_write_buf(uint8_t reg, const uint8_t* data, uint8_t len) {
    uint8_t tx[1 + 32];
    if(len > 32) len = 32;
    tx[0] = NRF_CMD_W_REGISTER | (reg & 0x1F);
    memcpy(&tx[1], data, len);
    furi_hal_spi_bus_tx(&furi_hal_spi_bus_handle_nrf24, tx, 1 + len, 100);
}

void nrf24_hw_cmd(uint8_t cmd) {
    furi_hal_spi_bus_tx(&furi_hal_spi_bus_handle_nrf24, &cmd, 1, 100);
}

void nrf24_hw_ce(bool high) {
    furi_hal_gpio_write(&nrf24_ce, high);
}

/* Detect nRF24L01+ ("P") vs a non-P clone. Mirrors RF24::_init_radio: only the
 * P variant accepts the RF_DR_LOW bit (250 kbps). Write RF_SETUP with DR_LOW set
 * / DR_HIGH clear and verify the whole byte reads back — if it does, it's a P
 * variant. Restores the original RF_SETUP afterward. */
static void nrf24_detect_variant(void) {
    uint8_t saved = nrf24_hw_read_reg(NRF_REG_RF_SETUP);
    uint8_t test = (uint8_t)((saved & ~0x28) | 0x20); /* clear DR bits, set DR_LOW */
    nrf24_hw_write_reg(NRF_REG_RF_SETUP, test);
    nrf24_p_variant = (nrf24_hw_read_reg(NRF_REG_RF_SETUP) == test);
    nrf24_hw_write_reg(NRF_REG_RF_SETUP, saved);
}

bool nrf24_hw_probe(void) {
    nrf24_hw_write_reg(NRF_REG_CONFIG, NRF_CONFIG_PWR_UP);
    esp_rom_delay_us(5000);
    nrf24_hw_write_reg(NRF_REG_RF_CH, 0x55);
    if(nrf24_hw_read_reg(NRF_REG_RF_CH) != 0x55) return false;
    nrf24_hw_write_reg(NRF_REG_RF_CH, 0x2A);
    if(nrf24_hw_read_reg(NRF_REG_RF_CH) != 0x2A) return false;
    nrf24_detect_variant();
    return true;
}

bool nrf24_hw_is_p_variant(void) {
    return nrf24_p_variant;
}

uint8_t nrf24_hw_last_data_rate(void) {
    return nrf24_data_rate_last;
}

uint8_t nrf24_hw_read_rf_setup(void) {
    return nrf24_hw_read_reg(NRF_REG_RF_SETUP);
}

uint8_t nrf24_hw_read_config(void) {
    return nrf24_hw_read_reg(NRF_REG_CONFIG);
}

void nrf24_hw_power_down(void) {
    furi_hal_gpio_write(&nrf24_ce, false);
    nrf24_hw_write_reg(NRF_REG_CONFIG, 0x00);
}

void nrf24_hw_set_channel(uint8_t channel) {
    nrf24_hw_write_reg(NRF_REG_RF_CH, channel);
}

uint8_t nrf24_hw_listen_rpd(uint8_t channel) {
    furi_hal_gpio_write(&nrf24_ce, false);
    nrf24_hw_write_reg(NRF_REG_CONFIG, NRF_CONFIG_PWR_UP | NRF_CONFIG_PRIM_RX);
    nrf24_hw_write_reg(NRF_REG_RF_CH, channel);
    furi_hal_gpio_write(&nrf24_ce, true);
    esp_rom_delay_us(140);
    furi_hal_gpio_write(&nrf24_ce, false);
    return nrf24_hw_read_reg(NRF_REG_RPD) & 0x01;
}

/* Build an RF_SETUP byte for the given data rate (0=1M, 1=2M, 2=250k), PA level
 * (0..3 = MIN/LOW/HIGH/MAX) and optional continuous-wave + PLL-lock bits.
 * Bit0 (LNA enable) is always set to byte-match Bruce's RF24::setPALevel(level,
 * lnaEnable=true) — "don't care" on a genuine nRF24L01+, but the Pingequa cap's
 * AS01-ML01DP5 (PA+LNA) is exactly the kind of module where matching the RF24
 * reference byte-for-byte is safest. */
static uint8_t nrf24_rf_setup_byte(uint8_t data_rate, uint8_t pa_level, bool cont_wave) {
    uint8_t v = (uint8_t)((pa_level & 0x03) << 1); /* RF_PWR (bits 2:1) */
    v |= 0x01; /* LNA enable (bit0) — matches RF24 setPALevel default */
    if(data_rate == 1) {
        v |= 0x08; /* RF_DR_HIGH → 2 Mbps */
    } else if(data_rate == 2) {
        v |= 0x20; /* RF_DR_LOW → 250 kbps */
    } /* data_rate == 0 → 1 Mbps: neither DR bit set */
    if(cont_wave) v |= 0x80 | 0x10; /* CONT_WAVE | PLL_LOCK */
    return v;
}

/* Write RF_SETUP and confirm it read back. Bit0 (LNA/obsolete) is masked from the
 * comparison: some clones treat it as don't-care and always read it as 0, which
 * must NOT be taken as "the data rate didn't lock". */
static bool nrf24_write_rf_setup_verify(uint8_t val) {
    nrf24_hw_write_reg(NRF_REG_RF_SETUP, val);
    return (nrf24_hw_read_reg(NRF_REG_RF_SETUP) & ~0x01u) == (val & ~0x01u);
}

/* Negotiate a data rate that the module actually locks: try the preferred rate
 * first, then the others (250 kbps only on a P variant), writing RF_SETUP with
 * the given PA + optional CONT_WAVE bits and verifying each via readback. The
 * first rate that verifies is left programmed; its code (0=1M,1=2M,2=250k) is
 * recorded in nrf24_data_rate_last and returned. Mirrors Bruce's
 * setDataRate(2M→1M→250k) fallback so a module that won't lock 2 Mbps still
 * radiates instead of silently doing nothing. */
static uint8_t nrf24_negotiate_rf_setup(uint8_t pref_rate, uint8_t pa_level, bool cont_wave) {
    uint8_t order[3];
    order[0] = pref_rate;
    uint8_t idx = 1;
    for(uint8_t r = 0; r < 3; r++) {
        if(r != pref_rate) order[idx++] = r;
    }
    for(uint8_t i = 0; i < 3; i++) {
        uint8_t rate = order[i];
        if(rate == 2 && !nrf24_p_variant) continue; /* non-P clone can't do 250 kbps */
        if(nrf24_write_rf_setup_verify(nrf24_rf_setup_byte(rate, pa_level, cont_wave))) {
            nrf24_data_rate_last = rate;
            return rate;
        }
    }
    /* Nothing verified — leave the preferred byte programmed and report it. */
    nrf24_data_rate_last = pref_rate;
    return pref_rate;
}

/* Kill ONLY the RGB status LED while jamming. The LCD backlight is deliberately
 * LEFT ON so the operator can watch the jam — shedding it gave negligible
 * brownout relief (backlight ~tens of mA vs the PA's ~130 mA keying spike on a
 * rail that can't source it) and just blinded the user. The actual brownout fix
 * is the USB PA clamp below (+ a hardware bulk cap for full power on USB). */
static void nrf24_hw_jam_power_focus(bool jamming) {
    if(jamming) furi_hal_light_set_rgb_all(0, 0, 0);
}

/* On USB the ADV's 5 V→3.3 V path can't source the PA's full-power current
 * spike → brownout/reboot the instant the carrier keys (empirically: PA Max
 * crashes on USB but is stable on battery; PA Min is stable on USB). So when
 * running on USB, clamp PA to a USB-safe level; on battery the configured PA
 * (up to Max) passes through unchanged. Raise NRF24_JAM_USB_PA_CAP (1=Low,
 * 2=High, 3=Max) to probe a higher USB ceiling, or run on battery for full
 * power. 0 = Nrf24Pa_Min, the empirically confirmed stable-on-USB floor. */
#define NRF24_JAM_USB_PA_CAP 0 /* Nrf24Pa_Min — confirmed stable on USB */
static bool g_nrf24_pa_unclamped = false;
void nrf24_hw_pa_unclamped(bool on) {
#if defined(BOARD_NRF24_USB_PA_UNCLAMP_OK) && !BOARD_NRF24_USB_PA_UNCLAMP_OK
    /* This board (Cardputer ADV) USB-browns-out at PA_MAX on USB; ignore the unclamp
     * request and keep the USB PA clamp engaged. Battery power is unaffected either way. */
    (void)on;
    g_nrf24_pa_unclamped = false;
#else
    g_nrf24_pa_unclamped = on;
#endif
}
static uint8_t nrf24_jam_pa_for_supply(uint8_t pa_level) {
    if(g_nrf24_pa_unclamped) return pa_level; /* faithful PA_MAX (Hydra has caps) */
    if(furi_hal_power_is_usb_connected() && pa_level > NRF24_JAM_USB_PA_CAP) {
        return NRF24_JAM_USB_PA_CAP;
    }
    return pa_level;
}

void nrf24_hw_jammer_start_ex(uint8_t channel, uint8_t pa_level) {
    pa_level = nrf24_jam_pa_for_supply(pa_level);
    nrf24_hw_jam_power_focus(true);
    /* Standby / CE low */
    furi_hal_gpio_write(&nrf24_ce, false);

    /* Power up. Tpd2stby (~1.5 ms) must elapse before the oscillator/PLL is
     * stable on a cold start; wait 5 ms like the RF24 library to be safe. */
    nrf24_hw_write_reg(NRF_REG_CONFIG, NRF_CONFIG_PWR_UP);
    esp_rom_delay_us(5000);

    /* Continuous carrier, configurable PA, PLL locked. Prefer 2 Mbps but fall
     * back to 1M/250k if the module won't lock it (verified via readback). */
    nrf24_negotiate_rf_setup(1 /* prefer 2M */, pa_level, true /* CONT_WAVE|PLL */);

    nrf24_hw_write_reg(NRF_REG_EN_AA, 0x00);
    nrf24_hw_write_reg(NRF_REG_SETUP_RETR, 0x00);
    nrf24_hw_write_reg(NRF_REG_SETUP_AW, 0x03); /* 5-byte address */

    if(nrf24_p_variant) {
        /* Genuine nRF24L01+ only radiates the CONT_WAVE carrier while actually
         * transmitting, so load a dummy payload and keep re-sending it via
         * REUSE_TX_PL. Mirrors RF24::startConstCarrier()'s P-variant path. */
        uint8_t dummy[32];
        memset(dummy, 0xFF, sizeof(dummy));
        nrf24_hw_write_buf(NRF_REG_TX_ADDR, dummy, 5);
        nrf24_hw_cmd(NRF_CMD_FLUSH_TX);

        uint8_t tx[1 + 32];
        tx[0] = NRF_CMD_W_TX_PAYLOAD;
        memcpy(&tx[1], dummy, 32);
        furi_hal_spi_bus_tx(&furi_hal_spi_bus_handle_nrf24, tx, sizeof(tx), 100);

        nrf24_hw_write_reg(NRF_REG_RF_CH, channel);

        /* Settle PLL, transmit once, then enable payload reuse. */
        esp_rom_delay_us(150);
        furi_hal_gpio_write(&nrf24_ce, true);
        esp_rom_delay_us(1000); /* let the first frame go out (datasheet: 1 ms) */

        /* REUSE_TX_PL — toggle CE per RF24::reUseTX so the chip keeps sending. */
        furi_hal_gpio_write(&nrf24_ce, false);
        nrf24_hw_write_reg(NRF_REG_STATUS, NRF_STATUS_MAX_RT);
        nrf24_hw_cmd(NRF_CMD_REUSE_TX_PL);
        furi_hal_gpio_write(&nrf24_ce, true);
    } else {
        /* Non-P clone: CONT_WAVE + PLL_LOCK emit an unmodulated carrier with just
         * PWR_UP and CE HIGH — no dummy payload / REUSE_TX_PL needed (and the
         * P-variant payload dance can actually keep such clones silent). Mirrors
         * RF24::startConstCarrier()'s non-P path. */
        nrf24_hw_write_reg(NRF_REG_RF_CH, channel);
        esp_rom_delay_us(150);
        furi_hal_gpio_write(&nrf24_ce, true);
    }
}

void nrf24_hw_jammer_start(uint8_t channel) {
    nrf24_hw_jammer_start_ex(channel, 3 /* PA max */);
}

void nrf24_hw_jammer_set_channel(uint8_t channel) {
    /* Carrier stays on; just retune. */
    nrf24_hw_write_reg(NRF_REG_RF_CH, channel);
    /* The PLL needs ~130 µs to re-lock after a retune. Dwell here so the carrier
     * actually settles and radiates on this channel instead of smearing across a
     * too-fast hop (the same settle class fixed in the standalone jammer FAP). */
    esp_rom_delay_us(180);
}

void nrf24_hw_jammer_stop(void) {
    nrf24_hw_jam_power_focus(false);
    /* Datasheet: with CONT_WAVE + REUSE_TX_PL the chip ignores CE low — only
     * PWR_UP=0 turns TX off. Power down first, then clear the CW bits. */
    nrf24_hw_write_reg(NRF_REG_CONFIG, 0x00);
    nrf24_hw_write_reg(NRF_REG_RF_SETUP, NRF_RF_SETUP_2M_MAX);
    furi_hal_gpio_write(&nrf24_ce, false);
    nrf24_hw_cmd(NRF_CMD_FLUSH_TX);
}

/* RX + pre-loaded TX FIFO so react_to_tx radiates instantly. CE left LOW. */
void nrf24_hw_reactive_setup(uint8_t channel) {
    furi_hal_gpio_write(&nrf24_ce, false);
    nrf24_hw_write_reg(NRF_REG_EN_AA, 0x00);
    nrf24_hw_write_reg(NRF_REG_SETUP_RETR, 0x00);
    nrf24_hw_write_reg(NRF_REG_CONFIG, NRF_CONFIG_PWR_UP | NRF_CONFIG_PRIM_RX);
    nrf24_hw_write_reg(NRF_REG_RF_CH, channel);
    /* normal 2M (CW off) so RPD is meaningful while listening */
    nrf24_negotiate_rf_setup(1, nrf24_jam_pa_for_supply(3), false);
    /* pre-fill TX FIFO with 32x0xFF */
    uint8_t pl[1 + 32];
    pl[0] = NRF_CMD_W_TX_PAYLOAD;
    memset(&pl[1], 0xFF, 32);
    nrf24_hw_cmd(NRF_CMD_FLUSH_TX);
    furi_hal_spi_bus_tx(&furi_hal_spi_bus_handle_nrf24, pl, sizeof(pl), 100);
    esp_rom_delay_us(1500); /* Tpd2stby */
}

/* RX -> CW/TX flip. Caller must have CE LOW; caller raises CE after. */
void nrf24_hw_react_to_tx(void) {
    nrf24_negotiate_rf_setup(1, nrf24_jam_pa_for_supply(3), true); /* CONT_WAVE|PLL */
    uint8_t cfg = nrf24_hw_read_reg(NRF_REG_CONFIG);
    cfg &= (uint8_t)~NRF_CONFIG_PRIM_RX; /* enter TX */
    nrf24_hw_write_reg(NRF_REG_CONFIG, cfg);
}

/* CW/TX -> RX flip + retune. Caller must have CE LOW; caller raises CE after. */
void nrf24_hw_react_to_rx(uint8_t channel) {
    nrf24_negotiate_rf_setup(1, nrf24_jam_pa_for_supply(3), false); /* CW off */
    uint8_t cfg = nrf24_hw_read_reg(NRF_REG_CONFIG);
    cfg |= NRF_CONFIG_PRIM_RX; /* enter RX */
    nrf24_hw_write_reg(NRF_REG_CONFIG, cfg);
    nrf24_hw_write_reg(NRF_REG_RF_CH, channel);
}

uint8_t nrf24_hw_read_rpd_raw(void) {
    return nrf24_hw_read_reg(NRF_REG_RPD) & 0x01;
}

/* ===== Data-flooding jammer ===== */

/* RF_SETUP for 250 kbps, PA max: RF_DR_LOW(bit5) | RF_PWR=3 | LNA-enable(bit0).
 * Longer on-air time per packet → blocks bigger time slices. */
#define NRF_RF_SETUP_250K_MAX 0x27

/* Fill a 32-byte payload with pseudo-random bytes. Random garbage spreads the
 * modulation energy across the channel far better than a fixed periodic
 * pattern (which produces narrow spectral lines). */
static void nrf24_flood_fill_random(uint8_t* buf) {
    for(uint8_t i = 0; i < 32; i += 4) {
        uint32_t r = (uint32_t)rand();
        buf[i + 0] = (uint8_t)(r);
        buf[i + 1] = (uint8_t)(r >> 8);
        buf[i + 2] = (uint8_t)(r >> 16);
        buf[i + 3] = (uint8_t)(r >> 24);
    }
}

static void nrf24_flood_write_payload(void) {
    uint8_t tx[1 + 32];
    tx[0] = NRF_CMD_W_TX_PAYLOAD;
    nrf24_flood_fill_random(&tx[1]);
    furi_hal_spi_bus_tx(&furi_hal_spi_bus_handle_nrf24, tx, sizeof(tx), 100);
}

/* Bruce's 32-byte garbage base, XORed by the LFSR seed for the turbo pattern. */
static const uint8_t nrf24_flood_base[32] = {0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA,
                                             0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE,
                                             0xFF, 0x00, 0xFF, 0x00, 0xA5, 0x5A, 0xA5, 0x5A,
                                             0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};

/* Fill a 32-byte payload for a turbo phase:
 *   0 = LFSR-XOR varied base (broadband, defeats fixed CRC filters)
 *   1 = all 0xFF        (max RF energy per bit at 2 Mbps)
 *   2 = 0x55 / 0xAA     (max bit transitions)
 * Phase 0 advances a persistent 8-bit xorshift seed, mirroring Bruce. */
static void nrf24_flood_fill_phase(uint8_t* buf, uint8_t phase) {
    static uint8_t xor_seed = 0xA5;
    switch(phase) {
    case 1:
        memset(buf, 0xFF, 32);
        break;
    case 2:
        for(uint8_t i = 0; i < 32; i++) {
            buf[i] = (i & 1) ? 0xAA : 0x55;
        }
        break;
    default:
        for(uint8_t i = 0; i < 32; i++) {
            xor_seed ^= (uint8_t)(xor_seed << 5);
            xor_seed ^= (uint8_t)(xor_seed >> 3);
            buf[i] = nrf24_flood_base[i] ^ xor_seed;
        }
        break;
    }
}

static void nrf24_flood_write_payload_phase(uint8_t phase) {
    uint8_t tx[1 + 32];
    tx[0] = NRF_CMD_W_TX_PAYLOAD;
    nrf24_flood_fill_phase(&tx[1], phase);
    furi_hal_spi_bus_tx(&furi_hal_spi_bus_handle_nrf24, tx, sizeof(tx), 100);
}

void nrf24_hw_flood_start_ex(uint8_t channel, uint8_t data_rate, uint8_t pa_level) {
    pa_level = nrf24_jam_pa_for_supply(pa_level);
    nrf24_hw_jam_power_focus(true);
    furi_hal_gpio_write(&nrf24_ce, false);

    /* TX mode (PRIM_RX=0), CRC off. Wait Tpd2stby on this cold start. */
    nrf24_hw_write_reg(NRF_REG_CONFIG, NRF_CONFIG_PWR_UP);
    esp_rom_delay_us(5000);

    nrf24_hw_write_reg(NRF_REG_EN_AA, 0x00);
    nrf24_hw_write_reg(NRF_REG_SETUP_RETR, 0x00);
    nrf24_hw_write_reg(NRF_REG_SETUP_AW, 0x01); /* 3-byte address */
    nrf24_hw_write_reg(NRF_REG_EN_RXADDR, 0x01);
    /* Prefer the requested rate but fall back (verified) if the module won't
     * lock it, so flooding actually transmits on marginal clone modules. */
    nrf24_negotiate_rf_setup(data_rate, pa_level, false);
    nrf24_hw_write_reg(NRF_REG_DYNPD, 0x00);
    nrf24_hw_write_reg(NRF_REG_FEATURE, 0x00);

    const uint8_t addr[3] = {0xE7, 0xE7, 0xE7};
    nrf24_hw_write_buf(NRF_REG_TX_ADDR, addr, 3);
    nrf24_hw_write_buf(NRF_REG_RX_ADDR_P0, addr, 3);
    nrf24_hw_write_reg(NRF_REG_RX_PW_P0, 32);

    nrf24_hw_write_reg(NRF_REG_STATUS, NRF_STATUS_RX_DR | NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT);
    nrf24_hw_cmd(NRF_CMD_FLUSH_TX);
    nrf24_hw_write_reg(NRF_REG_RF_CH, channel);
}

void nrf24_hw_flood_start(uint8_t channel, bool low_rate) {
    /* 0=1M, 1=2M, 2=250k → legacy bool maps to 2 Mbps / 250 kbps. */
    nrf24_hw_flood_start_ex(channel, low_rate ? 2 : 1, 3 /* PA max */);
}

void nrf24_hw_flood_channel(uint8_t channel) {
    /* CE low so we can safely retune and refill the FIFO. */
    furi_hal_gpio_write(&nrf24_ce, false);
    nrf24_hw_cmd(NRF_CMD_FLUSH_TX);
    nrf24_hw_write_reg(NRF_REG_RF_CH, channel);
    nrf24_hw_write_reg(NRF_REG_STATUS, NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT);

    /* Fill the 3-deep TX FIFO with random garbage packets. */
    for(uint8_t i = 0; i < 3; i++) {
        nrf24_flood_write_payload();
    }

    /* Pulse CE high long enough for all three frames to go out
     * (~144 µs each at 2 Mbps for a 32-byte payload). */
    furi_hal_gpio_write(&nrf24_ce, true);
    esp_rom_delay_us(500);
    furi_hal_gpio_write(&nrf24_ce, false);
}

void nrf24_hw_flood_burst(uint8_t channel, uint8_t burst_count, bool turbo) {
    if(burst_count < 1) burst_count = 1;

    /* CE low so we can safely retune and prime the FIFO. */
    furi_hal_gpio_write(&nrf24_ce, false);
    nrf24_hw_cmd(NRF_CMD_FLUSH_TX);
    nrf24_hw_write_reg(NRF_REG_RF_CH, channel);
    nrf24_hw_write_reg(NRF_REG_STATUS, NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT);

    /* Turbo cycles three spectral patterns (burst_count packets each); plain
     * flood streams a single random-garbage burst. CE stays HIGH the whole
     * time, topping up the 3-deep FIFO as it drains so frames go back-to-back. */
    const uint8_t phase_count = turbo ? 3 : 1;
    furi_hal_gpio_write(&nrf24_ce, true);
    for(uint8_t p = 0; p < phase_count; p++) {
        uint8_t sent = 0;
        uint16_t guard = 0;
        while(sent < burst_count && guard++ < 4000) {
            if(nrf24_hw_read_reg(NRF_REG_FIFO_STATUS) & NRF_FIFO_TX_FULL) {
                /* FIFO full → let one 32-byte frame drain (~144 µs @ 2 Mbps). */
                esp_rom_delay_us(60);
                continue;
            }
            if(turbo) {
                nrf24_flood_write_payload_phase(p);
            } else {
                nrf24_flood_write_payload();
            }
            sent++;
        }
    }

    /* Let the FIFO tail radiate before dropping CE. */
    esp_rom_delay_us(180);
    furi_hal_gpio_write(&nrf24_ce, false);
}

void nrf24_hw_flood_pump(void) {
    /* Refill any free FIFO slots, then keep CE high so the radio transmits the
     * queued packets back-to-back. With AutoAck off each sent packet is dropped
     * from the FIFO, so calling this in a tight loop keeps the FIFO ~full and
     * the carrier busy ~continuously on one channel. */
    for(uint8_t i = 0; i < 3; i++) {
        if(nrf24_hw_read_reg(NRF_REG_FIFO_STATUS) & NRF_FIFO_TX_FULL) break;
        nrf24_flood_write_payload();
    }
    nrf24_hw_write_reg(NRF_REG_STATUS, NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT);
    furi_hal_gpio_write(&nrf24_ce, true);
}

void nrf24_hw_flood_stop(void) {
    nrf24_hw_jam_power_focus(false);
    furi_hal_gpio_write(&nrf24_ce, false);
    nrf24_hw_write_reg(NRF_REG_CONFIG, 0x00);
    nrf24_hw_cmd(NRF_CMD_FLUSH_TX);
}

/* ===== Promiscuous RX ===== */

/* Noise addresses used to maximize promiscuous packet pickup.
 * The 2-byte SETUP_AW=0 trick means anything starting with these
 * common preamble-friendly bytes will pass the address filter. */
static const uint8_t mj_noise_addr[6][2] = {
    {0x55, 0x55},
    {0xAA, 0xAA},
    {0xA0, 0xAA},
    {0xAB, 0xAA},
    {0xAC, 0xAA},
    {0xAD, 0xAA},
};

void nrf24_hw_rx_start_promiscuous(uint8_t channel) {
    furi_hal_gpio_write(&nrf24_ce, false);

    /* CRC off, PWR_UP, RX */
    nrf24_hw_write_reg(NRF_REG_CONFIG, NRF_CONFIG_PWR_UP | NRF_CONFIG_PRIM_RX);
    nrf24_hw_write_reg(NRF_REG_EN_AA, 0x00);
    nrf24_hw_write_reg(NRF_REG_EN_RXADDR, 0x3F); /* enable all 6 pipes */
    /* SETUP_AW=0 is illegal per datasheet -> 2-byte addresses (promisc trick). */
    nrf24_hw_write_reg(NRF_REG_SETUP_AW, 0x00);
    nrf24_hw_write_reg(NRF_REG_SETUP_RETR, 0x00);
    nrf24_hw_write_reg(NRF_REG_RF_CH, channel);
    nrf24_hw_write_reg(NRF_REG_RF_SETUP, NRF_RF_SETUP_2M_MAX);
    nrf24_hw_write_reg(NRF_REG_DYNPD, 0x00);
    nrf24_hw_write_reg(NRF_REG_FEATURE, 0x00);

    /* All pipes use 32-byte fixed payloads. */
    for(uint8_t i = 0; i < 6; i++) {
        nrf24_hw_write_reg(NRF_REG_RX_PW_P0 + i, 32);
    }

    /* Pipes 0/1: full 2-byte address. Pipes 2..5: only LSB (top 4 bits inherit P1). */
    nrf24_hw_write_buf(NRF_REG_RX_ADDR_P0, mj_noise_addr[0], 2);
    nrf24_hw_write_buf(NRF_REG_RX_ADDR_P1, mj_noise_addr[1], 2);
    nrf24_hw_write_reg(NRF_REG_RX_ADDR_P2, mj_noise_addr[2][0]);
    nrf24_hw_write_reg(NRF_REG_RX_ADDR_P3, mj_noise_addr[3][0]);
    nrf24_hw_write_reg(NRF_REG_RX_ADDR_P4, mj_noise_addr[4][0]);
    nrf24_hw_write_reg(NRF_REG_RX_ADDR_P5, mj_noise_addr[5][0]);

    /* Clear interrupt flags */
    nrf24_hw_write_reg(NRF_REG_STATUS, NRF_STATUS_RX_DR | NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT);
    /* Drain any leftover FIFO data */
    nrf24_hw_cmd(NRF_CMD_FLUSH_RX);

    /* Settle PLL, then start listening. */
    esp_rom_delay_us(150);
    furi_hal_gpio_write(&nrf24_ce, true);
}

bool nrf24_hw_rx_data_ready(void) {
    return (nrf24_hw_read_reg(NRF_REG_FIFO_STATUS) & NRF_FIFO_RX_EMPTY) == 0;
}

uint8_t nrf24_hw_rx_read_payload(uint8_t* buf, uint8_t maxlen) {
    if(maxlen > 32) maxlen = 32;
    uint8_t tx[1 + 32];
    uint8_t rx[1 + 32];
    tx[0] = NRF_CMD_R_RX_PAYLOAD;
    memset(&tx[1], NRF_CMD_NOP, maxlen);
    furi_hal_spi_bus_trx(&furi_hal_spi_bus_handle_nrf24, tx, rx, 1 + maxlen, 100);
    memcpy(buf, &rx[1], maxlen);
    /* Clear RX_DR */
    nrf24_hw_write_reg(NRF_REG_STATUS, NRF_STATUS_RX_DR);
    return maxlen;
}

void nrf24_hw_rx_stop(void) {
    furi_hal_gpio_write(&nrf24_ce, false);
    /* Stay PWR_UP; caller decides full power-down. */
}

/* ===== Targeted TX ===== */

void nrf24_hw_tx_setup(
    const uint8_t* addr,
    uint8_t addr_len,
    uint8_t channel,
    uint8_t payload_len) {
    if(addr_len < 3) addr_len = 3;
    if(addr_len > 5) addr_len = 5;
    if(payload_len > 32) payload_len = 32;

    furi_hal_gpio_write(&nrf24_ce, false);

    /* CRC off, PWR_UP, TX (PRIM_RX=0) */
    nrf24_hw_write_reg(NRF_REG_CONFIG, NRF_CONFIG_PWR_UP);
    nrf24_hw_write_reg(NRF_REG_EN_AA, 0x00);
    nrf24_hw_write_reg(NRF_REG_EN_RXADDR, 0x01);
    /* SETUP_AW: 1=3byte, 2=4byte, 3=5byte */
    nrf24_hw_write_reg(NRF_REG_SETUP_AW, addr_len - 2);
    nrf24_hw_write_reg(NRF_REG_SETUP_RETR, 0x00);
    nrf24_hw_write_reg(NRF_REG_RF_CH, channel);
    nrf24_hw_write_reg(NRF_REG_RF_SETUP, NRF_RF_SETUP_2M_MAX);
    nrf24_hw_write_reg(NRF_REG_DYNPD, 0x00);
    nrf24_hw_write_reg(NRF_REG_FEATURE, 0x00);

    /* nRF24 transmits LSB-first per byte but expects address MSB-first in the
     * register. The Bruce reference passes the address as captured from the
     * promiscuous sniff so we mirror that here. */
    nrf24_hw_write_buf(NRF_REG_TX_ADDR, addr, addr_len);
    nrf24_hw_write_buf(NRF_REG_RX_ADDR_P0, addr, addr_len);
    nrf24_hw_write_reg(NRF_REG_RX_PW_P0, payload_len);

    nrf24_hw_write_reg(NRF_REG_STATUS, NRF_STATUS_RX_DR | NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT);
    nrf24_hw_cmd(NRF_CMD_FLUSH_TX);
    nrf24_hw_cmd(NRF_CMD_FLUSH_RX);
}

#ifndef PUBLIC_BUILD
/* Superset of nrf24_hw_tx_setup with a selectable data rate + CRC mode (see
 * header). PRIVATE-only -- does not affect nrf24_hw_tx_setup, which stays
 * fixed at 2 Mbps/CRC-off for the public MouseJack path. */
void nrf24_hw_tx_setup_ex(
    const uint8_t* addr,
    uint8_t addr_len,
    uint8_t channel,
    uint8_t payload_len,
    uint8_t data_rate,
    bool crc16) {
    if(addr_len < 3) addr_len = 3;
    if(addr_len > 5) addr_len = 5;
    if(payload_len > 32) payload_len = 32;

    furi_hal_gpio_write(&nrf24_ce, false);

    /* PWR_UP, TX (PRIM_RX=0); CRC per crc16 (EN_CRC + CRCO=1 for 16-bit, or
     * off entirely to match nrf24_hw_tx_setup's default). */
    uint8_t config = NRF_CONFIG_PWR_UP;
    if(crc16) config |= NRF_CONFIG_EN_CRC | NRF_CONFIG_CRCO;
    nrf24_hw_write_reg(NRF_REG_CONFIG, config);
    nrf24_hw_write_reg(NRF_REG_EN_AA, 0x00);
    nrf24_hw_write_reg(NRF_REG_EN_RXADDR, 0x01);
    /* SETUP_AW: 1=3byte, 2=4byte, 3=5byte */
    nrf24_hw_write_reg(NRF_REG_SETUP_AW, addr_len - 2);
    nrf24_hw_write_reg(NRF_REG_SETUP_RETR, 0x00);
    nrf24_hw_write_reg(NRF_REG_RF_CH, channel);
    /* RF_SETUP: requested data rate, PA max, LNA enable -- same byte-builder
     * used by the jammer/flood paths (data_rate 0=1M/1=2M/2=250k). */
    nrf24_hw_write_reg(NRF_REG_RF_SETUP, nrf24_rf_setup_byte(data_rate, 3 /* PA max */, false));
    nrf24_hw_write_reg(NRF_REG_DYNPD, 0x00);
    nrf24_hw_write_reg(NRF_REG_FEATURE, 0x00);

    nrf24_hw_write_buf(NRF_REG_TX_ADDR, addr, addr_len);
    nrf24_hw_write_buf(NRF_REG_RX_ADDR_P0, addr, addr_len);
    nrf24_hw_write_reg(NRF_REG_RX_PW_P0, payload_len);

    nrf24_hw_write_reg(NRF_REG_STATUS, NRF_STATUS_RX_DR | NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT);
    nrf24_hw_cmd(NRF_CMD_FLUSH_TX);
    nrf24_hw_cmd(NRF_CMD_FLUSH_RX);
}
#endif /* !PUBLIC_BUILD */

bool nrf24_hw_tx_send(const uint8_t* data, uint8_t len) {
    if(len > 32) len = 32;

    /* Clear stale flags */
    nrf24_hw_write_reg(NRF_REG_STATUS, NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT);

    /* Write payload to TX FIFO */
    uint8_t tx[1 + 32];
    tx[0] = NRF_CMD_W_TX_PAYLOAD;
    memcpy(&tx[1], data, len);
    furi_hal_spi_bus_tx(&furi_hal_spi_bus_handle_nrf24, tx, 1 + len, 100);

    /* Pulse CE >= 10 µs to start TX. */
    furi_hal_gpio_write(&nrf24_ce, true);
    esp_rom_delay_us(15);
    furi_hal_gpio_write(&nrf24_ce, false);

    /* Wait for TX_DS or MAX_RT. With AA off MAX_RT cannot happen, so we poll
     * TX_DS with a short timeout (≈1 ms covers any 32-byte 2 Mbps frame). */
    for(uint16_t i = 0; i < 200; i++) {
        uint8_t st = nrf24_hw_read_reg(NRF_REG_STATUS);
        if(st & (NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT)) {
            nrf24_hw_write_reg(NRF_REG_STATUS, NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT);
            return (st & NRF_STATUS_TX_DS) != 0;
        }
        esp_rom_delay_us(10);
    }
    /* Timeout — flush to keep the FIFO clean. */
    nrf24_hw_cmd(NRF_CMD_FLUSH_TX);
    return false;
}

void nrf24_hw_tx_stop(void) {
    furi_hal_gpio_write(&nrf24_ce, false);
    nrf24_hw_cmd(NRF_CMD_FLUSH_TX);
}

void nrf24_hw_flush_rx(void) {
    nrf24_hw_cmd(NRF_CMD_FLUSH_RX);
}

void nrf24_hw_flush_tx(void) {
    nrf24_hw_cmd(NRF_CMD_FLUSH_TX);
}
