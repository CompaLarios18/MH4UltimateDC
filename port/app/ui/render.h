// What the Azahar app's windows call to draw the in-game menu (ui/render_gl.cpp, render_sw.cpp;
// render_vk.cpp registers itself through layer.h): the menu over each presented frame, on the
// window's present thread. Used by window_*.cpp and ui/render_*.cpp only; mh4u-native draws the
// menu with its own compositor and does not have these.
#pragma once

#include "util/types.h"

struct SDL_Surface;

namespace App::Ui {

/// OpenGL, on the window's present thread with its context current: the menu's renderer before
/// the first frame, the menu over each presented frame (`width` x `height` pixels, before the
/// swap) and the renderer again before the thread ends.
void InitOpenGL();
void DrawOpenGL(u32 width, u32 height);
void ShutdownOpenGL();

/// The software window, on its present thread: the menu drawn into the window's surface, and
/// the renderer it used released before the thread ends.
void DrawSoftware(SDL_Surface* surface);
void ShutdownSoftware();

} // namespace App::Ui
