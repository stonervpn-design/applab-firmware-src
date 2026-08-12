#pragma once
#include <gui/view.h>
#include "../helpers/subghz_custom_event.h"
#include "../helpers/subghz_fob_buttons.h"

typedef struct SubGhzKeeloqFob SubGhzKeeloqFob;
typedef void (*SubGhzKeeloqFobCallback)(SubGhzCustomEvent event, void* context);

// Which fob layout to render: KeeLoq (4 tiles LOCK/UNLOCK/TRUNK/PANIC, codes
// 0x1/0x2/0x4/0x8) or VAG (3 tiles UNLOCK/LOCK/BOOT, codes 0x1/0x2/0x4).
// These are the two legacy convenience layouts; internally both resolve to a
// SubGhzFobButton[] table via the generic set_table path below, so the render
// and navigation code is a single table-driven path shared with every other
// supported car protocol.
typedef enum {
    SubGhzFobKindKeeloq = 0,
    SubGhzFobKindVag,
} SubGhzFobKind;

SubGhzKeeloqFob* subghz_keeloq_fob_alloc(void);
void subghz_keeloq_fob_free(SubGhzKeeloqFob* instance);
View* subghz_keeloq_fob_get_view(SubGhzKeeloqFob* instance);
void subghz_keeloq_fob_set_callback(
    SubGhzKeeloqFob* instance, SubGhzKeeloqFobCallback callback, void* context);

// Generic entry point: render/navigate an arbitrary button table. `buttons` must
// point to a static-lifetime SubGhzFobButton[] (e.g. one returned by
// subghz_fob_buttons()) — only the pointer + count are stored, no copy is made,
// so nothing is bounded-heap-allocated on the no-PSRAM boards. `n` is clamped to
// SUBGHZ_KEELOQ_FOB_MAX_TILES. real_btn = captured button code (marks the
// confirmed-real tile); pass SUBGHZ_KEELOQ_FOB_NO_BTN when no button was
// captured so a table whose first tile is code 0x0 (Subaru Lock, Honda Panic)
// is not falsely flagged. cnt is up to 24-bit so it is uint32_t. The title
// defaults to "Fob Emulator" and the counter renders 16-bit (%04lX); use
// set_title to override the title.
#define SUBGHZ_KEELOQ_FOB_MAX_TILES 8
// Sentinel real_btn meaning "no captured button" — never a valid code in any
// provider table (max is 0x60), so it marks no tile.
#define SUBGHZ_KEELOQ_FOB_NO_BTN    0xFF
void subghz_keeloq_fob_set_table(
    SubGhzKeeloqFob* instance,
    const SubGhzFobButton* buttons,
    size_t n,
    uint8_t real_btn,
    uint32_t cnt,
    uint16_t jump);

// Override the title shown at the top of the view (static-lifetime string, e.g.
// a protocol name). Not needed for the VAG/KeeLoq legacy layouts, which set their
// own titles via set_state.
void subghz_keeloq_fob_set_title(SubGhzKeeloqFob* instance, const char* title);

// Legacy convenience wrappers over set_table: kind selects the VAG or KeeLoq
// button table (looked up from the generic provider) plus its title, counter
// width and desync note, reproducing exactly the previous VAG/KeeLoq screens.
// real_btn = captured button code (marks the confirmed-real tile; 0 = none).
void subghz_keeloq_fob_set_state(
    SubGhzKeeloqFob* instance,
    SubGhzFobKind kind,
    uint8_t real_btn,
    uint32_t cnt,
    uint16_t jump);
uint8_t subghz_keeloq_fob_selected_nibble(SubGhzKeeloqFob* instance);
void subghz_keeloq_fob_set_sent(SubGhzKeeloqFob* instance, uint32_t new_cnt);
