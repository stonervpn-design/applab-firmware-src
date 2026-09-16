#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <furi_hal_cortex.h>
#include <furi_hal_debug.h>
#include <furi_hal_rtc.h>
#include <furi_hal_power.h>
#include <furi_hal_interrupt.h>
#include <furi_hal_bt.h>
#include <furi_hal_crypto.h>
#include <furi_hal_info.h>
#include <furi_hal_gpio.h>
#include <furi_hal_resources.h>
#include <furi_hal_usb.h>
#include <furi_hal_random.h>
#include <furi_hal_memory.h>
#include <furi_hal_version.h>
#include <furi_hal_light.h>
#include <furi_hal_vibro.h>
#include <furi_hal_speaker.h>
#include <furi_hal_display.h>
#include <furi_hal_touch.h>
#include <furi_hal_spi.h>
#include <furi_hal_subghz.h>
#include <furi_hal_nfc.h>
#include <furi_hal_infrared.h>
#include <furi_hal_rfid.h>

void furi_hal_init_early(void);
void furi_hal_deinit_early(void);
void furi_hal_init(void);

/** Enable/disable the M5Stick Grove Port.A 5V boost (M5PM1 reg 0x06 bit3), which
 * powers an external Grove NFC unit. Off by default when no NFC frontend is found
 * at boot, to stop the boost converter idling. No-op on boards without the M5PM1. */
void furi_hal_grove_5v_set(bool on);

/** Read the M5PM1 PMIC battery voltage in millivolts (M5Stick S3). Returns true on
 * a successful I2C read. Always false on boards without the M5PM1. */
bool furi_hal_m5pm1_read_vbat_mv(uint16_t* out_mv);

/** Read the M5PM1 PMIC charging status (true = charging). Returns true on a
 * successful I2C read. Always false on boards without the M5PM1. */
bool furi_hal_m5pm1_read_charging(bool* out_charging);

#ifdef __cplusplus
}
#endif
