#include "can_commander_scene.h"

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_enter,
void (*const can_commander_scene_on_enter_handlers[])(void*) = {
#include "can_commander_scene_config.h"
};
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_event,
bool (*const can_commander_scene_on_event_handlers[])(void* context, SceneManagerEvent event) = {
#include "can_commander_scene_config.h"
};
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_exit,
void (*const can_commander_scene_on_exit_handlers[])(void* context) = {
#include "can_commander_scene_config.h"
};
#undef ADD_SCENE

const SceneManagerHandlers can_commander_scene_handlers = {
    .on_enter_handlers = can_commander_scene_on_enter_handlers,
    .on_event_handlers = can_commander_scene_on_event_handlers,
    .on_exit_handlers = can_commander_scene_on_exit_handlers,
    .scene_num = CanCommanderSceneNum,
};
