#pragma once
#include <furi.h>
#include "camera_hunter_app.h"
#include "camera_hunter_custom_event.h"
#include "scenes/camera_hunter_scene.h"
#include "cam_hunter.h"

#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/widget.h>
#include <gui/modules/text_input.h>
#include <notification/notification_messages.h>

typedef enum {
    CamHunterViewSubmenu,
    CamHunterViewTextInput,
    CamHunterViewStatus,
    CamHunterViewWidget,
} CamHunterView;

struct CameraHunterApp {
    Gui* gui;
    NotificationApp* notifications;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;

    Submenu* submenu;
    TextInput* text_input;
    View* view_status; // live hunt progress
    Widget* widget; // authorize notice + result detail

    CamHunt* hunt; // active/last hunt (NULL when none)
    CamHuntMode pending_mode; // mode chosen at menu, run after authorize
    bool pending_valid; // true if authorize was reached via a gated action
    bool authorized; // session consent flag

    char ssid[33];
    char pass[65];
    char input_buf[65]; // text-input scratch
    uint8_t result_index; // finding shown on the detail screen
    bool hunt_finished_sent; // one-shot guard for the done->results hop
};
