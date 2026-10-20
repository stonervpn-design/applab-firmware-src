#pragma once
#include <furi.h>

// ESP32 STATIC PORT of ProtoPirate 4.1.
// The stock Flipper build (fbt) injects FAP_VERSION from the manifest's
// fap_version; this port's fam build does not, so define it here. Guarded so a
// real fbt build still wins.
#ifndef FAP_VERSION
#define FAP_VERSION "4.1"
#endif

// **** User Adjustable ****
//#define ENABLE_WELCOME_SCREEN
//#define ENABLE_TIMING_TUNER_SCENE
#define ENABLE_SUB_DECODE_SCENE
// ESP32 static port: TX/emulate is ENABLED. Upstream ships each TX protocol as
// a separate tx_<key>.fal built from ONE template (protopirate_tx_protocol_plugin.c),
// all exporting the same entry-point symbol — not flattenable. This port instead
// defines all 23 TX plugins once, each with a unique protopirate_tx_<key>_ep()
// symbol (protocols/plugins/protopirate_tx_static_plugins.c), dispatched by the
// static loader. NOTE: enabling this REQUIRES dropping PROTOPIRATE_PROTOCOL_RX_ONLY
// from application.fam cdefines, so protocol encoders (PROTOPIRATE_WITH_ENCODER)
// are compiled in alongside the decoders.
#define ENABLE_EMULATE_FEATURE
#define ENABLE_MODELS_DATABASE
#define ENABLE_REMOTE_ANALYZER

// ESP32 static port: upstream builds each tool-scene plugin as a separate .fal
// with these per-plugin cdefines set by its App() in application.fam. This port
// compiles them all into one FAP, so define them globally here (each is used
// ONLY as its own file's compile guard — verified no shared-header usage).
#define PROTOPIRATE_SUB_DECODE_PLUGIN_BUILD
#define PROTOPIRATE_BRUTEFORCE_PLUGIN_BUILD
#define PROTOPIRATE_REMOTE_ANALYZER_PLUGIN_BUILD
#define PROTOPIRATE_CONFIG_PLUGIN_BUILD

// **** DO NOT TOUCH! ****
#if defined(ENABLE_EMULATE_FEATURE) && !defined(PROTOPIRATE_PROTOCOL_RX_ONLY)
#define PROTOPIRATE_WITH_ENCODER 1
#else
#define PROTOPIRATE_WITH_ENCODER 0
#endif

#ifndef PROTOPIRATE_PROTOCOL_TX_ONLY
#define PROTOPIRATE_WITH_DECODER 1
#else
#define PROTOPIRATE_WITH_DECODER 0
#endif

// Assets (keystore + models_db) are packaged with the app via fap_file_assets,
// so resolve them under APP_ASSETS_PATH.
#define EMBEDDED_FALS

// **** Add logs back for debugging and reporting problems ****
#define REMOVE_LOGS

// **** DO NOT TOUCH! ****
#ifdef REMOVE_LOGS

#undef FURI_LOG_E
#undef FURI_LOG_W
#undef FURI_LOG_I
#undef FURI_LOG_D
#undef FURI_LOG_T
#define FURI_LOG_E(tag, format, ...)
#define FURI_LOG_W(tag, format, ...)
#define FURI_LOG_I(tag, format, ...)
#define FURI_LOG_D(tag, format, ...)
#define FURI_LOG_T(tag, format, ...)

#endif // REMOVE_LOGS
