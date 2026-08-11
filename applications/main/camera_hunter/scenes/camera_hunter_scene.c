#include "camera_hunter_scene.h"

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_enter,
void (*const camera_hunter_scene_on_enter_handlers[])(void*) = {
#include "camera_hunter_scene_config.h"
};
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_event,
bool (*const camera_hunter_scene_on_event_handlers[])(void* context, SceneManagerEvent event) = {
#include "camera_hunter_scene_config.h"
};
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_exit,
void (*const camera_hunter_scene_on_exit_handlers[])(void* context) = {
#include "camera_hunter_scene_config.h"
};
#undef ADD_SCENE

const SceneManagerHandlers camera_hunter_scene_handlers = {
    .on_enter_handlers = camera_hunter_scene_on_enter_handlers,
    .on_event_handlers = camera_hunter_scene_on_event_handlers,
    .on_exit_handlers = camera_hunter_scene_on_exit_handlers,
    .scene_num = CameraHunterSceneNum,
};
