#include "pin_config_apply.h"
#include "pin_config_store.h"

#include <furi.h>
#include <furi_hal_resources.h>
#include <furi_hal_infrared.h>
#include <furi_hal_spi.h>
#include <furi_hal_spi_types.h>

#define TAG "PinConfig"

void pin_config_apply(void) {
    /* Persist the movable pins to NVS ONLY. They are read back and applied at boot
     * in furi_hal_init (before the CC1101/nRF24 probe). Nothing is driven live here:
     * the running GpioPin structs and the SPI buses are left exactly as booted, so
     * editing a pin can never reconfigure a GPIO mid-session — critically it cannot
     * clobber the TCA8418 keyboard bus (GPIO 8/9) or hang the shared SPI3/SD bus.
     *
     * Changes therefore take effect on the next reboot ("Reboot & Apply"). This is
     * also why the app now allows assigning a pin another function already uses
     * (shared pins): no live double-drive can happen, and the user decides the final
     * layout that boot will apply. */
    furi_hal_module_pins_save_nvs_only(
        pin_config_get(PinFuncCc1101Csn),
        pin_config_get(PinFuncCc1101Gdo0),
        pin_config_get(PinFuncNrf24Csn),
        pin_config_get(PinFuncNrf24Ce),
        pin_config_get(PinFuncIrTx),
        pin_config_get(PinFuncIrRx));

    FURI_LOG_I(
        TAG,
        "saved (reboot to apply): cc1101 csn=%d gdo0=%d nrf24 csn=%d ce=%d ir tx=%d rx=%d",
        pin_config_get(PinFuncCc1101Csn),
        pin_config_get(PinFuncCc1101Gdo0),
        pin_config_get(PinFuncNrf24Csn),
        pin_config_get(PinFuncNrf24Ce),
        pin_config_get(PinFuncIrTx),
        pin_config_get(PinFuncIrRx));
}
