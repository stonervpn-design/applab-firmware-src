#pragma once
// HWID lock (PRIVATE builds): binds the firmware to authorized ESP32-S3 chips by
// their factory eFuse MAC. This is the reversible SOFTWARE layer; the strong lock
// is eFuse Secure Boot v2 + Flash Encryption (set up separately). A determined RE
// can patch this out, but it stops a copied bin from running on another board.
#include <stdbool.h>

// Returns true iff the running chip's factory MAC is on the compiled allowlist.
bool hwid_lock_is_authorized(void);

// If unauthorized: shows a full-screen "DEVICE LOCKED" screen, captures all input,
// and NEVER returns (halts boot). Returns immediately if authorized. Must be called
// AFTER the gui service has started (it opens RECORD_GUI).
void hwid_lock_enforce(void);
