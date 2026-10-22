// A fake host for the app's unit tests (input_test.cpp, keyboard_test.cpp, ...): the services of
// app_window.h's Host, answering from what the test sets and recording what the app asks, with
// synthetic data. fake_host.cpp defines them, so the test programs link no SDL, and not even its
// headers.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "app_window.h"

namespace fake {

/// SDL2's scancodes of the few keys the tests name.
inline constexpr int SCANCODE_F = 9, SCANCODE_W = 26, SCANCODE_F12 = 69;

struct HostState {
    // what the tests set
    std::uint32_t ticks = 1000;                          ///< Host::Ticks
    std::vector<App::ControllerInfo> controllers;        ///< Host::ListControllers
    std::vector<std::pair<std::string, int>> scancodes;  ///< Host::ScancodeFromName / ScancodeName
    std::string clipboard;                               ///< Host::GetClipboard / SetClipboard
    App::Point mouse;                                    ///< Host::MousePosition

    // what the app did
    bool cursor_shown = true;
    struct Box {
        std::string title, text;
        bool error = false;
        bool had_parent = false;
    };
    std::vector<Box> boxes;
    int icons_set = 0;

    /// Back to the start: the clock at 1000, no controllers, the keys W, F and F12.
    void Reset();
};

HostState& Host();

} // namespace fake
