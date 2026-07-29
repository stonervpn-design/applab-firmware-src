/* ST25R3916 NFC Read diagnostic — STEP 1 of the NFC-read bring-up plan.
 *
 * On-screen visibility into the raw NFC layer (the FURI log console is dead over
 * USB on this port). Two independent presence checks plus the raw chip-identity
 * byte, then a bare REQA:
 *   - ack   : raw 0x50 address ACK (bus/power/pins alive)
 *   - chip  : st25r3916_hal_present() — IC_IDENTITY ic_type == 0x05 signature
 *   - IDENT : the raw IC_IDENTITY register byte + read error + extracted ic_type
 *   - REQA  : field_on + bare REQA short frame, ATQA + exact RX bit count
 *
 * Current finding: ack:Y but chip:absent — the module ACKs at 0x50 but the
 * ic_type check fails. IDENT shows whether that's a comms failure (0x00/0xFF or
 * err!=0) or a chip-variant mismatch (valid byte, ic_type != 5, e.g. ST25R3916B).
 *
 * Bus bring-up mirrors the proven grove_i2c_scan app exactly. */
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <boards/board.h>
#include <string.h>

#if defined(BOARD_HAS_ST25R3916) && BOARD_HAS_ST25R3916

#include <driver/i2c.h>
#include <driver/gpio.h>
#include "st25r3916/st25r3916_hal.h"
#include "st25r3916/st25r3916_i2c.h"
#include "st25r3916/st25r3916_reg.h"

#define POLL_PERIOD_MS  200
#define ST25R_I2C_ADDR7 0x50

typedef struct {
    bool ack; /* raw 0x50 address ACK (bus/power/pins alive) */
    bool present; /* st25r3916_hal_present() ic_type signature match */
    int id_err; /* esp_err_t from the direct IC_IDENTITY read */
    uint8_t id_raw; /* raw IC_IDENTITY register byte (direct read) */
    int init_err; /* esp_err_t from st25r3916_i2c_init() (probe's 1st step) */
    int cc_err; /* esp_err_t from st25r3916_check_chip_id() after i2c_init */
    uint8_t cc_ictype; /* ic_type check_chip_id read after i2c_init */
    bool field_ok; /* last field_on() succeeded */
    St25rHalStatus last; /* last REQA result */
    uint8_t atqa[8]; /* raw REQA response bytes */
    uint16_t rx_len; /* REQA response length (bytes) */
    uint16_t rx_bits; /* exact RX bit count of last frame */
    uint32_t poll_count; /* poll cycles */
    uint32_t ok_count; /* REQA responses received */
} DiagState;

static const char* status_str(St25rHalStatus s) {
    switch(s) {
    case St25rHalOk: return "OK";
    case St25rHalNoCard: return "nocard";
    case St25rHalError: return "ERR";
    default: return "?";
    }
}

/* Establish the Grove NFC I2C bus exactly like the working grove_i2c_scan app:
 * reclaim the pins from IR/RMT (GPIO1 SCL is shared with internal IR RX), then
 * param_config -> install(ignore already-installed) -> param_config. */
static void bus_bringup(void) {
    gpio_reset_pin((gpio_num_t)BOARD_PIN_NFC_SCL);
    gpio_reset_pin((gpio_num_t)BOARD_PIN_NFC_SDA);

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BOARD_PIN_NFC_SDA,
        .scl_io_num = BOARD_PIN_NFC_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 200000,
    };
    i2c_param_config(BOARD_NFC_I2C_PORT, &conf);
    i2c_driver_install(BOARD_NFC_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0); /* ignore INVALID_STATE */
    i2c_param_config(BOARD_NFC_I2C_PORT, &conf);
}

/* Gentle presence ping: START + addr(W) + STOP, checking for the ACK. */
static bool raw_ack(uint8_t addr7) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (uint8_t)((addr7 << 1) | I2C_MASTER_WRITE), true);
    i2c_master_stop(cmd);
    esp_err_t e = i2c_master_cmd_begin(BOARD_NFC_I2C_PORT, cmd, pdMS_TO_TICKS(30));
    i2c_cmd_link_delete(cmd);
    return e == ESP_OK;
}

/* One presence refresh + (if present) a field-cycle + REQA. */
static void poll_once(DiagState* s) {
    s->poll_count++;
    s->ack = raw_ack(ST25R_I2C_ADDR7);

    /* Read IC_IDENTITY directly so we can show the raw byte + esp_err even when
     * the ic_type signature check (st25r3916_hal_present) rejects it. */
    s->id_raw = 0;
    s->id_err = st25r3916_read_reg(ST25R3916_REG_IC_IDENTITY, &s->id_raw);

    /* Replicate probe()'s exact internal sequence so we can see each step: an
     * i2c_init immediately followed by a chip-id read (the read our direct one
     * above shows works fine WITHOUT a preceding i2c_init). */
    s->init_err = st25r3916_i2c_init();
    s->cc_ictype = 0xFF;
    s->cc_err = st25r3916_check_chip_id(&s->cc_ictype);

    s->present = st25r3916_hal_present();

    if(!s->present) {
        s->field_ok = false;
        s->last = St25rHalError;
        s->rx_len = 0;
        s->rx_bits = 0;
        return;
    }

    st25r3916_hal_field_off();
    furi_delay_ms(5);
    St25rHalStatus fs = st25r3916_hal_field_on();
    s->field_ok = (fs == St25rHalOk);
    if(!s->field_ok) {
        s->last = St25rHalError;
        s->rx_len = 0;
        s->rx_bits = 0;
        return;
    }

    uint16_t rxlen = 0;
    s->last = st25r3916_hal_reqa(false, s->atqa, sizeof(s->atqa), &rxlen);
    s->rx_len = rxlen;
    s->rx_bits = st25r3916_hal_last_rx_bits();
    if(s->last == St25rHalOk) s->ok_count++;
}

static void draw_cb(Canvas* c, void* ctx) {
    DiagState* s = ctx;
    canvas_clear(c);
    char line[72];

    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 2, 9, "NFC Diag ST25R3916");

    canvas_set_font(c, FontSecondary);
    snprintf(line, sizeof(line), "ack:%s  chip:%s", s->ack ? "Y" : "N", s->present ? "OK" : "absent");
    canvas_draw_str(c, 2, 20, line);

    snprintf(line, sizeof(line), "IDENT:0x%02X err:0x%X", s->id_raw, (unsigned)s->id_err);
    canvas_draw_str(c, 2, 31, line);

    /* probe() internals: i2c_init ret, then check_chip_id ret + ic_type. If
     * init is 0 but cc errors (while IDENT above is 0x2A/no-err) the i2c_init
     * reconfig is what breaks the read. */
    snprintf(
        line, sizeof(line), "init:0x%X cc:0x%X i:%u", (unsigned)s->init_err,
        (unsigned)s->cc_err, s->cc_ictype);
    canvas_draw_str(c, 2, 42, line);

    snprintf(
        line, sizeof(line), "field:%s REQA:%s b=%u", s->field_ok ? "on" : "ERR",
        status_str(s->last), s->rx_bits);
    canvas_draw_str(c, 2, 53, line);

    snprintf(line, sizeof(line), "n=%lu seen=%lu Back=exit", (unsigned long)s->poll_count,
        (unsigned long)s->ok_count);
    canvas_draw_str(c, 2, 63, line);
}

static void input_cb(InputEvent* e, void* ctx) {
    FuriMessageQueue* q = ctx;
    furi_message_queue_put(q, e, FuriWaitForever);
}

int32_t st25r_nfc_diag_app(void* p) {
    UNUSED(p);

    static DiagState st;
    memset(&st, 0, sizeof(st));

    bus_bringup();
    furi_delay_ms(150); /* settle: the just-reclaimed module/bus needs ~150ms */

    FuriMessageQueue* q = furi_message_queue_alloc(8, sizeof(InputEvent));
    ViewPort* vp = view_port_alloc();
    view_port_draw_callback_set(vp, draw_cb, &st);
    view_port_input_callback_set(vp, input_cb, q);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);

    bool running = true;
    while(running) {
        InputEvent e;
        FuriStatus qs = furi_message_queue_get(q, &e, POLL_PERIOD_MS);
        if(qs == FuriStatusOk) {
            if(e.type == InputTypeShort && e.key == InputKeyBack) running = false;
        } else {
            poll_once(&st);
            view_port_update(vp);
        }
    }

    st25r3916_hal_field_off();

    gui_remove_view_port(gui, vp);
    furi_record_close(RECORD_GUI);
    view_port_free(vp);
    furi_message_queue_free(q);
    return 0;
}

#else /* !BOARD_HAS_ST25R3916 — this diagnostic is ST25R3916-specific */

int32_t st25r_nfc_diag_app(void* p) {
    UNUSED(p);
    return 0;
}

#endif
