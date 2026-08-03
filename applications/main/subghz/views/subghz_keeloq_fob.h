#pragma once
#include <gui/view.h>
#include "../helpers/subghz_custom_event.h"

typedef struct SubGhzKeeloqFob SubGhzKeeloqFob;
typedef void (*SubGhzKeeloqFobCallback)(SubGhzCustomEvent event, void* context);

SubGhzKeeloqFob* subghz_keeloq_fob_alloc(void);
void subghz_keeloq_fob_free(SubGhzKeeloqFob* instance);
View* subghz_keeloq_fob_get_view(SubGhzKeeloqFob* instance);
void subghz_keeloq_fob_set_callback(
    SubGhzKeeloqFob* instance, SubGhzKeeloqFobCallback callback, void* context);
// real_btn = captured button nibble (marks the confirmed-real tile; 0 = none)
void subghz_keeloq_fob_set_state(
    SubGhzKeeloqFob* instance, uint8_t real_btn, uint16_t cnt, uint16_t jump);
uint8_t subghz_keeloq_fob_selected_nibble(SubGhzKeeloqFob* instance);
void subghz_keeloq_fob_set_sent(SubGhzKeeloqFob* instance, uint16_t new_cnt);
