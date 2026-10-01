#include "nfc_share_progress_view.h"
#include <gui/elements.h>
#include <furi.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    NfcShareStatus status;
    bool sending;
} NfcShareProgressViewModel;

struct NfcShareProgressView {
    View* view;
};

static void nfc_share_progress_view_draw(Canvas* canvas, void* model) {
    NfcShareProgressViewModel* m = model;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, m->sending ? "NFC Send" : "NFC Receive");

    canvas_set_font(canvas, FontSecondary);
    if(!m->sending && m->status.dest_name[0]) {
        char name_line[sizeof("nfc_share/") - 1 + sizeof(m->status.dest_name)];
        snprintf(name_line, sizeof(name_line), "nfc_share/%s", m->status.dest_name);
        canvas_draw_str(canvas, 2, 22, name_line);
    } else {
        canvas_draw_str(canvas, 2, 22, m->status.name[0] ? m->status.name : "-");
    }
    canvas_draw_line(canvas, 0, 25, 127, 25);

    float progress = (m->status.file_size > 0) ?
                          (float)m->status.bytes_done / (float)m->status.file_size :
                          0.0f;
    if(progress > 1.0f) progress = 1.0f;
    elements_progress_bar(canvas, 8, 32, 112, progress);

    /* Worst case: two 10-digit uint32 byte counts + " B  " + a 10-digit
     * kbps_x10 integer part + ".N KB/s" + NUL — sized generously above that. */
    char line[64];
    snprintf(
        line,
        sizeof(line),
        "%lu/%lu B  %lu.%lu KB/s",
        (unsigned long)m->status.bytes_done,
        (unsigned long)m->status.file_size,
        (unsigned long)(m->status.kbps_x10 / 10),
        (unsigned long)(m->status.kbps_x10 % 10));
    canvas_draw_str(canvas, 2, 48, line);

    char status_buf[32];
    const char* status_line = NULL;
    if(m->status.state == NfcShareStateDoneOk) {
        status_line = m->sending ? "Done - md5 OK" : "Saved to SD - md5 OK";
    } else if(m->status.state == NfcShareStateFail) {
        snprintf(status_buf, sizeof(status_buf), "Failed (err %u)", m->status.err);
        status_line = status_buf;
    } else if(m->status.state == NfcShareStateWaiting) {
        status_line = "Waiting...";
    } else if(m->status.state == NfcShareStateRunning) {
        status_line = "Transferring...";
    }
    if(status_line) {
        canvas_draw_str(canvas, 2, 62, status_line);
    }
}

static bool nfc_share_progress_view_input(InputEvent* event, void* ctx) {
    UNUSED(event);
    UNUSED(ctx);
    return false;
}

NfcShareProgressView* nfc_share_progress_view_alloc(void) {
    NfcShareProgressView* v = malloc(sizeof(NfcShareProgressView));
    v->view = view_alloc();
    view_allocate_model(v->view, ViewModelTypeLocking, sizeof(NfcShareProgressViewModel));
    view_set_draw_callback(v->view, nfc_share_progress_view_draw);
    view_set_input_callback(v->view, nfc_share_progress_view_input);
    return v;
}

void nfc_share_progress_view_free(NfcShareProgressView* v) {
    view_free(v->view);
    free(v);
}

View* nfc_share_progress_view_get_view(NfcShareProgressView* v) {
    return v->view;
}

void nfc_share_progress_view_update(NfcShareProgressView* v, const NfcShareStatus* s, bool sending) {
    with_view_model(
        v->view,
        NfcShareProgressViewModel * m,
        {
            m->status = *s;
            m->sending = sending;
        },
        true);
}
