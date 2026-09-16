#include "subghz_txrx_i.h" // IWYU pragma: keep

#include <lib/subghz/protocols/protocol_items.h>
#include <applications/drivers/subghz/cc1101_ext/cc1101_ext_interconnect.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <lib/subghz/blocks/custom_btn.h>
#include <storage/storage.h>
#include <string.h>
#ifndef PUBLIC_BUILD
#include "subghz_keeloq_default_keys.h"
#endif

#define TAG "SubGhzTxRx"

/* PRIVATE-ONLY: all firmware-embedded car key material — the KeeLoq manufacturer
 * keystore (keeloq_mfcodes_user), the VAG AUT64 keystore, and the VAG test captures —
 * plus the functions that provision them to /ext, are compiled out of public builds.
 * PUBLIC_BUILD (root CMakeLists, FLIPPER_VARIANT=public) must strip this so the keys
 * never ship in a public webflasher bin. (Regression fixed 2026-08-05 after they had
 * leaked into the public bins; the guard previously covered only the car DECODERS.) */
#ifndef PUBLIC_BUILD
/* Provision the firmware-embedded default keeloq_mfcodes_user (needed on SD-less
 * boards; also brings older devices up to the current key set). Version-gated: a
 * single-char marker file records which embedded set was last written. We (re)write
 * the keystore only when it is MISSING or the embedded version is NEWER than the
 * marker — so bumping SUBGHZ_KEELOQ_PROVISION_VERSION syncs every device once, while
 * a user's later edits (marker already current) are preserved across reboots. */
#define SUBGHZ_KEELOQ_PROVISION_VERSION '4' /* bump when keeloq_mfcodes_user_default changes */
#define SUBGHZ_KEELOQ_VERSION_FILE      EXT_PATH("subghz/assets/.keeloq_ver")
static void subghz_txrx_provision_default_keeloq_keys(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);

    bool exists = storage_file_exists(storage, SUBGHZ_KEYSTORE_DIR_USER_NAME);
    char stored_ver = 0;
    File* vf = storage_file_alloc(storage);
    if(storage_file_open(vf, SUBGHZ_KEELOQ_VERSION_FILE, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_read(vf, &stored_ver, 1);
    }
    storage_file_close(vf);
    storage_file_free(vf);

    if(!exists || stored_ver < SUBGHZ_KEELOQ_PROVISION_VERSION) {
        storage_simply_mkdir(storage, EXT_PATH("subghz"));
        storage_simply_mkdir(storage, EXT_PATH("subghz/assets"));
        File* file = storage_file_alloc(storage);
        if(storage_file_open(
               file, SUBGHZ_KEYSTORE_DIR_USER_NAME, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
            size_t len = strlen(keeloq_mfcodes_user_default);
            size_t wr = storage_file_write(file, keeloq_mfcodes_user_default, len);
            FURI_LOG_I(
                TAG, "Provisioned keeloq_mfcodes_user (%u/%u bytes)", (unsigned)wr, (unsigned)len);
        } else {
            FURI_LOG_W(TAG, "keeloq_mfcodes_user create failed");
        }
        storage_file_close(file);
        storage_file_free(file);

        char ver = SUBGHZ_KEELOQ_PROVISION_VERSION;
        vf = storage_file_alloc(storage);
        if(storage_file_open(vf, SUBGHZ_KEELOQ_VERSION_FILE, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
            storage_file_write(vf, &ver, 1);
        }
        storage_file_close(vf);
        storage_file_free(vf);
    }
    furi_record_close(RECORD_STORAGE);
}

/* Firmware-embedded VAG test captures (1.sub / 2.sub) written to /ext/subghz on
 * first launch of the SD-less M5StickS3 so they can be opened/replayed to test
 * decode. Never overwrites existing files. */
static const char test_sub_1_default[] =
    "Filetype: Flipper SubGhz Key File\n"
    "Version: 1\n"
    "Frequency: 434420000\n"
    "Preset: FuriHalSubGhzPresetOok270Async\n"
    "Protocol: VAG\n"
    "Bit: 80\n"
    "Key: C0 6B AC 39 D8 F9 F3 60\n"
    "Key2: 00 00 00 00 00 00 73 2B\n"
    "Type: 3\n";
static const char test_sub_2_default[] =
    "Filetype: Flipper SubGhz Key File\n"
    "Version: 1\n"
    "Frequency: 434420000\n"
    "Preset: FuriHalSubGhzPresetOok270Async\n"
    "Protocol: VAG\n"
    "Bit: 80\n"
    "Key: C0 D5 C8 CF 0B 83 C7 33\n"
    "Key2: 00 00 00 00 00 00 57 1D\n"
    "Type: 3\n";

/* VAG AUT64 keystore. Without valid keys VAG frames decode but can't be decrypted,
 * so get_string shows "Key2:xxxx (corrupted)". VAG reads it from APP_ASSETS_PATH("vag")
 * = /ext/apps_assets/subghz/vag.
 *
 * This port's crypto enclave is stubbed: furi_hal_crypto_decrypt() is a memcpy
 * pass-through, so it CANNOT decrypt a real AES-encrypted keystore (that yields
 * garbage keys). We turn that into a feature: the raw-keystore reader still requires
 * Encryption:1 and runs the "decrypt", but since it's a pass-through the Encrypt_data
 * blob is handed to VAG verbatim. So the blob below is the PLAINTEXT 3x16-byte AUT64
 * keys (packed, index bytes 1/2/3) that vag.c expects — the real VAG keys, from KAT's
 * VAG keystore. IV is unused by the pass-through. */
static const char vag_keystore_default[] =
    "Filetype: Flipper SubGhz Keystore RAW File\r\n"
    "Version: 0\r\n"
    "Encryption: 1\r\n"
    "IV: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00\r\n"
    "Encrypt_data: RAW\r\n"
    "01376C86ADABCC43074DE859C12F36AB02377C65CEDC42EAA453E861D9B720FC038AA37B1E561F8384B619C52E0A3FD7";

static void subghz_txrx_provision_test_subs(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, EXT_PATH("subghz"));
    storage_simply_mkdir(storage, EXT_PATH("apps_assets"));
    storage_simply_mkdir(storage, EXT_PATH("apps_assets/subghz"));
    const char* paths[3] = {
        EXT_PATH("subghz/1.sub"),
        EXT_PATH("subghz/2.sub"),
        EXT_PATH("apps_assets/subghz/vag"),
    };
    const char* data[3] = {test_sub_1_default, test_sub_2_default, vag_keystore_default};
    // Always refresh the vag keystore (index 2) so a stale/wrong copy is replaced;
    // the .subs (0,1) are user-openable so only write them if absent.
    const bool force[3] = {false, false, true};
    for(size_t i = 0; i < 3; i++) {
        if(!force[i] && storage_file_exists(storage, paths[i])) continue;
        File* file = storage_file_alloc(storage);
        if(storage_file_open(file, paths[i], FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
            size_t len = strlen(data[i]);
            size_t wr = storage_file_write(file, data[i], len);
            FURI_LOG_I(TAG, "Provisioned %s (%u/%u bytes)", paths[i], (unsigned)wr, (unsigned)len);
        }
        storage_file_close(file);
        storage_file_free(file);
    }
    furi_record_close(RECORD_STORAGE);
}
#endif /* !PUBLIC_BUILD — car keystore provisioning is private-only */

static void subghz_txrx_radio_device_power_on(SubGhzTxRx* instance) {
    UNUSED(instance);
    uint8_t attempts = 0;
    while(!furi_hal_power_is_otg_enabled() && attempts++ < 5) {
        furi_hal_power_enable_otg();
        //CC1101 power-up time
        furi_delay_ms(10);
    }
}

static void subghz_txrx_radio_device_power_off(SubGhzTxRx* instance) {
    UNUSED(instance);
    if(furi_hal_power_is_otg_enabled()) furi_hal_power_disable_otg();
}

// Swap `receiver` between the full decoder set and a single named decoder (defined below).
static void subghz_txrx_receiver_switch(SubGhzTxRx* instance, const char* name);

SubGhzTxRx* subghz_txrx_alloc(void) {
    SubGhzTxRx* instance = malloc(sizeof(SubGhzTxRx));
    bool system_keystore_loaded = false;
    bool user_keystore_loaded = false;

    instance->setting = subghz_setting_alloc();
    subghz_setting_load(instance->setting, EXT_PATH("subghz/assets/setting_user"));

    instance->preset = malloc(sizeof(SubGhzRadioPreset));
    instance->preset->name = furi_string_alloc();
    subghz_txrx_set_default_preset(instance, 0);

    instance->txrx_state = SubGhzTxRxStateSleep;

    subghz_txrx_hopper_set_state(instance, SubGhzHopperStateOFF);
    subghz_txrx_set_debug_pin_state(instance, false);

    // RX worker is LAZY — created on demand in subghz_txrx_rx (rx_start) and freed in
    // subghz_txrx_stop. It is NOT allocated here so the receiver's ~170 decoders (below)
    // get the full internal RAM; holding the ~8K worker across receiver creation starved
    // the decoder allocs, which receiver.c then skips -> saved subs fail to "parse".
    instance->worker = NULL;
    instance->fff_data = flipper_format_string_alloc();

    instance->environment = subghz_environment_alloc();
#ifndef PUBLIC_BUILD
    subghz_txrx_provision_default_keeloq_keys(); /* ensure keys exist on SD-less boards */
    subghz_txrx_provision_test_subs(); /* VAG test captures for replay/decode testing */
#endif
    user_keystore_loaded =
        subghz_environment_load_keystore(instance->environment, SUBGHZ_KEYSTORE_DIR_USER_NAME);
    if(!user_keystore_loaded) {
        system_keystore_loaded =
            subghz_environment_load_keystore(instance->environment, SUBGHZ_KEYSTORE_DIR_NAME);
    }
    instance->is_database_loaded = system_keystore_loaded || user_keystore_loaded;
    subghz_environment_set_alutech_at_4n_rainbow_table_file_name(
        instance->environment, SUBGHZ_ALUTECH_AT_4N_DIR_NAME);
    subghz_environment_set_nice_flor_s_rainbow_table_file_name(
        instance->environment, SUBGHZ_NICE_FLOR_S_DIR_NAME);
    // NOTE: CAME Atomo rainbow-table decryption is NOT implemented in this port —
    // subghz_environment_set/get_came_atomo_rainbow_table_file_name are stubs (no struct
    // field) and came_atomo.c never consumes a table, so a setter call here would be a
    // no-op. It is intentionally omitted. Implementing Atomo rolling-code decrypt (env
    // field + getter + the cipher in came_atomo.c, mirroring alutech_at_4n) is a separate
    // task; the /ext/subghz/assets/came_atomo keystore is staged for when it is done.
    subghz_environment_set_protocol_registry(
        instance->environment, (void*)subghz_protocol_registry_active());
    instance->receiver = subghz_receiver_alloc_init(instance->environment);
    instance->receiver_single_mode = false; // full decoder set until a saved file narrows it

    // Worker callbacks/context are wired in subghz_txrx_reacquire_rx_buffer (lazy, at rx_start).

    //set default device External
    subghz_devices_init();
    instance->radio_device_type = SubGhzRadioDeviceTypeInternal;
    instance->radio_device_type =
        subghz_txrx_radio_device_set(instance, SubGhzRadioDeviceTypeExternalCC1101);

    // Lazy RX worker: instance->worker stays NULL here (set above) so SubGhz starts with the
    // ~8K internal available for the first non-RX scene the user opens (Saved/Spectrum/TX).
    // subghz_txrx_rx() calls subghz_txrx_reacquire_rx_buffer() to create+wire it on demand.

    return instance;
}

void subghz_txrx_set_car_mode(SubGhzTxRx* instance, uint8_t mode) {
    furi_check(instance);
    if(subghz_protocol_registry_car_mode() == mode) return;
    subghz_protocol_registry_set_car_mode(mode);
    // Rebuild the receiver from the newly selected registry (general vs the single
    // merged automotive set). Caller must have RX stopped.
    subghz_environment_set_protocol_registry(
        instance->environment, (void*)subghz_protocol_registry_active());
    subghz_receiver_free(instance->receiver);
    instance->receiver = subghz_receiver_alloc_init(instance->environment);
    instance->receiver_single_mode = false; // full set rebuilt for the new registry
    instance->decoder_result = NULL; // old slot pointers are gone
    // Worker is lazy; repoint it at the new receiver only if it currently exists (else it
    // is wired at the next rx reacquire).
    if(instance->worker) subghz_worker_set_context(instance->worker, instance->receiver);
}

uint8_t subghz_txrx_get_car_mode(SubGhzTxRx* instance) {
    UNUSED(instance);
    return subghz_protocol_registry_car_mode();
}

void subghz_txrx_free(SubGhzTxRx* instance) {
    furi_assert(instance);

    if(instance->radio_device_type != SubGhzRadioDeviceTypeInternal) {
        subghz_txrx_radio_device_power_off(instance);
        subghz_devices_end(instance->radio_device);
    }

    subghz_devices_deinit();

    if(instance->worker) subghz_worker_free(instance->worker); // lazy: may already be freed
    subghz_receiver_free(instance->receiver);
    subghz_environment_free(instance->environment);
    flipper_format_free(instance->fff_data);
    furi_string_free(instance->preset->name);
    subghz_setting_free(instance->setting);

    free(instance->preset);
    free(instance);
}

bool subghz_txrx_is_database_loaded(SubGhzTxRx* instance) {
    furi_assert(instance);
    return instance->is_database_loaded;
}

void subghz_txrx_set_preset(
    SubGhzTxRx* instance,
    const char* preset_name,
    uint32_t frequency,
    uint8_t* preset_data,
    size_t preset_data_size) {
    furi_assert(instance);
    furi_string_set(instance->preset->name, preset_name);

    SubGhzRadioPreset* preset = instance->preset;
    preset->frequency = frequency;
    preset->data = preset_data;
    preset->data_size = preset_data_size;
}

uint8_t*
    subghz_txrx_set_tx_power(uint8_t* preset_data, size_t preset_data_size, uint8_t tx_power) {
#define PRESET_POWER_OFFSET_FM 8
#define PRESET_POWER_OFFSET_AM 7
#define TX_PATABLE_OFFSET_AM   8
#define TX_PATABLE_COUNT       17

    //I had to skip the +10dBM and -6dBm Values, use only ones AM/FM have in common.
    //Highest Value is 12dBm for AM, 10 for FM. So Menu needs to reflect that.
    const uint8_t tx_pa_table[TX_PATABLE_COUNT] = {
        0,
        0xC0, //12dBm
        0xCD, //7dBm
        0x86, //5dBm
        0x50, //0dBm
        0x26, // -10dBm
        0x1D, // -15dBm
        0x17, //-20dBm
        0x03, //-30dBm
        0xC0, // 10dBm
        0xC8, //7dBm
        0x84, //5dBm
        0x60, //0dBm
        0x34, //-10dBm
        0x1D, //-15dBm
        0x0E, // -20dBm
        0x12, //-30dBm
    };

    //Memory-safety guard: a short custom preset would underflow preset_data_size
    //(size_t) and read out-of-bounds. PRESET_POWER_OFFSET_FM (8) is the larger of
    //the two offsets, so it is the minimum size required for either access below.
    if(preset_data == NULL || preset_data_size < PRESET_POWER_OFFSET_FM) {
        return preset_data;
    }

    //Grab the AM and FM byte now, so we can do proper checks.
    uint8_t fm_byte = preset_data[preset_data_size - PRESET_POWER_OFFSET_FM];
    uint8_t am_byte = preset_data[preset_data_size - PRESET_POWER_OFFSET_AM];

    //Set the TX Power Here in the CC1101 register...

    //If we have both bytes 1st bytes set or none, this isnt a preset we can deal with here.
    if(fm_byte && !am_byte) {
        //Use FM Table
        if(tx_power) {
            preset_data[preset_data_size - PRESET_POWER_OFFSET_FM] =
                tx_pa_table[TX_PATABLE_OFFSET_AM + tx_power];
        } else {
            preset_data[preset_data_size - PRESET_POWER_OFFSET_FM] =
                tx_pa_table[1]; //Max Power 0xC0 10dBm
        }
    } else if(am_byte && !fm_byte) {
        //Use AM Table
        if(tx_power) {
            preset_data[preset_data_size - PRESET_POWER_OFFSET_AM] = tx_pa_table[tx_power];
        } else {
            preset_data[preset_data_size - PRESET_POWER_OFFSET_AM] =
                tx_pa_table[1]; //Max Power 0xC0 12dBm
        }
    }

    //Pass back the preset_so we can call one liners.
    return preset_data;
}

const char* subghz_txrx_get_preset_name(SubGhzTxRx* instance, const char* preset) {
    UNUSED(instance);
    const char* preset_name = "";
    if(!strcmp(preset, "FuriHalSubGhzPresetOok270Async")) {
        preset_name = "AM270";
    } else if(!strcmp(preset, "FuriHalSubGhzPresetOok650Async")) {
        preset_name = "AM650";
    } else if(!strcmp(preset, "FuriHalSubGhzPreset2FSKDev238Async")) {
        preset_name = "FM238";
    } else if(!strcmp(preset, "FuriHalSubGhzPreset2FSKDev12KAsync")) {
        preset_name = "FM12K";
    } else if(!strcmp(preset, "FuriHalSubGhzPreset2FSKDev476Async")) {
        preset_name = "FM476";
    } else if(!strcmp(preset, "FuriHalSubGhzPresetCustom")) {
        preset_name = "CUSTOM";
    } else {
        FURI_LOG_E(TAG, "Unknown preset");
    }
    return preset_name;
}

SubGhzRadioPreset subghz_txrx_get_preset(SubGhzTxRx* instance) {
    furi_assert(instance);
    return *instance->preset;
}

void subghz_txrx_get_frequency_and_modulation(
    SubGhzTxRx* instance,
    FuriString* frequency,
    FuriString* modulation,
    bool long_name) {
    furi_assert(instance);
    SubGhzRadioPreset* preset = instance->preset;
    if(frequency != NULL) {
        furi_string_printf(
            frequency,
            "%03ld.%02ld",
            preset->frequency / 1000000 % 1000,
            preset->frequency / 10000 % 100);
    }
    if(modulation != NULL) {
        if(long_name) {
            furi_string_printf(modulation, "%s", furi_string_get_cstr(preset->name));
        } else {
            furi_string_printf(modulation, "%.2s", furi_string_get_cstr(preset->name));
        }
    }
}

static void subghz_txrx_begin(SubGhzTxRx* instance, uint8_t* preset_data) {
    furi_assert(instance);
    subghz_devices_reset(instance->radio_device);
    subghz_devices_idle(instance->radio_device);
    subghz_devices_load_preset(instance->radio_device, FuriHalSubGhzPresetCustom, preset_data);
    instance->txrx_state = SubGhzTxRxStateIDLE;
}

static uint32_t subghz_txrx_rx(SubGhzTxRx* instance, uint32_t frequency) {
    furi_assert(instance);
    furi_assert(
        instance->txrx_state != SubGhzTxRxStateRx && instance->txrx_state != SubGhzTxRxStateSleep);

    // Live RX needs EVERY decoder. If a prior saved-file view left the receiver in
    // single-decoder mode, rebuild the full set now (worker is still NULL here — it is
    // (re)created just below — so the swap is safe). No-op if already full.
    subghz_txrx_receiver_switch(instance, NULL);

    // Lazy RX worker: (re)create it for this RX session. Idempotent — a no-op if it already
    // exists (e.g. during hopper cycling, which calls this per hop), so no 8K churn; it only
    // allocates when RX resumes after the worker was freed (in subghz_txrx_stop / at alloc)
    // to return internal RAM to other scenes (Saved browser, Spectrum, TX).
    subghz_txrx_reacquire_rx_buffer(instance);

    subghz_devices_idle(instance->radio_device);

    uint32_t value = subghz_devices_set_frequency(
        instance->radio_device, (uint32_t)((int64_t)frequency + instance->frequency_offset));
    subghz_devices_flush_rx(instance->radio_device);

    subghz_devices_start_async_rx(
        instance->radio_device, subghz_worker_rx_callback, instance->worker);
    subghz_worker_start(instance->worker);
    instance->txrx_state = SubGhzTxRxStateRx;
    return value;
}

static void subghz_txrx_idle(SubGhzTxRx* instance) {
    furi_assert(instance);
    if(instance->txrx_state != SubGhzTxRxStateSleep) {
        subghz_devices_idle(instance->radio_device);
        instance->txrx_state = SubGhzTxRxStateIDLE;
    }
}

static void subghz_txrx_rx_end(SubGhzTxRx* instance) {
    furi_assert(instance);
    furi_assert(instance->txrx_state == SubGhzTxRxStateRx);

    if(instance->worker && subghz_worker_is_running(instance->worker)) {
        subghz_worker_stop(instance->worker);
        subghz_devices_stop_async_rx(instance->radio_device);
    }
    subghz_devices_idle(instance->radio_device);
    instance->txrx_state = SubGhzTxRxStateIDLE;
}

void subghz_txrx_sleep(SubGhzTxRx* instance) {
    furi_assert(instance);
    subghz_devices_sleep(instance->radio_device);
    instance->txrx_state = SubGhzTxRxStateSleep;
}

static bool subghz_txrx_tx(SubGhzTxRx* instance, uint32_t frequency) {
    furi_assert(instance);
    furi_assert(instance->txrx_state != SubGhzTxRxStateSleep);

    subghz_devices_idle(instance->radio_device);
    subghz_devices_set_frequency(
        instance->radio_device, (uint32_t)((int64_t)frequency + instance->frequency_offset));

    bool ret = subghz_devices_set_tx(instance->radio_device);
    if(ret) {
        instance->txrx_state = SubGhzTxRxStateTx;
    }

    return ret;
}

SubGhzTxRxStartTxState subghz_txrx_tx_start(SubGhzTxRx* instance, FlipperFormat* flipper_format) {
    furi_assert(instance);
    furi_assert(flipper_format);

    subghz_txrx_stop(instance);

    SubGhzTxRxStartTxState ret = SubGhzTxRxStartTxStateErrorParserOthers;
    FuriString* temp_str = furi_string_alloc();
    do {
        if(!flipper_format_rewind(flipper_format)) {
            FURI_LOG_E(TAG, "Rewind error");
            break;
        }
        if(!flipper_format_read_string(flipper_format, "Protocol", temp_str)) {
            FURI_LOG_E(TAG, "Missing Protocol");
            break;
        }
        ret = SubGhzTxRxStartTxStateOk;

        SubGhzRadioPreset* preset = instance->preset;
        instance->transmitter =
            subghz_transmitter_alloc_init(instance->environment, furi_string_get_cstr(temp_str));

        if(instance->transmitter) {
            // Reading "Protocol" above advanced the FlipperFormat cursor. Some protocol
            // encoder deserializers (e.g. Ford V0) verify the protocol name from the
            // current position WITHOUT rewinding first, so rewind here — every protocol's
            // deserialize gets a fresh cursor (rewinding ones are unaffected). Without
            // this, non-rewinding encoders fail with "Error in protocol parameters".
            flipper_format_rewind(flipper_format);
            if(subghz_transmitter_deserialize(instance->transmitter, flipper_format) ==
               SubGhzProtocolStatusOk) {
                if(strcmp(furi_string_get_cstr(preset->name), "") != 0) {
                    subghz_txrx_begin(
                        instance,
                        subghz_setting_get_preset_data_by_name(
                            instance->setting, furi_string_get_cstr(preset->name)));
                    if(preset->frequency) {
                        if(!subghz_txrx_tx(instance, preset->frequency)) {
                            FURI_LOG_E(TAG, "Only Rx");
                            ret = SubGhzTxRxStartTxStateErrorOnlyRx;
                        }
                    } else {
                        ret = SubGhzTxRxStartTxStateErrorParserOthers;
                    }

                } else {
                    FURI_LOG_E(
                        TAG, "Unknown name preset \" %s \"", furi_string_get_cstr(preset->name));
                    ret = SubGhzTxRxStartTxStateErrorParserOthers;
                }

                if(ret == SubGhzTxRxStartTxStateOk) {
                    //Start TX
                    subghz_devices_start_async_tx(
                        instance->radio_device, subghz_transmitter_yield, instance->transmitter);
                }
            } else {
                ret = SubGhzTxRxStartTxStateErrorParserOthers;
            }
        } else {
            ret = SubGhzTxRxStartTxStateErrorParserOthers;
        }
        if(ret != SubGhzTxRxStartTxStateOk) {
            //Memory-safety guard: transmitter may be NULL on an early parser
            //error; subghz_transmitter_free() furi_check-aborts on NULL.
            if(instance->transmitter) {
                subghz_transmitter_free(instance->transmitter);
            }
            if(instance->txrx_state != SubGhzTxRxStateIDLE) {
                subghz_txrx_idle(instance);
            }
        }

    } while(false);
    furi_string_free(temp_str);
    return ret;
}

void subghz_txrx_rx_start(SubGhzTxRx* instance) {
    furi_assert(instance);
    subghz_txrx_stop(instance);
    subghz_txrx_begin(
        instance,
        subghz_setting_get_preset_data_by_name(
            subghz_txrx_get_setting(instance), furi_string_get_cstr(instance->preset->name)));
    subghz_txrx_rx(instance, instance->preset->frequency);
}

void subghz_txrx_set_need_save_callback(
    SubGhzTxRx* instance,
    SubGhzTxRxNeedSaveCallback callback,
    void* context) {
    furi_assert(instance);
    instance->need_save_callback = callback;
    instance->need_save_context = context;
}

static void subghz_txrx_tx_stop(SubGhzTxRx* instance) {
    furi_assert(instance);
    furi_assert(instance->txrx_state == SubGhzTxRxStateTx);
    //Stop TX
    subghz_devices_stop_async_tx(instance->radio_device);
    subghz_transmitter_stop(instance->transmitter);
    subghz_transmitter_free(instance->transmitter);

    //if protocol dynamic then we save the last upload
    if(instance->decoder_result->protocol->type == SubGhzProtocolTypeDynamic) {
        if(instance->need_save_callback) {
            instance->need_save_callback(instance->need_save_context);
        }
    }
    subghz_txrx_idle(instance);
}

FlipperFormat* subghz_txrx_get_fff_data(SubGhzTxRx* instance) {
    furi_assert(instance);
    return instance->fff_data;
}

SubGhzSetting* subghz_txrx_get_setting(SubGhzTxRx* instance) {
    furi_assert(instance);
    return instance->setting;
}

void subghz_txrx_stop(SubGhzTxRx* instance) {
    furi_assert(instance);

    switch(instance->txrx_state) {
    case SubGhzTxRxStateTx:
        subghz_txrx_tx_stop(instance);
        break;
    case SubGhzTxRxStateRx:
        subghz_txrx_rx_end(instance);
        break;

    default:
        break;
    }

    // Lazy RX worker: RX has fully stopped (scene exit, and the top of rx_start). Free the
    // worker (~8K internal) so other scenes have room; rx_start's internal rx re-creates it.
    // The hopper cycles RX via internal rx_end/rx and does NOT come through here (no churn).
    subghz_txrx_release_rx_buffer(instance);
}

void subghz_txrx_release_rx_buffer(SubGhzTxRx* instance) {
    furi_assert(instance);
    // Free the ENTIRE RX worker (its ~4K thread stack + ~4K capture buffer = ~8K internal)
    // while it is not needed — e.g. the Saved file browser is open and internal DRAM is
    // exhausted. Only safe while RX is stopped (which it is in the Saved scene). The 8K is
    // enough for both the browser's view/struct allocs AND its worker's 4096 stack.
    if(instance->worker && !subghz_worker_is_running(instance->worker)) {
        subghz_worker_free(instance->worker);
        instance->worker = NULL;
    }
}

void subghz_txrx_reacquire_rx_buffer(SubGhzTxRx* instance) {
    furi_assert(instance);
    if(!instance->worker) {
        instance->worker = subghz_worker_alloc();
        subghz_worker_set_overrun_callback(
            instance->worker, (SubGhzWorkerOverrunCallback)subghz_receiver_reset);
        subghz_worker_set_pair_callback(
            instance->worker, (SubGhzWorkerPairCallback)subghz_receiver_decode);
        subghz_worker_set_context(instance->worker, instance->receiver);
    }
}

void subghz_txrx_hopper_update(SubGhzTxRx* instance, float stay_threshold) {
    furi_assert(instance);

    switch(instance->hopper_state) {
    case SubGhzHopperStateOFF:
    case SubGhzHopperStatePause:
        return;
    case SubGhzHopperStateRSSITimeOut:
        if(instance->hopper_timeout != 0) {
            instance->hopper_timeout--;
            return;
        }
        break;
    default:
        break;
    }
    //    Init value isn't using
    //    float rssi = -127.0f;
    if(instance->hopper_state != SubGhzHopperStateRSSITimeOut) {
        // See RSSI Calculation timings in CC1101 17.3 RSSI
        float rssi = subghz_devices_get_rssi(instance->radio_device);

        // Stay if RSSI is high enough
        if(rssi > stay_threshold) {
            instance->hopper_timeout = 10;
            instance->hopper_state = SubGhzHopperStateRSSITimeOut;
            return;
        }
    } else {
        instance->hopper_state = SubGhzHopperStateRunning;
    }
    // Select next frequency
    if(instance->hopper_idx_frequency <
       subghz_setting_get_hopper_frequency_count(instance->setting) - 1) {
        instance->hopper_idx_frequency++;
    } else {
        instance->hopper_idx_frequency = 0;
    }

    if(instance->txrx_state == SubGhzTxRxStateRx) {
        subghz_txrx_rx_end(instance);
    }
    if(instance->txrx_state == SubGhzTxRxStateIDLE) {
        subghz_receiver_reset(instance->receiver);
        instance->preset->frequency =
            subghz_setting_get_hopper_frequency(instance->setting, instance->hopper_idx_frequency);
        subghz_txrx_rx(instance, instance->preset->frequency);
    }
}

SubGhzHopperState subghz_txrx_hopper_get_state(SubGhzTxRx* instance) {
    furi_assert(instance);
    return instance->hopper_state;
}

void subghz_txrx_hopper_set_state(SubGhzTxRx* instance, SubGhzHopperState state) {
    furi_assert(instance);
    instance->hopper_state = state;
}

void subghz_txrx_hopper_unpause(SubGhzTxRx* instance) {
    furi_assert(instance);
    if(instance->hopper_state == SubGhzHopperStatePause) {
        instance->hopper_state = SubGhzHopperStateRunning;
    }
}

void subghz_txrx_hopper_pause(SubGhzTxRx* instance) {
    furi_assert(instance);
    if(instance->hopper_state == SubGhzHopperStateRunning) {
        instance->hopper_state = SubGhzHopperStatePause;
    }
}

// Cross-firmware protocol-name aliases. Other Flipper firmwares label some protocols
// differently than this fork (combined/renamed decoders). When a .sub's Protocol
// name isn't registered here, map a known alternate to this fork's name and retry,
// so external captures LOAD instead of failing "Protocol not found". Extend this
// table for any new naming mismatch. NOTE: this only bridges NAME differences -- a
// genuinely different on-air FORMAT still needs its own decoder (e.g. the ProtoPirate
// 112-bit "Fiat V2" is a real registered decoder, not an alias to "Fiat Multi").
static const char* subghz_txrx_alias_protocol(const char* name) {
    static const struct {
        const char* from;
        const char* to;
    } aliases[] = {
        {"iDo", "iDo 117/111"},
        {"iDo 117", "iDo 117/111"},
        {"iDo 111", "iDo 117/111"},
        {"Telcoma", "Telcoma/Cardin EDGE"},
        {"Cardin", "Telcoma/Cardin EDGE"},
        {"Telcoma EDGE", "Telcoma/Cardin EDGE"},
        {"Cardin EDGE", "Telcoma/Cardin EDGE"},
    };
    for(size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        if(strcmp(name, aliases[i].from) == 0) return aliases[i].to;
    }
    return NULL;
}

// Swap `receiver` to hold ONLY `name` (RAM-lean, for a known-protocol saved file), or
// back to the full decoder set (name == NULL). Only safe while no RX worker is live
// (the worker task reads the receiver); a no-op otherwise. Clears decoder_result because
// its slot pointers belong to the receiver being freed.
static void subghz_txrx_receiver_switch(SubGhzTxRx* instance, const char* name) {
    if(instance->worker) return; // never swap the receiver out from under the RX worker
    bool want_single = (name != NULL);
    if(want_single == instance->receiver_single_mode && !want_single) return; // already full
    if(instance->receiver) subghz_receiver_free(instance->receiver);
    instance->decoder_result = NULL;
    instance->receiver = want_single ?
                             subghz_receiver_alloc_init_single(instance->environment, name) :
                             subghz_receiver_alloc_init(instance->environment);
    instance->receiver_single_mode = want_single;
}

bool subghz_txrx_load_decoder_by_name_protocol(SubGhzTxRx* instance, const char* name_protocol) {
    furi_assert(instance);
    furi_assert(name_protocol);
    bool res = false;
    // Re-parsing a known-protocol saved file needs exactly this one decoder; drop the
    // full ~113-decoder set to a single decoder to save internal RAM (no-op during RX).
    subghz_txrx_receiver_switch(instance, name_protocol);
    instance->decoder_result =
        subghz_receiver_search_decoder_base_by_name(instance->receiver, name_protocol);
    if(!instance->decoder_result) {
        // Not found by its stored name -> try a cross-firmware alias before giving up.
        const char* alias = subghz_txrx_alias_protocol(name_protocol);
        if(alias) {
            instance->decoder_result =
                subghz_receiver_search_decoder_base_by_name(instance->receiver, alias);
        }
    }
    if(instance->decoder_result) {
        res = true;
    }
    return res;
}

SubGhzProtocolDecoderBase* subghz_txrx_get_decoder(SubGhzTxRx* instance) {
    furi_assert(instance);
    return instance->decoder_result;
}

bool subghz_txrx_protocol_is_serializable(SubGhzTxRx* instance) {
    furi_assert(instance);
    return (instance->decoder_result->protocol->flag & SubGhzProtocolFlag_Save) ==
           SubGhzProtocolFlag_Save;
}

bool subghz_txrx_protocol_is_transmittable(SubGhzTxRx* instance, bool check_type) {
    furi_assert(instance);
    const SubGhzProtocol* protocol = instance->decoder_result->protocol;
    if(check_type) {
        return ((protocol->flag & SubGhzProtocolFlag_Send) == SubGhzProtocolFlag_Send) &&
               protocol->encoder->deserialize && protocol->type == SubGhzProtocolTypeStatic;
    }
    return ((protocol->flag & SubGhzProtocolFlag_Send) == SubGhzProtocolFlag_Send) &&
           protocol->encoder->deserialize;
}

void subghz_txrx_receiver_set_filter(SubGhzTxRx* instance, SubGhzProtocolFlag filter) {
    furi_assert(instance);
    subghz_receiver_set_filter(instance->receiver, filter);
}

void subghz_txrx_set_rx_callback(
    SubGhzTxRx* instance,
    SubGhzReceiverCallback callback,
    void* context) {
    subghz_receiver_set_rx_callback(instance->receiver, callback, context);
}

void subghz_txrx_set_raw_file_encoder_worker_callback_end(
    SubGhzTxRx* instance,
    SubGhzProtocolEncoderRAWCallbackEnd callback,
    void* context) {
    subghz_protocol_raw_file_encoder_worker_set_callback_end(
        (SubGhzProtocolEncoderRAW*)subghz_transmitter_get_protocol_instance(instance->transmitter),
        callback,
        context);
}

bool subghz_txrx_radio_device_is_external_connected(SubGhzTxRx* instance, const char* name) {
    furi_assert(instance);

    bool is_connect = false;
    bool is_otg_enabled = furi_hal_power_is_otg_enabled();

    if(!is_otg_enabled) {
        subghz_txrx_radio_device_power_on(instance);
    }

    const SubGhzDevice* device = subghz_devices_get_by_name(name);
    if(device) {
        is_connect = subghz_devices_is_connect(device);
    }

    if(!is_otg_enabled) {
        subghz_txrx_radio_device_power_off(instance);
    }
    return is_connect;
}

SubGhzRadioDeviceType
    subghz_txrx_radio_device_set(SubGhzTxRx* instance, SubGhzRadioDeviceType radio_device_type) {
    furi_assert(instance);

    if(radio_device_type == SubGhzRadioDeviceTypeExternalCC1101 &&
       subghz_txrx_radio_device_is_external_connected(instance, SUBGHZ_DEVICE_CC1101_EXT_NAME)) {
        subghz_txrx_radio_device_power_on(instance);
        instance->radio_device = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_EXT_NAME);
        subghz_devices_begin(instance->radio_device);
        instance->radio_device_type = SubGhzRadioDeviceTypeExternalCC1101;
    } else {
        subghz_txrx_radio_device_power_off(instance);
        if(instance->radio_device_type != SubGhzRadioDeviceTypeInternal) {
            subghz_devices_end(instance->radio_device);
        }
        instance->radio_device = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
        instance->radio_device_type = SubGhzRadioDeviceTypeInternal;
    }

    return instance->radio_device_type;
}

SubGhzRadioDeviceType subghz_txrx_radio_device_get(SubGhzTxRx* instance) {
    furi_assert(instance);
    return instance->radio_device_type;
}

float subghz_txrx_radio_device_get_rssi(SubGhzTxRx* instance) {
    furi_assert(instance);
    return subghz_devices_get_rssi(instance->radio_device);
}

const char* subghz_txrx_radio_device_get_name(SubGhzTxRx* instance) {
    furi_assert(instance);
    return subghz_devices_get_name(instance->radio_device);
}

bool subghz_txrx_radio_device_is_frequency_valid(SubGhzTxRx* instance, uint32_t frequency) {
    furi_assert(instance);
    return subghz_devices_is_frequency_valid(instance->radio_device, frequency);
}

bool subghz_txrx_radio_device_is_tx_allowed(SubGhzTxRx* instance, uint32_t frequency) {
    // TODO: Remake this function to check if the frequency is allowed on specific module - for modules not based on CC1101
    furi_assert(instance);
    UNUSED(frequency);
    /*
    furi_assert(instance->txrx_state != SubGhzTxRxStateSleep);

    subghz_devices_idle(instance->radio_device);
    subghz_devices_set_frequency(instance->radio_device, frequency);

    bool ret = subghz_devices_set_tx(instance->radio_device);
    subghz_devices_idle(instance->radio_device);

    return ret;
    */
    return true;
}

void subghz_txrx_set_debug_pin_state(SubGhzTxRx* instance, bool state) {
    furi_assert(instance);
    instance->debug_pin_state = state;
}

bool subghz_txrx_get_debug_pin_state(SubGhzTxRx* instance) {
    furi_assert(instance);
    return instance->debug_pin_state;
}

void subghz_txrx_set_frequency_offset(SubGhzTxRx* instance, int32_t offset_hz) {
    furi_assert(instance);
    instance->frequency_offset = offset_hz;
}

int32_t subghz_txrx_get_frequency_offset(SubGhzTxRx* instance) {
    furi_assert(instance);
    return instance->frequency_offset;
}

void subghz_txrx_reset_dynamic_and_custom_btns(SubGhzTxRx* instance) {
    furi_assert(instance);
    subghz_environment_reset_keeloq(instance->environment);

    faac_slh_reset_prog_mode();

    subghz_custom_btns_reset();
}

SubGhzReceiver* subghz_txrx_get_receiver(SubGhzTxRx* instance) {
    furi_assert(instance);
    return instance->receiver;
}

void subghz_txrx_set_default_preset(SubGhzTxRx* instance, uint32_t frequency) {
    furi_assert(instance);

    const char* default_modulation = "AM650";
    if(frequency == 0) {
        frequency = subghz_setting_get_default_frequency(subghz_txrx_get_setting(instance));
    }
    subghz_txrx_set_preset(instance, default_modulation, frequency, NULL, 0);
}

const char* subghz_txrx_set_preset_internal(
    SubGhzTxRx* instance,
    uint32_t frequency,
    uint8_t index,
    uint8_t tx_power) {
    furi_assert(instance);

    //Grab the prset name.
    SubGhzSetting* setting = subghz_txrx_get_setting(instance);
    const char* preset_name = subghz_setting_get_preset_name(setting, index);
    subghz_setting_set_default_frequency(setting, frequency);

    //Get the preset data now so we can set TX power.
    uint8_t* preset_data = subghz_setting_get_preset_data(setting, index);
    size_t preset_data_size = subghz_setting_get_preset_data_size(setting, index);

    //Edit TX power, if necessary.
    subghz_txrx_set_tx_power(preset_data, preset_data_size, tx_power);

    //Set the Updated Preset.
    subghz_txrx_set_preset(instance, preset_name, frequency, preset_data, preset_data_size);

    return preset_name;
}
