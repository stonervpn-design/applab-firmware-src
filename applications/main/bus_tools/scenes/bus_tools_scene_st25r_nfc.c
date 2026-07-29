#include "../bus_tools_app_i.h"
#include "../bus_tools_nfc_view.h"
#include "furi_hal/st25r3916/st25r3916_nfca.h"
#include <string.h>

/* "Prove it reads": init the ST25R3916 NFC-A poller once on enter, then poll
 * for a card every tick and show the UID. */
static int s_init_status;

void bus_tools_scene_st25r_nfc_on_enter(void* context) {
    furi_assert(context);
    BusToolsApp* app = context;
    s_init_status = (int)st25r3916_nfca_init();
    with_view_model(
        app->nfc_view,
        BusToolsNfcViewModel * m,
        {
            memset(m, 0, sizeof(*m));
            m->init_ok = s_init_status;
            m->last_status = (int)St25rNfcaErrNoCard;
        },
        true);
    view_dispatcher_switch_to_view(app->view_dispatcher, BusToolsViewStNfc);
}

bool bus_tools_scene_st25r_nfc_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    BusToolsApp* app = context;
    if(event.type != SceneManagerEventTypeTick || s_init_status != (int)St25rNfcaOk) return false;

    St25rNfcaTag tag;
    St25rNfcaStatus s = st25r3916_nfca_poll(&tag);
    with_view_model(
        app->nfc_view,
        BusToolsNfcViewModel * m,
        {
            m->last_status = (int)s;
            if(s == St25rNfcaOk) {
                memcpy(m->uid, tag.uid, tag.uid_len);
                m->uid_len = tag.uid_len;
                m->atqa = tag.atqa;
                m->sak = tag.sak;
                m->reads++;
            }
        },
        true);
    return false;
}

void bus_tools_scene_st25r_nfc_on_exit(void* context) {
    UNUSED(context);
    st25r3916_nfca_deinit();
}
