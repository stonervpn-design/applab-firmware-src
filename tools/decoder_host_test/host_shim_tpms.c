// TPMS-build-only stub: furi_hal_rtc_get_datetime (used only in tpms_generic
// serialize, never in the decode/feed path this harness exercises).
#include <string.h>
#include <datetime/datetime.h>
void furi_hal_rtc_get_datetime(DateTime* dt) {
    if(dt) memset(dt, 0, sizeof(*dt));
}
