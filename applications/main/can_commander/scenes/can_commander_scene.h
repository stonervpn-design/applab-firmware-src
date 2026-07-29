#pragma once

#include <gui/scene_manager.h>

#define ADD_SCENE(prefix, name, id) CanCommanderScene##id,
typedef enum {
#include "can_commander_scene_config.h"
    CanCommanderSceneNum,
} CanCommanderScene;
#undef ADD_SCENE

extern const SceneManagerHandlers can_commander_scene_handlers;

#define ADD_SCENE(prefix, name, id) void prefix##_scene_##name##_on_enter(void*);
#include "can_commander_scene_config.h"
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) \
    bool prefix##_scene_##name##_on_event(void* context, SceneManagerEvent event);
#include "can_commander_scene_config.h"
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) void prefix##_scene_##name##_on_exit(void* context);
#include "can_commander_scene_config.h"
#undef ADD_SCENE
