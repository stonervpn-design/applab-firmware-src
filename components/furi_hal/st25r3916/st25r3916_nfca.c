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
    uint8_t* rx_len) {
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

    /* Wait end-of-receive / error / silence (~20 ms budget). */
    for(int t = 0; t < 20; t++) {
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
        st25r_transceive(ST25R3916_CMD_TRANSMIT_REQA, NULL, 0, true, rx, sizeof(rx), &rxn);
    if(s != St25rNfcaOk || rxn < 2) return St25rNfcaErrNoCard;
    out->atqa = (uint16_t)(rx[0] | (rx[1] << 8));

    static const uint8_t sel_cmd[3] = {0x93, 0x95, 0x97}; /* cascade levels 1..3 */
    uint8_t uid[10];
    uint8_t uidlen = 0;

    for(int cl = 0; cl < 3; cl++) {
        /* Anticollision: [SEL][NVB=0x20] without CRC -> 4 UID bytes + BCC. */
        uint8_t ac[2] = {sel_cmd[cl], 0x20};
        s = st25r_transceive(ST25R3916_CMD_TRANSMIT_WITHOUT_CRC, ac, 2, true, rx, sizeof(rx), &rxn);
        if(s != St25rNfcaOk || rxn < 5) return St25rNfcaErrAnticoll;
        if((uint8_t)(rx[0] ^ rx[1] ^ rx[2] ^ rx[3]) != rx[4]) return St25rNfcaErrAnticoll;

        /* Select: [SEL][0x70][UID CL][BCC] with CRC -> SAK. */
        uint8_t sel[7] = {sel_cmd[cl], 0x70, rx[0], rx[1], rx[2], rx[3], rx[4]};
        uint8_t sak_rx[8];
        uint8_t sakn = 0;
        s = st25r_transceive(
            ST25R3916_CMD_TRANSMIT_WITH_CRC, sel, 7, false, sak_rx, sizeof(sak_rx), &sakn);
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

#else /* !BOARD_HAS_ST25R3916 */

St25rNfcaStatus st25r3916_nfca_init(void) {
    return St25rNfcaErrInit;
}
St25rNfcaStatus st25r3916_nfca_poll(St25rNfcaTag* out) {
    (void)out;
    return St25rNfcaErrInit;
}
void st25r3916_nfca_deinit(void) {
}
const char* st25r3916_nfca_status_str(St25rNfcaStatus s) {
    (void)s;
    return "unsupported";
}

#endif /* BOARD_HAS_ST25R3916 */
