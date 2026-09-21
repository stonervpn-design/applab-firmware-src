#include "pin_config_store.h"
#include <string.h>
#include "boards/board.h" /* per-board BOARD_PIN_* defaults (selected by build target) */

// Display names + machine keys, indexed by PinFunc.
static const char* const k_names[PinFuncCount] = {
    "IR TX",
    "IR RX",
    "SPI SCK",
    "SPI MISO",
    "SPI MOSI",
    "CC1101 CSN",
    "CC1101 G0",
    "NRF24 CE",
    "NRF24 CSN",
    "I2C SDA",
    "I2C SCL",
};

static const char* const k_keys[PinFuncCount] = {
    "ir_tx",
    "ir_rx",
    "spi_sck",
    "spi_miso",
    "spi_mosi",
    "cc1101_csn",
    "cc1101_gdo0",
    "nrf24_ce",
    "nrf24_csn",
    "i2c_sda",
    "i2c_scl",
};

/* Board defaults, derived from the active board header (boards/board.h picks it
 * per build target). These were previously hardcoded to the Cardputer-ADV pins,
 * which put IR TX on GPIO44 and the radio/I2C pins on the Cardputer's GPIOs for
 * EVERY board -- breaking IR (and "Reset to Defaults") on the M5Stick S3 (IR TX
 * is GPIO46, RX 42) and the T-Embed. Order must match the PinFunc enum. The
 * "SPI" rows are the shared CC1101/NRF24 SPI bus. */
#define PIN_CONFIG_DEFAULTS                 \
    {                                       \
        (uint8_t)BOARD_PIN_IR_TX,       /* IR TX      */ \
        (uint8_t)BOARD_PIN_IR_RX,       /* IR RX      */ \
        (uint8_t)BOARD_PIN_CC1101_SCK,  /* SPI SCK    */ \
        (uint8_t)BOARD_PIN_CC1101_MISO, /* SPI MISO   */ \
        (uint8_t)BOARD_PIN_CC1101_MOSI, /* SPI MOSI   */ \
        (uint8_t)BOARD_PIN_CC1101_CSN,  /* CC1101 CSN */ \
        (uint8_t)BOARD_PIN_CC1101_GDO0, /* CC1101 G0  */ \
        (uint8_t)BOARD_PIN_NRF24_CE,    /* NRF24 CE   */ \
        (uint8_t)BOARD_PIN_NRF24_CSN,   /* NRF24 CSN  */ \
        (uint8_t)BOARD_PIN_QWIIC_SDA,   /* I2C SDA    */ \
        (uint8_t)BOARD_PIN_QWIIC_SCL,   /* I2C SCL    */ \
    }

static const uint8_t k_defaults[PinFuncCount] = PIN_CONFIG_DEFAULTS;

static uint8_t s_current[PinFuncCount] = PIN_CONFIG_DEFAULTS;

const uint8_t pin_config_choices[] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
                                      16, 17, 18, 21, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48};
const size_t pin_config_choices_count = sizeof(pin_config_choices) / sizeof(pin_config_choices[0]);

const char* pin_config_func_name(PinFunc f) {
    if(f >= PinFuncCount) return "?";
    return k_names[f];
}

uint8_t pin_config_default(PinFunc f) {
    if(f >= PinFuncCount) return 0;
    return k_defaults[f];
}

void pin_config_reset_defaults(void) {
    memcpy(s_current, k_defaults, sizeof(s_current));
}

uint8_t pin_config_get(PinFunc f) {
    if(f >= PinFuncCount) return 0;
    return s_current[f];
}

void pin_config_set(PinFunc f, uint8_t gpio) {
    if(f < PinFuncCount) s_current[f] = gpio;
}

bool pin_config_is_movable(PinFunc f) {
    switch(f) {
    case PinFuncIrTx:
    case PinFuncIrRx:
    case PinFuncCc1101Csn:
    case PinFuncCc1101Gdo0:
    case PinFuncNrf24Ce:
    case PinFuncNrf24Csn:
        return true;
    default: // SPI SCK/MISO/MOSI, I2C SDA/SCL are fixed by the bus init
        return false;
    }
}

bool pin_config_pin_conflict(PinFunc changing, uint8_t gpio, PinFunc* out_conflict) {
    for(PinFunc f = 0; f < PinFuncCount; f++) {
        if(f == changing) continue;
        if(s_current[f] == gpio) {
            if(out_conflict) *out_conflict = f;
            return true;
        }
    }
    return false;
}

static size_t u8_to_str(uint8_t v, char* out) {
    char tmp[4];
    size_t n = 0;
    if(v == 0) {
        out[0] = '0';
        return 1;
    }
    while(v > 0 && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for(size_t i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

size_t pin_config_serialize(char* out, size_t out_max) {
    if(!out || out_max == 0) return 0;
    size_t pos = 0;
    for(PinFunc f = 0; f < PinFuncCount; f++) {
        const char* key = k_keys[f];
        size_t klen = strlen(key);
        if(pos + klen + 6 >= out_max) break; // key + '=' + up to 3 digits + '\n' + NUL
        memcpy(out + pos, key, klen);
        pos += klen;
        out[pos++] = '=';
        pos += u8_to_str(s_current[f], out + pos);
        out[pos++] = '\n';
    }
    out[pos < out_max ? pos : out_max - 1] = '\0';
    return pos;
}

static int find_key(const char* key, size_t len) {
    for(PinFunc f = 0; f < PinFuncCount; f++) {
        if(strlen(k_keys[f]) == len && strncmp(k_keys[f], key, len) == 0) return (int)f;
    }
    return -1;
}

void pin_config_parse(const char* text) {
    // Start from defaults so any missing key falls back sensibly.
    pin_config_reset_defaults();
    if(!text) return;

    const char* p = text;
    while(*p) {
        const char* line = p;
        while(*p && *p != '\n') p++;
        size_t linelen = (size_t)(p - line);
        if(*p == '\n') p++;

        // split key=val
        const char* eq = memchr(line, '=', linelen);
        if(!eq) continue;
        size_t klen = (size_t)(eq - line);
        int fi = find_key(line, klen);
        if(fi < 0) continue;

        // parse the number after '='
        const char* v = eq + 1;
        const char* vend = line + linelen;
        uint32_t val = 0;
        bool any = false;
        while(v < vend && *v >= '0' && *v <= '9') {
            val = val * 10 + (uint32_t)(*v - '0');
            any = true;
            v++;
        }
        if(any && val <= 48) s_current[fi] = (uint8_t)val;
    }
}
