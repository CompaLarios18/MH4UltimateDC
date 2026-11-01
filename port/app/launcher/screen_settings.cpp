// Settings: the app's main settings, in the groups of the in-game menu's tabs (Display: the
// renderer, full screen, vsync; Graphics: the internal resolution, texture packs; Touch screen: the
// screens; Audio: the volume) and the launcher's own (whether it shows, updates), saved in
// mh4u-app.ini as they change (App::Save). The game's menu has all of them but the updates, and the
// rest.
#include <string>
#include <fmt/format.h>
#include <imgui.h>

#include "common/file_util.h"
#include "frontend.h"
#include "launcher/state.h"
#include "launcher/ui.h"

namespace Launcher {

namespace {

struct Choice {
    const char* value;
    const char* label;
    const char* about;
};

constexpr Choice RENDERERS[] = {
#ifdef ENABLE_VULKAN
    {"vulkan", "Vulkan", "The fastest on most PCs (on a Mac, Vulkan runs on Metal)."},
#endif
#ifdef ENABLE_OPENGL
    {"opengl", "OpenGL", "For graphics drivers without Vulkan."},
#endif
#ifdef ENABLE_SOFTWARE_RENDERER
    {"software", "Software", "Without the graphics card: slow, for when nothing else works."},
#endif
};

// named as in the game's menu (Touch screen > Screens)
constexpr Choice SCREENS[] = {
    {"overlay", "The game, the touch screen in a corner",
     "The game fills the window; the touch screen sits over it in a corner, in quests the map alone (its "
     "corner, size and map: the game's menu, Touch screen). F12 or the right stick's click switches it."},
    {"single", "The game alone", "Only the top screen."},
    {"stacked", "Stacked, as on a 3DS", "The touch screen under the game."},
    {"side", "Side by side", "The touch screen to the right of the game."},
    {"windows", "The touch screen in a window of its own", "Two windows (not with the software renderer)."},
};

/// A label in the first column, the control in the second.
void Label(const char* text, const char* about = nullptr) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    if (about) {
        Ui::PushFont(0.85f);
        Ui::TextDim(about);
        Ui::PopFont();
    }
    ImGui::TableNextColumn();
}

/// A group's heading, as the game's menu groups its tabs (the small capitals of Online's lists).
void Group(const char* title) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Spacing();
    Ui::PushFont(0.85f, true);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Ui::Color::Dim), "%s", title);
    Ui::PopFont();
    ImGui::TableNextColumn();
}

std::string ResolutionLabel(int r) {
    return r == 0 ? std::string("As large as the window needs") : fmt::format("{}x ({}x{})", r, 400 * r, 240 * r);
}

} // namespace

void DrawSettings(State& s) {
    const float scale = Ui::Scale();
    const auto& c = s.config;
    ImGui::BeginChild("##settings", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
    Ui::Heading("Settings");
    Ui::TextDim(fmt::format("Saved as they change, in {}. The game's menu (Esc while you play, or Back and Start "
                            "on a controller) has these but the updates, and all the rest: the frame rate, "
                            "widescreen, the touch screen, the effects, the keys. The game's own options are kept "
                            "in {}.",
                            App::ConfigPath(), Port::OptionsPath()));
    ImGui::Spacing();
    if (!ImGui::BeginTable("##settings_table", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
        ImGui::EndChild();
        return;
    }
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.42f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.58f);

    // ── Display ──
    Group("DISPLAY");
    Label("Renderer", "How the game is drawn.");
    const Choice* renderer = nullptr;
    for (const auto& r : RENDERERS) {
        if (c.renderer == r.value) {
            renderer = &r;
        }
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    FocusNext(s);
    if (ImGui::BeginCombo("##renderer", renderer ? renderer->label : c.renderer.c_str())) {
        for (const auto& r : RENDERERS) {
            if (ImGui::Selectable(r.label, renderer == &r)) {
                const std::string value = r.value;
                s.Change([&](App::Config& cfg) { cfg.renderer = value; });
            }
            if (renderer == &r) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (renderer) {
        Ui::TextDim(renderer->about);
    }

    Label("Full screen", "F11 or Alt+Enter switches it while you play.");
    bool fullscreen = c.fullscreen;
    if (Ui::Toggle(fullscreen ? "On##fullscreen" : "Off##fullscreen", &fullscreen)) {
        s.Change([&](App::Config& cfg) { cfg.fullscreen = fullscreen; });
    }

    Label("Vsync", "Frames shown in step with the monitor, without tearing.");
    bool vsync = c.vsync;
    if (Ui::Toggle(vsync ? "On##vsync" : "Off##vsync", &vsync)) {
        s.Change([&](App::Config& cfg) { cfg.vsync = vsync; });
    }

    // ── Graphics ──
    Group("GRAPHICS");
    Label("Internal resolution", "The 3DS's 400x240 drawn this many times larger.");
    const std::string current = ResolutionLabel(c.resolution);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##resolution", current.c_str())) {
        for (int r = 0; r <= 10; ++r) {
            const std::string label = ResolutionLabel(r);
            if (ImGui::Selectable(label.c_str(), c.resolution == r)) {
                s.Change([&](App::Config& cfg) { cfg.resolution = r; });
            }
            if (c.resolution == r) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    Label("Texture packs", "Texture packs in load/textures/0004000000126300/ (the Mods screen's catalog "
                           "installs them there).");
    bool textures = c.custom_textures;
    if (Ui::Toggle(textures ? "On##custom_textures" : "Off##custom_textures", &textures)) {
        s.Change([&](App::Config& cfg) { cfg.custom_textures = textures; });
    }

    // ── Touch screen ──
    Group("TOUCH SCREEN");
    Label("Screens", "Where the 3DS's two screens go.");
    for (const auto& screen : SCREENS) {
        if (ImGui::RadioButton(screen.label, c.screens == screen.value)) {
            const std::string value = screen.value;
            s.Change([&](App::Config& cfg) { cfg.screens = value; });
        }
        if (c.screens == screen.value) {
            ImGui::Indent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
            Ui::PushFont(0.85f);
            Ui::TextDim(screen.about);
            Ui::PopFont();
            ImGui::Unindent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
        }
    }

    // ── Audio ──
    Group("AUDIO");
    Label("Volume");
    int volume = c.volume;
    ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, 420.0f * scale));
    Ui::SliderInt("##volume", &volume, 0, 100, "%d%%", s.pad);
    if (volume != c.volume) {
        s.config.volume = volume;  // while it moves; saved when let go
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        s.Change([&](App::Config& cfg) { cfg.volume = volume; });
    }

    // ── This launcher ──
    Group("THIS LAUNCHER");
    // (named as in the game's menu, Display > Window and renderer)
    Label("Launcher before the game", "Off: the app starts the game straight away (the launcher still opens "
                                      "while something in the setup is missing). --launcher shows it once.");
    bool launcher = c.launcher;
    if (Ui::Toggle(launcher ? "Shown when the app starts##launcher" : "Not shown##launcher", &launcher)) {
        s.Change([&](App::Config& cfg) { cfg.launcher = launcher; });
    }

    Label("Updates", "New releases of the port from its GitHub page: downloaded and put in place over this "
                     "one. Your settings, saves, mods and code library aren't touched.");
    DrawUpdateSettings(s);
    ImGui::EndTable();
    ImGui::Spacing();
    Ui::TextDim("More in the game's menu: the frame rate and widescreen in Display, the effects in Graphics, the "
                "touch screen's corner and the quest map in Touch screen, every key in Keys, the kick, the end of "
                "quests, fast forward and touch taps in Gameplay. Ctrl+F there (Cmd+F on a Mac, Y on a controller) "
                "finds any setting.");
    ImGui::EndChild();
}

} // namespace Launcher
