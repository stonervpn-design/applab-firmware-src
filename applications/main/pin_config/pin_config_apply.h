#pragma once

// Persist the current in-memory pin assignments (CC1101 CSN/GDO0, NRF24 CSN/CE,
// IR TX/RX) to NVS so they are applied at the NEXT BOOT. Does NOT drive any GPIO
// live and does NOT touch the SPI buses, so calling it while editing can never
// disturb the keyboard or the shared SPI bus. Use "Reboot & Apply" to activate.
void pin_config_apply(void);
