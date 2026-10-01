#pragma once
#include "nfc_share.h"
#include "scenes/nfc_share_scene.h"
#include "views/nfc_share_progress_view.h"
#include "nfc_share_engine.h"
#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <dialogs/dialogs.h>
#include <storage/storage.h>

#define NFC_SHARE_BASE_FOLDER "/ext"
#define NFC_SHARE_RECV_FOLDER "/ext/nfc_share"
#define NFC_SHARE_PATH_MAX    256

typedef enum {
    NfcShareViewSubmenu,
    NfcShareViewProgress,
} NfcShareView;

struct NfcShareApp {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    Submenu* submenu;
    DialogsApp* dialogs;
    Storage* storage;
    NfcShareProgressView* progress;   // custom view (Task 2 step 5)
    FuriString* file_path;            // selected send path
    NfcShareRole role;                // set by start scene
    NfcShareEngine* engine;           // owned by transfer scene
};
typedef struct NfcShareApp NfcShareApp;
