#pragma once

#include "gpio_app.h"
#include "gpio_items.h"
#include "scenes/gpio_scene.h"
#include "gpio_custom_event.h"
#include "usb_uart_bridge.h"
#include <power/power_service/power.h>

#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <notification/notification_messages.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/widget.h>
#include <gui/modules/dialog_ex.h>
#include "views/gpio_manual_list.h"
#include "views/gpio_manual_edit.h"
#include "views/gpio_usb_uart.h"
#include "gpio_pin_catalog.h"
#include "gpio_driver.h"
#include <assets_icons.h>
#include <expansion/expansion.h>

/* Per-pin runtime state (one per catalog index). Pwm/Adc used in Tasks 4/5. */
typedef enum {
    GpioPinModeOff,
    GpioPinModeOutput,
    GpioPinModeInput,
    GpioPinModePwm,
    GpioPinModeAdc,
} GpioPinMode;

typedef struct {
    GpioPinMode mode; // default GpioPinModeOff
    bool out_level; // Output
    GpioDriverPull pull; // Input
    uint32_t pwm_freq; // Task 4 (default 1000)
    uint8_t pwm_duty; // Task 4 (default 50)
} GpioPinState;

struct GpioApp {
    Expansion* expansion;
    Gui* gui;
    NotificationApp* notifications;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    Widget* widget;
    DialogEx* dialog;
    Power* power;

    VariableItemList* var_item_list;
    VariableItem* var_item_flow;
    GpioManualList* manual_list;
    GpioManualEdit* manual_edit;
    GpioUsbUart* gpio_usb_uart;
    GPIOItems* gpio_items;
    UsbUartBridge* usb_uart_bridge;
    UsbUartConfig* usb_uart_cfg;

    /* Manual Control state */
    GpioPinState pin_state[16]; // indexed by catalog index (max catalog size 8)
    bool show_advanced;
    size_t edit_index;
    FuriTimer* manual_timer;
};

typedef enum {
    GpioAppViewVarItemList,
    GpioAppViewManualList,
    GpioAppViewManualEdit,
    GpioAppViewUsbUart,
    GpioAppViewUsbUartCfg,
    GpioAppViewUsbUartCloseRpc,
    GpioAppViewExitConfirm,
} GpioAppView;
