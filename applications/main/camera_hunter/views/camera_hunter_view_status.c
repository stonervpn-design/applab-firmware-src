#include "camera_hunter_view_status.h"
#include <gui/elements.h>
#include <furi.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char phase_text[48];
    uint16_t aps_seen;
    uint16_t detected;
    uint16_t hosts_alive;
    uint16_t ports_open;
    uint16_t creds_hit;
    uint8_t progress_pct;
    uint32_t elapsed_sec;
    bool done;
} StatusModel;

static void status_draw_cb(Canvas* canvas, void* model) {
    StatusModel* m = model;
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Camera Hunter");

    canvas_set_font(canvas, FontSecondary);

    // Phase line (truncate to fit).
    char phase[32];
    strncpy(phase, m->phase_text, sizeof(phase) - 1);
    phase[sizeof(phase) - 1] = '\0';
    canvas_draw_str(canvas, 2, 22, phase);

    // Counters.
    char l1[32], l2[32];
    snprintf(l1, sizeof(l1), "Found:%u  APs:%u", m->detected, m->aps_seen);
    snprintf(l2, sizeof(l2), "Open:%u  Cracked:%u", m->ports_open, m->creds_hit);
    canvas_draw_str(canvas, 2, 34, l1);
    canvas_draw_str(canvas, 2, 44, l2);

    // Progress bar.
    uint8_t pct = m->progress_pct > 100 ? 100 : m->progress_pct;
    elements_progress_bar(canvas, 2, 48, 124, (float)pct / 100.0f);

    // Footer.
    char foot[32];
    if(m->done) {
        snprintf(foot, sizeof(foot), "Done  %lus", (unsigned long)m->elapsed_sec);
    } else {
        snprintf(foot, sizeof(foot), "%lus  Back: stop", (unsigned long)m->elapsed_sec);
    }
    canvas_draw_str(canvas, 2, 62, foot);
}

static bool status_input_cb(InputEvent* event, void* context) {
    UNUSED(event);
    UNUSED(context);
    return false; // let the view dispatcher handle Back
}

View* camera_hunter_view_status_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(StatusModel));
    view_set_draw_callback(view, status_draw_cb);
    view_set_input_callback(view, status_input_cb);
    return view;
}

void camera_hunter_view_status_free(View* view) {
    view_free(view);
}

void camera_hunter_view_status_update(View* view, const CamHuntStatus* status) {
    with_view_model(
        view,
        StatusModel * m,
        {
            strncpy(m->phase_text, status->phase_text, sizeof(m->phase_text) - 1);
            m->phase_text[sizeof(m->phase_text) - 1] = '\0';
            m->aps_seen = status->aps_seen;
            m->detected = status->detected;
            m->hosts_alive = status->hosts_alive;
            m->ports_open = status->ports_open;
            m->creds_hit = status->creds_hit;
            m->progress_pct = status->progress_pct;
            m->elapsed_sec = status->elapsed_sec;
            m->done = status->done;
        },
        true);
}
