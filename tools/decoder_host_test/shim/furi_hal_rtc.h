#pragma once
// Host stub for <furi_hal_rtc.h>: some decoders (e.g. dickert_mahs) include it
// but reference no RTC symbol on the decode/feed path this harness exercises.
// datetime + the RTC getter are declared for any that do; the definition of
// furi_hal_rtc_get_datetime lives in host_shim_tpms.c.
#include <datetime/datetime.h>
void furi_hal_rtc_get_datetime(DateTime* datetime);
