// Derived in part from Azahar src/core/frontend/emu_window.cpp, src/core/frontend/framebuffer_layout.h, src/core/frontend/emu_window.h and src/core/frontend/top_screen.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later): the use of EmuWindow::UpdateCurrentFramebufferLayout, TouchPressed, TouchMoved and TouchReleased, and of the layout structures, that the oracle runs on Azahar's own code.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (emu_window.cpp, emu_window.h)
// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project (framebuffer_layout.h)
// Copyright 2026 Azahar Emulator Project (top_screen.h, the port's patch)
//
// The driver of the layout oracle (tools/native/layout_oracle/layout_oracle.py): it is compiled
// with Azahar's framebuffer_layout.cpp and emu_window.cpp (patched with the port's hunks), reads
// the cases the script wrote on standard input, makes the window Azahar makes for each, and prints
// what Azahar's code computes: the layout for the window size, and the touch point after each step
// of the case's touches. No value is computed here. The format is in layout_oracle.py.
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "common/settings.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/frontend/input.h"
#include "core/frontend/top_screen.h"

namespace {

class TestWindow final : public Frontend::EmuWindow {
public:
    TestWindow() : Frontend::EmuWindow(false) {}
    void PollEvents() override {}
};

float F(const std::string& token) {
    return std::strtof(token.c_str(), nullptr);
}

std::string Hex(float value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%a", static_cast<double>(value));
    return buffer;
}

std::vector<std::string> Split(const std::string& text, char separator) {
    std::vector<std::string> out;
    std::string item;
    std::istringstream stream(text);
    while (std::getline(stream, item, separator)) {
        out.push_back(item);
    }
    return out;
}

Settings::LayoutOption ModeOption(const std::string& mode) {
    if (mode == "overlay") {
        return Settings::LayoutOption::CustomLayout;
    }
    if (mode == "single") {
        return Settings::LayoutOption::SingleScreen;
    }
    if (mode == "side") {
        return Settings::LayoutOption::SideScreen;
    }
    return Settings::LayoutOption::Default; // stacked
}

Settings::AspectRatio AspectOption(const std::string& name) {
    if (name == "stretch") {
        return Settings::AspectRatio::Stretch;
    }
    if (name == "4:3") {
        return Settings::AspectRatio::R4_3;
    }
    if (name == "16:9") {
        return Settings::AspectRatio::R16_9;
    }
    if (name == "16:10") {
        return Settings::AspectRatio::R16_10;
    }
    if (name == "21:9") {
        return Settings::AspectRatio::R21_9;
    }
    return Settings::AspectRatio::Default;
}

void PrintRect(const Common::Rectangle<u32>& r) {
    std::printf(" %u %u %u %u", r.left, r.top, r.right, r.bottom);
}

void PrintCrop(const Common::Rectangle<float>& r) {
    std::printf(" %s %s %s %s", Hex(r.left).c_str(), Hex(r.top).c_str(), Hex(r.right).c_str(),
                Hex(r.bottom).c_str());
}

void RunCase(const std::vector<std::string>& t) {
    // case <id> <mode> <w> <h> <top_width> <aspect> <corner> <height> <opacity> <cl> <ct> <cr> <cb>
    //      <x> <y> <view0: height x y cl ct cr cb> <view1: ...> <ops>
    std::size_t i = 1;
    const std::string id = t[i++];
    const std::string mode = t[i++];
    const unsigned width = static_cast<unsigned>(std::stoul(t[i++]));
    const unsigned height = static_cast<unsigned>(std::stoul(t[i++]));
    const u32 top_width = static_cast<u32>(std::stoul(t[i++]));
    const std::string aspect = t[i++];

    Layout::ScreenOverlay overlay;
    overlay.corner = static_cast<Layout::ScreenOverlay::Corner>(std::stoi(t[i++]));
    overlay.height = F(t[i++]);
    overlay.opacity = F(t[i++]);
    overlay.crop_left = F(t[i++]);
    overlay.crop_top = F(t[i++]);
    overlay.crop_right = F(t[i++]);
    overlay.crop_bottom = F(t[i++]);
    overlay.x = F(t[i++]);
    overlay.y = F(t[i++]);
    for (auto& view : overlay.views) {
        view.height = F(t[i++]);
        view.x = F(t[i++]);
        view.y = F(t[i++]);
        view.crop_left = F(t[i++]);
        view.crop_top = F(t[i++]);
        view.crop_right = F(t[i++]);
        view.crop_bottom = F(t[i++]);
    }
    const std::string ops = t[i++];

    Settings::values.layout_option = ModeOption(mode);
    Settings::values.aspect_ratio = AspectOption(aspect);
    Layout::SetTopScreenWidth(top_width);
    Layout::SetScreenOverlay(overlay);

    {
        TestWindow window;
        window.UpdateCurrentFramebufferLayout(width, height, false);
        const auto& layout = window.GetFramebufferLayout();
        std::printf("layout %s %u %u %d %d", id.c_str(), layout.width, layout.height,
                    layout.top_screen_enabled ? 1 : 0, layout.bottom_screen_enabled ? 1 : 0);
        PrintRect(layout.top_screen);
        PrintRect(layout.bottom_screen);
        std::printf(" %s %s", Hex(layout.top_opacity).c_str(), Hex(layout.bottom_opacity).c_str());
        PrintCrop(layout.bottom_crop);
        std::printf(" %u", layout.bottom_view_count);
        for (u32 v = 0; v < layout.bottom_view_count; ++v) {
            PrintRect(layout.bottom_views[v].screen);
            PrintCrop(layout.bottom_views[v].crop);
        }
        std::printf("\n");

        if (ops != "-") {
            auto device = Input::CreateDevice<Input::TouchDevice>("emu_window");
            int step = 0;
            for (const auto& op : Split(ops, ';')) {
                const auto parts = Split(op, ',');
                int handled = -1;
                if (parts[0] == "P") {
                    handled = window.TouchPressed(static_cast<unsigned>(std::stoul(parts[1])),
                                                  static_cast<unsigned>(std::stoul(parts[2])))
                                  ? 1
                                  : 0;
                } else if (parts[0] == "M") {
                    window.TouchMoved(static_cast<unsigned>(std::stoul(parts[1])),
                                      static_cast<unsigned>(std::stoul(parts[2])));
                } else {
                    window.TouchReleased();
                }
                // what the HID module makes of the touch state (hid.cpp, Module::UpdatePadCallback)
                const auto [x, y, pressed] = device->GetStatus();
                const u16 px = static_cast<u16>(x * Core::kScreenBottomWidth);
                const u16 py = static_cast<u16>(y * Core::kScreenBottomHeight);
                std::printf("step %s %d %s %d %d %s %s %u %u\n", id.c_str(), step++, parts[0].c_str(),
                            handled, pressed ? 1 : 0, Hex(x).c_str(), Hex(y).c_str(), px, py);
            }
        }
    }
    // the next case starts with a touch state of its own
    Input::Impl::factories<Input::TouchDevice>.clear();
}

} // namespace

int main() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto tokens = Split(line, ' ');
        if (tokens[0] != "case") {
            std::fprintf(stderr, "unknown line: %s\n", line.c_str());
            return 2;
        }
        RunCase(tokens);
    }
    return 0;
}
