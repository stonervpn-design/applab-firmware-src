#include "power_view.h"
#include <gui/elements.h>
#include <furi.h>
#include <stdio.h>

static void power_draw(Canvas* canvas, void* model) {
    PowerViewModel* m = model;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontBigNumbers);
    char pct[8];
    snprintf(pct, sizeof(pct), "%u%%", (unsigned)m->charge_pct);
    canvas_draw_str(canvas, 2, 22, pct);

    canvas_set_font(canvas, FontSecondary);
    char line[32];
    snprintf(line, sizeof(line), "%lu.%02luV %s",
             (unsigned long)m->voltage,
             (unsigned long)((m->voltage - (int)m->voltage) * 100),
             m->charging ? "CHG" : "BAT");
    canvas_draw_str(canvas, 60, 12, line);
    snprintf(line, sizeof(line), "%ld mA", (long)m->current_ma);
    canvas_draw_str(canvas, 60, 24, line);
    if(m->est_minutes >= 0) {
        snprintf(line, sizeof(line), "%s %ldh%02ldm",
                 m->charging ? "full in" : "left",
                 (long)(m->est_minutes / 60), (long)(m->est_minutes % 60));
    } else {
        snprintf(line, sizeof(line), "%s --", m->charging ? "full in" : "left");
    }
    canvas_draw_str(canvas, 2, 34, line);

    // Curve: charge % over time in the strip y 40..63, x 2..125.
    canvas_draw_line(canvas, 0, 38, 127, 38);
    uint8_t n = m->curve.filled;
    if(n >= 2) {
        int gx0 = 2, gw = 124, gy0 = 63, gh = 24;
        for(uint8_t i = 1; i < n; i++) {
            int x1 = gx0 + (int)((i - 1) * gw / (n - 1));
            int x2 = gx0 + (int)(i * gw / (n - 1));
            int y1 = gy0 - power_curve_get(&m->curve, (uint8_t)(i - 1)) * gh / 100;
            int y2 = gy0 - power_curve_get(&m->curve, i) * gh / 100;
            canvas_draw_line(canvas, x1, y1, x2, y2);
        }
    }
}

static bool power_input(InputEvent* event, void* ctx) {
    UNUSED(event); UNUSED(ctx);
    return false; // Back exits via the view's previous-callback
}

View* power_view_alloc(void) {
    View* v = view_alloc();
    view_allocate_model(v, ViewModelTypeLocking, sizeof(PowerViewModel));
    with_view_model(v, PowerViewModel * m, { power_curve_reset(&m->curve); m->est_minutes = -1; }, true);
    view_set_draw_callback(v, power_draw);
    view_set_input_callback(v, power_input);
    return v;
}

void power_view_free(View* v) { view_free(v); }

void power_view_update(View* v, uint8_t charge_pct, float voltage,
                       int32_t current_ma, bool charging, int32_t est_minutes) {
    with_view_model(
        v, PowerViewModel * m,
        {
            m->charge_pct = charge_pct;
            m->voltage = voltage;
            m->current_ma = current_ma;
            m->charging = charging;
            m->est_minutes = est_minutes;
            power_curve_push(&m->curve, charge_pct);
        },
        true);
}
