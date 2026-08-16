#include <furi.h>
#include <furi_hal_version.h>

#include <gui/gui.h>
#include <dolphin/dolphin.h>
#include <dolphin/helpers/dolphin_state.h>

#include <assets_icons.h>

#define MOODS_TOTAL  3
#define BUTTHURT_MAX 3

static const Icon* const portrait_happy[BUTTHURT_MAX] = {
    &I_passport_happy1_46x49,
    &I_passport_happy2_46x49,
    &I_passport_happy3_46x49};
static const Icon* const portrait_ok[BUTTHURT_MAX] = {
    &I_passport_okay1_46x49,
    &I_passport_okay2_46x49,
    &I_passport_okay3_46x49};
static const Icon* const portrait_bad[BUTTHURT_MAX] = {
    &I_passport_bad1_46x49,
    &I_passport_bad2_46x49,
    &I_passport_bad3_46x49};

static const Icon* const* portraits[MOODS_TOTAL] = {portrait_happy, portrait_ok, portrait_bad};

static void input_callback(InputEvent* input, void* ctx) {
    FuriSemaphore* semaphore = ctx;

    if((input->type == InputTypeShort) && (input->key == InputKeyBack)) {
        furi_semaphore_release(semaphore);
    }
}

static void render_callback(Canvas* canvas, void* ctx) {
    DolphinStats* stats = ctx;

    char level_str[20];
    char mood_str[32];
    uint8_t mood = 0;

    if(stats->butthurt <= 4) {
        mood = 0;
        snprintf(mood_str, 20, "Mood: Happy");
    } else if(stats->butthurt <= 9) {
        mood = 1;
        snprintf(mood_str, 20, "Mood: Ok");
    } else {
        mood = 2;
        snprintf(mood_str, 20, "Mood: Angry");
    }

    uint32_t xp_progress = 0;
    uint32_t xp_to_levelup = dolphin_state_xp_to_levelup(stats->icounter);
    uint32_t xp_for_current_level =
        xp_to_levelup + dolphin_state_xp_above_last_levelup(stats->icounter);
    // xp_for_current_level is 0 only right at a level-up boundary (including
    // the moment the max level, DOLPHIN_LEVEL_COUNT, is first reached) --
    // guard the divide directly instead of hardcoding the old 3-level cap.
    if(xp_for_current_level == 0) {
        xp_progress = 0;
    } else {
        xp_progress = xp_to_levelup * 64 / xp_for_current_level;
    }

    // multipass
    canvas_draw_icon(canvas, 0, 0, &I_passport_left_6x46);
    canvas_draw_icon(canvas, 0, 46, &I_passport_bottom_128x18);
    canvas_draw_line(canvas, 6, 0, 125, 0);
    canvas_draw_line(canvas, 127, 2, 127, 47);
    canvas_draw_dot(canvas, 126, 1);

    // portrait -- only MOODS_TOTAL (3) portrait art variants exist per mood,
    // but level now ranges 1..DOLPHIN_LEVEL_COUNT (30). Scale the level down
    // into the available art range instead of indexing by level directly
    // (which would read out of bounds past level 3).
    furi_assert((stats->level > 0) && (stats->level <= DOLPHIN_LEVEL_COUNT));
    uint8_t portrait_index = (uint8_t)((stats->level - 1) * BUTTHURT_MAX / DOLPHIN_LEVEL_COUNT);
    if(portrait_index >= BUTTHURT_MAX) {
        portrait_index = BUTTHURT_MAX - 1;
    }
    canvas_draw_icon(canvas, 9, 5, portraits[mood][portrait_index]);
    canvas_draw_line(canvas, 58, 16, 123, 16);
    canvas_draw_line(canvas, 58, 30, 123, 30);
    canvas_draw_line(canvas, 58, 44, 123, 44);

    const char* my_name = furi_hal_version_get_name_ptr();
    snprintf(level_str, 20, "Level: %hu", stats->level);
    canvas_draw_str(canvas, 58, 12, my_name ? my_name : "Unknown");
    canvas_draw_str(canvas, 58, 26, mood_str);
    canvas_draw_str(canvas, 58, 40, level_str);

    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, 123 - xp_progress, 47, xp_progress + 1, 6);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_line(canvas, 123, 47, 123, 52);
}

int32_t passport_app(void* p) {
    UNUSED(p);
    FuriSemaphore* semaphore = furi_semaphore_alloc(1, 0);
    ViewPort* view_port = view_port_alloc();

    Dolphin* dolphin = furi_record_open(RECORD_DOLPHIN);
    DolphinStats stats = dolphin_stats(dolphin);
    furi_record_close(RECORD_DOLPHIN);
    view_port_draw_callback_set(view_port, render_callback, &stats);
    view_port_input_callback_set(view_port, input_callback, semaphore);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);
    view_port_update(view_port);

    furi_check(furi_semaphore_acquire(semaphore, FuriWaitForever) == FuriStatusOk);

    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);
    furi_semaphore_free(semaphore);

    return 0;
}
