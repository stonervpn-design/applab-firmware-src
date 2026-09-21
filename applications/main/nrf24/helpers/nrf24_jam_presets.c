#include "nrf24_jam_presets.h"
#include "nrf24_jam_config.h" /* Nrf24JamStrategy values */

#include <stdlib.h>

/* ── Channel lists (channel = frequency in MHz minus 2400) ──────────── */

/* WiFi 2.4GHz non-overlapping primaries ch 1/6/11 = centres 2412/2437/2462 MHz =
 * nRF24 {12,37,62}. WiFi is FIXED-channel (no hopping), so a narrowband CW parked
 * on the AP's 20 MHz primary centre raises its energy-detect/CCA and starves the
 * link far better than smearing 32 sub-channels (each hit only 1/32 of the time,
 * and a narrowband CW leaves the OFDM carriers between combs intact). Use Activity
 * FOCUS or Manual PIN to park the one that is busy; the 3-entry set covers all
 * three primaries when the AP channel is unknown. */
static const uint8_t CH_WIFI[] = {12, 37, 62};

/* BLE DATA channels only: even nRF24 4..78 EXCLUDING 26. The three advertising
 * channels 37/38/39 = nRF24 {2,26,80} are handled by the BLE Advertising preset,
 * so they are left out here (the old list was even 2..80 = the full data+adv set,
 * mislabelled "data"). These 37 map to BLE data channel indices 0..36. */
static const uint8_t CH_BLE[] = {4,  6,  8,  10, 12, 14, 16, 18, 20, 22, 24, 28, 30, 32,
                                 34, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60,
                                 62, 64, 66, 68, 70, 72, 74, 76, 78};

/* BLE advertising: ch37=2402→nRF2, ch38=2426→nRF26, ch39=2480→nRF80. */
static const uint8_t CH_BLE_ADV[] = {2, 26, 80};

/* Classic Bluetooth: all FHSS channels 2..80. */
static const uint8_t CH_BLUETOOTH[] = {
    2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
    21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
    40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58,
    59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80};

/* BT Classic 2 — EXACT replica of the nRF24_jammer FAP (W0rthlessS0ul) BT list.
 * A curated, scattered 21-channel subset spanning the band (not the full 79).
 * Fewer channels → each is revisited ~4x more often (closer to the ~625us BT
 * hop slot), which is why the FAP beats a wide 79-channel smear. Order kept
 * verbatim; jammed sequentially with zero dwell (see scene_jam FAP path). */
static const uint8_t CH_BLUETOOTH2[] = {32, 34, 46, 48, 50, 52, 0,  1,  2,  4, 6,
                                        8,  22, 24, 26, 28, 30, 74, 76, 78, 80};

/* Wireless USB dongles. */
static const uint8_t CH_USB[] = {40, 50, 60};

/* 2.4GHz analog AV senders / wireless cameras / analog FPV: the DIP-switch
 * channel plan is 2414/2432/2450/2468 MHz = nRF24 {14,32,50,68}. The old
 * {70,75,80} (2470/2475/2480) sat at the top of the band and missed every real
 * AV-sender centre. Analog FM video has no FEC/hop, so a parked CW on the active
 * one of these four is a near-total kill. */
static const uint8_t CH_VIDEO[] = {14, 32, 50, 68};

/* RC controllers (low channels). */
static const uint8_t CH_RC[] = {1, 3, 5, 7};

/* Zigbee ch 11..26: 3 nRF sub-channels per Zigbee channel (±1 MHz). */
static const uint8_t CH_ZIGBEE[] = {4,  5,  6,  9,  10, 11, 14, 15, 16, 19, 20, 21,
                                    24, 25, 26, 29, 30, 31, 34, 35, 36, 39, 40, 41,
                                    44, 45, 46, 49, 50, 51, 54, 55, 56, 59, 60, 61,
                                    64, 65, 66, 69, 70, 71, 74, 75, 76, 79, 80, 81};

/* Curated FHSS subsets driven by the zero-dwell fast-hop loop (see scene_jam),
 * the same lever as CH_BLUETOOTH2: a small scattered channel set is revisited
 * far more often than a wide smear. Stored ascending; the worker's build-time
 * Fisher-Yates (random_hop default) scatters each set once so the carrier never
 * slews monotonically up-band. */

/* BLE data-channel connection: every other BLE data channel on a uniform 4 MHz
 * grid. nRF even 4..76 = BLE data ch (RF 1,3,..,37); none is an advertising
 * channel (adv = nRF 2/26/80). 19 ch, max gap 4 MHz, no contiguous camp window. */
static const uint8_t CH_BLE_CONN[] = {4,  8,  12, 16, 20, 24, 28, 32, 36, 40,
                                      44, 48, 52, 56, 60, 64, 68, 72, 76};

/* 2.4GHz RC / drone FHSS control links (FrSky, Spektrum, FlySky, Futaba, generic
 * 2.4G): uniform 4 MHz decimation across the in-band region. nRF 2..82 =
 * 2402..2482 MHz, all <= 83 (in-band). 21 ch, max gap 4 MHz. */
static const uint8_t CH_RC_FHSS[] = {2,  6,  10, 14, 18, 22, 26, 30, 34, 38, 42,
                                     46, 50, 54, 58, 62, 66, 70, 74, 78, 82};

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* Map a per-mode FHSS companion preset to the base preset whose channel set it
 * fast-hops. Returns Nrf24JamPresetCount for every non-companion preset, so a
 * caller can test "is this an FHSS companion?" with `!= Nrf24JamPresetCount`.
 * The companions carry no channel arrays of their own - channels/next/fill all
 * redirect through here to the base, so the two stay in lockstep by construction. */
static Nrf24JamPreset fhss_base_preset(Nrf24JamPreset preset) {
    switch(preset) {
    case Nrf24JamPresetFullFhss:
        return Nrf24JamPresetFull;
    case Nrf24JamPresetWifiFhss:
        return Nrf24JamPresetWifi;
    case Nrf24JamPresetBleFhss:
        return Nrf24JamPresetBle;
    case Nrf24JamPresetBleAdvFhss:
        return Nrf24JamPresetBleAdv;
    case Nrf24JamPresetBluetoothFhss:
        return Nrf24JamPresetBluetooth;
    case Nrf24JamPresetUsbFhss:
        return Nrf24JamPresetUsb;
    case Nrf24JamPresetVideoFhss:
        return Nrf24JamPresetVideo;
    case Nrf24JamPresetRcCtrlFhss:
        return Nrf24JamPresetRc;
    case Nrf24JamPresetZigbeeFhss:
        return Nrf24JamPresetZigbee;
    default:
        return Nrf24JamPresetCount;
    }
}

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
    case Nrf24JamPresetBluetooth2:
        return "BT Classic 2";
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
    case Nrf24JamPresetBleConn:
        return "BLE Connection";
    case Nrf24JamPresetRcFhss:
        return "RC/Drone FHSS";
    case Nrf24JamPresetFullFhss:
        return "Full FHSS";
    case Nrf24JamPresetWifiFhss:
        return "WiFi FHSS";
    case Nrf24JamPresetBleFhss:
        return "BLE Data FHSS";
    case Nrf24JamPresetBleAdvFhss:
        return "BLE Adv FHSS";
    case Nrf24JamPresetBluetoothFhss:
        return "BT Classic FHSS";
    case Nrf24JamPresetUsbFhss:
        return "USB FHSS";
    case Nrf24JamPresetVideoFhss:
        return "Video FHSS";
    case Nrf24JamPresetRcCtrlFhss:
        return "RC Ctrl FHSS";
    case Nrf24JamPresetZigbeeFhss:
        return "Zigbee FHSS";
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
    case Nrf24JamPresetBluetooth2:
        return "BT Cls 2";
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
    case Nrf24JamPresetBleConn:
        return "BLE Conn";
    case Nrf24JamPresetRcFhss:
        return "RC FHSS";
    case Nrf24JamPresetFullFhss:
        return "Full FHSS";
    case Nrf24JamPresetWifiFhss:
        return "WiFi FHSS";
    case Nrf24JamPresetBleFhss:
        return "BLED FHSS";
    case Nrf24JamPresetBleAdvFhss:
        return "BLEa FHSS";
    case Nrf24JamPresetBluetoothFhss:
        return "BT FHSS";
    case Nrf24JamPresetUsbFhss:
        return "USB FHSS";
    case Nrf24JamPresetVideoFhss:
        return "Vid FHSS";
    case Nrf24JamPresetRcCtrlFhss:
        return "RCc FHSS";
    case Nrf24JamPresetZigbeeFhss:
        return "Zig FHSS";
    default:
        return "?";
    }
}

const uint8_t* nrf24_jam_preset_channels(Nrf24JamPreset preset, size_t* count) {
    { /* FHSS companions carry no arrays of their own - use the base's channels. */
        Nrf24JamPreset base = fhss_base_preset(preset);
        if(base != Nrf24JamPresetCount) preset = base;
    }
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
    case Nrf24JamPresetBluetooth2:
        *count = ARRAY_COUNT(CH_BLUETOOTH2);
        return CH_BLUETOOTH2;
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
    case Nrf24JamPresetBleConn:
        *count = ARRAY_COUNT(CH_BLE_CONN);
        return CH_BLE_CONN;
    case Nrf24JamPresetRcFhss:
        *count = ARRAY_COUNT(CH_RC_FHSS);
        return CH_RC_FHSS;
    case Nrf24JamPresetFull:
    case Nrf24JamPresetDrone:
    default:
        *count = 0;
        return NULL;
    }
}

uint16_t nrf24_jam_preset_default_dwell_us(Nrf24JamPreset preset) {
    /* Every FHSS companion runs the zero-dwell fast-hop loop, same as the curated
     * subsets below. */
    if(fhss_base_preset(preset) != Nrf24JamPresetCount) return 20;
    switch(preset) {
    case Nrf24JamPresetBleAdv: /* only 3 channels */
    case Nrf24JamPresetRc: /* 4 channels */
        return 1500;
    case Nrf24JamPresetUsb: /* 3 channels */
    case Nrf24JamPresetVideo: /* 3 channels */
        return 1000;
    case Nrf24JamPresetWifi: /* fixed AP channels (1/6/11) */
    case Nrf24JamPresetZigbee: /* fixed channels */
        return 300;
    case Nrf24JamPresetBluetooth2: /* FAP replica — engine runs it zero-dwell */
    case Nrf24JamPresetBleConn: /* curated BLE-conn subset — same fast-hop loop */
    case Nrf24JamPresetRcFhss: /* curated RC/drone FHSS subset — same fast-hop loop */
        return 20;
    case Nrf24JamPresetBluetooth:
    case Nrf24JamPresetBle:
    case Nrf24JamPresetFull:
    case Nrf24JamPresetDrone:
    default:
        /* Wide/hopping targets (BT-Classic AFH ~1600 hops/s, BLE data hopping,
         * FHSS drones): a very short dwell keeps the PLL below its ~130us re-lock
         * time on purpose → the carrier chirps/smears across the whole band
         * instead of cleanly parking on one channel and leaving the other ~78
         * open. This is Bruce's zero-dwell smear, which blankets a fast hopper
         * far better than clean per-channel parks. (The narrow fixed-channel
         * presets above keep a long dwell — there clean parks concentrate more
         * energy on the few channels that matter.) */
        return 30;
    }
}

uint8_t nrf24_jam_preset_default_strategy(Nrf24JamPreset preset) {
    (void)preset;
    /* Match Bruce's nrf_jammer, which jams EVERY mode with a single continuous
     * carrier (RF24::startConstCarrier) that never switches off — it only
     * retunes across the mode's channels. A parked/sweeping CW tone saturates
     * the victim's AGC far more effectively than discrete garbage packets, so
     * data flooding (Flood/Turbo) is a much weaker jam in practice. Every preset
     * therefore defaults to CW; users can still pick Flood/Turbo/AFH per preset
     * in the config editor. */
    return Nrf24StrategyCw;
}

uint8_t nrf24_jam_preset_default_hop(Nrf24JamPreset preset) {
    /* FHSS companions default to random hop so the worker's build-time shuffle
     * scatters the base channel set (no monotonic up-band slew). */
    if(fhss_base_preset(preset) != Nrf24JamPresetCount) return 1;
    switch(preset) {
    case Nrf24JamPresetBluetooth:
    case Nrf24JamPresetBle:
    case Nrf24JamPresetFull:
    case Nrf24JamPresetDrone:
    case Nrf24JamPresetBleConn:
    case Nrf24JamPresetRcFhss:
        /* Wide smear presets: random hop spreads the chirp uniformly across the
         * band. Sequential-ascending would let the slewing PLL chronically lag
         * and weight the energy toward the low end of the band. The curated FHSS
         * subsets (BLE-Conn, RC-FHSS) share this: their ascending arrays are
         * scattered once by the worker's build-time Fisher-Yates so the zero-dwell
         * loop never walks them monotonically up-band. */
        return 1;
    default:
        /* Narrow fixed-channel presets walk their handful of channels
         * sequentially — nothing to gain from shuffling 3-4 channels. */
        return 0;
    }
}

bool nrf24_jam_preset_is_fast_hop(Nrf24JamPreset preset) {
    /* Every per-mode FHSS companion rides the zero-dwell fast-hop loop. */
    if(fhss_base_preset(preset) != Nrf24JamPresetCount) return true;
    switch(preset) {
    case Nrf24JamPresetBluetooth2:
    case Nrf24JamPresetBleConn:
    case Nrf24JamPresetRcFhss:
        return true;
    default:
        return false;
    }
}

uint8_t nrf24_jam_preset_next_channel(Nrf24JamPreset preset, uint32_t* hop_index) {
    { /* FHSS companions walk their base preset's channel set. */
        Nrf24JamPreset base = fhss_base_preset(preset);
        if(base != Nrf24JamPresetCount) preset = base;
    }
    if(preset == Nrf24JamPresetDrone) {
        /* FHSS target — random hop across the whole band. */
        return (uint8_t)(rand() % 125);
    }

    size_t count = 0;
    const uint8_t* list = nrf24_jam_preset_channels(preset, &count);

    if(list == NULL || count == 0) {
        /* Full spectrum — sequential sweep 0..124. */
        uint8_t ch = (uint8_t)(*hop_index % 125);
        (*hop_index)++;
        return ch;
    }

    uint8_t ch = list[*hop_index % count];
    (*hop_index)++;
    return ch;
}

size_t nrf24_jam_preset_fill_channels(Nrf24JamPreset preset, uint8_t* out, size_t cap) {
    if(out == NULL || cap == 0) return 0;

    { /* FHSS companions fill from their base preset's channel set. */
        Nrf24JamPreset base = fhss_base_preset(preset);
        if(base != Nrf24JamPresetCount) preset = base;
    }

    size_t count = 0;
    const uint8_t* list = nrf24_jam_preset_channels(preset, &count);

    if(list == NULL || count == 0) {
        /* Full / Drone — expand to every channel 0..124. */
        size_t n = cap < 125 ? cap : 125;
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
