#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* cve;
    const char* year_start; // "ALL" allowed
    const char* year_end; // "ALL" allowed
    const char* const* makes; // NULL-terminated
    const char* const* models; // NULL-terminated
    const char* region;
    const char* description;
    const char* url;
} VehicleVuln;

extern const VehicleVuln VEHICLE_VULN_DB[];
extern const size_t VEHICLE_VULN_DB_COUNT;

// Number of DISTINCT makes across the whole DB (skips "ALL").
size_t vehicle_vuln_make_count(void);
// i-th distinct make, or NULL if i is out of range.
const char* vehicle_vuln_make(size_t i);

#ifdef __cplusplus
}
#endif
