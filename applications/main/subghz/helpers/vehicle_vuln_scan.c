#include "vehicle_vuln_scan.h"
#include "vehicle_vuln_db.h"

#include <furi.h>
#include <storage/storage.h>
#include <lib/subghz/types.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#define VULN_MAX        48
#define VULN_REPORT_DIR "/ext/reports"

static VulnFinding s_findings[VULN_MAX];
static uint16_t s_count = 0;
static char s_last_path[64];

// KeeLoq / rolling protocols whose MANUFACTURER key ships with this firmware
// (KeeLoq Console) -- knowing the key makes the rolling code clonable, so these
// rate High instead of the plain rolling Low. Match is a case-insensitive
// substring on the decoded protocol name. Extend as more master keys are added.
static const char* const KNOWN_KEY_PROTOCOLS[] = {
    NULL,
};

static bool str_contains_ci(const char* hay, const char* needle) {
    if(!hay || !needle || !needle[0]) return false;
    size_t nl = strlen(needle);
    for(const char* p = hay; *p; p++) {
        size_t i = 0;
        while(i < nl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if(i == nl) return true;
    }
    return false;
}

static bool protocol_has_known_key(const char* name) {
    for(size_t i = 0; KNOWN_KEY_PROTOCOLS[i] != NULL; i++) {
        if(str_contains_ci(name, KNOWN_KEY_PROTOCOLS[i])) return true;
    }
    return false;
}

// Best-effort: match the protocol name against the known-make list so a
// make-named decoder (e.g. "Kia", "Hyundai TL", "Ford V0") attributes to a make.
static const char* infer_make(const char* protocol_name) {
    size_t n = vehicle_vuln_make_count();
    for(size_t i = 0; i < n; i++) {
        const char* mk = vehicle_vuln_make(i);
        if(mk && str_contains_ci(protocol_name, mk)) return mk;
    }
    return "Unknown";
}

void vehicle_vuln_scan_reset(void) {
    s_count = 0;
}

uint16_t vehicle_vuln_count(void) {
    return s_count;
}

const VulnFinding* vehicle_vuln_get(uint16_t i) {
    return (i < s_count) ? &s_findings[i] : NULL;
}

const char* vehicle_vuln_weakness_str(VulnWeakness w) {
    switch(w) {
    case VulnWeaknessFixedReplayable:
        return "Fixed code (replayable)";
    case VulnWeaknessRollingKnownKey:
        return "Rolling, mfr key known";
    case VulnWeaknessRolling:
        return "Rolling code";
    case VulnWeaknessTpmsSpoofable:
        return "TPMS telemetry (spoofable)";
    default:
        return "Unclassified";
    }
}

const char* vehicle_vuln_severity_str(VulnSeverity s) {
    switch(s) {
    case VulnSevCritical:
        return "CRITICAL";
    case VulnSevHigh:
        return "HIGH";
    case VulnSevMedium:
        return "MEDIUM";
    case VulnSevLow:
        return "LOW";
    default:
        return "INFO";
    }
}

const char* vehicle_vuln_recommendation(VulnWeakness w) {
    switch(w) {
    case VulnWeaknessFixedReplayable:
        return "Fixed code: a captured frame replays and works. Migrate to a "
               "rolling/authenticated remote.";
    case VulnWeaknessRollingKnownKey:
        return "Rolling code but the manufacturer key is recoverable, so a clone "
               "can be synthesised (see KeeLoq Console). Treat as clonable.";
    case VulnWeaknessRolling:
        return "Rolling code. Residual exposure: RollJam/RollBack capture-and-replay "
               "and counter-desync. Verify the receiver rejects stale codes.";
    case VulnWeaknessTpmsSpoofable:
        return "Tyre-pressure telemetry is unauthenticated and can be spoofed to "
               "trigger warnings or track the vehicle (see TPMS Spoofer).";
    default:
        return "Signal did not classify to a known weakness. Recapture with the "
               "correct band/modulation, or inspect it manually.";
    }
}

bool vehicle_vuln_classify(
    const char* protocol_name,
    uint8_t proto_type,
    uint32_t frequency,
    const char* preset,
    uint32_t bits,
    VulnFinding* out) {
    if(!protocol_name || !out) return false;

    memset(out, 0, sizeof(*out));
    snprintf(out->protocol, sizeof(out->protocol), "%s", protocol_name);
    snprintf(out->preset, sizeof(out->preset), "%s", preset ? preset : "?");
    snprintf(out->make, sizeof(out->make), "%s", infer_make(protocol_name));
    out->frequency = frequency;
    out->bits = bits;
    furi_hal_rtc_get_datetime(&out->when);

    switch(proto_type) {
    case SubGhzProtocolTypeStatic:
        out->weakness = VulnWeaknessFixedReplayable;
        out->severity = VulnSevCritical;
        break;
    case SubGhzProtocolTypeDynamic:
        if(protocol_has_known_key(protocol_name)) {
            out->weakness = VulnWeaknessRollingKnownKey;
            out->severity = VulnSevHigh;
        } else {
            out->weakness = VulnWeaknessRolling;
            out->severity = VulnSevLow;
        }
        break;
    case SubGhzProtocolTypeTpms:
        out->weakness = VulnWeaknessTpmsSpoofable;
        out->severity = VulnSevMedium;
        break;
    default:
        out->weakness = VulnWeaknessUnknown;
        out->severity = VulnSevInfo;
        break;
    }
    return true;
}

bool vehicle_vuln_add(const VulnFinding* f) {
    if(!f || s_count >= VULN_MAX) return false;
    // Dedup on protocol + make + frequency so re-assessing one signal does not
    // pile up identical rows.
    for(uint16_t i = 0; i < s_count; i++) {
        if(s_findings[i].frequency == f->frequency &&
           strcmp(s_findings[i].protocol, f->protocol) == 0 &&
           strcmp(s_findings[i].make, f->make) == 0) {
            return false;
        }
    }
    s_findings[s_count++] = *f;
    return true;
}

// Append every DB CVE whose make list contains `make` to the open report file.
static void write_known_cves(File* file, const char* make) {
    if(!make || strcmp(make, "Unknown") == 0) return;
    char line[256];
    bool any = false;
    for(size_t k = 0; k < VEHICLE_VULN_DB_COUNT; k++) {
        const VehicleVuln* v = &VEHICLE_VULN_DB[k];
        if(v->makes == NULL) continue;
        bool match = false;
        for(size_t m = 0; v->makes[m] != NULL; m++) {
            if(strcmp(v->makes[m], make) == 0) {
                match = true;
                break;
            }
        }
        if(!match) continue;
        if(!any) {
            const char* h = "  Known issues for this make:\n";
            storage_file_write(file, h, strlen(h));
            any = true;
        }
        int n = snprintf(
            line,
            sizeof(line),
            "    - %s (%s, %s-%s): %s\n",
            v->cve ? v->cve : "?",
            v->region ? v->region : "?",
            v->year_start ? v->year_start : "?",
            v->year_end ? v->year_end : "?",
            v->description ? v->description : "");
        if(n > 0) storage_file_write(file, line, (size_t)n);
    }
}

const char* vehicle_vuln_report_export(void) {
    if(s_count == 0) return NULL;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, VULN_REPORT_DIR);

    DateTime now;
    furi_hal_rtc_get_datetime(&now);
    snprintf(
        s_last_path,
        sizeof(s_last_path),
        "%s/vuln-%04u%02u%02u-%02u%02u%02u.txt",
        VULN_REPORT_DIR,
        now.year,
        now.month,
        now.day,
        now.hour,
        now.minute,
        now.second);

    File* file = storage_file_alloc(storage);
    if(!storage_file_open(file, s_last_path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return NULL;
    }

    char buf[320];
    int n = snprintf(
        buf,
        sizeof(buf),
        "RF Vehicle Security Assessment\n"
        "==============================\n"
        "Generated: %04u-%02u-%02u %02u:%02u:%02u\n"
        "Findings:  %u\n"
        "Tool:      Sub-GHz Vuln Scan (authorised testing only)\n\n",
        now.year,
        now.month,
        now.day,
        now.hour,
        now.minute,
        now.second,
        (unsigned)s_count);
    if(n > 0) storage_file_write(file, buf, (size_t)n);

    uint16_t sev_counts[5] = {0};
    for(uint16_t i = 0; i < s_count; i++) {
        const VulnFinding* f = &s_findings[i];
        if(f->severity <= VulnSevCritical) sev_counts[f->severity]++;

        n = snprintf(
            buf,
            sizeof(buf),
            "[%u] %s  --  %s\n"
            "  Make/guess:  %s\n"
            "  Protocol:    %s\n"
            "  RF:          %lu.%02lu MHz  %s  %lu-bit\n"
            "  Captured:    %04u-%02u-%02u %02u:%02u:%02u\n"
            "  Fix:         %s\n",
            (unsigned)(i + 1),
            vehicle_vuln_severity_str(f->severity),
            vehicle_vuln_weakness_str(f->weakness),
            f->make,
            f->protocol,
            (unsigned long)(f->frequency / 1000000UL),
            (unsigned long)((f->frequency % 1000000UL) / 10000UL),
            f->preset,
            (unsigned long)f->bits,
            f->when.year,
            f->when.month,
            f->when.day,
            f->when.hour,
            f->when.minute,
            f->when.second,
            vehicle_vuln_recommendation(f->weakness));
        if(n > 0) storage_file_write(file, buf, (size_t)n);
        write_known_cves(file, f->make);
        storage_file_write(file, "\n", 1);
    }

    n = snprintf(
        buf,
        sizeof(buf),
        "Summary by severity\n"
        "-------------------\n"
        "  CRITICAL: %u\n  HIGH:     %u\n  MEDIUM:   %u\n  LOW:      %u\n  INFO:     %u\n",
        sev_counts[VulnSevCritical],
        sev_counts[VulnSevHigh],
        sev_counts[VulnSevMedium],
        sev_counts[VulnSevLow],
        sev_counts[VulnSevInfo]);
    if(n > 0) storage_file_write(file, buf, (size_t)n);

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return s_last_path;
}
