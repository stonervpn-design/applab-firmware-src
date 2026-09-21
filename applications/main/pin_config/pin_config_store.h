#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Function -> GPIO assignment store. The pure part (get/set/serialize/parse) has
// no IDF deps and is host-testable; SD load/save lives in pin_config_sd.c.

typedef enum {
    PinFuncIrTx,
    PinFuncIrRx,
    PinFuncSpiSck,
    PinFuncSpiMiso,
    PinFuncSpiMosi,
    PinFuncCc1101Csn,
    PinFuncCc1101Gdo0,
    PinFuncNrf24Ce,
    PinFuncNrf24Csn,
    PinFuncI2cSda,
    PinFuncI2cScl,
    PinFuncCount,
} PinFunc;

// Selectable GPIO values (ESP32-S3 usable pins; flash pins 26-32 excluded).
extern const uint8_t pin_config_choices[];
extern const size_t pin_config_choices_count;

const char* pin_config_func_name(PinFunc f); // e.g. "IR TX"
uint8_t pin_config_default(PinFunc f); // board default GPIO

void pin_config_reset_defaults(void); // set all current -> defaults
uint8_t pin_config_get(PinFunc f); // current assignment
void pin_config_set(PinFunc f, uint8_t gpio);

// True only for pins that can actually be remapped at runtime on this board
// (IR TX/RX, CC1101 CSN/GDO0, NRF24 CSN/CE). SPI SCK/MISO/MOSI and I2C SDA/SCL are
// fixed by the bus init (SPI3 is shared with the SD card) — they are display-only.
bool pin_config_is_movable(PinFunc f);

// True if assigning `gpio` to `changing` would collide with a pin already used by
// another function — a fixed bus line (SPI SCK/MISO/MOSI, I2C SDA/SCL, all on the
// shared SPI3/SD bus) or another remappable pin. Driving such a pin as a second
// function hangs the bus and watchdog-reboots the device, so the caller must reject
// the change. `changing` is excluded from the comparison; the colliding function is
// returned via `out_conflict` (may be NULL).
bool pin_config_pin_conflict(PinFunc changing, uint8_t gpio, PinFunc* out_conflict);

// Pure text codec (used by pin_config_sd.c): "func=gpio" lines, one per function.
size_t pin_config_serialize(char* out, size_t out_max);
void pin_config_parse(const char* text); // unknown keys ignored; missing -> default
