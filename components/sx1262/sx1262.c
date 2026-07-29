#include "sx1262.h"

#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <driver/i2c.h>
#include <esp_log.h>

#include <furi_hal_spi.h>
#include <furi_hal_spi_types.h>

#define TAG "sx1262"

// --- pins (Cap LoRa-1262 on Cardputer-ADV) ---
#define PIN_SCK  40
#define PIN_MOSI 14
#define PIN_MISO 39
#define PIN_CS   5
#define PIN_BUSY 6
#define PIN_DIO1 4
#define PIN_RST  3
#define SX_SPI_HOST SPI3_HOST // shared with SD/CC1101 — guarded by furi_hal_spi_bus_lock()

#define I2C_PORT I2C_NUM_0 // same bus as the keyboard (SDA=8/SCL=9)

// --- SX126x opcodes ---
#define OP_SET_STANDBY       0x80
#define OP_SET_PACKET_TYPE   0x8A
#define OP_SET_RF_FREQ       0x86
#define OP_SET_PA_CONFIG     0x95
#define OP_SET_TX_PARAMS     0x8E
#define OP_SET_BUF_BASE      0x8F
#define OP_SET_MOD_PARAMS    0x8B
#define OP_SET_PKT_PARAMS    0x8C
#define OP_WRITE_REG         0x0D
#define OP_WRITE_BUFFER      0x0E
#define OP_READ_BUFFER       0x1E
#define OP_SET_DIO_IRQ       0x08
#define OP_GET_IRQ           0x12
#define OP_CLR_IRQ           0x02
#define OP_SET_TX            0x83
#define OP_SET_RX            0x82
#define OP_GET_RX_BUF_STATUS 0x13
#define OP_GET_PKT_STATUS    0x14
#define OP_SET_REGULATOR     0x96
#define OP_GET_DEVICE_ERRORS 0x17
#define OP_GET_STATUS        0xC0
#define OP_SET_DIO3_TCXO     0x97
#define OP_CALIBRATE         0x89
#define OP_CALIBRATE_IMAGE   0x98
#define OP_CLEAR_ERRORS      0x07

#define IRQ_TX_DONE  (1 << 0)
#define IRQ_RX_DONE  (1 << 1)
#define IRQ_TIMEOUT  (1 << 9)

static bool s_ready = false;

// ---- low level ----
static void sx_wait_busy(void) {
    // BUSY high = chip busy. Bounded so a stuck line can't hang the UI.
    for(int i = 0; i < 1000; i++) {
        if(gpio_get_level(PIN_BUSY) == 0) return;
        esp_rom_delay_us(20);
    }
    ESP_LOGW(TAG, "BUSY stuck high");
}

// One SPI transfer. Uses the CC1101/subghz SPI handle — the port's SD-SAFE SPI3
// path (takes the shared bus lock + applies proper per-device config, exactly like
// the CC1101 which coexists with the SD). The LoRa cap physically replaces the
// CC1101 cap, so this handle's bus is free; we drive the SX1262 CS (GPIO5) manually.
static void sx_xfer(const uint8_t* tx, uint8_t* rx, size_t len) {
    furi_hal_spi_acquire(&furi_hal_spi_bus_handle_subghz);
    sx_wait_busy();
    gpio_set_level(PIN_CS, 0);
    if(rx) {
        furi_hal_spi_bus_trx(&furi_hal_spi_bus_handle_subghz, tx, rx, len, 1000);
    } else {
        furi_hal_spi_bus_tx(&furi_hal_spi_bus_handle_subghz, tx, len, 1000);
    }
    gpio_set_level(PIN_CS, 1);
    furi_hal_spi_release(&furi_hal_spi_bus_handle_subghz);
}

static void sx_cmd(uint8_t op, const uint8_t* params, size_t n) {
    uint8_t buf[16];
    buf[0] = op;
    if(n && params) memcpy(&buf[1], params, n);
    sx_xfer(buf, NULL, n + 1);
}

static void sx_write_reg(uint16_t addr, const uint8_t* data, size_t n) {
    uint8_t buf[16];
    buf[0] = OP_WRITE_REG;
    buf[1] = addr >> 8;
    buf[2] = addr & 0xFF;
    memcpy(&buf[3], data, n);
    sx_xfer(buf, NULL, n + 3);
}

// GetIrqStatus: opcode, 1 status nop, 2 irq bytes
static uint16_t sx_get_irq(void) {
    uint8_t tx[4] = {OP_GET_IRQ, 0, 0, 0};
    uint8_t rx[4] = {0};
    sx_xfer(tx, rx, 4);
    return ((uint16_t)rx[2] << 8) | rx[3];
}

static void sx_clear_irq(uint16_t mask) {
    uint8_t p[2] = {mask >> 8, mask & 0xFF};
    sx_cmd(OP_CLR_IRQ, p, 2);
}

// ---- I2C antenna switch (PI4IOE5V6408): drive P0 high ----
// Register map per PI4IOE5V6408 datasheet. VERIFY address (0x43/0x44) + P0 bit.
static void sx_i2c_write_reg(uint8_t reg, uint8_t val) {
    uint8_t d[2] = {reg, val};
    i2c_master_write_to_device(I2C_PORT, SX1262_IO_EXPANDER_ADDR, d, 2, pdMS_TO_TICKS(50));
}
static void sx_ant_enable(void) {
    sx_i2c_write_reg(0x03, 0x01); // I/O direction: P0 = output
    sx_i2c_write_reg(0x07, 0x00); // High-Z disable: P0 driven
    sx_i2c_write_reg(0x05, 0x01); // Output state: P0 = high (enable ANT switch)
}

// ---- init ----
bool sx1262_init(void) {
    if(s_ready) return true;

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_CS) | (1ULL << PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_BUSY) | (1ULL << PIN_DIO1),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&in);
    gpio_set_level(PIN_CS, 1);

    // No raw device is added to the SD's SPI3 bus — all transfers go through
    // furi_hal_spi (subghz handle) in sx_xfer(), the same SD-safe path as the CC1101.

    // hardware reset
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    sx_wait_busy();

    uint8_t stby = 0x00; // STDBY_RC
    sx_cmd(OP_SET_STANDBY, &stby, 1);

    // The Cap LoRa-1262 uses a TCXO on DIO3 (confirmed by XOSC_START_ERR=0x0020).
    // Power it (1.8V, ~10ms startup), clear the stale error, then recalibrate all
    // blocks on the new clock — otherwise the RF PLL never locks and TX/RX fail.
    uint8_t tcxo[4] = {0x02 /*1.8V*/, 0x00, 0x02, 0x80 /*~10ms*/};
    sx_cmd(OP_SET_DIO3_TCXO, tcxo, 4);
    uint8_t clr[2] = {0x00, 0x00};
    sx_cmd(OP_CLEAR_ERRORS, clr, 2);
    vTaskDelay(pdMS_TO_TICKS(12));
    uint8_t cal = 0x7F; // calibrate all blocks
    sx_cmd(OP_CALIBRATE, &cal, 1);
    vTaskDelay(pdMS_TO_TICKS(12));

    uint8_t reg = 0x01; // DC-DC regulator
    sx_cmd(OP_SET_REGULATOR, &reg, 1);
    uint8_t lora = 0x01;
    sx_cmd(OP_SET_PACKET_TYPE, &lora, 1);

    sx_ant_enable();
    s_ready = true;
    ESP_LOGI(TAG, "init ok");
    return true;
}

void sx1262_deinit(void) {
    s_ready = false;
}

uint16_t sx1262_device_errors(void) {
    uint8_t tx[4] = {OP_GET_DEVICE_ERRORS, 0, 0, 0};
    uint8_t rx[4] = {0};
    sx_xfer(tx, rx, 4);
    return ((uint16_t)rx[2] << 8) | rx[3];
}

uint8_t sx1262_status(void) {
    uint8_t tx[2] = {OP_GET_STATUS, 0};
    uint8_t rx[2] = {0};
    sx_xfer(tx, rx, 2);
    return rx[1];
}

void sx1262_default_config(Sx1262Config* cfg) {
    cfg->frequency = 868000000u;
    cfg->spreading_factor = 7;
    cfg->bandwidth = Sx1262Bw125;
    cfg->coding_rate = 5;
    cfg->sync_word = 0x12; // private
    cfg->power_dbm = 14;
    cfg->preamble = 8;
}

void sx1262_meshtastic_config(Sx1262Config* cfg, uint32_t frequency) {
    // Meshtastic "LongFast" default modem preset.
    cfg->frequency = frequency ? frequency : 869525000u; // EU_868 LongFast default
    cfg->spreading_factor = 11;
    cfg->bandwidth = Sx1262Bw250;
    cfg->coding_rate = 5; // 4/5
    cfg->sync_word = 0x2b; // Meshtastic public sync word
    cfg->power_dbm = 22;
    cfg->preamble = 16;
}

bool sx1262_configure(const Sx1262Config* cfg) {
    if(!s_ready) return false;

    // frequency: F = freq * 2^25 / 32e6
    uint32_t frf = (uint32_t)(((uint64_t)cfg->frequency << 25) / 32000000u);
    uint8_t f[4] = {frf >> 24, frf >> 16, frf >> 8, frf};
    sx_cmd(OP_SET_RF_FREQ, f, 4);

    // image calibration for the operating band
    uint8_t img[2];
    if(cfg->frequency < 900000000u) {
        img[0] = 0xD7;
        img[1] = 0xDB; // 863-870 MHz
    } else {
        img[0] = 0xE1;
        img[1] = 0xE9; // 902-928 MHz
    }
    sx_cmd(OP_CALIBRATE_IMAGE, img, 2);

    // PA config for SX1262 (+22 dBm capable)
    uint8_t pa[4] = {0x04, 0x07, 0x00, 0x01};
    sx_cmd(OP_SET_PA_CONFIG, pa, 4);
    uint8_t txp[2] = {cfg->power_dbm, 0x04 /* ramp 200us */};
    sx_cmd(OP_SET_TX_PARAMS, txp, 2);

    uint8_t base[2] = {0x00, 0x00};
    sx_cmd(OP_SET_BUF_BASE, base, 2);

    uint8_t bw = (cfg->bandwidth == Sx1262Bw500) ? 0x06 : (cfg->bandwidth == Sx1262Bw250) ? 0x05 : 0x04;
    uint8_t cr = cfg->coding_rate - 4; // 4/5->1 .. 4/8->4
    uint8_t ldro = (cfg->spreading_factor >= 11) ? 0x01 : 0x00;
    uint8_t mod[4] = {cfg->spreading_factor, bw, cr, ldro};
    sx_cmd(OP_SET_MOD_PARAMS, mod, 4);

    // sync word registers 0x0740/0x0741
    uint8_t sync[2] = {(uint8_t)((cfg->sync_word & 0xF0) | 0x04),
                       (uint8_t)(((cfg->sync_word & 0x0F) << 4) | 0x04)};
    sx_write_reg(0x0740, sync, 2);

    // packet params: preamble (cfg), explicit header, crc on, no IQ invert
    uint16_t pre = cfg->preamble ? cfg->preamble : 8;
    uint8_t pkt[6] = {(uint8_t)(pre >> 8), (uint8_t)pre, 0x00, 0xFF, 0x01, 0x00};
    sx_cmd(OP_SET_PKT_PARAMS, pkt, 6);

    // route TX/RX-done + timeout to DIO1
    uint16_t irq = IRQ_TX_DONE | IRQ_RX_DONE | IRQ_TIMEOUT;
    uint8_t di[8] = {irq >> 8, irq, irq >> 8, irq, 0, 0, 0, 0};
    sx_cmd(OP_SET_DIO_IRQ, di, 8);
    return true;
}

bool sx1262_tx(const uint8_t* data, size_t len, uint32_t timeout_ms) {
    if(!s_ready || len > 255) return false;

    // payload length into packet params byte 3
    uint8_t pkt[6] = {0x00, 0x08, 0x00, (uint8_t)len, 0x01, 0x00};
    sx_cmd(OP_SET_PKT_PARAMS, pkt, 6);

    // write buffer at offset 0
    uint8_t buf[257];
    buf[0] = OP_WRITE_BUFFER;
    buf[1] = 0x00;
    memcpy(&buf[2], data, len);
    sx_xfer(buf, NULL, len + 2);

    sx_clear_irq(0xFFFF);
    uint8_t to[3] = {0x00, 0x00, 0x00}; // no radio timeout; we poll
    sx_cmd(OP_SET_TX, to, 3);

    uint32_t waited = 0;
    while(waited < timeout_ms) {
        if(sx_get_irq() & (IRQ_TX_DONE | IRQ_TIMEOUT)) {
            sx_clear_irq(0xFFFF);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
        waited += 5;
    }
    return false;
}

bool sx1262_start_rx(void) {
    if(!s_ready) return false;
    // restore max payload for RX
    uint8_t pkt[6] = {0x00, 0x08, 0x00, 0xFF, 0x01, 0x00};
    sx_cmd(OP_SET_PKT_PARAMS, pkt, 6);
    sx_clear_irq(0xFFFF);
    uint8_t to[3] = {0xFF, 0xFF, 0xFF}; // continuous
    sx_cmd(OP_SET_RX, to, 3);
    return true;
}

bool sx1262_rx_poll(uint8_t* out, size_t out_size, size_t* out_len, int16_t* rssi, int8_t* snr) {
    if(!s_ready) return false;
    uint16_t irq = sx_get_irq();
    if(!(irq & IRQ_RX_DONE)) return false;
    sx_clear_irq(0xFFFF);

    // GetRxBufferStatus: opcode, nop, payloadLen, startPtr
    uint8_t st_tx[4] = {OP_GET_RX_BUF_STATUS, 0, 0, 0};
    uint8_t st_rx[4] = {0};
    sx_xfer(st_tx, st_rx, 4);
    size_t plen = st_rx[2];
    uint8_t start = st_rx[3];
    if(plen > out_size) plen = out_size;

    // ReadBuffer: opcode, offset, nop, data...
    uint8_t rb_tx[3 + 255] = {OP_READ_BUFFER, start, 0x00};
    uint8_t rb_rx[3 + 255] = {0};
    sx_xfer(rb_tx, rb_rx, plen + 3);
    memcpy(out, &rb_rx[3], plen);
    *out_len = plen;

    // GetPacketStatus: opcode, nop, rssiPkt, snrPkt, signalRssi
    uint8_t ps_tx[5] = {OP_GET_PKT_STATUS, 0, 0, 0, 0};
    uint8_t ps_rx[5] = {0};
    sx_xfer(ps_tx, ps_rx, 5);
    if(rssi) *rssi = -(int16_t)(ps_rx[2] / 2);
    if(snr) *snr = (int8_t)ps_rx[3] / 4;

    return true;
}
