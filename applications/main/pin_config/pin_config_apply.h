#pragma once

// Apply the current in-memory pin assignments to the HAL for the runtime-movable
// pins (CC1101 CSN/GDO0, NRF24 CSN/CE, IR TX/RX). Call after pin_config_load()
// or after changing a value in the app. Tears down the CC1101 SPI device so the
// next SubGHz acquire re-reads the new CSN. Safe to call when the radio is idle.
void pin_config_apply(void);
