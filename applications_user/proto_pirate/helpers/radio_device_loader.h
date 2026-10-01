// helpers/radio_device_loader.h
#pragma once

#include <lib/subghz/devices/devices.h>

// ESP32 static port: this file is compiled into the shared firmware image
// alongside other apps (e.g. esubghz_chat) that vendor an identically-named
// radio_device_loader helper. Prefix ProtoPirate's copy so the monolith link
// doesn't see multiple definitions. Definitions and all call sites include this
// header, so both get renamed consistently; esubghz_chat uses its own header.
#define radio_device_loader_set              protopirate_radio_device_loader_set
#define radio_device_loader_is_connect_external \
    protopirate_radio_device_loader_is_connect_external
#define radio_device_loader_is_external      protopirate_radio_device_loader_is_external
#define radio_device_loader_end              protopirate_radio_device_loader_end

#define SUBGHZ_DEVICE_CC1101_INT_NAME "cc1101_int"
#define SUBGHZ_DEVICE_CC1101_EXT_NAME "cc1101_ext"

/** SubGhzRadioDeviceType */
typedef enum {
    SubGhzRadioDeviceTypeInternal,
    SubGhzRadioDeviceTypeExternalCC1101,
} SubGhzRadioDeviceType;

const SubGhzDevice* radio_device_loader_set(
    const SubGhzDevice* current_radio_device,
    SubGhzRadioDeviceType radio_device_type);

bool radio_device_loader_is_connect_external(const char* name);
bool radio_device_loader_is_external(const SubGhzDevice* radio_device);
void radio_device_loader_end(const SubGhzDevice* radio_device);
