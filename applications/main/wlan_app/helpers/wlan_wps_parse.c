#include "wlan_wps_parse.h"

#ifndef PUBLIC_BUILD

#include <string.h>

/* All position/length bookkeeping below is done in uint32_t even though
 * the public interface is uint16_t: ies_len tops out at 65535, and every
 * bound check below (pos+2+len, ap+4+alen) is computed in the wider type
 * so a position near the top of the uint16_t range can never wrap before
 * it is compared against ies_len. Every read of ies[i] is only reachable
 * after the corresponding bound check has passed. */

bool wlan_wps_parse_ies(const uint8_t* ies, uint16_t ies_len, WlanWpsInfo* out) {
    if(!out) return false;
    memset(out, 0, sizeof(*out));
    if(!ies) return false;

    uint32_t pos = 0;
    while(pos + 2 <= (uint32_t)ies_len) {
        uint8_t id = ies[pos];
        uint8_t len = ies[pos + 1];
        if(pos + 2 + (uint32_t)len > (uint32_t)ies_len) break; /* IE claims past buffer end */
        uint32_t ie_val = pos + 2;

        if(id == WLAN_WPS_IE_ID && len >= 4 && ies[ie_val + 0] == WLAN_WPS_OUI_0 &&
           ies[ie_val + 1] == WLAN_WPS_OUI_1 && ies[ie_val + 2] == WLAN_WPS_OUI_2 &&
           ies[ie_val + 3] == WLAN_WPS_OUI_TYPE) {
            out->present = true;

            uint32_t ap = ie_val + 4;
            uint32_t end = ie_val + len; /* safe: pos+2+len already validated <= ies_len */

            while(ap + 4 <= end) {
                uint16_t atype = ((uint16_t)ies[ap] << 8) | ies[ap + 1];
                uint16_t alen = ((uint16_t)ies[ap + 2] << 8) | ies[ap + 3];
                if(ap + 4 + (uint32_t)alen > end) break; /* attribute claims past IE end */
                const uint8_t* val = &ies[ap + 4];

                switch(atype) {
                case WLAN_WPS_ATTR_VERSION:
                    if(alen >= 1) out->version = val[0];
                    break;
                case WLAN_WPS_ATTR_WPS_STATE:
                    if(alen >= 1) out->wps_state = val[0];
                    break;
                case WLAN_WPS_ATTR_AP_SETUP_LOCKED:
                    if(alen >= 1) out->locked = (val[0] == 1);
                    break;
                case WLAN_WPS_ATTR_SELECTED_REGISTRAR:
                    if(alen >= 1) out->selected_registrar = (val[0] == 1);
                    break;
                case WLAN_WPS_ATTR_CONFIG_METHODS:
                    if(alen >= 2) out->config_methods = ((uint16_t)val[0] << 8) | val[1];
                    break;
                default:
                    break;
                }

                ap += 4 + (uint32_t)alen;
            }
        }

        pos += 2 + (uint32_t)len;
    }

    return out->present;
}

#endif /* PUBLIC_BUILD */
