// Frame ingest: the promiscuous callback (WiFi task) classifies beacons / pwngrid
// peers / EAPOL, and the worker processes queued EAPOL into the net table + stats.
#pragma once

#include "fg_app.h"

// WiFi-task context: classify one received frame.
void fg_capture_cb(FgApp* app, const uint8_t* fr, int len);

// Worker context: process one queued EAPOL frame (flags, dedup pwn, loot, reward).
void fg_capture_process(FgApp* app, const uint8_t* fr, uint16_t len);
