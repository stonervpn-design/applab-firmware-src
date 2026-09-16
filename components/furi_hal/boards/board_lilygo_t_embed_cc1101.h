/**
 * @file board_lilygo_t_embed_cc1101.h
 * Board definition: LilyGO T-Embed CC1101 ("Plus", T_EMBED_1101)
 *
 * MCU:      ESP32-S3 (dual-core Xtensa LX7, 16 MB Flash, 8 MB OPI PSRAM). Native
 *           USB-Serial/JTAG.
 * Display:  ST7789 170x320 RGB565, landscape. Shared SPI2 bus (MOSI=9 MISO=10
 *           SCK=11), CS=41 DC=16 RST=40 BL=21. Inversion on.
 * Power:    PIN_POWER_ON = GPIO15 MUST be driven HIGH (LDO enable) or the panel +
 *           CC1101 + fuel gauge stay unpowered. Handled generically by
 *           furi_hal_init_early() via BOARD_PIN_PWR_EN below.
 * Input:    Rotary encoder (A=4 B=5) + encoder push (GPIO0 = select/OK) + a
 *           dedicated Back button (GPIO6). All active-LOW.
 * SD Card:  microSD on the SAME shared SPI2 bus, CS=13 (CS-muxed with LCD +
 *           CC1101 + nRF24). /ext lives here.
 * Radios:   CC1101 (CSN=12 GDO0=3 GDO2=38 SW0=48 SW1=47) + nRF24 (CSN=44 CE=43),
 *           both parallel-attached on the shared SPI2 bus and CS-muxed.
 * IR:       TX=GPIO2, RX=GPIO1.
 * RGB:      WS2812 on GPIO14.
 * I2C:      Grove / fuel-gauge bus SDA=8 SCL=18 (BQ27220 fuel gauge @0x55,
 *           PN532 NFC — both deferred to a later phase).
 *
 * PHASE 1 bring-up: display + encoder input + microSD + SubGHz(CC1101) + nRF24 +
 * IR + BLE/WiFi + RGB. Gated OFF for phase 1: NFC (PN532), audio/speaker (I2S pin
 * clash with LCD RST), mic, IMU (none), real fuel gauge (battery virtualised).
 *
 * Pinout authority: Bruce boards/lilygo-t-embed-cc1101/pins_arduino.h (T_EMBED_1101).
 */

#pragma once

/* ---- Board metadata ---- */
#define BOARD_NAME        "LilyGO T-Embed CC1101"
#define BOARD_TARGET      "esp32s3"

/* ---- LCD (ST7789, 170x320) on the shared SPI2 bus ----
 * Landscape orientation (Bruce ROTATION 3): H_RES=320 (long axis), V_RES=170.
 * BOARD_PIN_LCD_SPI_MISO is intentionally NOT defined: this is a shared-bus board,
 * so furi_hal_display.c must claim the SD MISO line (GPIO10) when it brings the
 * SPI2 host up, otherwise SD reads on the same host have no MISO.
 * The 170px-wide panel is centered in the ST7789's 240-col RAM -> a 35px offset on
 * the short (170) axis; the long (320) axis has no offset. GAP_X/GAP_Y + the
 * mirror/swap bits below are the most likely values to need one on-hardware tweak. */
#define BOARD_PIN_LCD_MOSI      9
#define BOARD_PIN_LCD_SCLK      11
#define BOARD_PIN_LCD_CS        41
#define BOARD_PIN_LCD_DC        16
#define BOARD_PIN_LCD_RST       40
#define BOARD_PIN_LCD_BL        21

/* ---- LCD Display Configuration ---- */
#define BOARD_LCD_H_RES         320
#define BOARD_LCD_V_RES         170
#define BOARD_LCD_SPI_HOST      SPI2_HOST
#define BOARD_LCD_SPI_FREQ_HZ   (40 * 1000 * 1000)
#define BOARD_LCD_CMD_BITS      8
#define BOARD_LCD_PARAM_BITS    8
#define BOARD_LCD_SWAP_XY       true
/* 180° from the initial guess (screen came up upside down): both mirror axes
 * flipped. Panel window is centred on both axes (170-in-240 = 35px each side,
 * 320 full) so the gaps are symmetric and unchanged by the flip. */
#define BOARD_LCD_MIRROR_X      false
#define BOARD_LCD_MIRROR_Y      true
#define BOARD_LCD_INVERT_COLOR  true
#define BOARD_LCD_GAP_X         0
#define BOARD_LCD_GAP_Y         35
#define BOARD_LCD_BL_ACTIVE_LOW false
#define BOARD_LCD_COLOR_ORDER_BGR false

#define BOARD_LCD_FG_COLOR      0xA0FD
#define BOARD_LCD_BG_COLOR      0x0000

/* ---- SD Card on the shared SPI2 bus (CS-muxed) ----
 * BOARD_SD_SPI_HOST intentionally NOT defined: SD_SPI_HOST then defaults to
 * SPI2_HOST (the LCD's host, already brought up by the display init) and SD only
 * attaches a device — the legacy shared-bus path in furi_hal_sd.c. */
#define BOARD_HAS_SD            1
#define BOARD_HAS_SD_CARD       1
#define BOARD_PIN_SD_CS         13
#define BOARD_PIN_SD_MOSI       9
#define BOARD_PIN_SD_MISO       10
#define BOARD_PIN_SD_SCLK       11

/* ---- Input: rotary encoder + 2 buttons (no I2C keyboard) ---- */
#define BOARD_HAS_TCA8418       0
#define BOARD_HAS_ENCODER       1
#define BOARD_PIN_ENCODER_A     4
#define BOARD_PIN_ENCODER_B     5
#define BOARD_PIN_ENCODER_BTN   0    /* encoder push = select / OK */
#define BOARD_PIN_BUTTON_KEY    6    /* dedicated Back button */

/* Main I2C bus (BQ27220 fuel gauge + Grove/PN532). Named with the KB_* aliases
 * furi_hal_i2c_bus.c expects for I2C_NUM_0, even though there is no keyboard. */
#define KB_I2C_PORT             I2C_NUM_0
#define KB_I2C_FREQ_HZ          400000
#define KB_PIN_SDA              8
#define KB_PIN_SCL              18
#define KB_PIN_INT              UINT16_MAX
#define KB_I2C_ADDR             0x00
#define KB_I2C_PIN_SDA          KB_PIN_SDA
#define KB_I2C_PIN_SCL          KB_PIN_SCL

/* TCA8418 register defs kept for source compatibility (unused: no TCA8418). */
#define TCA8418_REG_CFG         0x01
#define TCA8418_REG_INT_STAT    0x02
#define TCA8418_REG_KEY_LCK_EC  0x03
#define TCA8418_REG_KEY_EVENT_A 0x04
#define TCA8418_REG_KP_GPIO1    0x1D
#define TCA8418_REG_KP_GPIO2    0x1E
#define TCA8418_REG_KP_GPIO3    0x1F
#define TCA8418_REG_DEBOUNCE1   0x29
#define TCA8418_REG_DEBOUNCE2   0x2A
#define TCA8418_REG_DEBOUNCE3   0x2B
#define TCA8418_CFG_KE_IEN      (1 << 0)
#define TCA8418_CFG_INT_CFG     (1 << 1)
#define TCA8418_CFG_OVR_FLOW_M  (1 << 2)
#define TCA8418_KEY_PRESS_MASK  0x80
#define TCA8418_KEY_ID_MASK     0x7F
#define TCA8418_FIFO_EMPTY      0x00

/* ---- Touch Controller — NOT PRESENT ---- */
#define BOARD_PIN_TOUCH_SCL     UINT16_MAX
#define BOARD_PIN_TOUCH_SDA     UINT16_MAX
#define BOARD_PIN_TOUCH_RST     UINT16_MAX
#define BOARD_PIN_TOUCH_INT     UINT16_MAX
#define BOARD_TOUCH_I2C_ADDR    0x00
#define BOARD_TOUCH_I2C_PORT    I2C_NUM_0
#define BOARD_TOUCH_I2C_FREQ_HZ 0
#define BOARD_TOUCH_I2C_TIMEOUT 0

/* ---- SubGHz / CC1101 on the shared SPI2 bus (CS-muxed with LCD + SD + nRF24) ----
 * BOARD_CC1101_SPI_SHARED=1 selects the shared-SPI2 path in furi_hal_spi.c (the
 * subghz bus host_id becomes SPI2_HOST) and makes furi_hal_spi_acquire/release
 * take the global bus lock so CC1101 traffic serialises against LCD DMA + SD.
 * Do NOT also define BOARD_SUBGHZ_SPI_HOST — that path is for a dedicated host
 * (Cardputer-ADV) and would override the shared path.
 * SW0/SW1 (GPIO48/47) are the RF band-select switch; furi_hal_subghz drives them. */
#define BOARD_PIN_CC1101_SCK    11
#define BOARD_PIN_CC1101_MISO   10
#define BOARD_PIN_CC1101_MOSI   9
#define BOARD_PIN_CC1101_CSN    12
#define BOARD_PIN_CC1101_GDO0   3
#define BOARD_PIN_CC1101_GDO2   38
#define BOARD_PIN_CC1101_SW1    47
#define BOARD_PIN_CC1101_SW0    48
#define BOARD_CC1101_SPI_SHARED 1

/* ---- NRF24L01 on the shared SPI2 bus (CS-muxed alongside the CC1101) ----
 * Parallel-attached; only CSN + CE are consumed (SCK/MISO/MOSI come from the
 * CC1101 defines via furi_hal_resources.c). furi_hal.c init_early drives CSN HIGH
 * / CE LOW before any CC1101 traffic so the nRF24 cannot snoop the bus. */
#define BOARD_HAS_NRF24         1
#define BOARD_PIN_NRF24_SCK     11   /* unused: shared, see CC1101_SCK */
#define BOARD_PIN_NRF24_MISO    10   /* unused: shared, see CC1101_MISO */
#define BOARD_PIN_NRF24_MOSI    9    /* unused: shared, see CC1101_MOSI */
#define BOARD_PIN_NRF24_CSN     44
#define BOARD_PIN_NRF24_CE      43

/* ---- Power Enable (LDO) — drive HIGH at boot (furi_hal_init_early) ---- */
#define BOARD_PIN_PWR_EN        15

/* ---- Audio (NS4168 I2S amp) — DISABLED for phase 1 ----
 * Pins recorded for later. Note the hard clash: WCLK(40)==LCD RST(40) and
 * MCLK(39)==Mic CLK(39). Enabling the speaker would fight the display, so it must
 * stay off until an alternate I2S mapping is worked out. */
#define BOARD_HAS_ES8311        0
#define BOARD_PIN_I2S_SCLK      46
#define BOARD_PIN_I2S_LRCK      40
#define BOARD_PIN_I2S_DOUT      7
#define BOARD_PIN_I2S_MCLK      39
#define BOARD_PIN_SPEAKER_BCLK  BOARD_PIN_I2S_SCLK
#define BOARD_PIN_SPEAKER_WCLK  BOARD_PIN_I2S_LRCK
#define BOARD_PIN_SPEAKER_DOUT  BOARD_PIN_I2S_DOUT

/* ---- IMU — NOT PRESENT on the T-Embed CC1101 ---- */
#define BOARD_HAS_BMI270        0
#define BMI270_I2C_ADDR         0x68

/* ---- Miscellaneous ---- */
#define BOARD_PIN_BUTTON_BOOT   0
#define BOARD_PIN_BATTERY_ADC   UINT16_MAX  /* battery via BQ27220 (phase 2), not ADC */

#define BOARD_PIN_IR_TX         2
#define BOARD_PIN_IR_RX         1

#define BOARD_PIN_MIC_DATA      42
#define BOARD_PIN_MIC_CLK       39

/* ---- WS2812 RGB LED ---- */
#define BOARD_PIN_WS2812_DATA   14
#define BOARD_WS2812_LED_COUNT  8
#define BOARD_HAS_RGB_LED       1

/* ---- RFID / RDM6300 — NOT PRESENT ---- */
#define BOARD_PIN_RFID_RX       UINT16_MAX
#define BOARD_PIN_RFID_TX       UINT16_MAX
#define BOARD_RFID_UART_NUM     1

/* ---- ONBOARD PN532 @0x24 on the main I2C bus (SDA8/SCL18). Per LilyGO's own
 * schematic (Xinyuan-LilyGO/T-Embed-CC1101): PN532 RSTPDN = GPIO45 (BOARD_PN532_RF_REST),
 * IRQ = GPIO17. The RST line MUST be driven HIGH to release the PN532 from reset —
 * furi_hal_nfc_init() drives BOARD_PIN_NFC_RST HIGH when it is != UINT16_MAX. Leaving
 * it unmanaged held the PN532 in reset, so it never ACKed on I2C. ---- */
#define BOARD_PIN_NFC_SCL       BOARD_PIN_QWIIC_SCL
#define BOARD_PIN_NFC_SDA       BOARD_PIN_QWIIC_SDA
#define BOARD_PIN_NFC_IRQ       17   /* onboard PN532 IRQ */
#define BOARD_PIN_NFC_RST       45   /* onboard PN532 RSTPDN — driven HIGH to release reset */
#define BOARD_NFC_I2C_PORT      I2C_NUM_0
#define BOARD_NFC_I2C_ADDR      0x24
#define BOARD_HAS_ST25R3916     1  /* keep the ST25R3916 backend COMPILED (furi_hal_nfc.c's runtime dispatch references it), but... */
#define BOARD_NFC_PN532_ONLY    1  /* ...this board has an ONBOARD PN532 @0x24 (SDA8/SCL18), no ST25R3916. Skip the ST25R3916 PROBE in furi_hal_nfc_init so pn532_i2c_init owns a clean I2C bus (the ST25R3916-first probe left the PN532 undetectable). */
#define BOARD_ST25R3916_I2C_ADDR 0x50

/* ---- Grove / Qwiic I2C (== the main I2C bus on this board) ---- */
#define BOARD_PIN_QWIIC_SDA     8
#define BOARD_PIN_QWIIC_SCL     18
#define I2C_SDA_GPIO            BOARD_PIN_QWIIC_SDA
#define I2C_SCL_GPIO            BOARD_PIN_QWIIC_SCL

/* ---- Feature flags (phase 1) ---- */
#define BOARD_HAS_TOUCH         0
#define BOARD_HAS_BLE           1
#define BOARD_HAS_VIBRO         0
#define BOARD_HAS_SPEAKER       0
#define BOARD_HAS_IR            1
#define BOARD_HAS_IR_TX         1
#define BOARD_HAS_IR_RX         1
#define FURI_HAL_SPEAKER_GPIO   BOARD_PIN_SPEAKER_DOUT

#define BOARD_HAS_IBUTTON       0
#define BOARD_HAS_RFID          0
#define BOARD_HAS_NFC           1  /* PN532 @0x24 on QWIIC/Grove I2C */
#define BOARD_HAS_SUBGHZ        1
#define BOARD_HAS_MIC           0
#define BOARD_HAS_IMU           0

/* ---- Battery / power: virtualised for phase 1 ----
 * The T-Embed has a real BQ27220 fuel gauge (@0x55 on the main I2C bus), but for
 * bring-up the battery level is reported virtually and the power HAL is kept off
 * the I2C bus (BOARD_POWER_SKIP_I2C). Wiring up the real gauge is a phase-2 task. */
#define BQ27220_ADDR            0x00
#define BQ_I2C_PORT             I2C_NUM_0
#define BQ_I2C_SDA              UINT16_MAX
#define BQ_I2C_SCL              UINT16_MAX
#define BOARD_POWER_SKIP_I2C
#define HIGH_DRAIN_CURRENT_THRESHOLD (-200)
#define FURI_HAL_POWER_VIRTUAL_CAPACITY_MAH     (1000U)
#define BQ25896_CHARGE_LIMIT    1280
#define FURI_HAL_POWER_ADC_DIVIDER_RATIO        (2.0f)
