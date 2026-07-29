// 4-screen UI (Face/Nets/Peers/Stats) + rename. D-pad driven, 128x64 mono.
#pragma once

#include "fg_app.h"

const char* fg_ui_face(FgApp* app, uint32_t now, const char** mood);
void fg_ui_draw(Canvas* canvas, void* model); // model = FgApp**
bool fg_ui_input(InputEvent* event, void* ctx); // ctx = FgApp*
