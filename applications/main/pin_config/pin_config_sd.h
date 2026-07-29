#pragma once
#include <stdbool.h>

// Load the pin assignments from SD (falls back to defaults if absent).
void pin_config_load(void);
// Persist the current assignments to SD. Returns true on success.
bool pin_config_save(void);
