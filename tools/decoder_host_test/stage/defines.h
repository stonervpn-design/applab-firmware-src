#pragma once

// ESP32 port: the stock Flipper build (fbt) injects FAP_VERSION from the
// manifest's fap_version. This port's fam build doesn't, so define it here
// (matches application.fam fap_version="3.2"). Guarded so a real fbt build wins.
#ifndef FAP_VERSION
#define FAP_VERSION "3.2"
#endif

// #define ENABLE_TIMING_TUNER_SCENE
#define ENABLE_SUB_DECODE_SCENE
// ESP32 static port: TX/emulate ENABLED. Upstream ships each TX protocol as a
// per-protocol .fal plugin, but every protocol's encoder is already compiled
// into this single image (behind PROTOPIRATE_WITH_ENCODER), so instead of a
// runtime plugin loader the 22 TX-capable protocols are wired statically in
// helpers/protopirate_protocol_plugin_host.c (protopirate_static_tx_plugin) and
// the emulate scene is dispatched via protopirate_emulate_plugin_ep() directly.
// Requires application.fam to NOT define PROTOPIRATE_PROTOCOL_RX_ONLY.
#define ENABLE_EMULATE_FEATURE

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

#define REMOVE_LOGS

#ifdef REMOVE_LOGS
// Undefine existing macros
#undef FURI_LOG_E
#undef FURI_LOG_W
#undef FURI_LOG_I
#undef FURI_LOG_D
#undef FURI_LOG_T
// Define empty macros
#define FURI_LOG_E(tag, format, ...)
#define FURI_LOG_W(tag, format, ...)
#define FURI_LOG_I(tag, format, ...)
#define FURI_LOG_D(tag, format, ...)
#define FURI_LOG_T(tag, format, ...)

#endif // REMOVE_LOGS
