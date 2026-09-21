#include "pin_config_apply.h"
#include "pin_config_store.h"

#include <furi.h>
#include <furi_hal_resources.h>
#include <furi_hal_infrared.h>
#include <furi_hal_spi.h>
#include <furi_hal_spi_types.h>

#define TAG "PinConfig"

void pin_config_apply(void) {
    /* Persist to NVS (read back at boot in furi_hal_init, before the CC1101
     * probe) and apply to the running structs now. */
    furi_hal_module_pins_save_nvs(
        pin_config_get(PinFuncCc1101Csn),
        pin_config_get(PinFuncCc1101Gdo0),
        pin_config_get(PinFuncNrf24Csn),
        pin_config_get(PinFuncNrf24Ce),
        pin_config_get(PinFuncIrTx),
        pin_config_get(PinFuncIrRx));

    /* Drop the CC1101 SPI device so a later SubGHz acquire re-reads the new CSN.
     * The chip is fully re-probed with the new pin on reboot. */
    furi_hal_spi_bus_handle_deinit(&furi_hal_spi_bus_handle_subghz);

    FURI_LOG_I(
        TAG,
        "saved+applied: cc1101 csn=%d gdo0=%d nrf24 csn=%d ce=%d ir tx=%d rx=%d",
        pin_config_get(PinFuncCc1101Csn),
        pin_config_get(PinFuncCc1101Gdo0),
        pin_config_get(PinFuncNrf24Csn),
        pin_config_get(PinFuncNrf24Ce),
        pin_config_get(PinFuncIrTx),
        pin_config_get(PinFuncIrRx));
}
