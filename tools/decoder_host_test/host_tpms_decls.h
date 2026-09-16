// TPMS-build-only force-include: gives tpms_generic.c the datetime + RTC decls it
// needs in serialize() (not on the decode path). Kept separate from
// host_hal_decls.h so the car harnesses are untouched.
#pragma once
#include <datetime/datetime.h>
void furi_hal_rtc_get_datetime(DateTime* datetime);
