#include "ui/hint.h"

#include <cmath>
#include <imgui.h>

#include "common/settings.h"
#include "config.h"
#include "hooks.h"
#include "ui/menu.h"

namespace App::Ui {

namespace {

namespace NativeButton = Settings::NativeButton;

/// mh4u.ini's 3DS button names (port/src/hooks.cpp, PAD_BITS) -> the app's buttons.
int ButtonIndex(const std::string& name) {
    static constexpr std::pair<const char*, int> buttons[] = {
        {"A", NativeButton::A},         {"B", NativeButton::B},       {"X", NativeButton::X},
        {"Y", NativeButton::Y},         {"L", NativeButton::L},       {"R", NativeButton::R},
        {"ZL", NativeButton::ZL},       {"ZR", NativeButton::ZR},     {"START", NativeButton::Start},
        {"UP", NativeButton::Up},       {"DOWN", NativeButton::Down}, {"LEFT", NativeButton::Left},
        {"RIGHT", NativeButton::Right}};
    for (const auto& [n, index] : buttons) {
        if (name == n) {
            return index;
        }
    }
    return -1;
}

} // namespace

std::string HintButtons(const Config& config) {
    if (!Port::Hooks::SkipOffered() || Port::Hooks::SkipHint() == 0) {
        return {};
    }
    const std::string button = Port::Hooks::SkipButton();
    const int index = ButtonIndex(button);
    if (index < 0) {
        return {};
    }
    // a button given full input parameters has no key of the app's
    std::string key = config.button_params[index].empty() ? config.keys[index] : "";
    const std::string pad = ControllerInputName(Settings::values.current_input_profile.buttons[index]);
    if (key.empty()) {
        key = pad;
    } else if (!pad.empty()) {
        key += " / " + pad;
    }
    return key.empty() ? "the 3DS's " + button : key;
}

void DrawHint(const std::string& buttons, bool flash) {
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec4 amber(0.92f, 0.63f, 0.20f, 1.0f);
    // a slow pulse of the frame (and a little of the background) when it flashes
    const float pulse = flash ? 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 5.0f) : 0.0f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.88f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.55f + 0.2f * pulse);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(amber.x, amber.y, amber.z, flash ? 0.25f + 0.75f * pulse : 0.45f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, flash ? 2.0f : 1.0f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##hint", nullptr, flags)) {
        ImGui::TextUnformatted("Press ");
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(amber, "%s", buttons.c_str());
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextUnformatted(" to skip");
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

} // namespace App::Ui
