# KeeLoq Fob Emulator + Auto-Clone Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** An on-screen car-fob view (Lock/Unlock/Trunk/Panic tiles) that sends each button through the existing no-desync clone TX, and a Clone action that auto-builds the clone (jump 100) and opens the fob directly.

**Architecture:** A new self-contained GUI `View` (modeled on `views/subghz_jammer.c`) renders a 2×2 tile grid and reports selection/send via a callback. A new scene owns the view, builds the clone from `gen_info` on enter, and on each send rewrites the `Btn` field in the in-memory clone `fff_data` and calls `subghz_tx_start` — reusing the clone forward-pair + counter engine unchanged. The two Clone actions route to the fob scene instead of SetJump.

**Tech Stack:** C (Flipper ESP32-S3 port), Flipper `gui/view.h` + `canvas`/`elements`, FlipperFormat `.sub`, host tests via WSL gcc.

## Global Constraints

- **Boards:** apply to BOTH `src_m5stick_s3` and `src_cardputer_adv`; build ONE env at a time. All new scene/view code + the Clone route changes guarded by `#ifndef PUBLIC_BUILD`.
- **Reuse the clone engine unchanged.** Sending = `flipper_format_update_uint32(fff_data,"Btn",&nibble,1)` then `subghz_tx_start(subghz, fff_data)`. `Cnt`/`CloneSynced` persist across button changes → counter keeps advancing; never desyncs.
- **Button nibbles:** Lock=`0x1`, Unlock=`0x2`, Trunk=`0x4`, Panic=`0x8`. Tile grid index: 0=Lock(TL) 1=Unlock(TR) 2=Trunk(BL) 3=Panic(BR).
- **Input:** Cardputer = arrow keys move highlight, OK sends. M5Stick S3 = tap cycles highlight, OK sends, Back exits (exact M5Stick gesture tuned on-device — its 2-button scheme is finicky; see the on-hardware task).
- **Icons:** canvas-drawn glyphs (no compiled image assets — dodge the no-Pillow snag). Labels are primary; glyphs are accents.
- **Host test command** (Git Bash has no gcc): `wsl.exe -- bash -lc "cd /mnt/c/Users/stone/Downloads/_fzcard_work/NEW_v2/src_m5stick_s3 && gcc -Wall -Wextra -o /tmp/t tests/<test>.c && /tmp/t"`
- **Compile gate:** `cmd //c "C:\\Users\\stone\\Downloads\\_fzcard_work\\NEW_v2\\src_m5stick_s3\\build_ci.bat"` — ends `=== BUILD OK ===`, exit 0. Baseline is green; subghz app auto-globs new `.c` under `applications/main/subghz/**`.
- **Commit isolation:** the tree has large unrelated WIP — `git add` ONLY the feature files per task; verify with `git diff` first.

## File Structure

**Create (both trees):**
- `applications/main/subghz/helpers/subghz_fob_nav.{c,h}` — pure grid-navigation core (hardware-free, host-tested).
- `tests/test_fob_nav.c` — host test.
- `applications/main/subghz/views/subghz_keeloq_fob.{c,h}` — the fob View (model, draw, input, callbacks).
- `applications/main/subghz/scenes/subghz_scene_keeloq_fob.c` — the scene.

**Modify (both trees):**
- `applications/main/subghz/helpers/subghz_types.h` — add `SubGhzViewIdKeeloqFob` to the `SubGhzViewId` enum (line ~85).
- `applications/main/subghz/subghz_i.h` — add `SubGhzKeeloqFob* subghz_keeloq_fob;` (line ~89) + `#include "views/subghz_keeloq_fob.h"`.
- `applications/main/subghz/subghz.c` — eager alloc+add_view (mirror frequency_analyzer `:197-201`) + remove+free (`:373-374`).
- `applications/main/subghz/scenes/subghz_scene_config.h` — `ADD_SCENE(subghz, keeloq_fob, KeeloqFob)`.
- `applications/main/subghz/scenes/subghz_scene_keeloq_action.c` — Clone case → `SubGhzSceneKeeloqFob`.
- `applications/main/subghz/scenes/subghz_scene_rolling_analyzer.c` — Clone handler → `SubGhzSceneKeeloqFob`.
- `applications/main/subghz/helpers/subghz_custom_event.h` — add fob send/back custom events.

---

## Task 1: Pure fob grid-navigation core (host-tested)

**Files:**
- Create: `applications/main/subghz/helpers/subghz_fob_nav.h`, `subghz_fob_nav.c`
- Test: `tests/test_fob_nav.c`

**Interfaces:**
- Produces: `uint8_t subghz_fob_nav_move(uint8_t idx, uint8_t key);` where key ∈ {FOB_LEFT,FOB_RIGHT,FOB_UP,FOB_DOWN,FOB_CYCLE}; returns the new index (0..3) for a 2×2 grid.

- [ ] **Step 1: Write the failing test** — `tests/test_fob_nav.c`:
```c
#include "../applications/main/subghz/helpers/subghz_fob_nav.c"
#include <assert.h>
#include <stdio.h>
int main(void) {
    // grid: 0 1 / 2 3
    assert(subghz_fob_nav_move(0, FOB_RIGHT) == 1);
    assert(subghz_fob_nav_move(1, FOB_RIGHT) == 0); // wrap row
    assert(subghz_fob_nav_move(0, FOB_LEFT)  == 1); // wrap row
    assert(subghz_fob_nav_move(0, FOB_DOWN)  == 2);
    assert(subghz_fob_nav_move(2, FOB_DOWN)  == 0); // wrap col
    assert(subghz_fob_nav_move(3, FOB_UP)    == 1);
    assert(subghz_fob_nav_move(0, FOB_CYCLE) == 1); // cycle order 0->1->2->3->0
    assert(subghz_fob_nav_move(3, FOB_CYCLE) == 0);
    assert(subghz_fob_nav_move(2, FOB_CYCLE) == 3);
    printf("test_fob_nav OK\n");
    return 0;
}
```
- [ ] **Step 2: Run — expect FAIL** (`subghz_fob_nav.c: No such file`). Command: the Global-Constraints WSL host-test line with `tests/test_fob_nav.c`.
- [ ] **Step 3: Header** `subghz_fob_nav.h`:
```c
#pragma once
#include <stdint.h>
// 2x2 fob tile navigation (pure, hardware-free). Grid index: 0=TL 1=TR 2=BL 3=BR.
enum { FOB_LEFT = 0, FOB_RIGHT, FOB_UP, FOB_DOWN, FOB_CYCLE };
uint8_t subghz_fob_nav_move(uint8_t idx, uint8_t key);
```
- [ ] **Step 4: Implementation** `subghz_fob_nav.c`:
```c
#include "subghz_fob_nav.h"
uint8_t subghz_fob_nav_move(uint8_t idx, uint8_t key) {
    if(idx > 3) idx = 0;
    uint8_t row = idx / 2, col = idx % 2;
    switch(key) {
    case FOB_LEFT:
    case FOB_RIGHT: col ^= 1; break;         // 2 cols: toggle
    case FOB_UP:
    case FOB_DOWN:  row ^= 1; break;          // 2 rows: toggle
    case FOB_CYCLE: return (uint8_t)((idx + 1) & 0x3);
    default: break;
    }
    return (uint8_t)(row * 2 + col);
}
```
- [ ] **Step 5: Run — expect PASS** (`test_fob_nav OK`).
- [ ] **Step 6: Commit** `git add applications/main/subghz/helpers/subghz_fob_nav.h applications/main/subghz/helpers/subghz_fob_nav.c tests/test_fob_nav.c` → `git commit -m "feat(subghz): pure fob 2x2 grid-nav core + host test"`

---

## Task 2: Fob View (render + input)

**Files:** Create `applications/main/subghz/views/subghz_keeloq_fob.{c,h}`. Modify `subghz_custom_event.h`.

**Interfaces:**
- Consumes: `subghz_fob_nav_move` (Task 1).
- Produces (header): `typedef struct SubGhzKeeloqFob SubGhzKeeloqFob;` `typedef void (*SubGhzKeeloqFobCallback)(SubGhzCustomEvent event, void* context);` and `subghz_keeloq_fob_alloc/free/get_view/set_callback`, plus `void subghz_keeloq_fob_set_state(SubGhzKeeloqFob*, uint8_t real_btn, uint16_t cnt, uint16_t jump);` and `uint8_t subghz_keeloq_fob_selected_nibble(SubGhzKeeloqFob*);` and `void subghz_keeloq_fob_set_sent(SubGhzKeeloqFob*, uint16_t new_cnt);`.

- [ ] **Step 1: Add custom events** — in `applications/main/subghz/helpers/subghz_custom_event.h`, add to the `SubGhzCustomEvent` enum (near the existing `SubGhzCustomEventSceneRollingAnalyzerClone`):
```c
    SubGhzCustomEventViewKeeloqFobSend,
    SubGhzCustomEventViewKeeloqFobBack,
```
- [ ] **Step 2: Header** `subghz_keeloq_fob.h` (mirror `subghz_jammer.h`):
```c
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
```
- [ ] **Step 3: Implementation** `subghz_keeloq_fob.c` — mirror `subghz_jammer.c`'s alloc/free/get_view/set_callback verbatim (swap the model). Model + tiles:
```c
#include "subghz_keeloq_fob.h"
#include "../helpers/subghz_fob_nav.h"
#include <gui/elements.h>
#include <furi.h>

static const uint8_t FOB_CODES[4] = {0x1, 0x2, 0x4, 0x8};
static const char* const FOB_LABELS[4] = {"LOCK", "UNLOCK", "TRUNK", "PANIC"};

typedef struct {
    uint8_t sel;        // 0..3
    uint8_t real_btn;   // captured nibble, marks confirmed-real tile
    uint16_t cnt;
    uint16_t jump;
    bool sent;
} SubGhzKeeloqFobModel;

struct SubGhzKeeloqFob {
    View* view;
    SubGhzKeeloqFobCallback callback;
    void* context;
};
```
Draw callback — 2×2 tiles with label, selected inverted, "•" on the real tile, status line (use `canvas_draw_rframe`/`canvas_draw_rbox`/`canvas_invert_color`/`canvas_draw_str_aligned`/`canvas_draw_disc` per the extracted canvas notes; 128×64 screen). Tile rects: TL(2,13,60,20) TR(66,13,60,20) BL(2,35,60,20) BR(66,35,60,20):
```c
static void keeloq_fob_draw_callback(Canvas* canvas, void* _model) {
    SubGhzKeeloqFobModel* m = _model;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 0, AlignCenter, AlignTop, "Fob Emulator");
    const uint8_t rx[4] = {2, 66, 2, 66};
    const uint8_t ry[4] = {13, 13, 35, 35};
    canvas_set_font(canvas, FontSecondary);
    for(uint8_t i = 0; i < 4; i++) {
        uint8_t x = rx[i], y = ry[i];
        if(i == m->sel) {
            canvas_draw_rbox(canvas, x, y, 60, 18, 3);
            canvas_invert_color(canvas);
        } else {
            canvas_draw_rframe(canvas, x, y, 60, 18, 3);
        }
        canvas_draw_str_aligned(canvas, x + 30, y + 9, AlignCenter, AlignCenter, FOB_LABELS[i]);
        if(m->real_btn == FOB_CODES[i]) canvas_draw_disc(canvas, x + 54, y + 4, 2);
        if(i == m->sel) canvas_invert_color(canvas);
    }
    char st[32];
    snprintf(st, sizeof(st), "cnt %04X  jump %u%s", m->cnt, m->jump, m->sent ? "  SENT" : "");
    canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, st);
}
```
Input callback — Cardputer arrows + M5Stick cycle; OK sends; Back exits. (Left/Right/Up/Down → `subghz_fob_nav_move`; a cycle key for the M5Stick; OK → fire `...FobSend`; Back → `...FobBack`.):
```c
static bool keeloq_fob_input_callback(InputEvent* event, void* context) {
    SubGhzKeeloqFob* inst = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;
    uint8_t navkey = 0xFF;
    switch(event->key) {
    case InputKeyLeft:  navkey = FOB_LEFT; break;
    case InputKeyRight: navkey = FOB_RIGHT; break;
    case InputKeyUp:    navkey = FOB_UP; break;
    case InputKeyDown:  navkey = FOB_DOWN; break;
    case InputKeyOk:
        with_view_model(inst->view, SubGhzKeeloqFobModel * m, { m->sent = false; }, false);
        if(inst->callback) inst->callback(SubGhzCustomEventViewKeeloqFobSend, inst->context);
        return true;
    case InputKeyBack:
        if(inst->callback) inst->callback(SubGhzCustomEventViewKeeloqFobBack, inst->context);
        return true;
    default: return false;
    }
    if(navkey != 0xFF) {
        with_view_model(
            inst->view, SubGhzKeeloqFobModel * m, { m->sel = subghz_fob_nav_move(m->sel, navkey); }, true);
        return true;
    }
    return false;
}
```
alloc/free/get_view/set_callback: copy `subghz_jammer_alloc/free/get_view/set_callback` verbatim, renaming to `subghz_keeloq_fob_*` and seeding the model `{sel=0, real_btn=0, cnt=0, jump=100, sent=false}`; no timer. Then the state accessors:
```c
void subghz_keeloq_fob_set_state(
    SubGhzKeeloqFob* inst, uint8_t real_btn, uint16_t cnt, uint16_t jump) {
    uint8_t sel = 0;
    for(uint8_t i = 0; i < 4; i++)
        if(FOB_CODES[i] == real_btn) sel = i;
    with_view_model(
        inst->view, SubGhzKeeloqFobModel * m,
        { m->real_btn = real_btn; m->cnt = cnt; m->jump = jump; m->sel = sel; m->sent = false; }, true);
}
uint8_t subghz_keeloq_fob_selected_nibble(SubGhzKeeloqFob* inst) {
    uint8_t nib = 0x1;
    with_view_model(inst->view, SubGhzKeeloqFobModel * m, { nib = FOB_CODES[m->sel]; }, false);
    return nib;
}
void subghz_keeloq_fob_set_sent(SubGhzKeeloqFob* inst, uint16_t new_cnt) {
    with_view_model(inst->view, SubGhzKeeloqFobModel * m, { m->cnt = new_cnt; m->sent = true; }, true);
}
```
- [ ] **Step 4: Compile gate** → `=== BUILD OK ===` (view compiles even before it's registered; it's auto-globbed). If `with_view_model`/`elements`/`canvas` symbols are missing, add the includes the jammer view uses (`<gui/elements.h>`, `<gui/canvas.h>` via view.h).
- [ ] **Step 5: Commit** the 3 files (`subghz_keeloq_fob.c/.h`, `subghz_custom_event.h`): `git commit -m "feat(subghz): KeeLoq fob emulator view (tiles + input)"`

---

## Task 3: Register the view

**Files:** `subghz_types.h`, `subghz_i.h`, `subghz.c`.

- [ ] **Step 1:** In `applications/main/subghz/helpers/subghz_types.h`, add before the closing `} SubGhzViewId;` (after `SubGhzViewIdProtoview`): `SubGhzViewIdKeeloqFob,`
- [ ] **Step 2:** In `applications/main/subghz/subghz_i.h`: add `#include "views/subghz_keeloq_fob.h"` next to the other view includes (~line 11), and `SubGhzKeeloqFob* subghz_keeloq_fob;` next to `SubGhzJammer* subghz_jammer;` (~line 89).
- [ ] **Step 3:** In `applications/main/subghz/subghz.c`, mirror the **frequency_analyzer eager pattern** (`:197-201` alloc+add_view). After that block add:
```c
    subghz->subghz_keeloq_fob = subghz_keeloq_fob_alloc();
    view_dispatcher_add_view(
        subghz->view_dispatcher,
        SubGhzViewIdKeeloqFob,
        subghz_keeloq_fob_get_view(subghz->subghz_keeloq_fob));
```
And in `subghz_free`, next to the frequency_analyzer remove+free (`:373-374`):
```c
    view_dispatcher_remove_view(subghz->view_dispatcher, SubGhzViewIdKeeloqFob);
    subghz_keeloq_fob_free(subghz->subghz_keeloq_fob);
```
- [ ] **Step 2: Compile gate** → `=== BUILD OK ===`.
- [ ] **Step 3: Commit** the 3 files: `git commit -m "feat(subghz): register KeeLoq fob view"`

---

## Task 4: Fob scene (build clone on enter, send per button)

**Files:** Create `applications/main/subghz/scenes/subghz_scene_keeloq_fob.c`. Modify `subghz_scene_config.h`.

**Interfaces:**
- Consumes: `subghz_txrx_gen_keeloq_clone` (from the clone feature), `subghz_txrx_get_fff_data`, `subghz_tx_start`, `flipper_format_update_uint32`, the fob view accessors (Task 2).

- [ ] **Step 1: Register scene** — in `subghz_scene_config.h` after `ADD_SCENE(subghz, set_jump, SetJump)`: `ADD_SCENE(subghz, keeloq_fob, KeeloqFob)`.
- [ ] **Step 2: Scene** `subghz_scene_keeloq_fob.c`:
```c
#include "../subghz_i.h"
#include "../helpers/subghz_txrx_create_protocol_key.h"
#include <lib/subghz/protocols/keeloq.h>

#define TAG "SubGhzKeeloqFob"

static void subghz_scene_keeloq_fob_callback(SubGhzCustomEvent event, void* context) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, event);
}

void subghz_scene_keeloq_fob_on_enter(void* context) {
    SubGhz* subghz = context;
    if(subghz->keeloq_clone_jump == 0) subghz->keeloq_clone_jump = 100;

    // Build the clone into fff_data from the gen_info the Clone action populated.
    bool ok = subghz_txrx_gen_keeloq_clone(
        subghz->txrx,
        subghz->gen_info->mod,
        subghz->gen_info->freq,
        subghz->gen_info->keeloq.serial,
        subghz->gen_info->keeloq.btn,
        subghz->gen_info->keeloq.cnt,
        subghz->keeloq_clone_jump,
        subghz->gen_info->keeloq.manuf);
    subghz->keeloq_clone_flow = false;
    if(!ok) {
        furi_string_set(subghz->error_str, "Function requires\nan SD card with\nfresh databases.");
        scene_manager_next_scene(subghz->scene_manager, SubGhzSceneShowError);
        return;
    }

    subghz_keeloq_fob_set_callback(
        subghz->subghz_keeloq_fob, subghz_scene_keeloq_fob_callback, subghz);
    subghz_keeloq_fob_set_state(
        subghz->subghz_keeloq_fob,
        (uint8_t)(subghz->gen_info->keeloq.btn & 0x0F),
        subghz->gen_info->keeloq.cnt,
        subghz->keeloq_clone_jump);
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdKeeloqFob);
}

bool subghz_scene_keeloq_fob_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SubGhzCustomEventViewKeeloqFobSend) {
            uint8_t nibble = subghz_keeloq_fob_selected_nibble(subghz->subghz_keeloq_fob);
            uint32_t btn = nibble;
            FlipperFormat* fff = subghz_txrx_get_fff_data(subghz->txrx);
            flipper_format_update_uint32(fff, "Btn", &btn, 1);
            subghz_txrx_hopper_pause(subghz->txrx);
            subghz->state_notifications = SubGhzNotificationStateTx;
            subghz_block_generic_global.endless_tx = false; // single burst (clone repeat=2)
            if(!subghz_tx_start(subghz, fff)) {
                subghz_txrx_rx_start(subghz->txrx);
                subghz->state_notifications = SubGhzNotificationStateRx;
            }
            return true;
        } else if(event.event == SubGhzCustomEventViewKeeloqFobBack) {
            scene_manager_previous_scene(subghz->scene_manager);
            return true;
        }
    } else if(event.type == SceneManagerEventTypeTick) {
        // When the burst finishes, stop TX, resume RX, refresh the counter shown.
        if(subghz->state_notifications == SubGhzNotificationStateTx &&
           !subghz_txrx_radio_device_is_tx_complete_ok(subghz->txrx)) {
            // still sending; wait
        }
        return true;
    }
    return false;
}

void subghz_scene_keeloq_fob_on_exit(void* context) {
    SubGhz* subghz = context;
    subghz_txrx_stop(subghz->txrx);
    subghz->state_notifications = SubGhzNotificationStateIDLE;
}
```
NOTE (implementer): the exact TX-completion / counter-refresh in the tick handler must match how `subghz_scene_transmitter.c` detects completion (read `subghz_scene_transmitter.c` and mirror its tick logic — the send is one burst with `endless_tx=false`, `encoder.repeat=2`). After completion, re-read `Cnt` from `fff_data` (or the decoded model) and call `subghz_keeloq_fob_set_sent(subghz->subghz_keeloq_fob, new_cnt)`. If a clean completion signal isn't readily available, set "SENT" immediately after `subghz_tx_start` returns true and refresh `cnt` from `gen_info->keeloq.cnt + jump` as a display estimate — correctness of TX doesn't depend on the display. Confirm against the transmitter scene.
- [ ] **Step 3: Compile gate** → `=== BUILD OK ===`. Resolve any accessor names (`subghz_txrx_hopper_pause`, `subghz_txrx_radio_device_is_tx_complete_ok`, `subghz_tx_start`, `subghz_block_generic_global`) by reading `subghz_scene_transmitter.c` / `subghz_txrx.h` — use the exact names that scene uses.
- [ ] **Step 4: Commit** `subghz_scene_keeloq_fob.c` + `subghz_scene_config.h`: `git commit -m "feat(subghz): KeeLoq fob scene (build clone + per-button TX)"`

---

## Task 5: Route both Clone actions to the fob

**Files:** `subghz_scene_keeloq_action.c`, `subghz_scene_rolling_analyzer.c`.

- [ ] **Step 1:** In `subghz_scene_keeloq_action.c`, the `SubGhzSceneKeeloqActionClone` case currently sets `keeloq_clone_flow=true` and `next_scene(SubGhzSceneSetJump)`. Change the destination to `SubGhzSceneKeeloqFob`:
```c
    case SubGhzSceneKeeloqActionClone:
        subghz->keeloq_mfr_emulate_flow = false;
        subghz->keeloq_clone_flow = true;
        scene_manager_next_scene(subghz->scene_manager, SubGhzSceneKeeloqFob);
        break;
```
- [ ] **Step 2:** In `subghz_scene_rolling_analyzer.c`, the Clone handler that fills `gen_info` + sets `keeloq_clone_flow=true` + `next_scene(SubGhzSceneSetJump)` — change its `next_scene` target to `SubGhzSceneKeeloqFob`. Leave the gen_info fill + gate unchanged.
- [ ] **Step 3: Compile gate** → `=== BUILD OK ===`.
- [ ] **Step 4: Commit** both scene files: `git commit -m "feat(subghz): Clone actions open the fob emulator (jump 100)"`

---

## Task 6: Guard the feature to private builds

**Files:** the new view/scene + the Clone route changes.

- [ ] **Step 1:** Wrap with `#ifndef PUBLIC_BUILD` / `#endif`: the body of `subghz_scene_keeloq_fob.c` (or make its on_enter show error under public), the fob view registration in `subghz.c` (alloc/add_view + remove/free), the `ADD_SCENE(... KeeloqFob)` line (guard per how other private scenes are gated — check for an existing `#ifndef PUBLIC_BUILD` in `subghz_scene_config.h`; if the config can't be #if'd, gate the scene's on_enter instead), and the Clone menu items so public builds don't route to the fob. Match exactly how car decoders / the existing Clone code are (or should be) gated. If no existing pattern in `subghz_scene_config.h`, gate at the scene/view level and leave the ADD_SCENE unconditional (a registered-but-unreachable scene is harmless).
- [ ] **Step 2: Compile gate BOTH variants** — private: `build_ci.bat` → BUILD OK. Public: build the public env (per the tree's public build script, e.g. `build_only_public.bat` / the `-public` merge) → BUILD OK with the fob compiled out or unreachable, no linker errors.
- [ ] **Step 3: Commit** `git commit -m "feat(subghz): guard fob emulator to private builds"`

---

## Task 7: Mirror to cardputer + build both

- [ ] **Step 1:** Copy the 4 new files verbatim into `src_cardputer_adv` at the same paths; apply the same edits to the shared files. Since the trees are content-parallel, generate the feature diff `git diff <base>..HEAD -- applications tests` and `git apply --ignore-whitespace` into the cardputer tree (as done for the clone feature), or copy per-file.
- [ ] **Step 2: Host test** in cardputer tree: `... tests/test_fob_nav.c` → PASS.
- [ ] **Step 3: Build both trees** (m5stick `build_ci.bat`; cardputer `build_ci.bat`) → BUILD OK.
- [ ] **Step 4: Commit** (m5stick tree records the mirror action / cardputer is non-git).

---

## Task 8: On-hardware verification (manual)

- [ ] Build the private m5stick image (`build_m5stick_s3.bat` + `build_ext_and_merge_private.bat`) and flash (`flash_s3_private.bat write`, COM10); flash cardputer (`flash_cardputer_com12.bat`, COM12).
- [ ] **Cardputer:** capture a clonable KeeLoq fob → open → Clone → **fob emulator opens directly** (no jump prompt). Arrow keys move highlight; Enter sends. Confirm Lock/Unlock/Trunk/Panic each transmit and the target reacts to the buttons it has; `cnt` on the status line increments per press.
- [ ] **M5Stick:** tap cycles tiles, OK sends, Back exits. **Tune the tap-vs-send gesture here** — if a tap both cycles and sends (the 2-button trap), adjust the input callback (e.g. cycle on `InputKeyDown`, send only on a long-press/`InputKeyOk` that the board emits distinctly) and reflash. Record the working mapping in `BUGFIXES.md`.
- [ ] **No-desync:** press the real fob several times, then send from the emulator → still actuates.
- [ ] **Guards:** confirm the public build shows no fob UI.

---

## Self-Review

**Spec coverage:** auto-clone→fob (Tasks 4,5); fob view tiles+labels+select (Task 2); grid-nav (Task 1); per-button no-desync TX (Task 4); Cardputer arrows / M5Stick cycle (Tasks 2,8); private guard (Task 6); both trees (Task 7). "Save fob" (spec §D) is intentionally **out of this plan** (spec marked it optional/last) — deferred to a follow-up; note for the reviewer.

**Placeholder scan:** the Task-4 tick/completion handling and the Task-8 M5Stick gesture are explicitly "confirm against transmitter scene / tune on-device" — these are genuine hardware/integration unknowns, bounded with a concrete fallback (set SENT after tx_start returns), not vague requirements.

**Type consistency:** `subghz_keeloq_fob_*` names match between header (Task 2), registration (Task 3), and scene (Task 4). `FOB_CODES`/nibbles consistent. `SubGhzCustomEventViewKeeloqFobSend/Back` consistent between view (Task 2) and scene (Task 4). `SubGhzViewIdKeeloqFob` / `SubGhzSceneKeeloqFob` consistent across Tasks 3/4/5.
