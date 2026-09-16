#pragma once

// Vehicle Vuln Scan (PRIVATE): classify a CAPTURED car RF signal into an RF
// security weakness, enrich it with known CVEs from vehicle_vuln_db, collect
// findings for the session, and export a human-readable pentest report to
// /ext/reports/. Capture-then-assess workflow; see subghz_scene_vuln_assess.c.

#include <stdint.h>
#include <stdbool.h>
#include <furi_hal_rtc.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VulnWeaknessFixedReplayable, // static / fixed code -> capture-replay
    VulnWeaknessRollingKnownKey, // rolling but manufacturer key is known -> clonable
    VulnWeaknessRolling, // rolling code (counter) -> RollJam/RollBack exposure
    VulnWeaknessTpmsSpoofable, // unauthenticated TPMS telemetry -> spoofable
    VulnWeaknessUnknown, // not classified
} VulnWeakness;

typedef enum {
    VulnSevInfo,
    VulnSevLow,
    VulnSevMedium,
    VulnSevHigh,
    VulnSevCritical,
} VulnSeverity;

typedef struct {
    char protocol[28];
    char make[24]; // "Unknown" if not inferred from the protocol name
    char preset[28]; // modulation / CC1101 preset name
    uint32_t frequency; // Hz
    uint32_t bits; // decoded bit length (0 = unknown)
    VulnWeakness weakness;
    VulnSeverity severity;
    DateTime when; // capture/assess time (RTC)
} VulnFinding;

// Clear the session findings buffer.
void vehicle_vuln_scan_reset(void);

// Findings collected this session.
uint16_t vehicle_vuln_count(void);
const VulnFinding* vehicle_vuln_get(uint16_t i);

// Classify one captured signal into *out (does NOT add it to the buffer).
// Always fills out (Unknown weakness for unclassifiable signals). preset may be
// NULL. Returns false only on a NULL argument.
bool vehicle_vuln_classify(
    const char* protocol_name,
    uint8_t proto_type, // SubGhzProtocolType
    uint32_t frequency,
    const char* preset,
    uint32_t bits,
    VulnFinding* out);

// Append a finding. Deduplicates on protocol+make+frequency. Returns false if
// the buffer is full or the finding is a duplicate.
bool vehicle_vuln_add(const VulnFinding* f);

const char* vehicle_vuln_weakness_str(VulnWeakness w);
const char* vehicle_vuln_severity_str(VulnSeverity s);
const char* vehicle_vuln_recommendation(VulnWeakness w);

// Write the session findings as a human-readable report to
// /ext/reports/vuln-YYYYMMDD-HHMMSS.txt. Returns the path (static buffer,
// valid until the next export) or NULL if storage is unavailable or there are
// no findings.
const char* vehicle_vuln_report_export(void);

#ifdef __cplusplus
}
#endif
