#pragma once

#include <gui/view.h>
#include "../cam_hunter.h"

// Live hunt-progress view: phase line, counters (APs / live hosts / open ports /
// cracked), a progress bar and elapsed time.

View* camera_hunter_view_status_alloc(void);
void camera_hunter_view_status_free(View* view);
void camera_hunter_view_status_update(View* view, const CamHuntStatus* status);
