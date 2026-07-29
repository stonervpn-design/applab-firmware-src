#pragma once

#include <gui/view.h>
#include "../gpio_app.h"

typedef struct GpioManualList GpioManualList;

GpioManualList* gpio_manual_list_alloc(void);

void gpio_manual_list_free(GpioManualList* instance);

View* gpio_manual_list_get_view(GpioManualList* instance);

/* Bind the list to the app so it can iterate the catalog + live pin state. */
void gpio_manual_list_set_context(GpioManualList* instance, GpioApp* app);

/* Force a redraw (called by the scene's ~5 Hz timer to refresh live reads). */
void gpio_manual_list_notify_redraw(GpioManualList* instance);
