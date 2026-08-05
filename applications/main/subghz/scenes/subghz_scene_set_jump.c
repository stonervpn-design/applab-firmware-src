#include "../subghz_i.h"
#include "../helpers/subghz_txrx_create_protocol_key.h"

#include <machine/endian.h>

#define TAG "SubGhzSetJump"

void subghz_scene_set_jump_byte_input_callback(void* context) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, SubGhzCustomEventByteInputDone);
}

void subghz_scene_set_jump_on_enter(void* context) {
    SubGhz* subghz = context;
    // Desync-safe default: no lead (0). Dial in a lead here only if you want the clone to
    // tolerate the real fob having been used since capture — it costs the original a few
    // catch-up presses. 0 = the original never gets stranded.
    subghz->keeloq_clone_jump = __bswap16(subghz->keeloq_clone_jump); // show big-endian

    ByteInput* byte_input = subghz->byte_input;
    byte_input_set_header_text(byte_input, "Enter JUMP (hex)");
    byte_input_set_result_callback(
        byte_input,
        subghz_scene_set_jump_byte_input_callback,
        NULL,
        subghz,
        (uint8_t*)&subghz->keeloq_clone_jump,
        sizeof(subghz->keeloq_clone_jump));
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdByteInput);
}

bool subghz_scene_set_jump_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    bool consumed = false;
    if(event.type == SceneManagerEventTypeCustom &&
       event.event == SubGhzCustomEventByteInputDone) {
        subghz->keeloq_clone_jump = __bswap16(subghz->keeloq_clone_jump);

        bool ok = subghz_txrx_gen_keeloq_clone(
            subghz->txrx,
            subghz->gen_info->mod,
            subghz->gen_info->freq,
            subghz->gen_info->keeloq.serial,
            subghz->gen_info->keeloq.btn,
            subghz->gen_info->keeloq.cnt,
            subghz->keeloq_clone_jump,
            subghz->gen_info->keeloq.manuf);

        consumed = true;
        subghz->keeloq_clone_flow = false;
        if(!ok) {
            furi_string_set(
                subghz->error_str, "Function requires\nan SD card with\nfresh databases.");
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneShowError);
        } else {
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneTransmitter);
        }
    }
    return consumed;
}

void subghz_scene_set_jump_on_exit(void* context) {
    SubGhz* subghz = context;
    byte_input_set_result_callback(subghz->byte_input, NULL, NULL, NULL, NULL, 0);
    byte_input_set_header_text(subghz->byte_input, "");
    subghz->keeloq_clone_flow = false; // clear on any exit incl. Back
}
