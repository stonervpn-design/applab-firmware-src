#include "nrf24_jam_presets.h"
#include "nrf24_jam_config.h" /* Nrf24JamStrategy values */

#include <stdlib.h>

/* ── Channel lists (channel = frequency in MHz minus 2400) ──────────── */

/* WiFi: concentrated sweep across the band (matches the standalone WS app, which
 * jams measurably harder — fewer channels = each revisited faster = harder hit).
 * Turbo noise-flood needs revisit rate more than dense coverage. */
static const uint8_t CH_WIFI[] = {1,  4,  7,  10, 13, 16, 19, 22, 26, 29, 32, 35,
                                  38, 41, 44, 47, 51, 54, 57, 60, 63, 66, 69, 72};

/* BLE data channels: even nRF24 ch 2..80 cover BLE ch 0..36. */
static const uint8_t CH_BLE[] = {2,  4,  6,  8,  10, 12, 14, 16, 18, 20, 22, 24, 26, 28,
                                 30, 32, 34, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56,
                                 58, 60, 62, 64, 66, 68, 70, 72, 74, 76, 78, 80};

/* BLE advertising: ch37=2402→nRF2, ch38=2426→nRF26, ch39=2480→nRF80. */
static const uint8_t CH_BLE_ADV[] = {2, 26, 80};

/* Classic Bluetooth: concentrated FHSS subset (matches the standalone WS app).
 * BT hops 1600×/s across 79 channels and a single radio can only sit on one at a
 * time — sweeping all 79 dilutes each hit to 1/79; a curated 21-channel subset
 * revisits ~4× faster and collides with the hop sequence far harder. */
static const uint8_t CH_BLUETOOTH[] =
    {32, 34, 46, 48, 50, 52, 0, 1, 2, 4, 6, 8, 22, 24, 26, 28, 30, 74, 76, 78, 80};

/* Wireless USB dongles. */
static const uint8_t CH_USB[] = {40, 50, 60};

/* Video streaming (upper ISM band). */
static const uint8_t CH_VIDEO[] = {70, 75, 80};

/* RC controllers (low channels). */
static const uint8_t CH_RC[] = {1, 3, 5, 7};

/* Zigbee: the 16 IEEE 802.15.4 channel CENTERS (ch 11..26) across the FULL band.
 * Zigbee ch k center = 2405 + 5*(k-11) MHz → nRF24 ch 5,10,...,80. One flood per
 * Zigbee channel = whole-band coverage with NO missed channels, at the same
 * 16-channel revisit speed. (The old contiguous {11..26} block only covered
 * 2411-2426 MHz = Zigbee ch ~12-16, missing any target on the upper band such as
 * the common WiFi-avoiding ch 25/26.) */
static const uint8_t CH_ZIGBEE[] = {5,  10, 15, 20, 25, 30, 35, 40,
                                    45, 50, 55, 60, 65, 70, 75, 80};

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* Real 2.4 GHz ISM sweep is nRF24 ch 0..83 (2400..2483.5 MHz). Channels 84..124
 * (2484..2525 MHz) are out of band / empty — sweeping them wastes ~33% of every
 * revisit on dead air. Full/Drone sweep 0..83 only (matches the standalone WS
 * app's i % 84). */
#define NRF24_ISM_CH_COUNT 84

const char* nrf24_jam_preset_name(Nrf24JamPreset preset) {
    switch(preset) {
    case Nrf24JamPresetFull:
        return "Full Spectrum";
    case Nrf24JamPresetWifi:
        return "WiFi 2.4GHz";
    case Nrf24JamPresetBle:
        return "BLE Data";
    case Nrf24JamPresetBleAdv:
        return "BLE Advertising";
    case Nrf24JamPresetBluetooth:
        return "BT Classic";
    case Nrf24JamPresetUsb:
        return "USB Dongles";
    case Nrf24JamPresetVideo:
        return "Video/FPV";
    case Nrf24JamPresetRc:
        return "RC Controllers";
    case Nrf24JamPresetZigbee:
        return "Zigbee";
    case Nrf24JamPresetDrone:
        return "Drone FHSS";
    default:
        return "?";
    }
}

const char* nrf24_jam_preset_short(Nrf24JamPreset preset) {
    switch(preset) {
    case Nrf24JamPresetFull:
        return "Full Spec";
    case Nrf24JamPresetWifi:
        return "WiFi 2.4";
    case Nrf24JamPresetBle:
        return "BLE Data";
    case Nrf24JamPresetBleAdv:
        return "BLE Adv";
    case Nrf24JamPresetBluetooth:
        return "BT Classic";
    case Nrf24JamPresetUsb:
        return "USB Dongle";
    case Nrf24JamPresetVideo:
        return "Video FPV";
    case Nrf24JamPresetRc:
        return "RC Ctrl";
    case Nrf24JamPresetZigbee:
        return "Zigbee";
    case Nrf24JamPresetDrone:
        return "Drone";
    default:
        return "?";
    }
}

const uint8_t* nrf24_jam_preset_channels(Nrf24JamPreset preset, size_t* count) {
    switch(preset) {
    case Nrf24JamPresetWifi:
        *count = ARRAY_COUNT(CH_WIFI);
        return CH_WIFI;
    case Nrf24JamPresetBle:
        *count = ARRAY_COUNT(CH_BLE);
        return CH_BLE;
    case Nrf24JamPresetBleAdv:
        *count = ARRAY_COUNT(CH_BLE_ADV);
        return CH_BLE_ADV;
    case Nrf24JamPresetBluetooth:
        *count = ARRAY_COUNT(CH_BLUETOOTH);
        return CH_BLUETOOTH;
    case Nrf24JamPresetUsb:
        *count = ARRAY_COUNT(CH_USB);
        return CH_USB;
    case Nrf24JamPresetVideo:
        *count = ARRAY_COUNT(CH_VIDEO);
        return CH_VIDEO;
    case Nrf24JamPresetRc:
        *count = ARRAY_COUNT(CH_RC);
        return CH_RC;
    case Nrf24JamPresetZigbee:
        *count = ARRAY_COUNT(CH_ZIGBEE);
        return CH_ZIGBEE;
    case Nrf24JamPresetFull:
    case Nrf24JamPresetDrone:
    default:
        *count = 0;
        return NULL;
    }
}

uint16_t nrf24_jam_preset_default_dwell_us(Nrf24JamPreset preset) {
    switch(preset) {
    case Nrf24JamPresetBleAdv: /* only 3 channels */
    case Nrf24JamPresetRc: /* 4 channels */
        return 1500;
    case Nrf24JamPresetUsb: /* 3 channels */
    case Nrf24JamPresetVideo: /* 3 channels */
        return 1000;
    case Nrf24JamPresetWifi:
    case Nrf24JamPresetBle:
    case Nrf24JamPresetZigbee:
        return 300;
    case Nrf24JamPresetFull:
    case Nrf24JamPresetBluetooth:
    case Nrf24JamPresetDrone:
    default:
        return 200; /* fast sweep across many channels */
    }
}

uint8_t nrf24_jam_preset_default_strategy(Nrf24JamPreset preset) {
    /* Correct noise per protocol (research 2026-07-21):
     *  - OFDM (WiFi) and DSSS (Zigbee) shrug off a single tone — OFDM subcarrier
     *    structure / DSSS despreading processing gain — so they need wideband
     *    NOISE in-channel: Turbo flood.
     *  - Everything GFSK/narrowband (BT, BLE, drone, USB, RC, video) is best hit
     *    by an on-channel TONE (targeted tone ~20 dB more efficient than
     *    barrage). Our CW is now ZERO-DWELL swept, so the AGC can't settle on it
     *    (the old parked-carrier weakness is gone). Full = barrage CW sweep. */
    switch(preset) {
    case Nrf24JamPresetWifi:   /* OFDM -> noise */
    case Nrf24JamPresetZigbee: /* DSSS -> noise */
        return Nrf24StrategyTurbo;
    default:
        return Nrf24StrategyCw; /* GFSK / narrowband / barrage -> swept tone */
    }
}

uint8_t nrf24_jam_preset_default_hop(Nrf24JamPreset preset) {
    /* CW presets walk their handful of channels sequentially (no point shuffling
     * 3 channels); the Turbo sweeps randomise to spread collisions. */
    return nrf24_jam_preset_default_strategy(preset) == Nrf24StrategyCw ? 0 : 1;
}

uint8_t nrf24_jam_preset_next_channel(Nrf24JamPreset preset, uint32_t* hop_index) {
    if(preset == Nrf24JamPresetDrone) {
        /* FHSS target — random hop across the real ISM band (0..83). */
        return (uint8_t)(rand() % NRF24_ISM_CH_COUNT);
    }

    size_t count = 0;
    const uint8_t* list = nrf24_jam_preset_channels(preset, &count);

    if(list == NULL || count == 0) {
        /* Full spectrum — sequential sweep 0..83 (real ISM only). */
        uint8_t ch = (uint8_t)(*hop_index % NRF24_ISM_CH_COUNT);
        (*hop_index)++;
        return ch;
    }

    uint8_t ch = list[*hop_index % count];
    (*hop_index)++;
    return ch;
}

size_t nrf24_jam_preset_fill_channels(Nrf24JamPreset preset, uint8_t* out, size_t cap) {
    if(out == NULL || cap == 0) return 0;

    size_t count = 0;
    const uint8_t* list = nrf24_jam_preset_channels(preset, &count);

    if(list == NULL || count == 0) {
        /* Full / Drone — expand to the real ISM band 0..83 only. */
        size_t n = cap < NRF24_ISM_CH_COUNT ? cap : NRF24_ISM_CH_COUNT;
        for(size_t i = 0; i < n; i++) {
            out[i] = (uint8_t)i;
        }
        return n;
    }

    if(count > cap) count = cap;
    for(size_t i = 0; i < count; i++) {
        out[i] = list[i];
    }
    return count;
}
