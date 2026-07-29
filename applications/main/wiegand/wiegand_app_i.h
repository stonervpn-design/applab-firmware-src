#pragma once

#include "wiegand_app.h"
#include "wiegand_custom_event.h"
#include "scenes/wiegand_scene.h"
#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>

typedef enum {
    WiegandViewSubmenu,
    WiegandViewCapture,
    WiegandViewCraft,
    WiegandViewReplay,
} WiegandView;

typedef struct {
    uint8_t d0_pin;
    uint8_t d1_pin;
    uint8_t bits[34];
    uint8_t bit_count;
    uint8_t facility;
    uint16_t card;
} WiegandState;

struct WiegandApp {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    Submenu* submenu;
    bool advanced_unlocked;
    WiegandState state;
};
