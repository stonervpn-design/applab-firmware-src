#pragma once
#include <gui/view.h>
#include "../helpers/subghz_custom_event.h"

typedef struct SubGhzKeeloqFob SubGhzKeeloqFob;
typedef void (*SubGhzKeeloqFobCallback)(SubGhzCustomEvent event, void* context);

// Which fob layout to render: KeeLoq (4 tiles LOCK/UNLOCK/TRUNK/PANIC, codes
// 0x1/0x2/0x4/0x8) or VAG (3 tiles UNLOCK/LOCK/BOOT, codes 0x1/0x2/0x4).
typedef enum {
    SubGhzFobKindKeeloq = 0,
    SubGhzFobKindVag,
} SubGhzFobKind;

SubGhzKeeloqFob* subghz_keeloq_fob_alloc(void);
void subghz_keeloq_fob_free(SubGhzKeeloqFob* instance);
View* subghz_keeloq_fob_get_view(SubGhzKeeloqFob* instance);
void subghz_keeloq_fob_set_callback(
    SubGhzKeeloqFob* instance, SubGhzKeeloqFobCallback callback, void* context);
// kind selects the button table; real_btn = captured button code (marks the
// confirmed-real tile; 0 = none). cnt is up to 24-bit (VAG) so it is uint32_t.
void subghz_keeloq_fob_set_state(
    SubGhzKeeloqFob* instance,
    SubGhzFobKind kind,
    uint8_t real_btn,
    uint32_t cnt,
    uint16_t jump);
uint8_t subghz_keeloq_fob_selected_nibble(SubGhzKeeloqFob* instance);
void subghz_keeloq_fob_set_sent(SubGhzKeeloqFob* instance, uint32_t new_cnt);
