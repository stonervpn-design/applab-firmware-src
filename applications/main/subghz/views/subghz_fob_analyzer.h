#pragma once

#include <gui/view.h>
#include "../helpers/subghz_custom_event.h"
#include "../helpers/subghz_txrx.h"

typedef struct SubGhzFobAnalyzer SubGhzFobAnalyzer;

typedef void (*SubGhzFobAnalyzerCallback)(SubGhzCustomEvent event, void* context);

void subghz_fob_analyzer_set_callback(
    SubGhzFobAnalyzer* instance,
    SubGhzFobAnalyzerCallback callback,
    void* context);

SubGhzFobAnalyzer* subghz_fob_analyzer_alloc(SubGhzTxRx* txrx);

void subghz_fob_analyzer_free(SubGhzFobAnalyzer* instance);

View* subghz_fob_analyzer_get_view(SubGhzFobAnalyzer* instance);
