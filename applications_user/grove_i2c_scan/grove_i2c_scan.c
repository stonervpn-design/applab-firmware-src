/* Grove I2C scanner — diagnostic for the ST25R3916 "not connected" issue.
 * Scans the NFC/Grove I2C bus (BOARD_NFC_I2C_PORT on QWIIC SCL/SDA), reclaiming
 * the pins from IR/RMT first (GPIO1 SCL is shared with internal IR RX), and
 * shows which 7-bit addresses ACK. 0x24 = PN532, 0x28 = WS1850S (M5 RFID2),
 * 0x50 = ST25R3916 (M5 NFC2). */
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <boards/board.h>
#include <driver/i2c.h>
#include <driver/gpio.h>

typedef struct {
    uint8_t found[24];
    int nfound;
} ScanResult;

static void scan_i2c(ScanResult* r) {
    r->nfound = 0;

    /* Make sure the Grove 5V boost is on: boot may have cut it if no NFC unit was
     * present, but this diagnostic exists precisely to find a just-attached unit. */
    furi_hal_grove_5v_set(true);
    furi_delay_ms(50); /* let the boost + any attached unit settle before probing */

    /* Reclaim the Grove pins from any peripheral (IR RMT on GPIO1) so the I2C
     * peripheral can drive them. */
    gpio_reset_pin((gpio_num_t)BOARD_PIN_QWIIC_SCL);
    gpio_reset_pin((gpio_num_t)BOARD_PIN_QWIIC_SDA);

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BOARD_PIN_QWIIC_SDA,
        .scl_io_num = BOARD_PIN_QWIIC_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 200000,
    };
    i2c_param_config(BOARD_NFC_I2C_PORT, &conf);
    /* may already be installed by furi_hal_nfc_init -> ignore INVALID_STATE */
    i2c_driver_install(BOARD_NFC_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    i2c_param_config(BOARD_NFC_I2C_PORT, &conf);

    for(uint8_t a = 0x08; a < 0x78; a++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (uint8_t)((a << 1) | I2C_MASTER_WRITE), true);
        i2c_master_stop(cmd);
        esp_err_t e = i2c_master_cmd_begin(BOARD_NFC_I2C_PORT, cmd, pdMS_TO_TICKS(30));
        i2c_cmd_link_delete(cmd);
        if(e == ESP_OK && r->nfound < (int)sizeof(r->found)) {
            r->found[r->nfound++] = a;
        }
    }
}

static void draw_cb(Canvas* c, void* ctx) {
    ScanResult* r = ctx;
    canvas_clear(c);
    canvas_set_font(c, FontSecondary);
    char line[72];

    snprintf(
        line, sizeof(line), "Grove I2C  SCL=G%d SDA=G%d",
        (int)BOARD_PIN_QWIIC_SCL, (int)BOARD_PIN_QWIIC_SDA);
    canvas_draw_str(c, 2, 9, line);

    if(r->nfound == 0) {
        canvas_draw_str(c, 2, 24, "No devices ACKed.");
        canvas_draw_str(c, 2, 34, "-> wiring / IR pin / speed");
    } else {
        int off = snprintf(line, sizeof(line), "ACK: ");
        for(int i = 0; i < r->nfound && off < 60; i++) {
            off += snprintf(line + off, sizeof(line) - off, "%02X ", r->found[i]);
        }
        canvas_draw_str(c, 2, 24, line);
        canvas_draw_str(c, 2, 36, "24=PN532 28=WS1850S 50=ST25R");
    }
    canvas_draw_str(c, 2, 60, "Any key to exit");
}

static void input_cb(InputEvent* e, void* ctx) {
    FuriMessageQueue* q = ctx;
    furi_message_queue_put(q, e, FuriWaitForever);
}

int32_t grove_i2c_scan_app(void* p) {
    UNUSED(p);

    static ScanResult res;
    scan_i2c(&res);

    FuriMessageQueue* q = furi_message_queue_alloc(8, sizeof(InputEvent));
    ViewPort* vp = view_port_alloc();
    view_port_draw_callback_set(vp, draw_cb, &res);
    view_port_input_callback_set(vp, input_cb, q);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, vp, GuiLayerFullscreen);

    InputEvent e;
    while(furi_message_queue_get(q, &e, FuriWaitForever) == FuriStatusOk) {
        if(e.type == InputTypeShort || e.type == InputTypeLong) break;
    }

    gui_remove_view_port(gui, vp);
    furi_record_close(RECORD_GUI);
    view_port_free(vp);
    furi_message_queue_free(q);
    return 0;
}
