#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifndef PUBLIC_BUILD

// TPMS car tracker (PRIVATE-only): a persisted watch list of TPMS sensor IDs. Add
// a target vehicle's sensor(s) from the TPMS info view; the TPMS scanner then
// alerts whenever a watched sensor is heard again — i.e. that specific vehicle is
// nearby. Surveillance/recon use of the TPMS RX. Persisted to
// /ext/subghz/tpms_watch.txt. Single-threaded UI use → simple static store.

#define TPMS_WATCH_MAX 32u

/** Load the watch list from /ext into memory. Call when entering the scanner. */
void tpms_watch_load(void);

/** Add a sensor ID + persist. Returns true if watched afterwards (incl. already
 * present); false only if the list is full. id 0 is ignored. */
bool tpms_watch_add(uint32_t id);

/** True if this sensor ID is on the watch list. */
bool tpms_watch_contains(uint32_t id);

/** Number of watched IDs currently loaded. */
uint8_t tpms_watch_count(void);

/** Clear the watch list (memory + file). */
void tpms_watch_clear(void);

#endif // PUBLIC_BUILD
