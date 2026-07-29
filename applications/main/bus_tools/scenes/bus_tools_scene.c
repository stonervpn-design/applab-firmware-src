#include "bus_tools_scene.h"

// Generate scene on_enter handlers array
#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_enter,
void (*const bus_tools_scene_on_enter_handlers[])(void*) = {
#include "bus_tools_scene_config.h"
};
#undef ADD_SCENE

// Generate scene on_event handlers array
#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_event,
bool (*const bus_tools_scene_on_event_handlers[])(void* context, SceneManagerEvent event) = {
#include "bus_tools_scene_config.h"
};
#undef ADD_SCENE

// Generate scene on_exit handlers array
#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_exit,
void (*const bus_tools_scene_on_exit_handlers[])(void* context) = {
#include "bus_tools_scene_config.h"
};
#undef ADD_SCENE

// Initialize scene handlers configuration structure
const SceneManagerHandlers bus_tools_scene_handlers = {
    .on_enter_handlers = bus_tools_scene_on_enter_handlers,
    .on_event_handlers = bus_tools_scene_on_event_handlers,
    .on_exit_handlers = bus_tools_scene_on_exit_handlers,
    .scene_num = BusToolsSceneNum,
};
