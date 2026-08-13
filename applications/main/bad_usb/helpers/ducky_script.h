#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <furi.h>
#include <furi_hal.h>
#include "bad_usb_hid.h"

#ifndef PUBLIC_BUILD
#include "ducky_os_cond.h" // DuckyOs (PRIVATE-only, USB1)
#endif

typedef enum {
    BadUsbStateInit,
    BadUsbStateNotConnected,
    BadUsbStateIdle,
    BadUsbStateWillRun,
    BadUsbStateRunning,
    BadUsbStateDelay,
    BadUsbStateStringDelay,
    BadUsbStateWaitForBtn,
    BadUsbStatePaused,
    BadUsbStateDone,
    BadUsbStateScriptError,
    BadUsbStateFileError,
} BadUsbWorkerState;

typedef struct {
    BadUsbWorkerState state;
    size_t line_cur;
    size_t line_nb;
    uint32_t delay_remain;
    size_t error_line;
    char error[64];
    uint32_t elapsed;
} BadUsbState;

typedef struct BadUsbScript BadUsbScript;

BadUsbScript* bad_usb_script_open(
    FuriString* file_path,
    BadUsbHidInterface* interface,
    BadUsbHidConfig* hid_cfg,
    bool load_id_cfg);

void bad_usb_script_close(BadUsbScript* bad_usb);

void bad_usb_script_set_keyboard_layout(BadUsbScript* bad_usb, FuriString* layout_path);

#ifndef PUBLIC_BUILD
// Operator-selected default target OS for IF_OS/ELSE_OS/END_IF/SET_OS blocks.
// Applied on the next script (re)start. PRIVATE-only (USB1).
void bad_usb_script_set_target_os(BadUsbScript* bad_usb, DuckyOs target_os);
#endif

void bad_usb_script_start(BadUsbScript* bad_usb);

void bad_usb_script_stop(BadUsbScript* bad_usb);

void bad_usb_script_start_stop(BadUsbScript* bad_usb);

void bad_usb_script_pause_resume(BadUsbScript* bad_usb);

BadUsbState* bad_usb_script_get_state(BadUsbScript* bad_usb);

#ifdef __cplusplus
}
#endif
