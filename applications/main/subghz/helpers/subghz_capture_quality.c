#include "subghz_capture_quality.h"

#define SQ_WEAK_MARGIN_DB 6.0f // above trigger but within this margin -> weak

SubGhzCaptureQuality subghz_capture_quality_eval(
    float rssi_dbm,
    float threshold_dbm,
    bool decoded_recently) {
    SubGhzCaptureQuality q = {SubGhzQualityIdle, ""};

    if(rssi_dbm < threshold_dbm) {
        return q; // nothing above the trigger — idle
    }
    if(decoded_recently) {
        q.level = SubGhzQualityGood;
        return q;
    }
    if((rssi_dbm - threshold_dbm) < SQ_WEAK_MARGIN_DB) {
        q.level = SubGhzQualityWeak;
        q.hint = "Weak - move closer";
    } else {
        q.level = SubGhzQualityNoDecode;
        q.hint = "Signal, no decode - check preset";
    }
    return q;
}
