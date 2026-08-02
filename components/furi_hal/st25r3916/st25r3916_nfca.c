/**
 * @file st25r3916_nfca.c
 * @brief Minimal NFC-A reader (REQA + single-card anticollision) on ST25R3916.
 * See st25r3916_nfca.h. Polled IRQ (no IRQ line on the M5Stack unit).
 */

#include "st25r3916_nfca.h"
#include "st25r3916_i2c.h"
#include "st25r3916_reg.h"

#include <furi.h>
#include <board.h>
#include <string.h>

#define TAG "St25rNfca"

#if defined(BOARD_HAS_ST25R3916) && BOARD_HAS_ST25R3916

/* IRQ status-register bit masks (register-local, ST25R3916 datasheet DS12484).
 * The reg header defines addresses but not these fields. */
#define IRQ_MAIN_OSC 0x80U /* oscillator frequency stable   */
#define IRQ_MAIN_RXS 0x20U /* start of receive              */
#define IRQ_MAIN_RXE 0x10U /* end of receive                */
#define IRQ_MAIN_TXE 0x08U /* end of transmit               */
#define IRQ_MAIN_COL 0x04U /* bit collision                 */
#define IRQ_TIMER_NRE 0x40U /* no-response timer expired    */
#define IRQ_ERR_CRC 0x80U /* CRC error                      */
#define IRQ_ERR_PAR 0x40U /* parity error                   */

/* Accumulated IRQ flags (status regs are clear-on-read; OR across polls). */
static uint8_t s_irq_main, s_irq_timer, s_irq_err;

static void irq_clear(void) {
    uint8_t v;
    st25r3916_read_reg(ST25R3916_REG_IRQ_MAIN, &v);
    st25r3916_read_reg(ST25R3916_REG_IRQ_TIMER_NFC, &v);
    st25r3916_read_reg(ST25R3916_REG_IRQ_ERROR_WUP, &v);
    s_irq_main = s_irq_timer = s_irq_err = 0;
}

static void irq_poll(void) {
    uint8_t v;
    if(st25r3916_read_reg(ST25R3916_REG_IRQ_MAIN, &v) == ESP_OK) s_irq_main |= v;
    if(st25r3916_read_reg(ST25R3916_REG_IRQ_TIMER_NFC, &v) == ESP_OK) s_irq_timer |= v;
    if(st25r3916_read_reg(ST25R3916_REG_IRQ_ERROR_WUP, &v) == ESP_OK) s_irq_err |= v;
}

/* Transmit a frame (or a fixed TX command like REQA) and receive the answer.
 * tx_cmd = the direct command that fires the frame; tx/tx_len = FIFO payload
 * (NULL/0 for REQA/WUPA which carry their own bits). */
static St25rNfcaStatus st25r_transceive(
    uint8_t tx_cmd,
    const uint8_t* tx,
    uint8_t tx_len,
    bool no_crc_rx,
    uint8_t* rx,
    uint8_t rx_cap,
    uint8_t* rx_len,
    int rx_wait_ms) {
    *rx_len = 0;

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
        if(st25r3916_write_fifo(tx, tx_len) != ESP_OK) return St25rNfcaErrProto;
        /* Byte-aligned frame: ntx = tx_len bytes, 0 partial bits.
         * NUM_TX_BYTES1 = ntx[12:5], NUM_TX_BYTES2[7:3] = ntx[4:0]. */
        st25r3916_write_reg(ST25R3916_REG_NUM_TX_BYTES1, (uint8_t)(tx_len >> 5));
        st25r3916_write_reg(ST25R3916_REG_NUM_TX_BYTES2, (uint8_t)((tx_len & 0x1F) << 3));
    }

    if(st25r3916_cmd(tx_cmd) != ESP_OK) return St25rNfcaErrProto;

    /* Wait end-of-transmit (~10 ms budget). */
    bool txe = false;
    for(int t = 0; t < 10; t++) {
        irq_poll();
        if(s_irq_main & IRQ_MAIN_TXE) {
            txe = true;
            break;
        }
        furi_delay_ms(1);
    }
    if(!txe) return St25rNfcaErrProto;

    /* Wait end-of-receive / error / silence (caller-supplied budget, ms). */
    for(int t = 0; t < rx_wait_ms; t++) {
        irq_poll();
        if(s_irq_err & (IRQ_ERR_CRC | IRQ_ERR_PAR)) return St25rNfcaErrProto;
        if(s_irq_main & IRQ_MAIN_RXE) break;
        if(s_irq_timer & IRQ_TIMER_NRE) return St25rNfcaErrNoCard;
        furi_delay_ms(1);
    }
    if(!(s_irq_main & IRQ_MAIN_RXE)) return St25rNfcaErrNoCard;

    uint8_t st1 = 0, st2 = 0;
    st25r3916_read_reg(ST25R3916_REG_FIFO_STATUS1, &st1);
    st25r3916_read_reg(ST25R3916_REG_FIFO_STATUS2, &st2);
    uint16_t cnt = (uint16_t)st1 +
                   (uint16_t)(((st2 & ST25R3916_REG_FIFO_STATUS2_fifo_b_mask) >>
                               ST25R3916_REG_FIFO_STATUS2_fifo_b_shift)
                              << 8);
    if(cnt == 0) return St25rNfcaErrNoCard;
    if(cnt > rx_cap) cnt = rx_cap;
    if(st25r3916_read_fifo(rx, (uint8_t)cnt) != ESP_OK) return St25rNfcaErrProto;
    *rx_len = (uint8_t)cnt;
    return St25rNfcaOk;
}

St25rNfcaStatus st25r3916_nfca_init(void) {
    if(!st25r3916_probe()) return St25rNfcaErrInit; /* also brings up I2C */

    st25r3916_cmd(ST25R3916_CMD_SET_DEFAULT);
    furi_delay_ms(1);

    /* Unmask all IRQ sources so the status registers latch every event. */
    st25r3916_write_reg(ST25R3916_REG_IRQ_MASK_MAIN, 0x00);
    st25r3916_write_reg(ST25R3916_REG_IRQ_MASK_TIMER_NFC, 0x00);
    st25r3916_write_reg(ST25R3916_REG_IRQ_MASK_ERROR_WUP, 0x00);
    st25r3916_write_reg(ST25R3916_REG_IRQ_MASK_TARGET, 0x00);

    /* Enable oscillator + regulator, wait for it to stabilize. */
    st25r3916_write_reg(ST25R3916_REG_OP_CONTROL, ST25R3916_REG_OP_CONTROL_en);
    irq_clear();
    for(int t = 0; t < 20; t++) {
        irq_poll();
        if(s_irq_main & IRQ_MAIN_OSC) break;
        furi_delay_ms(1);
    }
    st25r3916_cmd(ST25R3916_CMD_ADJUST_REGULATORS);
    furi_delay_ms(1);

    /* NFC-A poller (initiator), 106 kbit/s, hardware parity. */
    st25r3916_write_reg(ST25R3916_REG_MODE, ST25R3916_REG_MODE_om_iso14443a);
    st25r3916_write_reg(ST25R3916_REG_BIT_RATE, 0x00);
    st25r3916_write_reg(ST25R3916_REG_ISO14443A_NFC, 0x00);

    /* NOTE: analog RX config left at SET_DEFAULT values — first on-hardware
     * tuning point if the field powers but no tag ever answers. */

    /* Field on: enable driver + receiver, then honor the field-on guard time. */
    st25r3916_write_reg(
        ST25R3916_REG_OP_CONTROL,
        ST25R3916_REG_OP_CONTROL_en | ST25R3916_REG_OP_CONTROL_tx_en |
            ST25R3916_REG_OP_CONTROL_rx_en);
    furi_delay_ms(5);
    return St25rNfcaOk;
}

St25rNfcaStatus st25r3916_nfca_poll(St25rNfcaTag* out) {
    if(!out) return St25rNfcaErrProto;
    memset(out, 0, sizeof(*out));

    uint8_t rx[16];
    uint8_t rxn = 0;

    /* REQA -> ATQA (2 bytes, no CRC). Silence = empty field. */
    St25rNfcaStatus s =
        st25r_transceive(ST25R3916_CMD_TRANSMIT_REQA, NULL, 0, true, rx, sizeof(rx), &rxn, 20);
    if(s != St25rNfcaOk || rxn < 2) return St25rNfcaErrNoCard;
    out->atqa = (uint16_t)(rx[0] | (rx[1] << 8));

    static const uint8_t sel_cmd[3] = {0x93, 0x95, 0x97}; /* cascade levels 1..3 */
    uint8_t uid[10];
    uint8_t uidlen = 0;

    for(int cl = 0; cl < 3; cl++) {
        /* Anticollision: [SEL][NVB=0x20] without CRC -> 4 UID bytes + BCC. */
        uint8_t ac[2] = {sel_cmd[cl], 0x20};
        s = st25r_transceive(
            ST25R3916_CMD_TRANSMIT_WITHOUT_CRC, ac, 2, true, rx, sizeof(rx), &rxn, 20);
        if(s != St25rNfcaOk || rxn < 5) return St25rNfcaErrAnticoll;
        if((uint8_t)(rx[0] ^ rx[1] ^ rx[2] ^ rx[3]) != rx[4]) return St25rNfcaErrAnticoll;

        /* Select: [SEL][0x70][UID CL][BCC] with CRC -> SAK. */
        uint8_t sel[7] = {sel_cmd[cl], 0x70, rx[0], rx[1], rx[2], rx[3], rx[4]};
        uint8_t sak_rx[8];
        uint8_t sakn = 0;
        s = st25r_transceive(
            ST25R3916_CMD_TRANSMIT_WITH_CRC, sel, 7, false, sak_rx, sizeof(sak_rx), &sakn, 20);
        if(s != St25rNfcaOk || sakn < 1) return St25rNfcaErrAnticoll;
        out->sak = sak_rx[0];

        if(sak_rx[0] & 0x04) {
            /* Cascade: rx[0] is the cascade tag (0x88); UID bytes are rx[1..3]. */
            if(uidlen + 3 > (int)sizeof(uid)) return St25rNfcaErrProto;
            uid[uidlen++] = rx[1];
            uid[uidlen++] = rx[2];
            uid[uidlen++] = rx[3];
        } else {
            if(uidlen + 4 > (int)sizeof(uid)) return St25rNfcaErrProto;
            uid[uidlen++] = rx[0];
            uid[uidlen++] = rx[1];
            uid[uidlen++] = rx[2];
            uid[uidlen++] = rx[3];
            break; /* UID complete */
        }
    }

    out->uid_len = uidlen;
    memcpy(out->uid, uid, uidlen);
    return St25rNfcaOk;
}

St25rNfcaStatus st25r3916_nfca_poller_txrx(
    const uint8_t* tx, uint8_t tx_len, uint8_t* rx, uint8_t rx_cap, uint8_t* rx_len) {
    if(!tx || tx_len == 0 || !rx || !rx_len) return St25rNfcaErrProto;
    /* Post-activation standard-frame exchange: TX with CRC-A appended, then RX
     * the answer (CRC-A checked + stripped) within a generous ~90 ms window —
     * sized above the Flipper-Share transport's ~74 ms FWT so a slow software
     * (I2C) listener has time to load-modulate its response. Call only after a
     * successful st25r3916_nfca_poll() activation. NoCard on silence. */
    return st25r_transceive(
        ST25R3916_CMD_TRANSMIT_WITH_CRC, tx, tx_len, false, rx, rx_cap, rx_len, 90);
}

void st25r3916_nfca_deinit(void) {
    st25r3916_cmd(ST25R3916_CMD_STOP);
    st25r3916_write_reg(ST25R3916_REG_OP_CONTROL, 0x00); /* field + rx + osc off */
}

const char* st25r3916_nfca_status_str(St25rNfcaStatus s) {
    switch(s) {
    case St25rNfcaOk:
        return "OK";
    case St25rNfcaErrInit:
        return "chip init failed";
    case St25rNfcaErrField:
        return "field on failed";
    case St25rNfcaErrNoCard:
        return "no card";
    case St25rNfcaErrAnticoll:
        return "anticoll/select err";
    case St25rNfcaErrProto:
        return "framing/CRC err";
    default:
        return "?";
    }
}

/* ─────────────────── NFC-A card emulation (passive target) ──────────────── */

St25rNfcaStatus st25r3916_nfca_listen_start(
    const uint8_t* uid,
    uint8_t uid_len,
    uint16_t atqa,
    uint8_t sak) {
    if(!uid || (uid_len != 4 && uid_len != 7 && uid_len != 10)) return St25rNfcaErrProto;
    if(!st25r3916_probe()) return St25rNfcaErrInit;

    st25r3916_cmd(ST25R3916_CMD_SET_DEFAULT);
    furi_delay_ms(1);

    /* Passive Target Memory A-config (15 bytes): NFCID1 @0..9, ATQA @10..11
     * (LSB first), SEL_RES/SAK @12..14 (one per cascade level). The chip runs
     * REQA/WUPA -> anticollision -> SELECT autonomously from this, using the
     * NFCID1 length to drive single/double/triple cascade. */
    uint8_t ptm[ST25R3916_PTM_A_LEN];
    memset(ptm, 0, sizeof(ptm));
    memcpy(ptm, uid, uid_len);
    ptm[10] = (uint8_t)(atqa & 0xFF);
    ptm[11] = (uint8_t)(atqa >> 8);
    ptm[12] = sak; /* CL1 */
    ptm[13] = sak; /* CL2 */
    ptm[14] = sak; /* CL3 */
    if(st25r3916_write_pta_mem(ptm, ST25R3916_PTM_A_LEN) != ESP_OK) return St25rNfcaErrProto;

    /* Latch every event in the status registers (polled; no IRQ line). */
    st25r3916_write_reg(ST25R3916_REG_IRQ_MASK_MAIN, 0x00);
    st25r3916_write_reg(ST25R3916_REG_IRQ_MASK_TIMER_NFC, 0x00);
    st25r3916_write_reg(ST25R3916_REG_IRQ_MASK_ERROR_WUP, 0x00);
    st25r3916_write_reg(ST25R3916_REG_IRQ_MASK_TARGET, 0x00);

    /* Oscillator/regulator on, wait for stable. */
    st25r3916_write_reg(ST25R3916_REG_OP_CONTROL, ST25R3916_REG_OP_CONTROL_en);
    irq_clear();
    for(int t = 0; t < 20; t++) {
        irq_poll();
        if(s_irq_main & IRQ_MAIN_OSC) break;
        furi_delay_ms(1);
    }

    /* ── Card-emulation analog config — REQUIRED for the chip to leave IDLE. ──
     * This block is ported verbatim from the hardware-proven listener setup in
     * st25r3916_hal.c (itself ported from Flipper's ST25R3916 card-emulation
     * config). Without it the External Field Detector never fires on this
     * antenna's coupling (I_eon=0), the receiver stays gated, and the passive-
     * target FSM sits in IDLE forever — so REQA is never auto-answered and a
     * phone/reader sees no card. An earlier clean reimplementation of this
     * function dropped this block; that is what caused "phone doesn't detect
     * anything." Register order mirrors the proven path. */

    /* UID-length select drives the HW anticollision cascade (single vs double).
     * The 7-byte Share UID needs the double cascade — without this the SELECT
     * never completes even if the field is detected. */
    {
        uint8_t aux = 0;
        st25r3916_read_reg(ST25R3916_REG_AUX, &aux);
        aux &= (uint8_t)~ST25R3916_REG_AUX_nfc_id_mask;
        aux |= (uid_len <= 4) ? ST25R3916_REG_AUX_nfc_id_4bytes :
                                ST25R3916_REG_AUX_nfc_id_7bytes;
        st25r3916_write_reg(ST25R3916_REG_AUX, aux);
    }

    /* TX driver resistance minimum + full AM modulation (strong, clean drive). */
    {
        uint8_t v = 0;
        st25r3916_read_reg(ST25R3916_REG_TX_DRIVER, &v);
        v &= (uint8_t)~ST25R3916_REG_TX_DRIVER_d_res_mask; /* d_res = 0 */
        st25r3916_write_reg(ST25R3916_REG_TX_DRIVER, v);
        st25r3916_read_reg(ST25R3916_REG_RES_AM_MOD, &v);
        st25r3916_write_reg(ST25R3916_REG_RES_AM_MOD, v | ST25R3916_REG_RES_AM_MOD_fa3_f);
    }

    /* External-field-detector thresholds: activation 105 mV, deactivation 75 mV.
     * The DEFAULT thresholds are too high for this antenna, so the field
     * detector never fires and the receiver stays gated — THIS is the fix for
     * eon=0 / stuck-in-IDLE. */
    st25r3916_write_reg(
        ST25R3916_REG_FIELD_THRESHOLD_ACTV,
        ST25R3916_REG_FIELD_THRESHOLD_ACTV_trg_105mV |
            ST25R3916_REG_FIELD_THRESHOLD_ACTV_rfe_105mV);
    st25r3916_write_reg(
        ST25R3916_REG_FIELD_THRESHOLD_DEACTV,
        ST25R3916_REG_FIELD_THRESHOLD_DEACTV_trg_75mV |
            ST25R3916_REG_FIELD_THRESHOLD_DEACTV_rfe_75mV);

    /* Enable internal AND external driver load modulation (card-side response). */
    {
        uint8_t v = 0;
        st25r3916_read_reg(ST25R3916_REG_AUX_MOD, &v);
        st25r3916_write_reg(
            ST25R3916_REG_AUX_MOD,
            v | ST25R3916_REG_AUX_MOD_lm_ext | ST25R3916_REG_AUX_MOD_lm_dri);
    }

    /* PASSIVE_TARGET (proven value 0x5C): FDT compensation fdel=5, DISABLE AP2P
     * frame recognition and automatic FeliCa SENSF_RES so ONLY NFC-A is active,
     * and keep automatic 106k NFC-A anticollision (d_106_ac_a left clear). */
    st25r3916_write_reg(
        ST25R3916_REG_PASSIVE_TARGET,
        ST25R3916_REG_PASSIVE_TARGET_fdel_2 | ST25R3916_REG_PASSIVE_TARGET_fdel_0 |
            ST25R3916_REG_PASSIVE_TARGET_d_ac_ap2p | ST25R3916_REG_PASSIVE_TARGET_d_212_424_1r);

    /* Mask-receive timer masks RX briefly around TX. */
    st25r3916_write_reg(ST25R3916_REG_MASK_RX_TIMER, 0x02);

    /* Load-modulation DEPTH (ptm_res 0 modulated / pt_res 0xF non-modulated).
     * MUST be set before entering passive-target mode. */
    st25r3916_write_reg(ST25R3916_REG_PT_MOD, 0x0F);

    /* Start the receiver on the first bits so the short REQA frame is caught. */
    {
        uint8_t v = 0;
        st25r3916_read_reg(ST25R3916_REG_EMD_SUP_CONF, &v);
        st25r3916_write_reg(
            ST25R3916_REG_EMD_SUP_CONF, v | ST25R3916_REG_EMD_SUP_CONF_rx_start_emv_on);
    }

    /* Antenna tuning (AAT-A/B) card-emulation values. */
    st25r3916_write_reg(ST25R3916_REG_ANT_TUNE_A, 0x82);
    st25r3916_write_reg(ST25R3916_REG_ANT_TUNE_B, 0x82);

    /* ISO14443-A target, fixed listen (nfc_ar left 0 for a passive target),
     * 106 kbit/s, HW parity. */
    st25r3916_write_reg(
        ST25R3916_REG_MODE, ST25R3916_REG_MODE_targ | ST25R3916_REG_MODE_om_targ_nfca);
    st25r3916_write_reg(ST25R3916_REG_BIT_RATE, 0x00);
    st25r3916_write_reg(ST25R3916_REG_ISO14443A_NFC, 0x00);

    /* Receiver + automatic external-field detection on. No tx_en: as a passive
     * target the load-modulation path is driven by the target logic when it
     * answers, not by the active TX driver. */
    st25r3916_write_reg(
        ST25R3916_REG_OP_CONTROL,
        ST25R3916_REG_OP_CONTROL_en | ST25R3916_REG_OP_CONTROL_rx_en |
            ST25R3916_REG_OP_CONTROL_en_fd_auto_efd);

    /* Arm the passive-target state machine (Sense/Idle -> waits for a field). */
    st25r3916_cmd(ST25R3916_CMD_GOTO_SENSE);
    irq_clear();
    return St25rNfcaOk;
}

St25rNfcaStatus st25r3916_nfca_listen_poll(St25rListenState* state, uint8_t* raw_status) {
    irq_poll();
    uint8_t st = 0;
    st25r3916_read_reg(ST25R3916_REG_PASSIVE_TARGET_STATUS, &st);
    if(raw_status) *raw_status = st;

    /* PASSIVE_TARGET_STATUS low nibble tracks the target state machine
     * (0 = power-off/sense; rising as the field appears and the reader
     * activates). Exact encoding is device-specific and is the first thing to
     * confirm on hardware — until then we report field vs active heuristically
     * and rely on the main-IRQ RXE for actual data frames. */
    St25rListenState s = St25rListenIdle;
    if((st & 0x0FU) != 0U) s = St25rListenField;
    if((st & 0x0FU) >= 0x03U) s = St25rListenActive;
    if(s_irq_main & IRQ_MAIN_RXE) s = St25rListenRx;
    if(state) *state = s;
    return St25rNfcaOk;
}

St25rNfcaStatus st25r3916_nfca_listen_rx(uint8_t* buf, uint8_t cap, uint8_t* len) {
    if(!buf || !len) return St25rNfcaErrProto;
    *len = 0;
    if(!(s_irq_main & IRQ_MAIN_RXE)) return St25rNfcaErrNoCard;

    uint8_t st1 = 0, st2 = 0;
    st25r3916_read_reg(ST25R3916_REG_FIFO_STATUS1, &st1);
    st25r3916_read_reg(ST25R3916_REG_FIFO_STATUS2, &st2);
    uint16_t cnt = (uint16_t)st1 +
                   (uint16_t)(((st2 & ST25R3916_REG_FIFO_STATUS2_fifo_b_mask) >>
                               ST25R3916_REG_FIFO_STATUS2_fifo_b_shift)
                              << 8);
    s_irq_main &= (uint8_t)~IRQ_MAIN_RXE; /* consume */
    if(cnt == 0) return St25rNfcaErrNoCard;
    if(cnt > cap) cnt = cap;
    if(st25r3916_read_fifo(buf, (uint8_t)cnt) != ESP_OK) return St25rNfcaErrProto;
    *len = (uint8_t)cnt;
    return St25rNfcaOk;
}

St25rNfcaStatus st25r3916_nfca_listen_tx(const uint8_t* buf, uint8_t len) {
    if(!buf || len == 0) return St25rNfcaErrProto;
    st25r3916_cmd(ST25R3916_CMD_CLEAR_FIFO);
    if(st25r3916_write_fifo(buf, len) != ESP_OK) return St25rNfcaErrProto;
    st25r3916_write_reg(ST25R3916_REG_NUM_TX_BYTES1, (uint8_t)(len >> 5));
    st25r3916_write_reg(ST25R3916_REG_NUM_TX_BYTES2, (uint8_t)((len & 0x1F) << 3));
    s_irq_main &= (uint8_t)~IRQ_MAIN_TXE;
    if(st25r3916_cmd(ST25R3916_CMD_TRANSMIT_WITH_CRC) != ESP_OK) return St25rNfcaErrProto;
    /* Budget must cover the largest frame's on-air time: a full ~250-byte DATA
     * frame is ~21 ms at 106 kbit/s, so 10 ms was too short (TXE never seen ->
     * spurious failure). 50 ms leaves margin and stays well under the poller's
     * ~74 ms FWT. */
    for(int t = 0; t < 50; t++) {
        irq_poll();
        if(s_irq_main & IRQ_MAIN_TXE) return St25rNfcaOk;
        furi_delay_ms(1);
    }
    return St25rNfcaErrProto;
}

void st25r3916_nfca_listen_stop(void) {
    st25r3916_cmd(ST25R3916_CMD_STOP);
    st25r3916_write_reg(ST25R3916_REG_OP_CONTROL, 0x00);
}

const char* st25r3916_nfca_listen_state_str(St25rListenState s) {
    switch(s) {
    case St25rListenIdle:
        return "armed (no field)";
    case St25rListenField:
        return "field detected";
    case St25rListenActive:
        return "ACTIVATED";
    case St25rListenRx:
        return "frame RX";
    default:
        return "?";
    }
}

#else /* !BOARD_HAS_ST25R3916 */

St25rNfcaStatus st25r3916_nfca_init(void) {
    return St25rNfcaErrInit;
}
St25rNfcaStatus st25r3916_nfca_poll(St25rNfcaTag* out) {
    (void)out;
    return St25rNfcaErrInit;
}
St25rNfcaStatus st25r3916_nfca_poller_txrx(
    const uint8_t* tx, uint8_t tx_len, uint8_t* rx, uint8_t rx_cap, uint8_t* rx_len) {
    (void)tx;
    (void)tx_len;
    (void)rx;
    (void)rx_cap;
    (void)rx_len;
    return St25rNfcaErrInit;
}
void st25r3916_nfca_deinit(void) {
}
const char* st25r3916_nfca_status_str(St25rNfcaStatus s) {
    (void)s;
    return "unsupported";
}
St25rNfcaStatus st25r3916_nfca_listen_start(
    const uint8_t* uid,
    uint8_t uid_len,
    uint16_t atqa,
    uint8_t sak) {
    (void)uid;
    (void)uid_len;
    (void)atqa;
    (void)sak;
    return St25rNfcaErrInit;
}
St25rNfcaStatus st25r3916_nfca_listen_poll(St25rListenState* state, uint8_t* raw_status) {
    (void)state;
    (void)raw_status;
    return St25rNfcaErrInit;
}
St25rNfcaStatus st25r3916_nfca_listen_rx(uint8_t* buf, uint8_t cap, uint8_t* len) {
    (void)buf;
    (void)cap;
    (void)len;
    return St25rNfcaErrInit;
}
St25rNfcaStatus st25r3916_nfca_listen_tx(const uint8_t* buf, uint8_t len) {
    (void)buf;
    (void)len;
    return St25rNfcaErrInit;
}
void st25r3916_nfca_listen_stop(void) {
}
const char* st25r3916_nfca_listen_state_str(St25rListenState s) {
    (void)s;
    return "unsupported";
}

#endif /* BOARD_HAS_ST25R3916 */
