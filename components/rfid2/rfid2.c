#include "rfid2.h"

#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_log.h>

#define TAG "rfid2"

// MFRC522 registers
#define REG_COMMAND     0x01
#define REG_COMIEN      0x02
#define REG_COMIRQ      0x04
#define REG_DIVIRQ      0x05
#define REG_ERROR       0x06
#define REG_FIFODATA    0x09
#define REG_FIFOLEVEL   0x0A
#define REG_CONTROL     0x0C
#define REG_BITFRAMING  0x0D
#define REG_COLL        0x0E
#define REG_MODE        0x11
#define REG_TXMODE      0x12
#define REG_RXMODE      0x13
#define REG_TXCONTROL   0x14
#define REG_TXASK       0x15
#define REG_STATUS2     0x08
#define REG_CRCRESULTH  0x21
#define REG_CRCRESULTL  0x22
#define REG_MODWIDTH    0x24
#define REG_TMODE       0x2A
#define REG_TPRESCALER  0x2B
#define REG_TRELOADH    0x2C
#define REG_TRELOADL    0x2D
#define REG_VERSION     0x37

// MFRC522 commands
#define CMD_IDLE        0x00
#define CMD_CALCCRC     0x03
#define CMD_TRANSCEIVE  0x0C
#define CMD_MFAUTHENT   0x0E
#define CMD_SOFTRESET   0x0F

// PICC commands
#define PICC_REQA       0x26
#define PICC_WUPA       0x52 // wakes IDLE *and* HALT cards (needed to re-activate)
#define PICC_ANTICOLL1  0x93
#define PICC_ANTICOLL2  0x95
#define PICC_ANTICOLL3  0x97
#define PICC_SEL_NVB70  0x70 // NVB for a full SELECT (all 40 UID bits)
#define PICC_HLTA       0x50
#define PICC_READ       0x30
#define PICC_AUTH_KEYA  0x60
#define PICC_AUTH_KEYB  0x61

static bool rd_write(uint8_t reg, uint8_t val) {
    uint8_t b[2] = {reg, val};
    return i2c_master_write_to_device(RFID2_I2C_PORT, RFID2_I2C_ADDR, b, 2, pdMS_TO_TICKS(30)) ==
           ESP_OK;
}

static uint8_t rd_read(uint8_t reg) {
    uint8_t val = 0;
    i2c_master_write_read_device(
        RFID2_I2C_PORT, RFID2_I2C_ADDR, &reg, 1, &val, 1, pdMS_TO_TICKS(30));
    return val;
}

static void rd_setbits(uint8_t reg, uint8_t mask) {
    rd_write(reg, rd_read(reg) | mask);
}
static void rd_clrbits(uint8_t reg, uint8_t mask) {
    rd_write(reg, rd_read(reg) & (~mask));
}

static void antenna_on(void) {
    if((rd_read(REG_TXCONTROL) & 0x03) != 0x03) rd_setbits(REG_TXCONTROL, 0x03);
}

static bool i2c_bus_up(void);

uint8_t rfid2_version(void) {
    return rd_read(REG_VERSION);
}

int rfid2_scan(uint8_t* out, int max) {
    if(!i2c_bus_up()) return -1;
    int n = 0;
    uint8_t dummy = 0;
    char list[128];
    int off = 0;
    list[0] = 0;
    for(uint8_t a = 0x08; a <= 0x77 && n < max; a++) {
        // a 1-byte write; ESP_OK means the address was ACKed = device present
        if(i2c_master_write_to_device(RFID2_I2C_PORT, a, &dummy, 1, pdMS_TO_TICKS(8)) == ESP_OK) {
            out[n++] = a;
            if(off < (int)sizeof(list) - 6)
                off += snprintf(list + off, sizeof(list) - off, "0x%02X ", a);
        }
    }
    ESP_LOGI(TAG, "scan: SDA=%d SCL=%d found %d device(s): [ %s]",
             RFID2_SDA_PIN, RFID2_SCL_PIN, n, list);
    return n;
}

static bool i2c_bus_up(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = RFID2_SDA_PIN,
        .scl_io_num = RFID2_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    // The Grove bus (I2C_NUM_1, G2/G1) is normally already up — the PN532 NFC HAL
    // installs it at boot on the SAME pins. Configure best-effort and DON'T gate on
    // the result: the legacy i2c driver returns an error (ESP_FAIL) when the driver
    // already exists, but the bus still works. Real problems surface as failed reads
    // (version reads back 0x00). Both param_config and install here are best-effort.
    i2c_param_config(RFID2_I2C_PORT, &conf);
    i2c_driver_install(RFID2_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    return true;
}

bool rfid2_init(void) {
    if(!i2c_bus_up()) return false;
    rd_write(REG_COMMAND, CMD_SOFTRESET);
    vTaskDelay(pdMS_TO_TICKS(50));
    // wait for reset to clear (PowerDown bit)
    for(int i = 0; i < 20 && (rd_read(REG_COMMAND) & 0x10); i++) vTaskDelay(pdMS_TO_TICKS(5));

    // timer: TAuto, 25us..~25ms
    rd_write(REG_TMODE, 0x80);
    rd_write(REG_TPRESCALER, 0xA9);
    rd_write(REG_TRELOADH, 0x03);
    rd_write(REG_TRELOADL, 0xE8);
    rd_write(REG_TXASK, 0x40); // 100% ASK
    rd_write(REG_MODE, 0x3D); // CRC preset 0x6363
    antenna_on();

    uint8_t v = rfid2_version();
    ESP_LOGI(TAG, "version 0x%02X", v);
    return v != 0x00 && v != 0xFF; // 0x91/0x92 genuine; WS1850S clones vary but not 00/FF
}

// Transceive `send_len` bytes, collect up to `recv_max` reply bytes. `bits` is the
// number of valid bits in the last sent byte (0 = full byte). Returns recv byte count,
// or -1 on error/timeout.
static int transceive(
    const uint8_t* send,
    int send_len,
    uint8_t bits,
    uint8_t* recv,
    int recv_max) {
    rd_write(REG_COMMAND, CMD_IDLE);
    rd_write(REG_COMIRQ, 0x7F); // clear IRQs
    rd_setbits(REG_FIFOLEVEL, 0x80); // flush FIFO
    for(int i = 0; i < send_len; i++) rd_write(REG_FIFODATA, send[i]);
    rd_write(REG_BITFRAMING, bits & 0x07);
    rd_write(REG_COMMAND, CMD_TRANSCEIVE);
    rd_setbits(REG_BITFRAMING, 0x80); // StartSend

    // wait for RxIRq or IdleIRq (bit5/bit4) or timer (bit0)
    int i;
    for(i = 0; i < 50; i++) {
        uint8_t irq = rd_read(REG_COMIRQ);
        if(irq & 0x30) break; // Rx or Idle
        if(irq & 0x01) return -1; // timer expired, no card
        esp_rom_delay_us(200);
    }
    if(i >= 50) return -1;
    rd_clrbits(REG_BITFRAMING, 0x80);

    if(rd_read(REG_ERROR) & 0x13) return -1; // BufferOvfl/Coll/Protocol/Parity

    int n = rd_read(REG_FIFOLEVEL);
    if(n > recv_max) n = recv_max;
    for(int j = 0; j < n; j++) recv[j] = rd_read(REG_FIFODATA);
    return n;
}

/* ── Public low-level hooks for the unified furi_hal_nfc MFRC522 backend ── */
int rfid2_transceive(const uint8_t* tx, int tx_len, int tx_last_bits, uint8_t* rx, int rx_max) {
    return transceive(tx, tx_len, (uint8_t)(tx_last_bits & 0x07), rx, rx_max);
}

void rfid2_field_off(void) {
    rd_clrbits(REG_TXCONTROL, 0x03); /* Tx2RFEn | Tx1RFEn off (RF field down) */
}

// One anticollision cascade level. Returns 5 bytes (4 UID/CT + BCC) into out5.
static bool anticoll(uint8_t sel_cmd, uint8_t* out5) {
    rd_write(REG_COLL, 0x00); // clear ValuesAfterColl
    uint8_t cmd[2] = {sel_cmd, 0x20};
    int n = transceive(cmd, 2, 0, out5, 5);
    if(n != 5) return false;
    uint8_t bcc = out5[0] ^ out5[1] ^ out5[2] ^ out5[3];
    return bcc == out5[4];
}

bool rfid2_read_uid(uint8_t* uid, size_t uid_max, size_t* uid_len, uint8_t* sak) {
    // REQA (7-bit frame). Expect 2-byte ATQA.
    uint8_t atqa[2];
    if(transceive((const uint8_t[]){PICC_REQA}, 1, 7, atqa, 2) != 2) return false;

    size_t len = 0;
    uint8_t cl[5];

    // Cascade level 1
    if(!anticoll(PICC_ANTICOLL1, cl)) return false;
    if(cl[0] == 0x88) {
        // 7/10-byte UID: CT + 3 UID bytes here, rest in CL2
        for(int i = 1; i < 4 && len < uid_max; i++) uid[len++] = cl[i];
        uint8_t cl2[5];
        if(!anticoll(PICC_ANTICOLL2, cl2)) return false;
        for(int i = 0; i < 4 && len < uid_max; i++) uid[len++] = cl2[i];
    } else {
        for(int i = 0; i < 4 && len < uid_max; i++) uid[len++] = cl[i];
    }

    *uid_len = len;
    if(sak) *sak = 0; // full SELECT (for SAK) omitted; UID is enough for identification
    return len > 0;
}

// ── Full-read support ───────────────────────────────────────────────────────
// The reader is configured with CRC OFF on TX/RX (init sets no TxCRCEn/RxCRCEn),
// so SELECT/READ/HLTA frames must carry a CRC_A we append ourselves. Compute it
// on the MFRC522 (CalcCRC), result LSB-first into crc[2].
static bool rd_calc_crc(const uint8_t* data, int len, uint8_t* crc) {
    rd_write(REG_COMMAND, CMD_IDLE);
    rd_write(REG_DIVIRQ, 0x04); // clear CRCIRq
    rd_setbits(REG_FIFOLEVEL, 0x80); // flush FIFO
    for(int i = 0; i < len; i++) rd_write(REG_FIFODATA, data[i]);
    rd_write(REG_COMMAND, CMD_CALCCRC);
    int i;
    for(i = 0; i < 60; i++) {
        if(rd_read(REG_DIVIRQ) & 0x04) break; // CRCIRq
        esp_rom_delay_us(100);
    }
    rd_write(REG_COMMAND, CMD_IDLE);
    if(i >= 60) return false;
    crc[0] = rd_read(REG_CRCRESULTL);
    crc[1] = rd_read(REG_CRCRESULTH);
    return true;
}

bool rfid2_select(uint8_t* uid, size_t uid_max, size_t* uid_len, uint8_t* sak_out) {
    uint8_t atqa[2];
    // WUPA (not REQA) so this also re-activates a card that was HALTed or muted by a
    // failed auth — required to walk multiple sectors with a key dictionary.
    if(transceive((const uint8_t[]){PICC_WUPA}, 1, 7, atqa, 2) != 2) return false;

    static const uint8_t sel[3] = {PICC_ANTICOLL1, PICC_ANTICOLL2, PICC_ANTICOLL3};
    size_t len = 0;
    uint8_t sak = 0;
    for(int cl = 0; cl < 3; cl++) {
        rd_write(REG_COLL, 0x00); // clear ValuesAfterColl
        uint8_t cl5[5];
        uint8_t ac[2] = {sel[cl], 0x20};
        if(transceive(ac, 2, 0, cl5, 5) != 5) return false;
        if((cl5[0] ^ cl5[1] ^ cl5[2] ^ cl5[3]) != cl5[4]) return false;

        // SELECT: SEL, NVB=0x70, 4 UID/CT bytes, BCC, CRC_A
        uint8_t s[9] = {sel[cl], PICC_SEL_NVB70, cl5[0], cl5[1], cl5[2], cl5[3], cl5[4], 0, 0};
        if(!rd_calc_crc(s, 7, &s[7])) return false;
        uint8_t resp[3]; // SAK(1) + CRC(2)
        if(transceive(s, 9, 0, resp, 3) < 1) return false;
        sak = resp[0];

        if(cl5[0] == 0x88) { // cascade tag → real UID bytes are cl5[1..3]
            for(int i = 1; i < 4 && len < uid_max; i++) uid[len++] = cl5[i];
        } else {
            for(int i = 0; i < 4 && len < uid_max; i++) uid[len++] = cl5[i];
        }
        if(!(sak & 0x04)) break; // cascade complete (UID fully read)
    }
    *uid_len = len;
    if(sak_out) *sak_out = sak;
    return len > 0;
}

bool rfid2_classic_auth(
    uint8_t block,
    const uint8_t key[6],
    bool keyB,
    const uint8_t* uid,
    size_t uid_len) {
    // MFAuthent payload: cmd, block, key[6], uid[4]. Classic UID is 4 bytes; for a
    // 7-byte UID the auth uses the last 4 (not a real Classic case, but be safe).
    const uint8_t* u = uid + (uid_len > 4 ? uid_len - 4 : 0);
    uint8_t buf[12] = {keyB ? PICC_AUTH_KEYB : PICC_AUTH_KEYA, block};
    memcpy(&buf[2], key, 6);
    memcpy(&buf[8], u, 4);

    rd_write(REG_COMMAND, CMD_IDLE);
    rd_write(REG_COMIRQ, 0x7F);
    rd_setbits(REG_FIFOLEVEL, 0x80);
    for(int i = 0; i < 12; i++) rd_write(REG_FIFODATA, buf[i]);
    rd_write(REG_COMMAND, CMD_MFAUTHENT);

    for(int i = 0; i < 40; i++) {
        uint8_t irq = rd_read(REG_COMIRQ);
        if(irq & 0x10) break; // IdleIRq → auth handshake done
        if(irq & 0x01) return false; // timer expired
        esp_rom_delay_us(200);
    }
    // Crypto1On (Status2Reg bit3) set ⇒ authenticated.
    return (rd_read(REG_STATUS2) & 0x08) != 0;
}

bool rfid2_read16(uint8_t block, uint8_t out[16]) {
    uint8_t buf[4] = {PICC_READ, block, 0, 0};
    if(!rd_calc_crc(buf, 2, &buf[2])) return false;
    uint8_t resp[18]; // 16 data + 2 CRC
    int n = transceive(buf, 4, 0, resp, 18);
    if(n < 16) return false;
    memcpy(out, resp, 16);
    return true;
}

bool rfid2_halt(void) {
    uint8_t buf[4] = {PICC_HLTA, 0x00, 0, 0};
    if(!rd_calc_crc(buf, 2, &buf[2])) return false;
    uint8_t r[1];
    transceive(buf, 4, 0, r, 1); // a valid HLTA gets NO reply → transceive "fails"; that's OK
    rd_clrbits(REG_STATUS2, 0x08); // Crypto1 off
    return true;
}

// Built-in default MIFARE Classic keys (tried key A then key B per sector).
static const uint8_t DEFAULT_KEYS[][6] = {
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5},
    {0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5}, {0x4D, 0x3A, 0x99, 0xC3, 0x51, 0xDD},
    {0x1A, 0x98, 0x2C, 0x7E, 0x45, 0x9A}, {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF},
    {0x71, 0x4C, 0x5C, 0x88, 0x6E, 0x97}, {0x58, 0x7E, 0xE5, 0xF9, 0x35, 0x0F},
    {0xA0, 0x47, 0x8C, 0xC3, 0x90, 0x91}, {0x53, 0x3C, 0xB6, 0xC7, 0x23, 0xF6},
    {0x8F, 0xD0, 0xA4, 0xF2, 0x56, 0xE9},
};
#define N_DEFAULT_KEYS (sizeof(DEFAULT_KEYS) / sizeof(DEFAULT_KEYS[0]))

// Append "prefix: hh hh ..." (16 bytes) to text. Returns chars written.
static int append_row(char* text, size_t tsize, size_t off, const char* prefix, const uint8_t* d) {
    if(off >= tsize) return 0;
    int w = snprintf(text + off, tsize - off, "%s:", prefix);
    for(int i = 0; i < 16 && (off + w) < (int)tsize - 4; i++)
        w += snprintf(text + off + w, tsize - off - w, " %02X", d[i]);
    if((off + w) < (int)tsize - 1) w += snprintf(text + off + w, tsize - off - w, "\n");
    return w;
}

// First block index of MIFARE sector `s` (1K/4K layout: sectors 0-31 = 4 blocks,
// 32-39 = 16 blocks).
static uint8_t sector_first_block(int s) {
    return (s < 32) ? (uint8_t)(s * 4) : (uint8_t)(128 + (s - 32) * 16);
}
static int sector_block_count(int s) {
    return (s < 32) ? 4 : 16;
}

int rfid2_dump_text(
    char* text,
    size_t tsize,
    uint8_t* uid,
    size_t uid_max,
    size_t* uid_len,
    uint8_t* sak_out,
    char* type,
    size_t type_size) {
    size_t ulen = 0;
    uint8_t sak = 0;
    if(!rfid2_select(uid, uid_max, &ulen, &sak)) return -1;
    *uid_len = ulen;
    if(sak_out) *sak_out = sak;

    size_t off = 0;
    int rows = 0;
    char pfx[8];

    if(sak & 0x20) {
        // ISO14443-4 (DESFire / SmartMX / etc.) — needs APDUs, not a block dump.
        snprintf(type, type_size, "ISO14443-4");
        off += snprintf(text + off, tsize - off, "ISO14443-4 card (SAK 0x%02X).\n"
                        "UID read; block dump not supported.\n", sak);
        rfid2_halt();
        return 0;
    }

    if(sak == 0x00 || sak == 0x44) {
        // MIFARE Ultralight / NTAG — no auth, READ 0x30 returns 4 pages at a time.
        snprintf(type, type_size, "Ultralight/NTAG");
        for(int page = 0; page < 256; page += 4) {
            uint8_t d[16];
            if(!rfid2_read16((uint8_t)page, d)) break; // NAK past end of memory
            snprintf(pfx, sizeof(pfx), "P%02X", page);
            off += append_row(text, tsize, off, pfx, d);
            rows += 4;
            if(off >= tsize - 64) break;
        }
        rfid2_halt();
        return rows;
    }

    // Otherwise treat as MIFARE Classic. Sector count from SAK: 0x18=4K(40), else 1K(16).
    int sectors = (sak == 0x18) ? 40 : ((sak == 0x09) ? 5 : 16);
    snprintf(type, type_size, "Classic %s", (sak == 0x18) ? "4K" : (sak == 0x09) ? "Mini" : "1K");

    for(int s = 0; s < sectors; s++) {
        uint8_t first = sector_first_block(s);
        int nb = sector_block_count(s);

        // Find a working key by authing this sector's first block, trying key A then
        // B for each dictionary entry. A failed MFAuthent mutes the card, so after
        // each miss we stop Crypto1 and re-activate it (WUPA) before the next try.
        bool authed = false;
        bool card_gone = false;
        for(size_t k = 0; k < N_DEFAULT_KEYS && !authed && !card_gone; k++) {
            for(int kb = 0; kb < 2; kb++) {
                if(rfid2_classic_auth(first, DEFAULT_KEYS[k], kb == 1, uid, ulen)) {
                    authed = true;
                    break;
                }
                rd_clrbits(REG_STATUS2, 0x08); // stop Crypto1
                if(!rfid2_select(uid, uid_max, &ulen, &sak)) { // re-activate for next try
                    card_gone = true;
                    break;
                }
            }
        }
        if(card_gone) break;
        if(!authed) {
            snprintf(pfx, sizeof(pfx), "S%02d", s);
            off += snprintf(text + off, tsize - off, "%s: (no key)\n", pfx);
            continue;
        }
        for(int b = 0; b < nb && off < tsize - 64; b++) {
            uint8_t d[16];
            if(rfid2_read16(first + b, d)) {
                snprintf(pfx, sizeof(pfx), "B%02X", first + b);
                off += append_row(text, tsize, off, pfx, d);
                rows++;
            }
        }
        if(off >= tsize - 64) break;
    }
    rfid2_halt();
    return rows;
}
