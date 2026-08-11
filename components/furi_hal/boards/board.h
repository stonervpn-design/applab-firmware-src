/**
 * @file board.h
 * Board selection header — includes the active board definition.
 *
 * Select board via CMake: -DFLIPPER_BOARD=waveshare_c6_1.9
 * The build system defines BOARD_INCLUDE to the correct header path.
 */

#pragma once

#if defined(BOARD_INCLUDE)
/* CMake sets BOARD_INCLUDE to e.g. "boards/board_waveshare_c6_1.9.h" */
#include BOARD_INCLUDE
#elif defined(BOARD_WAVESHARE_C6_1_9)
#include "board_waveshare_c6_1.9.h"
#else
/* Default board */
#include "board_waveshare_c6_1.9.h"
#endif

/* Sanity check: every board must define these */
#ifndef BOARD_NAME
#error "Board header must define BOARD_NAME"
#endif
#ifndef BOARD_PIN_LCD_MOSI
#error "Board header must define LCD pins"
#endif

/* nRF24 jammer capability: may the USB PA clamp be lifted to run PA_MAX while on USB?
 * Boards with a weak USB power path (Cardputer ADV) brown out and USB-reboot at PA_MAX
 * even with the Hydra module's caps, so they set this to 0 to keep the clamp engaged.
 * Default 1 preserves the prior unclamp-on-USB behavior for every other board. */
#ifndef BOARD_NRF24_USB_PA_UNCLAMP_OK
#define BOARD_NRF24_USB_PA_UNCLAMP_OK 1
#endif
