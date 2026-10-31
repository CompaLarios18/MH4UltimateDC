// The in-game menu's tabs but Keys: what each row shows and how it changes its setting (the same
// key, value and way of applying as before the tabs were reorganized).
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <string>
#include <tuple>
#include <vector>
#include <fmt/format.h>
#include <imgui.h>

#include "common/settings.h"
#include "config.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "hooks.h"
#include "input.h"
#include "speed.h"
#include "ui/fps_counter.h"
#include "ui/key_guides.h"
#include "ui/layer.h"
#include "ui/menu.h"
#include "ui/menu_parts.h"
#include "ui/multiplayer_tab.h"
#include "ui/nav.h"
#include "ui/touch_taps.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"
#include "window.h"

namespace App::Ui {

using namespace MenuParts;

namespace {

using R = Index::RowId;
using S = Index::SectionId;

using Port::Hooks::PanelView;
using Port::Hooks::Placement;

Settings::LayoutOption LayoutFor(const std::string& screens) {
    if (screens == "single") {
        return Settings::LayoutOption::SingleScreen;
    }
    if (screens == "stacked") {
        return Settings::LayoutOption::Default;
    }
    if (screens == "side") {
        return Settings::LayoutOption::SideScreen;
    }
    if (screens == "windows") {
        return Settings::LayoutOption::SeparateWindows;
    }
    return Settings::LayoutOption::CustomLayout; // the overlay (mh4u.ini's touch_overlay)
}

bool IsMapKey(const std::string& key) {
    return key == "quest_overlay";
}

PanelView ParseView(const std::string& key, const std::string& value) {
    return IsMapKey(key) ? Port::Hooks::ParseQuestOverlay(value) : Port::Hooks::ParseTargetOverlay(value);
}

std::string FormatView(const std::string& key, const PanelView& view) {
    return IsMapKey(key) ? Port::Hooks::FormatQuestOverlay(view) : Port::Hooks::FormatTargetOverlay(view);
}

/// The renderers this build has, as the launcher's Settings lists them.
struct Renderer {
    const char* value;
    const char* label;
};
constexpr Renderer RENDERERS[] = {
#ifdef ENABLE_VULKAN
    {"vulkan", "Vulkan"},
#endif
#ifdef ENABLE_OPENGL
    {"opengl", "OpenGL"},
#endif
#ifdef ENABLE_SOFTWARE_RENDERER
    {"software", "Software (no graphics card)"},
#endif
};

const std::vector<std::string>& ResolutionLabels() {
    static const std::vector<std::string> labels = [] {
        std::vector<std::string> l = {"As large as the window needs"};
        for (int i = 1; i <= 10; ++i) {
            l.push_back(fmt::format("{}x ({}x{})", i, 400 * i, 240 * i));
        }
        return l;
    }();
    return labels;
}

} // namespace

// ── Display: how frames reach your screen ──

void Menu::DrawDisplay() {
    Config& config = layer.config;
    if (!backend_notice.empty()) {
        // R17: the code library isn't the port's, or lacks hook points; the whole notice here
        ImGui::PushStyleColor(ImGuiCol_Text, NOTE_COLOUR);
        ImGui::TextWrapped("%s", backend_notice.c_str());
        ImGui::PopStyleColor();
    }

    Section(S::DisplayFrameRate);
    if (BeginSettings("##frame_rate")) {
        if (BeginRow(R::DisplayFps)) {
            std::string fps = Game("fps");
            if (std::atoi(fps.c_str()) <= 0) {
                fps = "30";
            }
            if (ChooseValue("##fps", fps, {"30", "60", "90", "120", "144", "165", "180", "240"},
                            {"30 fps (as on a 3DS)", "60 fps", "90 fps", "120 fps", "144 fps", "165 fps", "180 fps",
                             "240 fps"})) {
                SetGame("fps", fps);
            }
            EndRow(R::DisplayFps);
        }
        if (BeginRow(R::DisplayVsync)) {
            bool vsync = config.vsync;
            if (ImGui::Checkbox("##vsync", &vsync)) {
                config.vsync = vsync;
                Changed(AppSetting::Vsync);
                Settings::values.use_vsync = vsync;
            }
            EndRow(R::DisplayVsync);
        }
        if (BeginRow(R::DisplayFpsCounter)) {
            std::string counter = FpsCounterOn(config.fps_counter) ? config.fps_counter : "off";
            if (ChooseValue("##fps_counter", counter, {std::begin(FPS_COUNTER_PLACES), std::end(FPS_COUNTER_PLACES)},
                            {"Off", "Top left", "Top right", "Bottom left", "Bottom right"})) {
                config.fps_counter = counter;
                Changed(AppSetting::FpsCounter);
            }
            EndRow(R::DisplayFpsCounter);
        }
        if (BeginRow(R::DisplayCpuClock)) {
            int clock = std::clamp(config.cpu_clock, 5, 400);
            if (SliderInt("##cpu_clock", clock, 50, 400, "%d%%", 5)) {
                config.cpu_clock = clock;
                Changed(AppSetting::CpuClock);
                layer.Post([&system = layer.system, clock] {
                    Settings::values.cpu_clock_percentage = static_cast<u32>(clock);
                    system.CoreTiming().UpdateClockSpeed(static_cast<u32>(clock));
                });
            }
            EndRow(R::DisplayCpuClock);
        }
        if (BeginRow(R::DisplayRefreshRate)) {
            std::string hz = Game("refresh_rate");
            if (std::atof(hz.c_str()) <= 0.0) {
                hz = "0";
            }
            if (ChooseValue("##hz", hz, {"0", "60", "120", "144", "165", "240"},
                            {"Auto (the frame rate above 60)", "60 Hz", "120 Hz", "144 Hz", "165 Hz", "240 Hz"})) {
                SetGame("refresh_rate", hz);
            }
            EndRow(R::DisplayRefreshRate);
        }
        EndSettings();
    }

    Section(S::DisplayWindow);
    if (BeginSettings("##window")) {
        if (BeginRow(R::DisplayFullscreen)) {
            bool full = layer.window.IsFullscreen();
            if (ImGui::Checkbox("##fullscreen", &full)) {
                config.fullscreen = full;
                Changed(AppSetting::Fullscreen);
                layer.Post([&window = layer.window, full] { window.SetFullscreen(full); });
            }
            EndRow(R::DisplayFullscreen);
        }
        if (BeginRow(R::DisplayRenderer)) {
            std::string renderer = Lower(config.renderer);
            std::vector<std::string> values, labels;
            for (const Renderer& r : RENDERERS) {
                values.push_back(r.value);
                labels.push_back(r.label);
            }
            if (ChooseValue("##renderer", renderer, values, labels)) {
                config.renderer = renderer;
                Changed(AppSetting::Renderer);
            }
            EndRow(R::DisplayRenderer);
        }
        if (BeginRow(R::DisplayLauncher)) {
            bool launcher = config.launcher;
            if (ImGui::Checkbox("##launcher", &launcher)) {
                config.launcher = launcher;
                Changed(AppSetting::Launcher);
            }
            EndRow(R::DisplayLauncher);
        }
        EndSettings();
    }

    Section(S::DisplayWidescreen);
    if (BeginSettings("##widescreen")) {
        if (BeginRow(R::DisplayAspect)) {
            std::string shape = Lower(Game("widescreen"));
            if (shape.empty() || IsOff(shape)) {
                shape = "off";
            }
            if (ChooseValue("##widescreen", shape, {"off", "16:9", "16:10", "21:9", "32:9", "4:3"},
                            {"5:3, as on a 3DS", "16:9", "16:10", "21:9", "32:9", "4:3"})) {
                SetGame("widescreen", shape);
            }
            EndRow(R::DisplayAspect);
        }
        if (BeginRow(R::DisplayWideRender)) {
            bool wide = IsOn(Game("wide_render"));
            if (ImGui::Checkbox("##wide_render", &wide)) {
                SetGame("wide_render", wide ? "1" : "0");
            }
            EndRow(R::DisplayWideRender);
        }
        if (BeginRow(R::DisplayHudEdges)) {
            bool edges = IsOn(Game("hud_edges"));
            if (ImGui::Checkbox("##hud_edges", &edges)) {
                SetGame("hud_edges", edges ? "1" : "0");
            }
            EndRow(R::DisplayHudEdges);
        }
        LinkRow("The screens and the touch screen's corner", Tab::TouchScreen,
                "Opens Touch screen: where the 3DS's two screens go, the touch screen's corner and the quest map.",
                "display_touch");
        EndSettings();
    }
}

// ── Graphics: how the picture looks ──

void Menu::DrawGraphics() {
    Config& config = layer.config;
    BackendLine();

    Section(S::GraphicsQuality);
    if (BeginSettings("##quality")) {
        if (BeginRow(R::GraphicsResolution)) {
            int resolution = std::clamp(config.resolution, 0, 10);
            if (Choose("##resolution", resolution, ResolutionLabels())) {
                config.resolution = resolution;
                Changed(AppSetting::Resolution);
                Settings::values.resolution_factor = static_cast<u32>(resolution);
            }
            EndRow(R::GraphicsResolution);
        }
        if (BeginRow(R::GraphicsLinearFilter)) {
            bool linear = config.linear_filter;
            if (ImGui::Checkbox("##linear", &linear)) {
                config.linear_filter = linear;
                Changed(AppSetting::LinearFilter);
                Settings::values.filter_mode = linear;
            }
            EndRow(R::GraphicsLinearFilter);
        }
        if (BeginRow(R::GraphicsAntialiasing)) {
            bool aa = !IsOff(Lower(Game("antialiasing"))); // on unless the file says off
            if (ImGui::Checkbox("##antialiasing", &aa)) {
                SetGame("antialiasing", aa ? "on" : "off");
            }
            EndRow(R::GraphicsAntialiasing);
        }
        if (BeginRow(R::GraphicsDither)) {
            bool dither = IsOn(Game("dither")); // on only when the file says on
            if (ImGui::Checkbox("##dither", &dither)) {
                SetGame("dither", dither ? "on" : "off");
            }
            EndRow(R::GraphicsDither);
        }
        if (BeginRow(R::GraphicsStereo3d)) {
            bool stereo = IsOn(Game("stereo_3d"));
            if (ImGui::Checkbox("##stereo_3d", &stereo)) {
                SetGame("stereo_3d", stereo ? "on" : "off");
            }
            EndRow(R::GraphicsStereo3d);
        }
        if (BeginRow(R::GraphicsCustomTextures)) {
            // only the setting: Settings::values and New 3DS mode (auto) follow it at the next start
            bool textures = config.custom_textures;
            if (ImGui::Checkbox("##custom_textures", &textures)) {
                config.custom_textures = textures;
                Changed(AppSetting::CustomTextures);
            }
            EndRow(R::GraphicsCustomTextures);
        }
        EndSettings();
    }

    Section(S::GraphicsEffects);
    if (BeginSettings("##effects")) {
        // the game's screen filters: on unless the file says off
        for (const auto& [row, key] : {std::pair{R::GraphicsBloom, "bloom"}, std::pair{R::GraphicsDepthOfField, "depth_of_field"},
                                       std::pair{R::GraphicsMotionBlur, "motion_blur"},
                                       std::pair{R::GraphicsRadialBlur, "radial_blur"}, std::pair{R::GraphicsHaze, "haze"}}) {
            if (BeginRow(row)) {
                bool filter = !IsOff(Lower(Game(key)));
                if (ImGui::Checkbox((std::string("##") + key).c_str(), &filter)) {
                    SetGame(key, filter ? "on" : "off");
                }
                EndRow(row);
            }
        }
        EndSettings();
    }
}

// ── Touch screen: where the screens go and what the touch screen shows ──

void Menu::DrawTouchScreen() {
    Config& config = layer.config;
    BackendLine();

    Section(S::TouchScreens);
    if (BeginSettings("##screens")) {
        if (BeginRow(R::TouchScreens)) {
            // a window of its own for the touch screen exists only from the start (R20); the
            // software renderer makes none (R7)
            const bool windows_now = facts.windows_now;
            const bool no_windows = verdict.windows_entry_disabled;
            std::string screens = config.screens;
            if (ChooseValue("##screens", screens, {"overlay", "single", "stacked", "side", "windows"},
                            {"The game, the touch screen in a corner", "The game alone", "Stacked, as on a 3DS",
                             "Side by side",
                             no_windows ? "The touch screen in a window of its own (not with the software renderer)"
                                        : "The touch screen in a window of its own (next start)"},
                            no_windows ? 4 : -1)) {
                config.screens = screens;
                Changed(AppSetting::Screens);
                if (!windows_now && screens != "windows") {
                    layer.Post([&system = layer.system, screens] {
                        Settings::values.layout_option = LayoutFor(screens);
                        Settings::values.swap_screen = false;
                        Settings::values.upright_screen = false;
                        system.GPU().Renderer().UpdateCurrentFramebufferLayout();
                    });
                }
            }
            EndRow(R::TouchScreens);
        }
        EndSettings();
    }

    Section(S::TouchCorner);
    if (BeginSettings("##corner")) {
        Layout::ScreenOverlay overlay = Port::Hooks::ParseOverlay(Game("touch_overlay"));
        bool on = overlay.height > 0.0f;
        const auto write = [&] {
            static constexpr const char* corners[] = {"top-left", "top-right", "bottom-left", "bottom-right"};
            SetGame("touch_overlay",
                    on ? fmt::format("{} {} {}", corners[static_cast<int>(overlay.corner)],
                                     std::lround(overlay.height * 100), std::lround(overlay.opacity * 100))
                       : "off");
        };
        if (BeginRow(R::TouchOverlay)) {
            if (ImGui::Checkbox("##touch_overlay", &on)) {
                if (on) {
                    overlay = Port::Hooks::ParseOverlay("on");
                }
                write();
            }
            EndRow(R::TouchOverlay);
        }
        if (BeginRow(R::TouchCorner)) {
            int corner = static_cast<int>(overlay.corner);
            if (Choose("##corner", corner, {"Top left", "Top right", "Bottom left", "Bottom right"})) {
                overlay.corner = static_cast<Layout::ScreenOverlay::Corner>(corner);
                write();
            }
            EndRow(R::TouchCorner);
        }
        if (BeginRow(R::TouchSize)) {
            int height = static_cast<int>(std::lround(overlay.height * 100));
            if (SliderInt("##size", height, 5, 100, "%d%% of the window's height", 1)) {
                overlay.height = static_cast<float>(height) / 100.0f;
                write();
            }
            EndRow(R::TouchSize);
        }
        if (BeginRow(R::TouchOpacity)) {
            int opacity = static_cast<int>(std::lround(overlay.opacity * 100));
            if (SliderInt("##opacity", opacity, 10, 100, "%d%%", 5)) {
                overlay.opacity = static_cast<float>(opacity) / 100.0f;
                write();
            }
            EndRow(R::TouchOpacity);
        }
        EndSettings();
    }

    Section(S::TouchQuests);
    if (BeginSettings("##quests")) {
        if (BeginRow(R::TouchMapOnly)) {
            auto map = Port::Hooks::ParseQuestOverlay(Game("quest_overlay"));
            if (ImGui::Checkbox("##quest_overlay", &map.on)) {
                SetGame("quest_overlay", map.on ? "map" : "off");
            }
            EndRow(R::TouchMapOnly);
        }
        if (BeginRow(R::TouchMapKey)) {
            DrawContextKeyRow(R::TouchMapKey, BindingOf(R::KeysTouchScreen));
            EndRow(R::TouchMapKey, false);
        }
        if (BeginRow(R::TouchHudEditor)) {
            // with the overlay running (R1, R2, R3 say why not)
            if (ImGui::Button("Edit HUD layout", ImVec2(-FLT_MIN, 0.0f))) {
                StartLayoutEdit();
            }
            EndRow(R::TouchHudEditor);
        }
        DrawPanelViewRows("quest_overlay", R::TouchMapSize, R::TouchMapWhere, R::TouchMapX, R::TouchMapY);
        if (BeginRow(R::TouchTargetView)) {
            auto target = Port::Hooks::ParseTargetOverlay(Game("target_overlay"));
            if (ImGui::Checkbox("##target_overlay", &target.on)) {
                SetGame("target_overlay", target.on ? "on" : "off");
            }
            EndRow(R::TouchTargetView);
        }
        DrawPanelViewRows("target_overlay", R::TouchTargetSize, R::TouchTargetWhere, R::TouchTargetX,
                          R::TouchTargetY);
        LinkRow("Buttons for the kick and target camera, and touch taps", Tab::Gameplay,
                "Opens Gameplay: the kick and the target camera on buttons, the end of quests, fast forward and "
                "touch taps.",
                "touch_gameplay");
        EndSettings();
    }
}

void Menu::DrawPanelViewRows(const char* key, RowId size_row, RowId where_row, RowId x_row, RowId y_row) {
    auto view = ParseView(key, Game(key));
    const auto touch = Port::Hooks::ParseOverlay(Game("touch_overlay"));
    const auto map = Port::Hooks::ParseQuestOverlay(Game("quest_overlay"));
    const Placement now = IsMapKey(key) ? Port::Hooks::MapPlacement(touch, view)
                                        : Port::Hooks::TargetPlacement(touch, map, view);
    const std::string id = std::string("##") + key;
    const auto set = [&] { SetGame(key, FormatView(key, view)); };

    if (BeginRow(size_row)) {
        float percent = view.height * 100.0f;
        bool changed = ImGui::SliderFloat((id + "_size").c_str(), &percent, 0.0f, 100.0f,
                                          percent < 1.0f ? "the touch screen's scale" : "%.0f%% of the window's height");
        if (ImGui::IsItemActive()) {
            slider_active = true;
        }
        // left and right: 0, then 3 to 100 by 1 (left from 3 goes back to 0)
        if (const int step = Nav::Step()) {
            const int at = static_cast<int>(std::lround(percent));
            const int next = step > 0 ? (at < 3 ? 3 : std::min(100, at + 1)) : (at <= 3 ? 0 : at - 1);
            if (next != at) {
                percent = static_cast<float>(next);
                changed = true;
            }
        }
        if (changed) {
            view.height = percent < 1.0f ? 0.0f : std::max(percent, 3.0f) / 100.0f;
            set();
        }
        EndRow(size_row);
    }
    if (BeginRow(where_row)) {
        int where = view.placed ? 1 : 0;
        if (Choose((id + "_where").c_str(), where,
                   {IsMapKey(key) ? "In the touch screen's corner" : "Under the map (over it at the bottom)",
                    "Where you put it"})) {
            view.placed = where == 1;
            view.x = now.x; // from where it is now
            view.y = now.y;
            set();
        }
        EndRow(where_row);
    }
    if (BeginRow(x_row)) {
        float x = view.x * 100.0f;
        if (SliderFloat((id + "_x").c_str(), x, 0.0f, 100.0f, "%.0f%% (0 left, 100 right)", 1.0f)) {
            view.x = x / 100.0f;
            set();
        }
        EndRow(x_row);
    }
    if (BeginRow(y_row)) {
        float y = view.y * 100.0f;
        if (SliderFloat((id + "_y").c_str(), y, 0.0f, 100.0f, "%.0f%% (0 top, 100 bottom)", 1.0f)) {
            view.y = y / 100.0f;
            set();
        }
        EndRow(y_row);
    }
}

// ── Controls: how you play ──

void Menu::DrawControls() {
    Config& config = layer.config;

    Section(S::ControlsInput);
    if (BeginSettings("##input")) {
        if (BeginRow(R::ControlsInputMode)) {
            std::string mode = Input::ModeName(Input::ParseMode(config.input_mode));
            if (ChooseValue("##input_mode", mode, {"auto", "keyboard_mouse", "controller"},
                            {"Whatever I use (auto)", "Keyboard and mouse", "Controller"})) {
                config.input_mode = mode;
                Changed(AppSetting::Controls);
            }
            EndRow(R::ControlsInputMode);
        }
        if (BeginRow(R::ControlsKeyGuides)) {
            std::string guides = FpsCounterOn(config.key_guides) ? config.key_guides : "off";
            if (ChooseValue("##key_guides", guides, {std::begin(KEY_GUIDES_PLACES), std::end(KEY_GUIDES_PLACES)},
                            {"Off", "Top left", "Top right", "Bottom left", "Bottom right"})) {
                config.key_guides = guides;
                Changed(AppSetting::Controls);
            }
            EndRow(R::ControlsKeyGuides);
        }
        EndSettings();
    }

    Section(S::ControlsMouse);
    if (BeginSettings("##mouse")) {
        if (BeginRow(R::ControlsMouseSensitivity)) {
            if (SliderInt("##mouse_sensitivity", config.mouse_sensitivity, 1, 100, "%d", 1)) {
                Changed(AppSetting::Controls);
            }
            EndRow(R::ControlsMouseSensitivity);
        }
        if (BeginRow(R::ControlsMouseVertical)) {
            if (SliderInt("##mouse_vertical", config.mouse_vertical, 10, 200, "%d%% of left and right", 5)) {
                Changed(AppSetting::Controls);
            }
            EndRow(R::ControlsMouseVertical);
        }
        if (BeginRow(R::ControlsMouseInvertY)) {
            if (ImGui::Checkbox("##mouse_invert_y", &config.mouse_invert_y)) {
                Changed(AppSetting::Controls);
            }
            EndRow(R::ControlsMouseInvertY);
        }
        if (BeginRow(R::ControlsMouseInvertX)) {
            if (ImGui::Checkbox("##mouse_invert_x", &config.mouse_invert_x)) {
                Changed(AppSetting::Controls);
            }
            EndRow(R::ControlsMouseInvertX);
        }
        if (BeginRow(R::ControlsMouseDeadzone)) {
            if (SliderInt("##mouse_deadzone", config.mouse_deadzone, 0, 60, "%d%%", 1)) {
                Changed(AppSetting::Controls);
            }
            EndRow(R::ControlsMouseDeadzone);
        }
        EndSettings();
    }

    Section(S::ControlsController);
    if (BeginSettings("##controller")) {
        if (BeginRow(R::ControlsControllerAuto)) {
            bool controller = config.controller_auto;
            if (ImGui::Checkbox("##controller_auto", &controller)) {
                config.controller_auto = controller;
                ControlsChanged();
            }
            ItemHelp(R::ControlsControllerAuto);
            ImGui::SameLine();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("(%s)", controller_description.c_str());
            ImGui::PopTextWrapPos();
            EndRow(R::ControlsControllerAuto, false);
        }
        LinkRow("Every key and button", Tab::Keys,
                "Opens Keys: every key and mouse button of the game, and what the controller does.", "controls_keys");
        LinkRow("Kick, target camera and touch taps", Tab::Gameplay,
                "Opens Gameplay: the kick and the target camera on buttons, the end of quests, fast forward and "
                "touch taps.",
                "controls_gameplay");
        EndSettings();
    }
}

// ── Gameplay: what the port adds to playing ──

void Menu::DrawButtonList(RowId row, const char* key) {
    if (!BeginRow(row)) {
        return;
    }
    std::string value = Upper(Game(key));
    if (value.empty() || IsOff(Lower(value)) || value == "NONE") {
        value = "OFF";
    }
    if (ChooseValue((std::string("##") + key).c_str(), value, {std::begin(BUTTONS), std::end(BUTTONS)},
                    button_labels)) {
        SetGame(key, Lower(value) == "off" ? "off" : value);
    }
    EndRow(row);
}

void Menu::DrawGameplay() {
    Config& config = layer.config;
    BackendLine();

    Section(S::GameplayButtons);
    if (BeginSettings("##buttons")) {
        DrawButtonList(R::GameplayKick, "kick");
        DrawButtonList(R::GameplayTarget, "target");
        EndSettings();
    }

    Section(S::GameplayAfterQuest);
    if (BeginSettings("##after_quest")) {
        DrawButtonList(R::GameplaySkip, "skip");
        if (BeginRow(R::GameplaySkipEnd)) {
            std::string combo = Upper(Game("skip_end"));
            if (combo.empty() || IsOff(Lower(combo)) || combo == "NONE") {
                combo = "OFF";
            }
            if (ChooseValue("##skip_end", combo, {std::begin(COMBOS), std::end(COMBOS)}, combo_labels)) {
                SetGame("skip_end", combo == "OFF" ? "off" : combo);
            }
            EndRow(R::GameplaySkipEnd);
        }
        if (BeginRow(R::GameplaySkipEndHold)) {
            float hold = std::strtof(Game("skip_end_hold").c_str(), nullptr);
            hold = hold > 0.0f ? hold : 1.0f;
            if (SliderFloat("##skip_end_hold", hold, 0.2f, 5.0f, "%.1f s", 0.1f)) {
                SetGame("skip_end_hold", fmt::format("{:.1f}", hold));
            }
            EndRow(R::GameplaySkipEndHold);
        }
        if (BeginRow(R::GameplaySkipHint)) {
            std::string hint = Lower(Game("skip_hint"));
            hint = IsOff(hint) ? "off" : hint == "flash" ? "flash" : "on";
            if (ChooseValue("##skip_hint", hint, {"on", "flash", "off"}, {"On", "Flashing", "Off"})) {
                SetGame("skip_hint", hint);
            }
            EndRow(R::GameplaySkipHint);
        }
        EndSettings();
    }

    Section(S::GameplaySpeed);
    if (BeginSettings("##speed")) {
        if (BeginRow(R::GameplayFfSpeed)) {
            std::string fast = std::to_string(config.fast_forward_speed);
            if (ChooseValue("##fast_forward", fast, {"150", "200", "300", "400", "0"},
                            {"1.5x", "2x", "3x", "4x", "As fast as the PC can"})) {
                config.fast_forward_speed = std::atoi(fast.c_str());
                Changed(AppSetting::FastForward);
                Speed::Get().Configure(config.fast_forward_speed, config.fast_forward_hold);
            }
            EndRow(R::GameplayFfSpeed);
        }
        if (BeginRow(R::GameplayFfMode)) {
            // was the checkbox "Fast forward while held": the same setting
            int mode = config.fast_forward_hold ? 1 : 0;
            if (Choose("##fast_forward_hold", mode, {"Press to start, press to stop", "Only while held"})) {
                config.fast_forward_hold = mode == 1;
                Changed(AppSetting::FastForward);
                Speed::Get().Configure(config.fast_forward_speed, config.fast_forward_hold);
            }
            EndRow(R::GameplayFfMode);
        }
        if (BeginRow(R::GameplayFfKey)) {
            DrawContextKeyRow(R::GameplayFfKey, BindingOf(R::KeysFastForward));
            EndRow(R::GameplayFfKey, false);
        }
        if (BeginRow(R::GameplayPauseKey)) {
            DrawContextKeyRow(R::GameplayPauseKey, BindingOf(R::KeysPause));
            EndRow(R::GameplayPauseKey, false);
        }
        EndSettings();
    }

    DrawTouchTaps();
}

void Menu::DrawTouchTaps() {
    // last on Gameplay, full width, outside any settings table (ui/touch_taps.h): the module draws
    // its own title and calls layer.Help after its items, which take the menu's pending focus
    SectionAnchor(S::GameplayTouchTaps); // LT/RT and the search land here
    item = {};
    item.row = R::GameplayTouchTaps; // its items are the row's: a jump to it takes the first
    // Changed, not ControlsChanged: the taps build the input profile again themselves, once a key
    // is set or a point let go (while a point is dragged this is true every frame)
    if (Ui::DrawTouchTaps(layer)) {
        Changed(AppSetting::Controls);
    }
    item = {};
    if (TouchTapCapturing()) {
        capture = -1; // one wait at a time
    }
}

// ── Audio ──

void Menu::DrawAudio() {
    Config& config = layer.config;
    Section(S::AudioSound);
    if (BeginSettings("##audio")) {
        if (BeginRow(R::AudioVolume)) {
            int volume = std::clamp(config.volume, 0, 100);
            if (SliderInt("##volume", volume, 0, 100, "%d%%", 5)) {
                config.volume = volume;
                Changed(AppSetting::Volume);
                Settings::values.volume = static_cast<float>(volume) / 100.0f;
            }
            EndRow(R::AudioVolume);
        }
        if (BeginRow(R::AudioOutput)) {
            std::string output = Lower(config.audio_output);
            if (ChooseValue("##output", output, {"auto", "cubeb", "sdl2", "openal", "null"},
                            {"Automatic", "cubeb", "SDL2", "OpenAL", "None"})) {
                config.audio_output = output;
                Changed(AppSetting::Output);
            }
            EndRow(R::AudioOutput);
        }
        EndSettings();
    }
}

// ── Online: the rooms (ui/multiplayer_tab.cpp), net sync at the top ──

void Menu::DrawOnline() {
    BackendLine();
    DrawMultiplayerTab(
        layer,
        [this] {
            SectionAnchor(S::OnlineNetSync);
            if (BeginSettings("##net_sync")) {
                if (BeginRow(R::OnlineNetSync)) {
                    std::string sync = Lower(Game("net_sync"));
                    sync = IsOff(sync) ? "off" : sync == "all" ? "all" : "on";
                    if (ChooseValue("##net_sync", sync, {"on", "off", "all"},
                                    {"On", "Off", "With every port player in the room"})) {
                        SetGame("net_sync", sync);
                    }
                    EndRow(R::OnlineNetSync);
                }
                EndSettings();
            }
        },
        [this] { SectionAnchor(S::OnlineRooms); });
}

} // namespace App::Ui
