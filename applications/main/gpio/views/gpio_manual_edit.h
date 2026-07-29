#pragma once

#include <gui/view.h>
#include "../gpio_app.h"

typedef struct GpioManualEdit GpioManualEdit;

GpioManualEdit* gpio_manual_edit_alloc(void);

void gpio_manual_edit_free(GpioManualEdit* instance);

View* gpio_manual_edit_get_view(GpioManualEdit* instance);

/* Rebuild the editor rows for app->edit_index and re-apply the pin's state. */
void gpio_manual_edit_set_context(GpioManualEdit* instance, GpioApp* app);

/* Refresh the read-only live Read row (called on the redraw tick). */
void gpio_manual_edit_tick(GpioManualEdit* instance);
