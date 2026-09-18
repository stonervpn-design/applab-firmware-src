/**
 * @file st25r3916_hal.c
 * @brief ST25R3916 poller engine for the furi_hal_nfc backend. See header.
 *
 * The transceive core and IRQ-polling mirror the validated bring-up poller
 * (st25r3916_nfca.c) but expose the individual activation steps so the upper
 * NFC stack drives REQA / anticollision / SELECT itself.
 */

#include "st25r3916_hal.h"
#include "st25r3916_i2c.h"
#include "st25r3916_reg.h"

#include <furi.h>
#include <board.h>
#include <string.h>
#include <stdio.h>
#include <esp_heap_caps.h>

#define TAG "St25rHal"

#if defined(BOARD_HAS_ST25R3916) && BOARD_HAS_ST25R3916

/* IRQ status-register bit masks (register-local; reg.h defines only addresses).
 * ST25R3916 datasheet DS12484. */
#define IRQ_MAIN_OSC  0x80U /* oscillator frequency stable */
#define IRQ_MAIN_RXS  0x20U /* start of receive */
#define IRQ_MAIN_RXE  0x10U /* end of receive */
#define IRQ_MAIN_TXE  0x08U /* end of transmit */
#define IRQ_MAIN_COL  0x04U /* bit collision */
#define IRQ_TIMER_NRE 0x40U /* no-response timer expired */
#define IRQ_ERR_CRC   0x80U /* CRC error */
#define IRQ_ERR_PAR   0x40U /* parity error */

/* FIFO single-transfer limit. Sized for ISO-DEP / EMV APDU frames (up to one
 * 256-byte ISO14443-4 block; capped at the 8-bit FIFO transfer length). */
#define ST25R_HAL_FIFO_MAX 255U

/* Accumulated IRQ flags (status regs are clear-on-read; OR across polls). */
static uint8_t s_irq_main, s_irq_timer, s_irq_err;
static uint16_t s_last_rx_exact_bits; /* exact bit count of the last transceive RX */

uint16_t st25r3916_hal_last_rx_bits(void) {
    return s_last_rx_exact_bits;
}

static void irq_clear(void) {
    /* One burst read of 0x1A..0x1C clears all three IRQ status regs (clear-on-
     * read) in a single I2C transaction instead of three. */
    uint8_t v[3];
    st25r3916_read_regs(ST25R3916_REG_IRQ_MAIN, v, 3);
    s_irq_main = s_irq_timer = s_irq_err = 0;
}

static void irq_poll(void) {
    /* IRQ_MAIN(0x1A), IRQ_TIMER_NFC(0x1B), IRQ_ERROR_WUP(0x1C) are consecutive —
     * read all three in ONE burst transaction. This is the hottest I2C path (it
     * runs every ~50µs while waiting on TX/RX), so collapsing 3 reads into 1
     * roughly thirds its I2C cost and directly speeds detection + reads. */
    uint8_t v[3];
    if(st25r3916_read_regs(ST25R3916_REG_IRQ_MAIN, v, 3) == ESP_OK) {
        s_irq_main |= v[0];
        s_irq_timer |= v[1];
        s_irq_err |= v[2];
    }
}

/* Core transceive. tx_cmd fires the frame; tx/tx_len is the FIFO payload
 * (NULL/0 for REQA/WUPA which carry their own bits). no_crc_rx keeps the
 * received CRC in the FIFO instead of letting the receiver strip it. */
static St25rHalStatus st25r_transceive(
    uint8_t tx_cmd,
    const uint8_t* tx,
    uint16_t tx_len,
    bool no_crc_rx,
    uint8_t* rx,
    uint16_t rx_cap,
    uint16_t* rx_len,
    uint32_t rx_budget_ms) {
    *rx_len = 0;

    if(tx_len > ST25R_HAL_FIFO_MAX) return St25rHalError;

    uint8_t aux = 0;
    st25r3916_read_reg(ST25R3916_REG_AUX, &aux);
    if(no_crc_rx)
        aux |= ST25R3916_REG_AUX_no_crc_rx;
    else
        aux &= (uint8_t)~ST25R3916_REG_AUX_no_crc_rx;
    st25r3916_write_reg(ST25R3916_REG_AUX, aux);

    st25r3916_cmd(ST25R3916_CMD_CLEAR_FIFO);
    irq_clear();

    if(tx && tx_len) {
        if(st25r3916_write_fifo(tx, (uint8_t)tx_len) != ESP_OK) return St25rHalError;
        /* Byte-aligned frame: ntx = tx_len bytes, 0 partial bits.
         * NUM_TX_BYTES1 = ntx[12:5], NUM_TX_BYTES2[7:3] = ntx[4:0]. */
        st25r3916_write_reg(ST25R3916_REG_NUM_TX_BYTES1, (uint8_t)(tx_len >> 5));
        st25r3916_write_reg(ST25R3916_REG_NUM_TX_BYTES2, (uint8_t)((tx_len & 0x1F) << 3));
    }

    if(st25r3916_cmd(tx_cmd) != ESP_OK) return St25rHalError;

    /* Wait end-of-transmit. HYBRID poll: tight µs-granularity for the first ~2 ms
     * (frames complete in well under 1 ms) then ms steps for the long tail — cuts
     * ~1 ms/frame of dead polling vs the old 1 ms granularity, which compounds to
     * seconds over a MIFARE dictionary attack (hundreds of auth frames). */
    bool txe = false;
    for(int s = 0; s < 40 && !txe; s++) {
        irq_poll();
        if(s_irq_main & IRQ_MAIN_TXE) txe = true;
        else
            furi_delay_us(50);
    }
    for(uint32_t t = 0; !txe && t < 10; t++) {
        irq_poll();
        if(s_irq_main & IRQ_MAIN_TXE) txe = true;
        else
            furi_delay_ms(1);
    }
    if(!txe) return St25rHalError;

    /* Wait end-of-receive / error / silence — same hybrid poll. */
    if(rx_budget_ms == 0) rx_budget_ms = 20;
    bool rxe = false;
    for(int s = 0; s < 60 && !rxe; s++) {
        irq_poll();
        if(s_irq_err & (IRQ_ERR_CRC | IRQ_ERR_PAR)) return St25rHalError;
        if(s_irq_timer & IRQ_TIMER_NRE) return St25rHalNoCard;
        if(s_irq_main & IRQ_MAIN_RXE) rxe = true;
        else
            furi_delay_us(50);
    }
    for(uint32_t t = 0; !rxe && t < rx_budget_ms; t++) {
        irq_poll();
        if(s_irq_err & (IRQ_ERR_CRC | IRQ_ERR_PAR)) return St25rHalError;
        if(s_irq_timer & IRQ_TIMER_NRE) return St25rHalNoCard;
        if(s_irq_main & IRQ_MAIN_RXE) rxe = true;
        else
            furi_delay_ms(1);
    }
    if(!rxe) return St25rHalNoCard;

    uint8_t fst[2] = {0, 0}; /* FIFO_STATUS1(0x1E) + FIFO_STATUS2(0x1F): one burst */
    st25r3916_read_regs(ST25R3916_REG_FIFO_STATUS1, fst, 2);
    const uint8_t st1 = fst[0], st2 = fst[1];
    uint16_t cnt = (uint16_t)st1 +
                   (uint16_t)(((st2 & ST25R3916_REG_FIFO_STATUS2_fifo_b_mask) >>
                               ST25R3916_REG_FIFO_STATUS2_fifo_b_shift)
                              << 8);
    if(cnt == 0) return St25rHalNoCard;
    if(cnt > rx_cap) cnt = rx_cap;
    if(cnt > ST25R_HAL_FIFO_MAX) cnt = ST25R_HAL_FIFO_MAX;
    if(st25r3916_read_fifo(rx, (uint8_t)cnt) != ESP_OK) return St25rHalError;
    *rx_len = cnt;
    /* Capture the EXACT bit count (last byte may be partial). Needed for short
     * responses like the 4-bit WRITE/AUTH ACK (0xA) — the byte count alone would
     * report 8 bits and the stack's "size == 4" ACK check would fail. */
    {
        uint8_t lb = (uint8_t)((st2 & ST25R3916_REG_FIFO_STATUS2_fifo_lb_mask) >>
                               ST25R3916_REG_FIFO_STATUS2_fifo_lb_shift);
        s_last_rx_exact_bits = (uint16_t)((cnt - 1) * 8 + (lb == 0 ? 8 : lb));
    }
    return St25rHalOk;
}

/* Bit-granular transceive with HARDWARE PARITY DISABLED — for MIFARE Classic
 * Crypto1. The tx bitstream already carries the (encrypted) parity bit after
 * every data byte (9-bit groups, tx_bits = 9*N), so we must transmit those exact
 * bits and receive raw so the software cipher can recover data+parity. Returns
 * the received frame with an EXACT bit count (rx_bits, a multiple of 9). */
St25rHalStatus st25r3916_hal_transceive_custom_parity(
    const uint8_t* tx,
    uint16_t tx_bits,
    uint8_t* rx,
    uint16_t rx_cap,
    uint16_t* rx_bits,
    uint32_t rx_budget_ms) {
    *rx_bits = 0;
    if(!tx || tx_bits == 0) return St25rHalError;

    const uint16_t tx_full = tx_bits / 8; /* complete bytes (ntx) */
    const uint8_t tx_nbtx = (uint8_t)(tx_bits % 8); /* residual bits (nbtx 0..7) */
    const uint16_t tx_fifo_bytes = tx_full + (tx_nbtx ? 1 : 0);
    if(tx_fifo_bytes == 0 || tx_fifo_bytes > ST25R_HAL_FIFO_MAX) return St25rHalError;

    /* Disable hardware parity both ways; keep any received CRC bytes raw too. */
    st25r3916_write_reg(
        ST25R3916_REG_ISO14443A_NFC,
        ST25R3916_REG_ISO14443A_NFC_no_tx_par | ST25R3916_REG_ISO14443A_NFC_no_rx_par);
    uint8_t aux = 0;
    st25r3916_read_reg(ST25R3916_REG_AUX, &aux);
    st25r3916_write_reg(ST25R3916_REG_AUX, aux | ST25R3916_REG_AUX_no_crc_rx);

    st25r3916_cmd(ST25R3916_CMD_CLEAR_FIFO);
    irq_clear();

    St25rHalStatus result = St25rHalError;
    do {
        if(st25r3916_write_fifo(tx, (uint8_t)tx_fifo_bytes) != ESP_OK) break;
        st25r3916_write_reg(ST25R3916_REG_NUM_TX_BYTES1, (uint8_t)(tx_full >> 5));
        st25r3916_write_reg(
            ST25R3916_REG_NUM_TX_BYTES2, (uint8_t)(((tx_full & 0x1F) << 3) | (tx_nbtx & 0x07)));

        if(st25r3916_cmd(ST25R3916_CMD_TRANSMIT_WITHOUT_CRC) != ESP_OK) break;

        /* Hybrid poll (µs then ms) — the big win for MIFARE: the dict attack runs
         * this path once per key tried, so shaving ~1 ms/frame saves seconds. */
        bool txe = false;
        for(int s = 0; s < 40 && !txe; s++) {
            irq_poll();
            if(s_irq_main & IRQ_MAIN_TXE) txe = true;
            else
                furi_delay_us(50);
        }
        for(uint32_t t = 0; !txe && t < 10; t++) {
            irq_poll();
            if(s_irq_main & IRQ_MAIN_TXE) txe = true;
            else
                furi_delay_ms(1);
        }
        if(!txe) break;

        if(rx_budget_ms == 0) rx_budget_ms = 20;
        bool rxe = false, nocard = false;
        /* Parity/CRC "errors" are EXPECTED on the encrypted stream — the software
         * cipher validates — so they are not checked here. */
        for(int s = 0; s < 60 && !rxe && !nocard; s++) {
            irq_poll();
            if(s_irq_main & IRQ_MAIN_RXE) rxe = true;
            else if(s_irq_timer & IRQ_TIMER_NRE) nocard = true;
            else
                furi_delay_us(50);
        }
        for(uint32_t t = 0; !rxe && !nocard && t < rx_budget_ms; t++) {
            irq_poll();
            if(s_irq_main & IRQ_MAIN_RXE) rxe = true;
            else if(s_irq_timer & IRQ_TIMER_NRE) nocard = true;
            else
                furi_delay_ms(1);
        }
        if(nocard || !rxe) {
            result = St25rHalNoCard;
            break;
        }

        uint8_t st1 = 0, st2 = 0;
        st25r3916_read_reg(ST25R3916_REG_FIFO_STATUS1, &st1);
        st25r3916_read_reg(ST25R3916_REG_FIFO_STATUS2, &st2);
        uint16_t rx_bytes = (uint16_t)st1 +
                            (uint16_t)(((st2 & ST25R3916_REG_FIFO_STATUS2_fifo_b_mask) >>
                                        ST25R3916_REG_FIFO_STATUS2_fifo_b_shift)
                                       << 8);
        if(rx_bytes == 0) {
            result = St25rHalNoCard;
            break;
        }
        /* bits in the last FIFO byte (0 => the last byte is full, i.e. 8). */
        uint8_t lb = (uint8_t)((st2 & ST25R3916_REG_FIFO_STATUS2_fifo_lb_mask) >>
                               ST25R3916_REG_FIFO_STATUS2_fifo_lb_shift);
        *rx_bits = (uint16_t)((rx_bytes - 1) * 8 + (lb == 0 ? 8 : lb));

        if(rx_bytes > rx_cap) rx_bytes = rx_cap;
        if(rx_bytes > ST25R_HAL_FIFO_MAX) rx_bytes = ST25R_HAL_FIFO_MAX;
        if(st25r3916_read_fifo(rx, (uint8_t)rx_bytes) != ESP_OK) {
            result = St25rHalError;
            break;
        }
        result = St25rHalOk;
    } while(0);

    /* Restore hardware parity + CRC handling for subsequent normal frames. */
    st25r3916_write_reg(ST25R3916_REG_ISO14443A_NFC, 0x00);
    st25r3916_read_reg(ST25R3916_REG_AUX, &aux);
    st25r3916_write_reg(ST25R3916_REG_AUX, aux & (uint8_t)~ST25R3916_REG_AUX_no_crc_rx);
    return result;
}

/* ──────────────────────────── Public engine API ──────────────────────────── */

bool st25r3916_hal_present(void) {
    return st25r3916_probe();
}

/* True once the chip is fully configured (osc running, analog + NFC-A mode set).
 * Kept across field_on calls so a scan doesn't re-run SET_DEFAULT (and the
 * oscillator re-stabilization) on every poll cycle — that was slow, flaky
 * ("oscillator did not stabilize"), and reset any in-progress ISO-DEP session.
 * Reset by field_off so a fresh session re-initializes. */
static bool s_chip_ready = false;
static St25rTech s_current_tech = St25rTechNfcA;

void st25r3916_hal_set_tech(St25rTech tech) {
    s_current_tech = tech;
}

/* Apply the RF-technology-specific poller config (mode, bitrate, analog RX,
 * framing). Ported from Flipper's per-tech furi_hal_nfc_*_poller_init
 * (targets/f7/furi_hal). Called from field_on so a tech switch re-tunes the
 * front end. NFC-A is the baseline (also set by chip_init). */
static void st25r_apply_poller_tech(void) {
    if(s_current_tech == St25rTechNfcB) {
        /* ── ISO14443-B 106k (Flipper furi_hal_nfc_iso14443b_poller_init) ── */
        st25r3916_write_reg(ST25R3916_REG_RX_CONF1, ST25R3916_REG_RX_CONF1_h200);
        st25r3916_write_reg(
            ST25R3916_REG_RX_CONF2,
            ST25R3916_REG_RX_CONF2_agc6_3 | ST25R3916_REG_RX_CONF2_agc_alg |
                ST25R3916_REG_RX_CONF2_agc_m | ST25R3916_REG_RX_CONF2_agc_en |
                ST25R3916_REG_RX_CONF2_pulz_61 | ST25R3916_REG_RX_CONF2_sqm_dyn);
        st25r3916_write_reg(ST25R3916_REG_RX_CONF3, 0x00);
        st25r3916_write_reg(ST25R3916_REG_RX_CONF4, 0x00);
        st25r3916_write_reg(
            ST25R3916_REG_CORR_CONF1,
            ST25R3916_REG_CORR_CONF1_corr_s0 | ST25R3916_REG_CORR_CONF1_corr_s1 |
                ST25R3916_REG_CORR_CONF1_corr_s3 | ST25R3916_REG_CORR_CONF1_corr_s4);
        st25r3916_write_reg(ST25R3916_REG_CORR_CONF2, 0x00);
        /* Mode: ISO14443B, AM modulation. Preserve the targ bit (clear). */
        st25r3916_write_reg(
            ST25R3916_REG_MODE, ST25R3916_REG_MODE_om_iso14443b | ST25R3916_REG_MODE_tr_am_am);
        st25r3916_write_reg(ST25R3916_REG_BIT_RATE, 0x00); /* 106k tx+rx */
        /* 10% ASK modulation depth. */
        {
            uint8_t v = 0;
            st25r3916_read_reg(ST25R3916_REG_TX_DRIVER, &v);
            v = (uint8_t)((v & ~ST25R3916_REG_TX_DRIVER_am_mod_mask) |
                          ST25R3916_REG_TX_DRIVER_am_mod_10percent);
            st25r3916_write_reg(ST25R3916_REG_TX_DRIVER, v);
        }
        /* Regulator AM (clear dis_reg_am + res_am). */
        {
            uint8_t v = 0;
            st25r3916_read_reg(ST25R3916_REG_AUX_MOD, &v);
            v &= (uint8_t)~(ST25R3916_REG_AUX_MOD_dis_reg_am | ST25R3916_REG_AUX_MOD_res_am);
            st25r3916_write_reg(ST25R3916_REG_AUX_MOD, v);
        }
        /* EGT=0, SOF=10etu low + 2etu high, EOF=10etu. */
        st25r3916_write_reg(
            ST25R3916_REG_ISO14443B_1,
            ST25R3916_REG_ISO14443B_1_sof_0_10etu | ST25R3916_REG_ISO14443B_1_sof_1_2etu |
                ST25R3916_REG_ISO14443B_1_eof_10etu);
        /* TR1 = 80/fs, B' mode off. */
        st25r3916_write_reg(ST25R3916_REG_ISO14443B_2, ST25R3916_REG_ISO14443B_2_tr1_80fs80fs);
        return;
    }

    if(s_current_tech == St25rTechNfcV) {
        /* ── ISO15693 / NFC-V 26.48kbps (Flipper furi_hal_nfc_iso15693_poller_init)
         * Subcarrier-stream mode: reader OOK 1-of-4 coded, tag replies with a
         * 424kHz subcarrier. TX 1-of-4 encoding + RX subcarrier decoding are done
         * in the furi_hal poller_tx/rx (tech-gated). ─────────────────────────── */
        st25r3916_write_reg(
            ST25R3916_REG_MODE,
            ST25R3916_REG_MODE_om_subcarrier_stream | ST25R3916_REG_MODE_tr_am_ook);
        st25r3916_write_reg(
            ST25R3916_REG_STREAM_MODE,
            ST25R3916_REG_STREAM_MODE_scf_sc424 | ST25R3916_REG_STREAM_MODE_stx_106 |
                ST25R3916_REG_STREAM_MODE_scp_8pulses);
        st25r3916_write_reg(ST25R3916_REG_BIT_RATE, 0x00);
        {
            uint8_t v = 0;
            st25r3916_read_reg(ST25R3916_REG_AUX_MOD, &v);
            v &= (uint8_t)~(ST25R3916_REG_AUX_MOD_dis_reg_am | ST25R3916_REG_AUX_MOD_res_am);
            st25r3916_write_reg(ST25R3916_REG_AUX_MOD, v);
        }
        st25r3916_write_reg(
            ST25R3916_REG_RX_CONF1,
            ST25R3916_REG_RX_CONF1_z12k | ST25R3916_REG_RX_CONF1_h80 |
                ST25R3916_REG_RX_CONF1_lp_600khz);
        st25r3916_write_reg(
            ST25R3916_REG_RX_CONF2,
            ST25R3916_REG_RX_CONF2_agc6_3 | ST25R3916_REG_RX_CONF2_agc_m |
                ST25R3916_REG_RX_CONF2_agc_en | ST25R3916_REG_RX_CONF2_sqm_dyn);
        st25r3916_write_reg(ST25R3916_REG_RX_CONF3, 0x00);
        st25r3916_write_reg(ST25R3916_REG_RX_CONF4, 0x00);
        st25r3916_write_reg(
            ST25R3916_REG_CORR_CONF1,
            ST25R3916_REG_CORR_CONF1_corr_s0 | ST25R3916_REG_CORR_CONF1_corr_s1 |
                ST25R3916_REG_CORR_CONF1_corr_s4);
        st25r3916_write_reg(ST25R3916_REG_CORR_CONF2, ST25R3916_REG_CORR_CONF2_corr_s8);
        return;
    }

    if(s_current_tech == St25rTechFelica) {
        /* ── FeliCa / NFC-F 212kbps (Flipper furi_hal_nfc_felica_poller_init) ─── */
        st25r3916_write_reg(
            ST25R3916_REG_MODE, ST25R3916_REG_MODE_om_felica | ST25R3916_REG_MODE_tr_am_am);
        st25r3916_write_reg(
            ST25R3916_REG_BIT_RATE,
            ST25R3916_REG_BIT_RATE_txrate_212 | ST25R3916_REG_BIT_RATE_rxrate_212);
        {
            uint8_t v = 0;
            st25r3916_read_reg(ST25R3916_REG_TX_DRIVER, &v);
            v = (uint8_t)((v & ~ST25R3916_REG_TX_DRIVER_am_mod_mask) |
                          ST25R3916_REG_TX_DRIVER_am_mod_10percent);
            st25r3916_write_reg(ST25R3916_REG_TX_DRIVER, v);
            st25r3916_read_reg(ST25R3916_REG_AUX_MOD, &v);
            v &= (uint8_t)~(ST25R3916_REG_AUX_MOD_dis_reg_am | ST25R3916_REG_AUX_MOD_res_am);
            st25r3916_write_reg(ST25R3916_REG_AUX_MOD, v);
        }
        st25r3916_write_reg(
            ST25R3916_REG_RX_CONF1,
            ST25R3916_REG_RX_CONF1_lp0 | ST25R3916_REG_RX_CONF1_hz_12_80khz);
        st25r3916_write_reg(
            ST25R3916_REG_RX_CONF2,
            ST25R3916_REG_RX_CONF2_sqm_dyn | ST25R3916_REG_RX_CONF2_agc_en |
                ST25R3916_REG_RX_CONF2_agc_m);
        st25r3916_write_reg(ST25R3916_REG_RX_CONF3, 0x00);
        st25r3916_write_reg(ST25R3916_REG_RX_CONF4, 0x00);
        st25r3916_write_reg(
            ST25R3916_REG_CORR_CONF1,
            ST25R3916_REG_CORR_CONF1_corr_s6 | ST25R3916_REG_CORR_CONF1_corr_s4 |
                ST25R3916_REG_CORR_CONF1_corr_s3);
        st25r3916_write_reg(ST25R3916_REG_CORR_CONF2, 0x00);
        return;
    }

    /* Default: NFC-A 106k (baseline). */
    st25r3916_write_reg(ST25R3916_REG_MODE, ST25R3916_REG_MODE_om_iso14443a);
    st25r3916_write_reg(ST25R3916_REG_BIT_RATE, 0x00);
    st25r3916_write_reg(ST25R3916_REG_ISO14443A_NFC, 0x00);
    st25r3916_write_reg(ST25R3916_REG_RX_CONF1, 0x08);
    st25r3916_write_reg(
        ST25R3916_REG_RX_CONF2,
        ST25R3916_REG_RX_CONF2_sqm_dyn | ST25R3916_REG_RX_CONF2_agc_en |
            ST25R3916_REG_RX_CONF2_agc_m | ST25R3916_REG_RX_CONF2_agc6_3);
    st25r3916_write_reg(ST25R3916_REG_RX_CONF3, 0x00);
    st25r3916_write_reg(ST25R3916_REG_RX_CONF4, 0x00);
    st25r3916_write_reg(ST25R3916_REG_CORR_CONF1, 0x51);
    st25r3916_write_reg(ST25R3916_REG_CORR_CONF2, 0x00);
}

/* One-time chip bring-up: oscillator, regulators, NFC-A 106 mode, analog RX. */
static St25rHalStatus st25r3916_chip_init(void) {
    if(!st25r3916_probe()) return St25rHalError; /* also brings up I2C */

    st25r3916_cmd(ST25R3916_CMD_SET_DEFAULT);
    furi_delay_ms(1);

    /* Unmask all IRQ sources so the status registers latch every event. */
    st25r3916_write_reg(ST25R3916_REG_IRQ_MASK_MAIN, 0x00);
    st25r3916_write_reg(ST25R3916_REG_IRQ_MASK_TIMER_NFC, 0x00);
    st25r3916_write_reg(ST25R3916_REG_IRQ_MASK_ERROR_WUP, 0x00);
    st25r3916_write_reg(ST25R3916_REG_IRQ_MASK_TARGET, 0x00);

    /* Enable oscillator + regulator, then wait for the osc_ok LEVEL status.
     * NOTE: do NOT poll the latched OSC IRQ here — the osc stabilizes within
     * ~1ms and its IRQ would be cleared by any intervening status read before
     * the poll loop sees it (that race is why "oscillator did not stabilize"
     * kept firing). The AUX_DISPLAY.osc_ok bit is a level flag: set while the
     * oscillator runs, so it can't be missed. */
    st25r3916_write_reg(ST25R3916_REG_OP_CONTROL, ST25R3916_REG_OP_CONTROL_en);
    bool osc = false;
    for(int t = 0; t < 50; t++) {
        uint8_t disp = 0;
        if(st25r3916_read_reg(ST25R3916_REG_AUX_DISPLAY, &disp) == ESP_OK &&
           (disp & ST25R3916_REG_AUX_DISPLAY_osc_ok)) {
            osc = true;
            break;
        }
        furi_delay_ms(1);
    }
    if(!osc) {
        FURI_LOG_W(TAG, "oscillator did not stabilize");
        return St25rHalError;
    }
    st25r3916_cmd(ST25R3916_CMD_ADJUST_REGULATORS);
    furi_delay_ms(1);

    /* NFC-A poller (initiator), 106 kbit/s, hardware parity. */
    st25r3916_write_reg(ST25R3916_REG_MODE, ST25R3916_REG_MODE_om_iso14443a);
    st25r3916_write_reg(ST25R3916_REG_BIT_RATE, 0x00);
    st25r3916_write_reg(ST25R3916_REG_ISO14443A_NFC, 0x00);

    /* Analog RX front-end config — rfal-style defaults for the NFC-A 106 poller.
     * SET_DEFAULT leaves the receiver under-tuned, so weak or varied cards are
     * missed even when a genuine ST25R3916 (Flipper) reads them. Enabling dynamic
     * squelch + AGC-with-memory and the reference correlator config restores that
     * sensitivity. (Values from ST's rfal ST25R3916 default analog config.) */
    st25r3916_write_reg(ST25R3916_REG_RX_CONF1, 0x08);
    st25r3916_write_reg(
        ST25R3916_REG_RX_CONF2,
        ST25R3916_REG_RX_CONF2_sqm_dyn | ST25R3916_REG_RX_CONF2_agc_en |
            ST25R3916_REG_RX_CONF2_agc_m | ST25R3916_REG_RX_CONF2_agc6_3); /* 0x2D */
    st25r3916_write_reg(ST25R3916_REG_RX_CONF3, 0x00);
    st25r3916_write_reg(ST25R3916_REG_RX_CONF4, 0x00);
    st25r3916_write_reg(ST25R3916_REG_CORR_CONF1, 0x51);
    st25r3916_write_reg(ST25R3916_REG_CORR_CONF2, 0x00);
    /* Max TX drive for maximum read range: d_res = 0 (1 ohm, strongest field the
     * driver can produce). The unit is USB-powered so the higher drive current
     * (~67 mA continuous per spec) is fine. Flipper uses this value globally.
     * Preserve am_mod (modulation index) — only clear the d_res field. */
    {
        uint8_t v = 0;
        st25r3916_read_reg(ST25R3916_REG_TX_DRIVER, &v);
        v &= (uint8_t)~ST25R3916_REG_TX_DRIVER_d_res_mask;
        st25r3916_write_reg(ST25R3916_REG_TX_DRIVER, v);
    }
    /* Calibrate the TX driver timing for the actual antenna (after setting drive). */
    st25r3916_cmd(ST25R3916_CMD_CALIBRATE_DRIVER_TIMING);
    furi_delay_ms(1);
    FURI_LOG_I(TAG, "ST25R3916 chip init OK (NFC-A poller, max drive)");
    return St25rHalOk;
}

St25rHalStatus st25r3916_hal_field_on(void) {
    if(!s_chip_ready) {
        if(st25r3916_chip_init() != St25rHalOk) return St25rHalError;
        s_chip_ready = true;
    }
    /* Apply the selected RF technology (mode/bitrate/analog/framing). Also
     * restores poller/initiator framing after a card-emulation session (which
     * leaves the chip in target mode and would otherwise break reading). */
    st25r_apply_poller_tech();
    /* (Re)assert the RF field + receiver, honor the field-on guard time. Cheap
     * when the chip is already up — no SET_DEFAULT / osc restabilization. */
    st25r3916_write_reg(
        ST25R3916_REG_OP_CONTROL,
        ST25R3916_REG_OP_CONTROL_en | ST25R3916_REG_OP_CONTROL_tx_en |
            ST25R3916_REG_OP_CONTROL_rx_en);
    furi_delay_ms(5);
    return St25rHalOk;
}

static void emu_trace_dump(void); /* defined below; flushes the emulation trace */

/* Emulation trace buffer. Defined here (above its first use in field_off) rather
 * than with the other s_emu_* counters below, which sit after this function. */
static char* s_emu_trace = NULL;

void st25r3916_hal_field_off(void) {
    emu_trace_dump(); /* if we were emulating, dump the exchange now (CDC free) */
    /* Release the on-demand emulation trace buffer. This is the teardown that pairs
     * with the listen_start alloc; field_off is also called for poller (reader)
     * sessions where the buffer was never allocated, and heap_caps_free(NULL) is a
     * documented no-op, so this is safe on every field_off path. */
    heap_caps_free(s_emu_trace);
    s_emu_trace = NULL;
    st25r3916_cmd(ST25R3916_CMD_STOP); /* abort any active transceive */
    /* Drop the RF field (tx/rx) but KEEP the oscillator + all configuration.
     * The reader's poll loop cycles the field off/on every ~100ms to reset tag
     * power; re-running SET_DEFAULT + osc stabilization there was slow and made
     * the oscillator intermittently fail. Keeping the chip up makes field_on
     * instant and the field cycle clean. */
    st25r3916_write_reg(ST25R3916_REG_OP_CONTROL, ST25R3916_REG_OP_CONTROL_en);
    /* s_chip_ready stays true — chip remains configured. */
}

St25rHalStatus
    st25r3916_hal_reqa(bool wupa, uint8_t* rx, uint16_t rx_cap, uint16_t* rx_len) {
    uint8_t cmd = wupa ? ST25R3916_CMD_TRANSMIT_WUPA : ST25R3916_CMD_TRANSMIT_REQA;
    return st25r_transceive(cmd, NULL, 0, true, rx, rx_cap, rx_len, 20);
}

St25rHalStatus st25r3916_hal_transceive(
    const uint8_t* tx,
    uint16_t tx_len,
    uint8_t* rx,
    uint16_t rx_cap,
    uint16_t* rx_len,
    uint32_t rx_budget_ms) {
    if(!tx || tx_len == 0) return St25rHalError;
    /* Software owns the CRC (stack appended it, if any) → transmit WITHOUT
     * hardware CRC and keep the received CRC in the FIFO (no_crc_rx=1).
     *
     * ISO-DEP I-blocks carry the EMV APDUs. The upper NFC stack does NOT retry a
     * dropped APDU, so a single lost frame on this intermittently-dropping
     * I2C-polled path fails the whole EMV read ("unknown"). Retry I-blocks here:
     * re-sending an I-block with the SAME block number is the ISO-DEP
     * retransmission rule — the card processes it fresh (if it never arrived) or
     * re-sends its last reply (if only the reply was lost). Non-I-block frames
     * (activation probes / anticollision / RATS) are sent once so empty scans
     * stay fast. I-block PCB test: (pcb & 0xE2) == 0x02. */
    const bool is_iblock = ((tx[0] & 0xE2U) == 0x02U);
    /* I-blocks carry EMV APDUs. Give them a longer per-attempt budget (some EMV
     * crypto steps respond slower than a plain read, and a too-short budget
     * makes us re-send while the card is still computing) plus ONE retry to
     * recover a genuinely dropped frame — the upper stack itself never retries.
     * Two attempts, not three, so a card that truly can't complete fails fast
     * instead of dragging on the "don't move" screen. */
    const int attempts = is_iblock ? 2 : 1;
    const uint32_t budget = is_iblock ? 350U : rx_budget_ms;
    St25rHalStatus s = St25rHalNoCard;
    for(int i = 0; i < attempts; i++) {
        s = st25r_transceive(
            ST25R3916_CMD_TRANSMIT_WITHOUT_CRC, tx, tx_len, true, rx, rx_cap, rx_len, budget);
        if(s != St25rHalNoCard) break; /* got a reply, or a hard framing error */
    }
    return s;
}

/* ─────────────────────────── Listener / card emulation ─────────────────────
 * Passive-target IRQ bits (register 0x1D — not defined in st25r3916_reg.h;
 * ST25R3916 datasheet Table 65). */
#define IRQ_TARGET_WU_A    0x01U /* passive target reached Active state (SELECT done) */
#define IRQ_TARGET_WU_A_X  0x02U /* Active* state (re-select after HLTA) */
#define IRQ_TARGET_RXE_PTA 0x10U /* end of receive while chip auto-handled a response */

static uint8_t s_irq_target; /* accumulator for the target IRQ register (clear-on-read) */

/* MIFARE Classic emulation enters an encrypted phase after the reader sends AUTH:
 * from then on reader frames carry ENCRYPTED parity (not standard) and no CRC, so
 * the card-side RX must disable hardware parity/CRC and return de-interleaved data
 * bytes. Reset whenever the target is (re-)sensed. */
static bool s_listen_encrypted;

/* ── Emulation trace ring-buffer ────────────────────────────────────────────
 * The emulation poll loop hammers the I2C bus, which starves the USB-CDC task,
 * so any FURI_LOG emitted DURING emulation never escapes over serial. Instead
 * we record the reader<->card exchange into this RAM buffer and flush it out
 * AFTER the loop stops (field_off on teardown), when the CDC has recovered. */
#define EMU_TRACE_CAP 3072
/* On-demand: allocated in listen_start, freed in field_off, so this 3 KB is
 * resident ONLY during an active NFC card-emulation session and is reclaimed
 * the rest of the time (e.g. while Sub-GHz runs). NULL when no session is up.
 * (s_emu_trace itself is defined earlier, above st25r3916_hal_field_off.) */
static uint16_t s_emu_trace_len;
static uint16_t s_emu_seq;
static uint16_t s_emu_poll_calls; /* how many times listen_poll ran */
static uint16_t s_emu_wua_seen; /* WU_A activations observed */
static uint16_t s_emu_rxe_seen; /* post-activation RX frames observed */
static uint16_t s_emu_pta_states; /* bitmask of pta_state FSM values seen (1<<state) */
static uint16_t s_emu_eon; /* external-field-detect (I_eon interrupt) count */
static uint16_t s_emu_efd; /* external-field-detect (AUX_DISPLAY.efd_o LEVEL) count */

static void emu_trace_reset(void) {
    s_emu_trace_len = 0;
    s_emu_seq = 0;
    s_emu_poll_calls = 0;
    s_emu_wua_seen = 0;
    s_emu_rxe_seen = 0;
    s_emu_pta_states = 0;
    s_emu_eon = 0;
    s_emu_efd = 0;
}

static void emu_trace_add(const char* tag, const uint8_t* d, uint16_t n) {
    if(!s_emu_trace) return; /* trace buffer not allocated (no session / alloc failed) */
    if(s_emu_trace_len + 56 >= EMU_TRACE_CAP) return; /* buffer full: drop */
    int w = snprintf(
        s_emu_trace + s_emu_trace_len,
        EMU_TRACE_CAP - s_emu_trace_len,
        "#%u @%lums %s n=%u [%02X %02X %02X %02X %02X]\n",
        (unsigned)s_emu_seq++,
        (unsigned long)furi_get_tick(),
        tag,
        (unsigned)n,
        (n > 0) ? d[0] : 0,
        (n > 1) ? d[1] : 0,
        (n > 2) ? d[2] : 0,
        (n > 3) ? d[3] : 0,
        (n > 4) ? d[4] : 0);
    if(w > 0) s_emu_trace_len += (uint16_t)w;
}

/* Public wrapper so the Type-4 NDEF emulator (in furi_hal_nfc.c) can record its
 * exchange into the same ring-buffer. */
void st25r3916_hal_emu_trace(const char* tag, const uint8_t* data, uint16_t n) {
    emu_trace_add(tag, data, n);
}

/* Flush the emulation trace over serial. Safe no-op when nothing was recorded
 * during a plain poller cycle. Called from field_off (loop stopped, CDC free). */
static void emu_trace_dump(void) {
    if(s_emu_poll_calls == 0 && s_emu_seq == 0) return; /* not an emulation session */
    FURI_LOG_I(
        TAG,
        "=== EMU TRACE: %u events, poll=%u wua=%u rxe=%u eon=%u efd=%u fsm=0x%04X ===",
        (unsigned)s_emu_seq,
        (unsigned)s_emu_poll_calls,
        (unsigned)s_emu_wua_seen,
        (unsigned)s_emu_rxe_seen,
        (unsigned)s_emu_eon,
        (unsigned)s_emu_efd,
        (unsigned)s_emu_pta_states);
    /* fsm bitmask: bit N set => pta_state N was observed. 0=PWROFF 1=IDLE
     * 2=READY_L1 3=READY_L2 5=ACTIVE 9=HALT 0xA=RDY_L1* 0xB=RDY_L2* 0xD=ACTIVE* */
    uint16_t start = 0;
    if(s_emu_trace) {
        for(uint16_t i = 0; i < s_emu_trace_len; i++) {
            if(s_emu_trace[i] == '\n') {
                s_emu_trace[i] = '\0';
                FURI_LOG_I(TAG, "%s", &s_emu_trace[start]);
                start = (uint16_t)(i + 1);
                furi_delay_ms(2); /* let the CDC drain each line */
            }
        }
    }
    FURI_LOG_I(TAG, "=== EMU TRACE end ===");
    emu_trace_reset();
}

/* Configure the passive-target RX for the current phase. */
static void st25r_listen_set_rx_mode(bool encrypted) {
    uint8_t aux = 0;
    st25r3916_read_reg(ST25R3916_REG_AUX, &aux);
    if(encrypted) {
        st25r3916_write_reg(
            ST25R3916_REG_ISO14443A_NFC, ST25R3916_REG_ISO14443A_NFC_no_rx_par);
        st25r3916_write_reg(ST25R3916_REG_AUX, aux | ST25R3916_REG_AUX_no_crc_rx);
    } else {
        st25r3916_write_reg(ST25R3916_REG_ISO14443A_NFC, 0x00);
        st25r3916_write_reg(ST25R3916_REG_AUX, aux & (uint8_t)~ST25R3916_REG_AUX_no_crc_rx);
    }
}

/* De-interleave a raw 9-bit-group RX stream (from no_rx_par) into data bytes,
 * discarding the parity bit after each byte. Returns the number of data bytes. */
static uint16_t st25r_listen_deparity(
    const uint8_t* stream,
    uint16_t total_bits,
    uint8_t* out,
    uint16_t out_cap) {
    uint16_t nbytes = total_bits / 9;
    if(nbytes > out_cap) nbytes = out_cap;
    for(uint16_t i = 0; i < nbytes; i++) {
        uint16_t base = (uint16_t)(i * 9);
        uint8_t b = 0;
        for(int k = 0; k < 8; k++) {
            uint16_t bp = (uint16_t)(base + k);
            if(stream[bp >> 3] & (uint8_t)(1U << (bp & 7))) b |= (uint8_t)(1U << k);
        }
        out[i] = b;
    }
    return nbytes;
}

St25rHalStatus st25r3916_hal_listen_start(
    const uint8_t* uid,
    uint8_t uid_len,
    const uint8_t* atqa,
    uint8_t sak) {
    if(!uid || !atqa) return St25rHalError;
    if(!s_chip_ready) {
        if(st25r3916_chip_init() != St25rHalOk) return St25rHalError;
        s_chip_ready = true;
    }
    st25r3916_cmd(ST25R3916_CMD_STOP); /* drop any poller field/activity */

    /* (Card-emulation experiment note: bumping the shared I2C bus to 1 MHz was
     * tried to shrink the AUTH->nonce latency; it reached ~300-500 us but the
     * reader still rejected the response — the MIFARE ~87 us window is
     * unreachable over I2C — so the bus stays at the safe standard 400 kHz.) */

    /* PT_Memory-A: NFCID1 @0.., SENS_RES(ATQA) @10-11, SEL_RES L1/L2 @12-13.
     * The chip auto-answers REQA/anticollision/SELECT (incl. SAK) from this. */
    uint8_t ptm[14];
    memset(ptm, 0, sizeof(ptm));
    if(uid_len <= 4) {
        memcpy(ptm, uid, uid_len);
        ptm[12] = sak; /* single cascade: final SAK returned at level 1 */
    } else {
        memcpy(ptm, uid, (uid_len > 7) ? 7 : uid_len);
        ptm[12] = 0x04; /* CL1 SAK: cascade bit set => "UID not complete" */
        ptm[13] = sak; /* CL2 SAK: final */
    }
    ptm[10] = atqa[0];
    ptm[11] = atqa[1];
    if(st25r3916_write_pta_mem(ptm, sizeof(ptm)) != ESP_OK) return St25rHalError;

    /* UID length select. */
    uint8_t aux = 0;
    st25r3916_read_reg(ST25R3916_REG_AUX, &aux);
    aux &= (uint8_t)~ST25R3916_REG_AUX_nfc_id_mask;
    aux |= (uid_len <= 4) ? ST25R3916_REG_AUX_nfc_id_4bytes : ST25R3916_REG_AUX_nfc_id_7bytes;
    st25r3916_write_reg(ST25R3916_REG_AUX, aux);

    /* ── Card-emulation analog config — ported verbatim from Flipper's proven
     * ST25R3916 listener setup (targets/f7/furi_hal/furi_hal_nfc.c). Without
     * these the chip never detects the reader's field (I_eon) nor demodulates
     * the REQA: the FSM sits in IDLE forever (observed: eon=0, fsm=IDLE). ─── */

    /* TX driver resistance 1Ω + minimum non-overlap (strong, clean drive). */
    {
        uint8_t v = 0;
        st25r3916_read_reg(ST25R3916_REG_TX_DRIVER, &v);
        v &= (uint8_t)~ST25R3916_REG_TX_DRIVER_d_res_mask; /* d_res = 0 */
        st25r3916_write_reg(ST25R3916_REG_TX_DRIVER, v);
        st25r3916_read_reg(ST25R3916_REG_RES_AM_MOD, &v);
        st25r3916_write_reg(ST25R3916_REG_RES_AM_MOD, v | ST25R3916_REG_RES_AM_MOD_fa3_f);
    }

    /* External-field-detector thresholds: activation 105 mV, deactivation 75 mV.
     * The DEFAULT thresholds are too high for this antenna's coupling, so the
     * field detector never fires (I_eon) and the receiver stays gated. THIS is
     * the fix for eon=0 / stuck-in-IDLE. */
    st25r3916_write_reg(
        ST25R3916_REG_FIELD_THRESHOLD_ACTV,
        ST25R3916_REG_FIELD_THRESHOLD_ACTV_trg_105mV |
            ST25R3916_REG_FIELD_THRESHOLD_ACTV_rfe_105mV);
    st25r3916_write_reg(
        ST25R3916_REG_FIELD_THRESHOLD_DEACTV,
        ST25R3916_REG_FIELD_THRESHOLD_DEACTV_trg_75mV |
            ST25R3916_REG_FIELD_THRESHOLD_DEACTV_rfe_75mV);

    /* Enable internal AND external driver load modulation. */
    {
        uint8_t v = 0;
        st25r3916_read_reg(ST25R3916_REG_AUX_MOD, &v);
        st25r3916_write_reg(
            ST25R3916_REG_AUX_MOD,
            v | ST25R3916_REG_AUX_MOD_lm_ext | ST25R3916_REG_AUX_MOD_lm_dri);
    }

    /* PASSIVE_TARGET (Flipper's exact value 0x5C): FDT compensation fdel = 5
     * (bits fdel_2|fdel_0), DISABLE AP2P frame recognition (d_ac_ap2p) and
     * DISABLE automatic FeliCa SENSF_RES (d_212_424_1r) so ONLY NFC-A is active,
     * and keep automatic 106k NFC-A anticollision (d_106_ac_a = 0). */
    st25r3916_write_reg(
        ST25R3916_REG_PASSIVE_TARGET,
        ST25R3916_REG_PASSIVE_TARGET_fdel_2 | ST25R3916_REG_PASSIVE_TARGET_fdel_0 |
            ST25R3916_REG_PASSIVE_TARGET_d_ac_ap2p | ST25R3916_REG_PASSIVE_TARGET_d_212_424_1r);

    /* Mask-receive timer (Flipper sets 0x02) — masks RX briefly around TX. */
    st25r3916_write_reg(ST25R3916_REG_MASK_RX_TIMER, 0x02);

    /* Load-modulation DEPTH: ptm_res 0 (low R, modulated) / pt_res 0xF (high R,
     * non-modulated) = 0x0F. Flipper's value ("reduce RFO resistance in the
     * modulated state"). MUST be set before entering passive-target mode. */
    st25r3916_write_reg(ST25R3916_REG_PT_MOD, 0x0F);

    /* Start the receiver on the first 4 bits so the short REQA frame is caught. */
    {
        uint8_t v = 0;
        st25r3916_read_reg(ST25R3916_REG_EMD_SUP_CONF, &v);
        st25r3916_write_reg(
            ST25R3916_REG_EMD_SUP_CONF, v | ST25R3916_REG_EMD_SUP_CONF_rx_start_emv_on);
    }

    /* Antenna tuning (AAT-A/B) — Flipper's card-emulation values. */
    st25r3916_write_reg(ST25R3916_REG_ANT_TUNE_A, 0x82);
    st25r3916_write_reg(ST25R3916_REG_ANT_TUNE_B, 0x82);

    /* ISO14443-A target, fixed listen mode (om3=0), 106 kbit/s, HW parity. */
    st25r3916_write_reg(
        ST25R3916_REG_MODE, ST25R3916_REG_MODE_targ | ST25R3916_REG_MODE_om_targ_nfca);
    st25r3916_write_reg(ST25R3916_REG_BIT_RATE, 0x00);
    st25r3916_write_reg(ST25R3916_REG_ISO14443A_NFC, 0x00);

    /* Oscillator + receiver + driver on, external field detector auto. In target
     * mode tx_en drives LOAD MODULATION (impedance change on RFO1/2), not a field
     * — required for the card-side response to transmit (else TXE never fires =>
     * "Tx error"). Internal-driver load modulation (lm_dri) is on by default. */
    /* OP_CONTROL (Flipper's exact listener value): oscillator + receiver + auto
     * Enhanced Field Detector — NO tx_en. Keeping tx_en on drives the antenna
     * load continuously, which disturbs field detection and reception (observed:
     * eon=0, FSM stuck IDLE). The card-side response does NOT need tx_en here:
     * the TRANSMIT command drives load modulation via lm_dri on its own. The EFD
     * detects the reader's field (I_eon), un-gates RX, and the HW anticollision
     * engine advances IDLE -> READY_L1 -> ... -> ACTIVE (I_wu_a). */
    st25r3916_write_reg(
        ST25R3916_REG_OP_CONTROL,
        ST25R3916_REG_OP_CONTROL_en | ST25R3916_REG_OP_CONTROL_rx_en |
            ST25R3916_REG_OP_CONTROL_en_fd_auto_efd);

    s_listen_encrypted = false; /* fresh session: plaintext until AUTH */
    /* Fully reset the RX decoder to plaintext — clears AUX.no_crc_rx that a PRIOR
     * encrypted (AUTH) session left set. Without this, a re-armed emulation keeps
     * no_crc_rx on and the anticollision/reception is corrupted (observed: first
     * enter activates, subsequent re-arms stall at READY_L1). */
    st25r_listen_set_rx_mode(false);
    /* On-demand: allocate the diagnostic trace buffer for this emulation session.
     * Guarded (if !s_emu_trace) so a re-listen / re-sense that re-enters
     * listen_start WITHOUT an intervening field_off reuses the existing buffer and
     * never leaks. It is a pure debug ring-buffer: if the alloc fails we log and
     * proceed — emu_trace_add is null-guarded and simply drops trace output, so a
     * transient low-memory condition must not block NFC card emulation. */
    if(!s_emu_trace) {
        s_emu_trace = heap_caps_malloc(EMU_TRACE_CAP, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if(!s_emu_trace) FURI_LOG_W(TAG, "emu trace alloc failed; tracing disabled");
    }
    emu_trace_reset(); /* fresh trace for this emulation session */
    st25r3916_cmd(ST25R3916_CMD_GOTO_SENSE); /* ready to be sensed/activated */
    s_irq_target = 0;
    irq_clear();
    FURI_LOG_I(TAG, "ST25R3916 listen start (NFC-A target)");
    return St25rHalOk;
}

St25rHalStatus st25r3916_hal_listen_poll(
    uint8_t* out_events,
    uint8_t* rx,
    uint16_t rx_cap,
    uint16_t* rx_len,
    uint32_t timeout_ms) {
    *out_events = 0;
    *rx_len = 0;
    if(timeout_ms == 0) timeout_ms = 1;
    s_emu_poll_calls++;
    uint32_t start_tick = furi_get_tick();
    uint32_t iter = 0;
    for(;;) {
        uint8_t ev = 0;

        /* ── RX FIRST (minimum latency for the AUTH-response window) ────────── */
        uint8_t m = 0;
        if(st25r3916_read_reg(ST25R3916_REG_IRQ_MAIN, &m) == ESP_OK) s_irq_main |= m;
        if(s_irq_main & IRQ_MAIN_RXE) {
            s_irq_main &= (uint8_t)~IRQ_MAIN_RXE;
            uint8_t st1 = 0, st2 = 0;
            st25r3916_read_reg(ST25R3916_REG_FIFO_STATUS1, &st1);
            st25r3916_read_reg(ST25R3916_REG_FIFO_STATUS2, &st2);
            uint16_t cnt = (uint16_t)st1 +
                           (uint16_t)(((st2 & ST25R3916_REG_FIFO_STATUS2_fifo_b_mask) >>
                                       ST25R3916_REG_FIFO_STATUS2_fifo_b_shift)
                                      << 8);
            if(cnt > ST25R_HAL_FIFO_MAX) cnt = ST25R_HAL_FIFO_MAX;
            uint8_t raw[ST25R_HAL_FIFO_MAX];
            uint16_t rawn = (cnt > sizeof(raw)) ? sizeof(raw) : cnt;
            if(rawn > 0 && st25r3916_read_fifo(raw, (uint8_t)rawn) == ESP_OK) {
                if(s_listen_encrypted) {
                    uint8_t lb = (uint8_t)((st2 & ST25R3916_REG_FIFO_STATUS2_fifo_lb_mask) >>
                                           ST25R3916_REG_FIFO_STATUS2_fifo_lb_shift);
                    uint16_t bits = (uint16_t)((rawn - 1) * 8 + (lb == 0 ? 8 : lb));
                    *rx_len = st25r_listen_deparity(raw, bits, rx, rx_cap);
                    ev |= ST25R_LISTEN_EV_ENC;
                } else {
                    uint16_t n = (rawn > rx_cap) ? rx_cap : rawn;
                    memcpy(rx, raw, n);
                    *rx_len = n;
                    /* AUTH (0x60/0x61) starts the MIFARE encrypted phase: the
                     * reader's next frame ({nR}{aR}) carries encrypted parity and
                     * no CRC, so switch the RX decoder before surfacing this to the
                     * stack. NB: the full Crypto1 sector emulation cannot complete
                     * on this unit — the ~87us reader window is unreachable over
                     * I2C (measured; see notes) — but UID/anticollision emulation
                     * (all handled by the chip in hardware) works regardless. */
                    if(n >= 1 && (rx[0] == 0x60 || rx[0] == 0x61)) {
                        s_listen_encrypted = true;
                        st25r_listen_set_rx_mode(true);
                    }
                }
                ev |= ST25R_LISTEN_EV_RX;
                s_emu_rxe_seen++;
                emu_trace_add((ev & ST25R_LISTEN_EV_ENC) ? "RXe" : "RXp", rx, *rx_len);
            }
        }

        /* ── activation + diagnostics (less time-critical; every 8th pass) ─── */
        if((iter & 0x07U) == 0) {
            uint8_t tgt = 0;
            if(st25r3916_read_reg(ST25R3916_REG_IRQ_TARGET, &tgt) == ESP_OK) s_irq_target |= tgt;
            if(s_irq_target & (IRQ_TARGET_WU_A | IRQ_TARGET_WU_A_X)) {
                ev |= ST25R_LISTEN_EV_ACTIVATED;
                s_irq_target &= (uint8_t)~(IRQ_TARGET_WU_A | IRQ_TARGET_WU_A_X);
                s_emu_wua_seen++;
                emu_trace_add("ACT", NULL, 0);
            }
            uint8_t pta = 0;
            if(st25r3916_read_reg(ST25R3916_REG_PASSIVE_TARGET_STATUS, &pta) == ESP_OK)
                s_emu_pta_states |= (uint16_t)(1U << (pta & 0x0FU));
            if(s_irq_main & 0x10U) s_emu_eon++; /* I_eon (latched) */
            uint8_t adisp = 0;
            if(st25r3916_read_reg(ST25R3916_REG_AUX_DISPLAY, &adisp) == ESP_OK) {
                if(adisp & ST25R3916_REG_AUX_DISPLAY_efd_o) s_emu_efd++;
            }
        }

        if(ev) {
            *out_events = ev;
            return St25rHalOk;
        }
        iter++;
        /* Outer loop only needs to catch the reader's next command promptly; the
         * latency-critical part is the inner {nR}{aR} probe (fully busy). Give the
         * idle task a real RTOS slice every 16 passes so the task watchdog is fed
         * (a bare furi_thread_yield won't run idle if we stay highest-ready). */
        if((iter & 0x1FU) == 0) furi_delay_ms(1);
        if(furi_get_tick() - start_tick >= timeout_ms) break;
    }
    return St25rHalNoCard; /* no event within the budget */
}

St25rHalStatus st25r3916_hal_listen_tx(const uint8_t* data, uint16_t len, bool with_crc) {
    if(!data || len == 0 || len > ST25R_HAL_FIFO_MAX) return St25rHalError;
    st25r3916_write_reg(ST25R3916_REG_ISO14443A_NFC, 0x00); /* hardware parity */
    st25r3916_cmd(ST25R3916_CMD_CLEAR_FIFO);
    irq_clear();
    if(st25r3916_write_fifo(data, (uint8_t)len) != ESP_OK) return St25rHalError;
    st25r3916_write_reg(ST25R3916_REG_NUM_TX_BYTES1, (uint8_t)(len >> 5));
    st25r3916_write_reg(ST25R3916_REG_NUM_TX_BYTES2, (uint8_t)((len & 0x1F) << 3));
    uint8_t cmd = with_crc ? ST25R3916_CMD_TRANSMIT_WITH_CRC : ST25R3916_CMD_TRANSMIT_WITHOUT_CRC;
    if(st25r3916_cmd(cmd) != ESP_OK) return St25rHalError;
    for(int t = 0; t < 10; t++) {
        irq_poll();
        if(s_irq_main & IRQ_MAIN_TXE) {
            emu_trace_add("TXp", data, len);
            return St25rHalOk;
        }
        furi_delay_ms(1);
    }
    emu_trace_add("TXp!txe", data, len); /* no TXE within window */
    return St25rHalError;
}

St25rHalStatus st25r3916_hal_listen_tx_custom_parity(
    const uint8_t* data,
    const uint8_t* parity,
    uint16_t nbytes) {
    if(!data || !parity || nbytes == 0 || nbytes > 28) return St25rHalError;
    /* Interleave separate data[] + parity[] into a 9-bit-group stream (8 data
     * bits LSB-first, then 1 parity bit), byte-packed. */
    uint8_t stream[36];
    memset(stream, 0, sizeof(stream));
    uint16_t bitpos = 0;
    for(uint16_t i = 0; i < nbytes; i++) {
        uint8_t b = data[i];
        uint8_t p = (uint8_t)((parity[i >> 3] >> (i & 7)) & 1U);
        for(int k = 0; k < 8; k++) {
            if(b & (1U << k)) stream[bitpos >> 3] |= (uint8_t)(1U << (bitpos & 7));
            bitpos++;
        }
        if(p) stream[bitpos >> 3] |= (uint8_t)(1U << (bitpos & 7));
        bitpos++;
    }
    uint16_t total_bits = bitpos; /* 9 * nbytes */
    uint16_t full = total_bits / 8;
    uint8_t nb = (uint8_t)(total_bits % 8);
    uint16_t fifo_bytes = full + (nb ? 1 : 0);

    st25r3916_write_reg(ST25R3916_REG_ISO14443A_NFC, ST25R3916_REG_ISO14443A_NFC_no_tx_par);
    st25r3916_cmd(ST25R3916_CMD_CLEAR_FIFO);
    irq_clear();
    St25rHalStatus r = St25rHalError;
    do {
        if(st25r3916_write_fifo(stream, (uint8_t)fifo_bytes) != ESP_OK) break;
        st25r3916_write_reg(ST25R3916_REG_NUM_TX_BYTES1, (uint8_t)(full >> 5));
        st25r3916_write_reg(
            ST25R3916_REG_NUM_TX_BYTES2, (uint8_t)(((full & 0x1F) << 3) | (nb & 0x07)));
        if(st25r3916_cmd(ST25R3916_CMD_TRANSMIT_WITHOUT_CRC) != ESP_OK) break;
        for(int t = 0; t < 10; t++) {
            irq_poll();
            if(s_irq_main & IRQ_MAIN_TXE) {
                r = St25rHalOk;
                break;
            }
            furi_delay_ms(1);
        }
    } while(0);
    st25r3916_write_reg(ST25R3916_REG_ISO14443A_NFC, 0x00); /* restore HW parity */
    emu_trace_add((r == St25rHalOk) ? "TXe" : "TXe!txe", data, nbytes);
    return r;
}

void st25r3916_hal_listen_sleep(void) {
    st25r3916_cmd(ST25R3916_CMD_GOTO_SLEEP);
}

void st25r3916_hal_listen_sense(void) {
    s_listen_encrypted = false;
    st25r_listen_set_rx_mode(false); /* back to plaintext HW parity/CRC */
    st25r3916_cmd(ST25R3916_CMD_GOTO_SENSE);
    s_irq_target = 0;
    irq_clear();
}

void st25r3916_hal_listen_stop(void) {
    st25r3916_cmd(ST25R3916_CMD_STOP);
    /* Return the chip to poller/initiator defaults so a later read still works. */
    st25r3916_write_reg(ST25R3916_REG_MODE, ST25R3916_REG_MODE_om_iso14443a);
    st25r3916_write_reg(ST25R3916_REG_OP_CONTROL, ST25R3916_REG_OP_CONTROL_en);
}

#else /* !BOARD_HAS_ST25R3916 */

bool st25r3916_hal_present(void) {
    return false;
}
St25rHalStatus st25r3916_hal_field_on(void) {
    return St25rHalError;
}
void st25r3916_hal_field_off(void) {
}
St25rHalStatus st25r3916_hal_reqa(bool wupa, uint8_t* rx, uint16_t rx_cap, uint16_t* rx_len) {
    (void)wupa;
    (void)rx;
    (void)rx_cap;
    if(rx_len) *rx_len = 0;
    return St25rHalError;
}
St25rHalStatus st25r3916_hal_transceive(
    const uint8_t* tx,
    uint16_t tx_len,
    uint8_t* rx,
    uint16_t rx_cap,
    uint16_t* rx_len,
    uint32_t rx_budget_ms) {
    (void)tx;
    (void)tx_len;
    (void)rx;
    (void)rx_cap;
    (void)rx_budget_ms;
    if(rx_len) *rx_len = 0;
    return St25rHalError;
}
St25rHalStatus st25r3916_hal_transceive_custom_parity(
    const uint8_t* tx,
    uint16_t tx_bits,
    uint8_t* rx,
    uint16_t rx_cap,
    uint16_t* rx_bits,
    uint32_t rx_budget_ms) {
    (void)tx;
    (void)tx_bits;
    (void)rx;
    (void)rx_cap;
    (void)rx_budget_ms;
    if(rx_bits) *rx_bits = 0;
    return St25rHalError;
}
St25rHalStatus st25r3916_hal_listen_start(
    const uint8_t* uid,
    uint8_t uid_len,
    const uint8_t* atqa,
    uint8_t sak) {
    (void)uid;
    (void)uid_len;
    (void)atqa;
    (void)sak;
    return St25rHalError;
}
St25rHalStatus st25r3916_hal_listen_poll(
    uint8_t* out_events,
    uint8_t* rx,
    uint16_t rx_cap,
    uint16_t* rx_len,
    uint32_t timeout_ms) {
    (void)rx;
    (void)rx_cap;
    (void)timeout_ms;
    if(out_events) *out_events = 0;
    if(rx_len) *rx_len = 0;
    return St25rHalError;
}
St25rHalStatus st25r3916_hal_listen_tx(const uint8_t* data, uint16_t len, bool with_crc) {
    (void)data;
    (void)len;
    (void)with_crc;
    return St25rHalError;
}
St25rHalStatus st25r3916_hal_listen_tx_custom_parity(
    const uint8_t* data,
    const uint8_t* parity,
    uint16_t nbytes) {
    (void)data;
    (void)parity;
    (void)nbytes;
    return St25rHalError;
}
void st25r3916_hal_listen_sleep(void) {
}
void st25r3916_hal_listen_sense(void) {
}
void st25r3916_hal_listen_stop(void) {
}

#endif /* BOARD_HAS_ST25R3916 */
