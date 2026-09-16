#include "ble_hid_ext_profile.h"

#include <furi.h>
#include <string.h>

/* [BLE-FIX] fwd decl so start() can build the GAP config from the UI params. */
static void
    ble_profile_hid_ext_get_config(GapConfig* config, FuriHalBleProfileParams profile_params);

static FuriHalBleProfileBase* ble_profile_hid_ext_start(FuriHalBleProfileParams profile_params) {
    /* [BLE-FIX] The old code passed NULL, so the device advertised under the
     * default "Control ..." identity/MAC instead of the configured BadUSB name.
     * Build the real GAP config from the params and start with it. */
    if(!profile_params) {
        return ble_profile_hid->start(NULL); /* no params -> default identity */
    }
    GapConfig config = {0};
    ble_profile_hid_ext_get_config(&config, profile_params);
    return ble_profile_hid_start_with_config(ble_profile_hid, &config);
}

static void ble_profile_hid_ext_stop(FuriHalBleProfileBase* profile) {
    ble_profile_hid->stop(profile);
}

static void
    ble_profile_hid_ext_get_config(GapConfig* config, FuriHalBleProfileParams profile_params) {
    BleProfileHidExtParams* hid_ext_profile_params = profile_params;

    furi_check(config);
    furi_check(profile_params);

    ble_profile_hid->get_gap_config(config, NULL);
    memcpy(config->mac_address, hid_ext_profile_params->mac, sizeof(config->mac_address));
    strlcpy(config->adv_name, hid_ext_profile_params->name, sizeof(config->adv_name));
    config->bonding_mode = hid_ext_profile_params->bonding;
    config->pairing_method = hid_ext_profile_params->pairing;
}

static const FuriHalBleProfileTemplate profile_callbacks = {
    .start = ble_profile_hid_ext_start,
    .stop = ble_profile_hid_ext_stop,
    .get_gap_config = ble_profile_hid_ext_get_config,
};

const FuriHalBleProfileTemplate* ble_profile_hid_ext = &profile_callbacks;
