#pragma once

typedef enum {
    CanCommanderEvtSend = 100,
    CanCommanderEvtPeriodic,
    CanCommanderEvtMonitor,
    CanCommanderEvtPresets,
    CanCommanderEvtSettings,
    CanCommanderEvtFrameEntered, // text input committed a frame string
    CanCommanderEvtPresetNameEntered, // text input committed a preset name
    CanCommanderEvtMonitorTick, // refresh monitor view
} CanCommanderCustomEvent;
