#pragma once

#include "qrcode_app.h"
#include "scenes/qrcode_scene.h"
#include "views/qrcode_display.h"

#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/variable_item_list.h>

#define QRCODE_TEXT_MAX_LEN 256

/** Views registered with the view dispatcher */
typedef enum {
    QRCodeViewSubmenu,
    QRCodeViewTextInput,
    QRCodeViewVarItemList,
    QRCodeViewDisplay,
} QRCodeAppView;

/** Custom events emitted by the start submenu */
typedef enum {
    QRCodeCustomEventStartText,
    QRCodeCustomEventStartWifi,
    QRCodeCustomEventStartVcard,
    QRCodeCustomEventStartFile,
} QRCodeStartEvent;

/** Custom events emitted by the text input scenes */
typedef enum {
    QRCodeCustomEventTextDone,
    QRCodeCustomEventFieldDone,
} QRCodeTextEvent;

/** Which field the shared field-input scene edits on the next visit */
typedef enum {
    QRCodeFieldSsid,
    QRCodeFieldPass,
    QRCodeFieldName,
    QRCodeFieldPhone,
    QRCodeFieldEmail,
} QRCodeEditField;

/** WiFi authentication modes (index into the Auth value cycler) */
typedef enum {
    QRCodeWifiAuthWpa,
    QRCodeWifiAuthWep,
    QRCodeWifiAuthOpen,
    QRCodeWifiAuthCount,
} QRCodeWifiAuth;

struct QRCodeApp {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;

    Submenu* submenu;
    TextInput* text_input;
    VariableItemList* var_item_list;
    QRCodeDisplay* qrcode_display;

    char payload[QRCODE_TEXT_MAX_LEN];
    char input_buf[QRCODE_TEXT_MAX_LEN];

    /* WiFi flow */
    char ssid[64];
    char pass[64];
    uint8_t wifi_auth; /* QRCodeWifiAuth */

    /* vCard flow */
    char name[64];
    char phone[48];
    char email[96];

    /* Which field the shared field-input scene currently edits */
    uint8_t edit_field; /* QRCodeEditField */
};
