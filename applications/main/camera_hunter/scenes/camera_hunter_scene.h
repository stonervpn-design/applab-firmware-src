#pragma once

#include <gui/scene_manager.h>

#define ADD_SCENE(prefix, name, id) CameraHunterScene##id,
typedef enum {
#include "camera_hunter_scene_config.h"
    CameraHunterSceneNum,
} CameraHunterScene;
#undef ADD_SCENE

extern const SceneManagerHandlers camera_hunter_scene_handlers;

#define ADD_SCENE(prefix, name, id) void prefix##_scene_##name##_on_enter(void*);
#include "camera_hunter_scene_config.h"
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) \
    bool prefix##_scene_##name##_on_event(void* context, SceneManagerEvent event);
#include "camera_hunter_scene_config.h"
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) void prefix##_scene_##name##_on_exit(void* context);
#include "camera_hunter_scene_config.h"
#undef ADD_SCENE
