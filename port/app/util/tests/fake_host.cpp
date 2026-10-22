// The fake host's services (fake_host.h): see there.

#include "util/tests/fake_host.h"

namespace fake {

void HostState::Reset() {
    *this = HostState{};
    scancodes = {{"W", SCANCODE_W}, {"F", SCANCODE_F}, {"F12", SCANCODE_F12}};
}

HostState& Host() {
    static HostState state = [] {
        HostState fresh;
        fresh.Reset();
        return fresh;
    }();
    return state;
}

} // namespace fake

namespace App::Host {

std::uint32_t Ticks() {
    return fake::Host().ticks;
}

std::string GetClipboard() {
    return fake::Host().clipboard;
}

void SetClipboard(const std::string& text) {
    fake::Host().clipboard = text;
}

void ShowCursor(bool show) {
    fake::Host().cursor_shown = show;
}

Point MousePosition() {
    return fake::Host().mouse;
}

int ScancodeFromName(const std::string& name) {
    for (const auto& [known, code] : fake::Host().scancodes) {
        if (known == name) {
            return code;
        }
    }
    return 0;
}

std::string ScancodeName(int scancode) {
    for (const auto& [known, code] : fake::Host().scancodes) {
        if (code == scancode) {
            return known;
        }
    }
    return "";
}

void ShowMessageBox(const std::string& title, const std::string& text, bool error, IWindow* parent) {
    fake::Host().boxes.push_back({title, text, error, parent != nullptr});
}

std::vector<ControllerInfo> ListControllers() {
    return fake::Host().controllers;
}

void SetWindowIcon(IWindow&) {
    ++fake::Host().icons_set;
}

} // namespace App::Host
