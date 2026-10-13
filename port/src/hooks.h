// Native code around game functions: the port's hooks on the hook points that the code
// library was recompiled with (config/hooks.txt).
#pragma once

#include <string>
#include <vector>

#include "core/frontend/top_screen.h"

struct RCodeInfo;

namespace Port::Hooks {

/// Installs the port's hooks on a freshly loaded code library. Options come from
/// "mh4u.ini" in Azahar's user directory (`key = value` lines: fps, widescreen,
/// trace_frames), and environment variables override them (MH4U_FPS, MH4U_WIDESCREEN,
/// MH4U_TRACE_FRAMES). Returns lines for the log: options in effect and hooks the library
/// has no hook point for (it was built with an older hooks.txt).
std::vector<std::string> Install(const RCodeInfo& info);

/// The touch_overlay option: "top-right 30 85" (corner, height in % of the window's, opacity
/// in %), "on" (top right, 30%, 85%) or "off".
Layout::ScreenOverlay ParseOverlay(std::string value);

} // namespace Port::Hooks
