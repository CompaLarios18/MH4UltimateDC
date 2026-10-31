#include "ui/menu.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <string>
#include <vector>
#include <fmt/format.h>
#include <imgui.h>

#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "config.h"
#include "frontend.h"
#include "hooks.h"
#include "input.h"
#include "multiplayer.h"
#include "ui/hint.h"
#include "ui/layer.h"
#include "ui/menu_parts.h"
#include "ui/multiplayer_tab.h"
#include "ui/nav.h"
#include "ui/options.h"
#include "window.h"

namespace App::Ui {

using namespace MenuParts;

namespace {

using R = Index::RowId;
using S = Index::SectionId;
using K = Menu::Focus::Kind;

constexpr const char* TABS[] = {"Display", "Graphics", "Touch screen", "Controls",
                                "Keys",    "Gameplay", "Audio",        "Online"};
constexpr int TAB_COUNT = static_cast<int>(std::size(TABS));
static_assert(TAB_COUNT == Index::TAB_COUNT);

// mh4u.ini: what the hooks take when a key has no line (port/src/hooks.cpp, Options), and
// the keys they take while the game runs (Port::Hooks::SetOption)
const std::map<std::string, std::string> GAME_DEFAULTS = {
    {"fps", "0"},           {"refresh_rate", "0"}, {"widescreen", "off"}, {"wide_render", "0"},
    {"hud_edges", "1"},     {"touch_overlay", "off"}, {"quest_overlay", "map"}, {"kick", "off"},
    {"target", "off"},      {"dither", "on"},      {"stereo_3d", "off"},  {"antialiasing", "on"}, {"bloom", "on"},
    {"depth_of_field", "on"},
    {"motion_blur", "on"},  {"radial_blur", "on"}, {"haze", "on"},        {"net_sync", "on"},
    {"target_overlay", "on"}, {"skip", "ZR"}, {"skip_hint", "on"}, {"skip_end", "ZL+ZR"},
    {"skip_end_hold", "1.0"}};
const std::set<std::string> GAME_LIVE = {"fps",           "refresh_rate", "hud_edges", "touch_overlay",
                                         "quest_overlay", "target_overlay", "kick",    "target",
                                         "skip",          "skip_hint",    "skip_end", "skip_end_hold",
                                         "dither",
                                         "stereo_3d",     "antialiasing", "bloom",         "depth_of_field", "motion_blur",
                                         "radial_blur",   "haze"};

/// The footer before the cursor or the mouse first moves, and for an item without help.
/// TODO(0.1.8, two releases after the tabs moved): "Saved when the menu closes; settings marked
/// (next start) apply the next time the game starts."
constexpr const char* DEFAULT_FOOTER = "Saved when the menu closes. Moved: Game's settings to Display, Graphics, "
                                       "Touch screen, Gameplay; Multiplayer is Online.";

/// `help` with its {key:...} placeholder replaced by `key`.
std::string WithKey(const char* help, const std::string& key) {
    std::string text = help;
    if (const auto at = text.find("{key:"); at != std::string::npos) {
        const auto end = text.find('}', at);
        text.replace(at, end == std::string::npos ? std::string::npos : end - at + 1, key);
    }
    return text;
}

const char* NameOf(Settings::GraphicsAPI api) {
    switch (api) {
    case Settings::GraphicsAPI::OpenGL:
        return "OpenGL";
    case Settings::GraphicsAPI::Software:
        return "Software";
    default:
        return "Vulkan";
    }
}

} // namespace

Menu::Menu(Layer& layer_) : layer{layer_} {
    key_labels.resize(Index::ROW_COUNT);
    search_extra.resize(Index::ROW_COUNT);
    link_helps.resize(Index::ROW_COUNT);
    // the layer comes up once the window is made (main.cpp): the renderer tried first is this one
    renderer_started = Lower(layer.config.renderer);
}

Menu::~Menu() = default;

// ── Opening, closing, the settings files ──

void Menu::Opened() {
    just_opened = true;
    capture = -1;
    capture_note.clear();
    EndTapEdit();
    std::string text;
    FileUtil::ReadFileToString(true, Port::OptionsPath(), text);
    game_file = ReadOptions(text);
    game_changes.clear();
    backend_notice = Port::BackendNotice();
    ClearSearch();
    footer_note.clear();
    fresh = true;
    fresh_nav = 0;
    texts_dirty = true;
    // where it was: the tab it had (Display the first time), on that tab's last row
    FocusRemembered(true);
}

void Menu::Closed() {
    capture = -1;
    EndTapEdit();
    ClearSearch();
    if (layout_edit) {
        EndLayoutEdit(); // closed from the editor (Done, or the controller's Guide): kept
    }
    if (!game_changes.empty()) {
        const std::string path = Port::OptionsPath();
        std::string text;
        FileUtil::ReadFileToString(true, path, text);
        if (FileUtil::WriteStringToFile(true, path, WriteOptions(text, game_changes))) {
            std::string what;
            for (const auto& [key, value] : game_changes) {
                what += fmt::format("{}{} = {}", what.empty() ? "" : ", ", key, value);
            }
            LOG_INFO(Frontend, "menu: {} in {}", what, path);
        } else {
            LOG_ERROR(Frontend, "menu: couldn't write {}", path);
        }
        game_changes.clear();
    }
    if (!app_changes.empty()) {
        // the file's settings with the menu's changes: not the command line's (--windowed, ...)
        Config file = App::Load();
        const Config& now = layer.config;
        for (const AppSetting setting : app_changes) {
            switch (setting) {
            case AppSetting::Fullscreen:
                file.fullscreen = now.fullscreen;
                break;
            case AppSetting::Screens:
                file.screens = now.screens;
                break;
            case AppSetting::Resolution:
                file.resolution = now.resolution;
                break;
            case AppSetting::LinearFilter:
                file.linear_filter = now.linear_filter;
                break;
            case AppSetting::Vsync:
                file.vsync = now.vsync;
                break;
            case AppSetting::CpuClock:
                file.cpu_clock = now.cpu_clock;
                break;
            case AppSetting::FastForward:
                file.fast_forward_speed = now.fast_forward_speed;
                file.fast_forward_hold = now.fast_forward_hold;
                break;
            case AppSetting::Renderer:
                file.renderer = now.renderer;
                break;
            case AppSetting::Volume:
                file.volume = now.volume;
                break;
            case AppSetting::Output:
                file.audio_output = now.audio_output;
                break;
            case AppSetting::FpsCounter:
                file.fps_counter = now.fps_counter;
                break;
            case AppSetting::CustomTextures:
                file.custom_textures = now.custom_textures;
                break;
            case AppSetting::Launcher:
                file.launcher = now.launcher;
                break;
            case AppSetting::Controls:
                file.keys = now.keys;
                file.stick_keys = now.stick_keys;
                file.controller_auto = now.controller_auto;
                file.button_params = now.button_params;
                file.stick_params = now.stick_params;
                file.input_mode = now.input_mode;
                file.mouse_sensitivity = now.mouse_sensitivity;
                file.mouse_vertical = now.mouse_vertical;
                file.mouse_invert_y = now.mouse_invert_y;
                file.mouse_invert_x = now.mouse_invert_x;
                file.mouse_deadzone = now.mouse_deadzone;
                file.item_prev = now.item_prev;
                file.item_next = now.item_next;
                file.stylus_key = now.stylus_key;
                file.touch_screen_key = now.touch_screen_key;
                file.key_guides = now.key_guides;
                file.key_guides_key = now.key_guides_key;
                file.pause_key = now.pause_key;
                file.fast_forward_key = now.fast_forward_key;
                file.fast_forward_hold = now.fast_forward_hold;
                file.touch_taps = now.touch_taps;
                break;
            }
        }
        App::Save(file);
        LOG_INFO(Frontend, "menu: settings saved in {}", App::ConfigPath());
        app_changes.clear();
    }
}

std::string Menu::Game(const std::string& key) const {
    if (const auto it = game_changes.find(key); it != game_changes.end()) {
        return it->second;
    }
    if (const auto it = game_file.find(key); it != game_file.end()) {
        return it->second;
    }
    const auto it = GAME_DEFAULTS.find(key);
    return it == GAME_DEFAULTS.end() ? "" : it->second;
}

void Menu::SetGame(const std::string& key, const std::string& value) {
    game_changes[key] = value;
    fresh = false; // the footer's default text has been seen: the help of what changes now
    if (GAME_LIVE.count(key)) {
        layer.Post([key, value] {
            if (!Port::Hooks::SetOption(key, value)) {
                LOG_WARNING(Frontend, "menu: {} applies from the next start", key);
            }
        });
    }
    if (key == "kick" || key == "target" || key == "skip" || key == "skip_end") {
        texts_dirty = true; // the key rows name the port's roles; the lists, what presses them
    }
}

void Menu::Changed(AppSetting setting) {
    app_changes.insert(setting);
    fresh = false;
}

void Menu::ControlsChanged() {
    Changed(AppSetting::Controls);
    // the main loop builds the input profile again from the app's settings (as when a
    // controller comes) and gives the console's input services the new one; the texts that
    // name the controller's buttons follow it two frames later
    layer.window.RequestInputReload();
    texts_dirty = true;
    refresh_in = 2;
}

// ── The texts that name keys and buttons ──

void Menu::RefreshTexts() {
    texts_dirty = false;
    const Config& config = layer.config;
    controller_description = Input::ControllerDescription();

    // help lines with a key in them
    const std::string fps_key = Input::FirstKey(config.fps_counter_key);
    help_fps_counter = fps_key.empty() ? "The frames per second and a frame's time in a corner of the screen. "
                                         "fps_counter_key in mh4u-app.ini can show and hide it."
                                       : WithKey(Index::Get(R::DisplayFpsCounter).help, fps_key);
    const std::string full_key = Input::FirstKey(config.fullscreen_key);
    help_fullscreen = full_key.empty() ? "The game fills the screen; off, it plays in a window. Alt+Enter switches "
                                         "it too."
                                       : WithKey(Index::Get(R::DisplayFullscreen).help, full_key);
    const std::string guides_key = Input::FirstKey(config.key_guides_key);
    help_key_guides = guides_key.empty() ? "The keys for what the hunter does, small in a corner while you play with "
                                           "the keyboard and mouse. Give them a key in Keys."
                                         : WithKey(Index::Get(R::ControlsKeyGuides).help, guides_key);

    // the lists of 3DS buttons: each entry with what presses it
    const auto entry = [&config](const std::string& value) {
        if (value == "OFF") {
            return std::string("None");
        }
        const std::string inputs = ButtonInputs(config, value);
        return Rules::ButtonText(value) + (inputs.empty() ? "  (no key)" : "  (" + inputs + ")");
    };
    button_labels.clear();
    for (const char* button : BUTTONS) {
        button_labels.push_back(entry(button));
    }
    combo_labels.clear();
    for (const char* combo : COMBOS) {
        combo_labels.push_back(entry(combo));
    }
    // and the buttons each list has now, the first one nothing presses (R15)
    static constexpr const char* LISTS[] = {"kick", "target", "skip", "skip_end"};
    for (int i = 0; i < static_cast<int>(Rules::Buttons::Count); ++i) {
        list_buttons[i] = ButtonsOf(Game(LISTS[i]));
        list_unbound[i].clear();
        std::size_t start = 0;
        while (!list_buttons[i].empty() && start <= list_buttons[i].size()) {
            std::size_t end = list_buttons[i].find('+', start);
            if (end == std::string::npos) {
                end = list_buttons[i].size();
            }
            const std::string part = list_buttons[i].substr(start, end - start);
            if (ButtonInputs(config, part).empty()) {
                list_unbound[i] = part;
                break;
            }
            start = end + 1;
        }
    }
    RefreshKeyTexts();
    // the notes' Go to links: where each goes
    for (int i = 0; i < Index::ROW_COUNT; ++i) {
        const auto row = static_cast<RowId>(i);
        const Index::Section& section = Index::Get(Index::SectionOf(row));
        link_helps[i] = fmt::format("Goes to {}: {}{}{}.", LabelOf(row), Index::TabName(section.tab),
                                    *section.title ? " > " : "", section.title);
    }
}

const char* Menu::HelpOf(RowId row) const {
    switch (row) {
    case R::DisplayFpsCounter:
        return help_fps_counter.c_str();
    case R::DisplayFullscreen:
        return help_fullscreen.c_str();
    case R::ControlsKeyGuides:
        return help_key_guides.c_str();
    default:
        return Index::Get(row).help;
    }
}

std::string Menu::LabelOf(RowId row) const {
    const std::string& label = key_labels[static_cast<int>(row)];
    return label.empty() ? Index::ListedLabel(row) : label;
}

// ── The window ──

void Menu::Draw() {
    if (drop_search_field) {
        drop_search_field = false;
        Nav::DropTextField(search_id); // (being typed in, it would put its text back)
    }
    if (refresh_in > 0 && --refresh_in == 0) {
        texts_dirty = true;
    }
    if (texts_dirty || Input::ControllerDescription() != controller_description) {
        RefreshTexts(); // before any help line points into them this frame
    }
    if (layout_edit) {
        DrawLayoutEditor();
        just_opened = false;
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const float em = ImGui::GetFontSize();
    // the game, dimmed behind the menu
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 110));
    const ImVec2 size(std::min(io.DisplaySize.x * 0.94f, em * 52.0f), std::min(io.DisplaySize.y * 0.94f, em * 32.0f));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    // and focused: on opening, and after a click or a finger on the game around it, which leaves no
    // window focused (Dear ImGui's click on the void), where the controller and LB/RB do nothing
    if (just_opened || !ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow)) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::Begin("##menu", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    pad_owner = ImGui::GetID("##pad_y");
    list_open = slider_active = false;

    // the footer's default text until the cursor or the mouse moves; a jump's message for a while
    const ImGuiID nav = Nav::FocusedId();
    if (fresh) {
        if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f || ImGui::IsAnyMouseDown()) {
            fresh = false;
        } else if (focus.kind == K::None) {
            if (fresh_nav == 0) {
                fresh_nav = nav;
            } else if (nav != fresh_nav) {
                fresh = false;
            }
        }
    }
    if (!footer_note.empty()) {
        if (ImGui::GetTime() > footer_note_until) {
            footer_note.clear();
        } else if (focus.kind == K::None) {
            if (footer_note_nav == 0) {
                footer_note_nav = nav;
            } else if (nav != footer_note_nav) {
                footer_note.clear();
            }
        }
    }

    DrawHeader();
    UpdateSearch(); // what the field has now: the tab bar dims for the results
    DrawTabBar();
    const float footer = ImGui::GetTextLineHeightWithSpacing() * 3.2f;
    if (Searching()) {
        DrawResults(footer);
    } else {
        DrawPage(footer);
    }
    DrawFooter();
    HandleButtons();
    ImGui::End();

    // what the page chose, now that it is drawn
    if (pending_jump != R::Count) {
        const RowId row = pending_jump;
        pending_jump = R::Count;
        Jump(row);
    }
    if (pending_tab >= 0) {
        ClearSearch();
        SelectTab(static_cast<Tab>(pending_tab), pending_tab_cursor);
        pending_tab = -1;
    }
    just_opened = false;
}

void Menu::DrawHeader() {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float em = ImGui::GetFontSize();
    const char* title = "MH4U PC port";
    const char* quit = "Quit game";
    const char* back = "Back to the game";
    const float quit_width = ImGui::CalcTextSize(quit).x + style.FramePadding.x * 2;
    const float back_width = ImGui::CalcTextSize(back).x + style.FramePadding.x * 2;
    ImGui::PushFont(nullptr, style.FontSizeBase * 1.25f);
    const float title_width = ImGui::CalcTextSize(title).x;
    ImGui::PopFont();
    // the search stretches between them; at least 10 em, else the title makes room
    float field = ImGui::GetContentRegionAvail().x - quit_width - back_width - style.ItemSpacing.x * 2;
    if (field - title_width - style.ItemSpacing.x >= em * 10.0f) {
        ImGui::PushFont(nullptr, style.FontSizeBase * 1.25f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        ImGui::SameLine();
        field -= title_width + style.ItemSpacing.x;
    }
    DrawSearchField(std::max(field, em * 4.0f));
    ImGui::SameLine();
    if (ImGui::Button(quit) || open_quit) {
        open_quit = false;
        ImGui::OpenPopup("Quit?");
    }
    ItemHelp(R::HeaderQuit);
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - back_width));
    if (ImGui::Button(back)) {
        layer.CloseMenu();
    }
    ItemHelp(R::HeaderBack);
    DrawQuitQuestion();
}

void Menu::DrawQuitQuestion() {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Quit?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted("Quit Monster Hunter 4 Ultimate? Save in the game first: what you did since your "
                               "last save is lost.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("Quit")) {
            ImGui::CloseCurrentPopup();
            layer.CloseMenu(); // its changes are saved first
            layer.Post([&window = layer.window] {
                LOG_INFO(Frontend, "quit from the menu");
                window.RequestClose();
            });
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || CancelPressed()) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
}

void Menu::DrawTabBar() {
    const ImGuiStyle& style = ImGui::GetStyle();
    // labels are never shrunk or cut: compact padding where the bar is narrower than they need
    // (4:3, 5:4), then the bar's scroll buttons
    float need = style.ItemInnerSpacing.x * (TAB_COUNT - 1);
    for (const char* label : TABS) {
        need += ImGui::CalcTextSize(label).x + style.FramePadding.x * 2 + 1.0f;
    }
    const bool compact = need > ImGui::GetContentRegionAvail().x;
    const float unit = ImGui::GetFontSize() / 13.0f;
    if (compact) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f * unit, style.FramePadding.y));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(2.0f * unit, style.ItemInnerSpacing.y));
    }
    const bool searching = Searching();
    if (searching) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.55f); // the results stand for the tabs
    }
    int picked = -1;
    bool picked_by_mouse = false;
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
        int shown = -1;
        for (int i = 0; i < TAB_COUNT; ++i) {
            // the tab bar follows the menu's tab (a frame later); its tab items carry no page
            const ImGuiTabItemFlags flags =
                i == static_cast<int>(tab) && bar_tab != static_cast<int>(tab) ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(TABS[i], nullptr, flags)) {
                shown = i;
                ImGui::EndTabItem();
            }
            if (ImGui::IsItemActivated() || ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                picked = i;
                picked_by_mouse = ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                                  ImGui::IsMouseClicked(ImGuiMouseButton_Right);
            }
        }
        bar_tab = shown;
        ImGui::EndTabBar();
    }
    if (searching) {
        ImGui::PopStyleVar();
    }
    if (compact) {
        ImGui::PopStyleVar(2);
    }
    if (picked >= 0) {
        ClearSearch();
        SelectTab(static_cast<Tab>(picked), !picked_by_mouse);
    }
}

void Menu::SelectTab(Tab to, bool cursor) {
    tab = to;
    FocusRemembered(cursor);
}

void Menu::GatherFacts() {
    const Config& c = layer.config;
    Rules::Facts f;
    f.screens = c.screens;
    f.renderer = Lower(c.renderer);
    f.overlay_on = Port::Hooks::ParseOverlay(Game("touch_overlay")).height > 0.0f;
    const auto map = Port::Hooks::ParseQuestOverlay(Game("quest_overlay"));
    f.map_on = map.on;
    f.map_placed = map.placed;
    const auto target = Port::Hooks::ParseTargetOverlay(Game("target_overlay"));
    f.target_on = target.on;
    f.target_placed = target.placed;
    f.widescreen = Lower(Game("widescreen"));
    f.wide_render = IsOn(Game("wide_render"));
    f.dither = IsOn(Game("dither"));
    f.resolution = c.resolution;
    const int fps = std::atoi(Game("fps").c_str());
    f.fps = fps > 0 ? fps : 30;
    f.refresh_rate = std::max(0.0f, std::strtof(Game("refresh_rate").c_str(), nullptr));
    f.controller_mode = Input::ParseMode(c.input_mode) == Input::Mode::Controller;
    f.controller_auto = c.controller_auto;
    f.buttons = list_buttons;
    f.unbound = list_unbound;
    const auto api = Settings::values.graphics_api.GetValue();
    f.software_now = api == Settings::GraphicsAPI::Software;
    f.renderer_now = NameOf(api);
    f.renderer_started = renderer_started;
    f.wide_now = Port::Hooks::DrawsWide();
    const auto layout = Settings::values.layout_option.GetValue();
    f.windows_now = layout == Settings::LayoutOption::SeparateWindows;
    f.custom_now = layout == Settings::LayoutOption::CustomLayout;
    f.in_room = App::Multiplayer::InRoom();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    f.window_aspect = display.y > 0.0f ? display.x / display.y : 0.0f;
    if (tab == Tab::TouchScreen && !Searching()) {
        GatherViewNotes(f); // the hooks' sentences, on this tab only
    }
    facts = std::move(f);
}

void Menu::DrawPage(float footer) {
    GatherFacts();
    verdict = Rules::Evaluate(facts);
    in_page = true;
    anchors.clear();
    current_section = S::Count;
    focused_section = S::Count;
    section_armed = next_armed = false;
    // a page per tab, so that each keeps its own scroll
    const std::string id = std::string("##page") + TABS[static_cast<int>(tab)];
    ImGui::BeginChild(id.c_str(), ImVec2(0.0f, -footer), ImGuiChildFlags_NavFlattened);
    switch (tab) {
    case Tab::Display:
        DrawDisplay();
        break;
    case Tab::Graphics:
        DrawGraphics();
        break;
    case Tab::TouchScreen:
        DrawTouchScreen();
        break;
    case Tab::Controls:
        DrawControls();
        break;
    case Tab::Keys:
        DrawKeys();
        break;
    case Tab::Gameplay:
        DrawGameplay();
        break;
    case Tab::Audio:
        DrawAudio();
        break;
    default:
        DrawOnline();
        break;
    }
    ImGui::EndChild();
    in_page = false;
    anchors_last.swap(anchors);
    focused_section_last = focused_section;
    AgeFocus();
}

void Menu::DrawFooter() {
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::Separator();
    const char* help = layer.HelpText();
    const char* text = !footer_note.empty() ? footer_note.c_str() : fresh || !help ? DEFAULT_FOOTER : help;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - style.WindowPadding.y - ImGui::GetTextLineHeight());
    ImGui::TextDisabled("%s", Hints());
}

const char* Menu::Hints() const {
    const bool pad = layer.PadLast();
    if (keys_up) {
        return "A: type   LB: delete   RB: space   Y or Start: done   B: close";
    }
    if (capture >= 0 || TapCapturing()) {
        return pad ? "Press a key or a mouse button   B: keep the old one"
                   : "Press a key or a mouse button   Esc: keep the old one   Delete: no key";
    }
    if (list_open || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        return pad ? "Up/Down: pick   A: choose   B: close" : "Up/Down: pick   Enter: choose   Esc: close";
    }
    if (slider_active) {
        return pad ? "Left/Right: change   LB/RB: slower/faster   A or B: done" : "Left/Right: change   Enter or Esc: done";
    }
    if (Searching()) {
        return pad ? "A: go to it   B: clear the search   Y: type   LB/RB: tabs"
                   : "Enter: go to it   Esc: clear the search   Ctrl+Tab: tabs";
    }
    return pad ? "A: choose   B: back   LB/RB: tabs   LT/RT: sections   Y: search" : PAGE_KEYS;
}

void Menu::HandleButtons() {
    const bool waiting = capture >= 0 || TapCapturing();
    const bool popup = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const bool idle = !layer.WasBusy() && !waiting && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const auto pressed = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, false); };
    const auto step_tab = [this](int by) {
        ClearSearch();
        SelectTab(static_cast<Tab>((static_cast<int>(tab) + TAB_COUNT + by) % TAB_COUNT), true);
    };
    // the controller's Y is the search's (Dear ImGui's would ask for a context menu); X and Alt are
    // nobody's: a tap of either toggles Dear ImGui's nav layer, which takes the cursor out of the page
    Nav::TakeKey(ImGuiKey_GamepadFaceUp, pad_owner);
    Nav::TakeKey(ImGuiKey_GamepadFaceLeft, pad_owner);
    Nav::TakeKey(ImGuiKey_LeftAlt, pad_owner);
    Nav::TakeKey(ImGuiKey_RightAlt, pad_owner);
    if (keys_up) {
        if (idle) {
            HandleSearchKeys(); // LB, RB, Y and Start are the keys' own; LT, RT and the tabs rest
        }
    } else if (idle) {
        if (pressed(ImGuiKey_GamepadL1)) {
            step_tab(-1);
        }
        if (pressed(ImGuiKey_GamepadR1)) {
            step_tab(1);
        }
        if (!Searching() && pressed(ImGuiKey_GamepadL2)) {
            JumpSection(-1);
        }
        if (!Searching() && pressed(ImGuiKey_GamepadR2)) {
            JumpSection(1);
        }
        if (Nav::KeyPressed(ImGuiKey_GamepadFaceUp, pad_owner)) {
            OpenKeys();
        }
    }
    // B: one step back, once Dear ImGui's own are done (a list, a question, an item being changed)
    if (idle && pressed(ImGuiKey_GamepadFaceRight) && !OnBack()) {
        layer.CloseMenu();
    }
    // Ctrl+Tab and Ctrl+Shift+Tab: the next and the previous tab, from the search field too
    if (!keys_up && !waiting && !popup) {
        if (ImGui::Shortcut(TABS_MOD | ImGuiKey_Tab, ImGuiInputFlags_Repeat | ImGuiInputFlags_RouteGlobal)) {
            step_tab(1);
        }
        if (ImGui::Shortcut(TABS_MOD | ImGuiMod_Shift | ImGuiKey_Tab,
                            ImGuiInputFlags_Repeat | ImGuiInputFlags_RouteGlobal)) {
            step_tab(-1);
        }
    }
    // a key awaited: B keeps the old one (Esc, through the layer)
    if (waiting && pressed(ImGuiKey_GamepadFaceRight)) {
        capture = -1;
        EndTapEdit();
        layer.SetKeyCapture(false);
    }
}

bool Menu::OnBack() {
    if (keys_up) {
        keys_up = false;
        if (Index::Searchable(search)) {
            focus = {};
            focus.kind = K::Result;
            focus.cursor = true;
        } else {
            FocusRemembered(true);
        }
        return true;
    }
    if (search[0] != '\0') {
        ClearSearch();
        FocusRemembered(true);
        return true;
    }
    return false;
}

// ── Focus ──

void Menu::FocusFirst(bool cursor) {
    focus = {};
    focus.kind = K::First;
    focus.cursor = cursor;
}

void Menu::FocusRemembered(bool cursor) {
    const ImGuiID id = remembered[static_cast<int>(tab)];
    focus = {};
    focus.kind = id != 0 ? K::Item : K::First;
    focus.item = id;
    focus.cursor = cursor;
}

void Menu::AfterItem() {
    if (!in_page) {
        return;
    }
    const bool focusable = Nav::LastItemFocusable();
    if (focusable && item.first && !anchors.empty() && anchors.back().section == current_section) {
        anchors.back().focusable = true;
    }
    if (ImGui::IsItemFocused()) {
        if (!in_results) {
            remembered[static_cast<int>(tab)] = ImGui::GetItemID();
        }
        focused_section = current_section;
    }
    if (focus.kind == K::None || !focusable) {
        return;
    }
    bool match = false;
    switch (focus.kind) {
    case K::First:
        match = item.first && !in_results;
        break;
    case K::Item:
        match = ImGui::GetItemID() == focus.item;
        break;
    case K::Row:
        match = item.row == focus.row && item.note < 0;
        break;
    case K::Note:
        match = item.note == focus.note;
        break;
    case K::Section:
        match = section_armed && item.first;
        break;
    case K::Next:
        match = next_armed;
        break;
    case K::Result:
        match = item.result;
        break;
    default:
        break;
    }
    TakeFocus(match);
}

void Menu::TakeFocus(bool match) {
    if (!match || ImGui::GetFrameCount() < focus.ready) {
        return;
    }
    if (focus.scroll && !focus.scrolled) {
        // a jump: the page shows it a little above the middle first; the cursor goes next frame,
        // when the navigation's own scrolling has nothing left to do
        ImGui::SetScrollHereY(0.35f);
        focus.scrolled = true;
        focus.ready = ImGui::GetFrameCount() + 1;
        focus.frames = 0;
        return;
    }
    Nav::FocusItemNow(focus.cursor);
    focus = {};
    section_armed = next_armed = false;
}

void Menu::AgeFocus() {
    if (focus.kind == K::None || ++focus.frames <= 2) {
        return;
    }
    // nothing took it (a row hidden or disabled meanwhile, a tab with nothing there): the first
    // control, and if not even that, nothing
    if (focus.kind == K::First) {
        focus = {};
    } else {
        FocusFirst(focus.cursor);
    }
}

void Menu::Jump(RowId row) {
    const Index::Row& r = Index::Get(row);
    ClearSearch();
    if (r.kind == Index::Kind::Link) {
        // a way into Online's rooms: Join a room or Host a room, and its button
        tab = Tab::Online;
        const int page = row == R::OnlineHost ? 1 : row == R::OnlineInvite || row == R::OnlineLeave ? -1 : 0;
        if (page >= 0) {
            focus = {};
            ShowMultiplayerPage(page);
        } else {
            FocusFirst(true);
        }
        return;
    }
    if (row == R::HeaderQuit) {
        open_quit = true;
        return;
    }
    if (Index::TabOf(row) == Tab::Header) {
        return;
    }
    tab = Index::TabOf(row);
    focus = {};
    focus.cursor = true;
    focus.scroll = true;
    flash_row = row;
    flash_until = ImGui::GetTime() + 1.0;
    if (verdict.Hidden(row)) {
        // the nearest parent shown now; the footer says when this one shows
        RowId hidden = row, parent = Index::ParentOf(row);
        while (parent != R::Count && verdict.Hidden(parent)) {
            hidden = parent;
            parent = Index::ParentOf(parent);
        }
        footer_note = fmt::format("{} shows when {}.", Index::ListedLabel(row), Index::Get(hidden).shown_when);
        footer_note_until = ImGui::GetTime() + 4.0;
        footer_note_nav = 0;
        focus.kind = K::Row;
        focus.row = parent;
        flash_row = parent;
    } else if (verdict.Disabled(row)) {
        // its note's Go to link: the row's, else its section's, else another row's in the section
        const Rules::Note* note = verdict.RowNote(row);
        const S section = Index::SectionOf(row);
        focus.kind = K::Note;
        if (note && note->link != R::Count) {
            focus.note = static_cast<int>(row);
        } else if (verdict.SectionNote(section).link != R::Count) {
            focus.note = Index::ROW_COUNT + static_cast<int>(section);
        } else {
            for (const auto& [other, other_note] : verdict.rows) {
                if (Index::SectionOf(other) == section && other_note.link != R::Count && !verdict.Hidden(other)) {
                    focus.note = static_cast<int>(other);
                    break;
                }
            }
        }
        if (focus.note < 0) {
            focus.kind = K::Row; // nowhere to land: the first control, in the end
            focus.row = row;
        }
    } else if (r.kind == Index::Kind::Fixed) {
        focus.kind = K::Next; // shown, not changed here: the cursor goes to the next control
        focus.row = row;
    } else {
        focus.kind = K::Row;
        focus.row = row;
    }
}

void Menu::JumpSection(int direction) {
    const auto& list = anchors_last;
    int at = -1;
    for (int i = 0; i < static_cast<int>(list.size()); ++i) {
        if (list[i].section == focused_section_last) {
            at = i;
        }
    }
    // from the top when nothing is focused
    for (int i = at < 0 ? 0 : at + direction; i >= 0 && i < static_cast<int>(list.size()); i += direction) {
        if (list[i].focusable) {
            focus = {};
            focus.kind = K::Section;
            focus.section = list[i].section;
            focus.cursor = true;
            return;
        }
    }
}

// ── Rows ──

bool Menu::BeginSettings(const char* id) {
    // compact rows: 1.77 em, against 2.38 before (13-pixel units, scaled with the font)
    const float unit = ImGui::GetFontSize() / 13.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().CellPadding.x, 1.0f * unit));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 4.0f * unit));
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::PopStyleVar(2);
        return false;
    }
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.46f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.54f);
    return true;
}

void Menu::EndSettings() {
    ImGui::EndTable();
    ImGui::PopStyleVar(2);
}

void Menu::SectionAnchor(SectionId section) {
    current_section = section;
    anchors.push_back({section, false});
    section_armed = false;
    if (focus.kind == K::Section && focus.section == section && ImGui::GetFrameCount() >= focus.ready) {
        if (!focus.scrolled) {
            // its title at the top of the page; its first control takes the cursor next frame
            ImGui::SetScrollHereY(0.0f);
            focus.scrolled = true;
            focus.ready = ImGui::GetFrameCount() + 1;
            focus.frames = 0;
        } else {
            section_armed = true;
        }
    }
}

void Menu::Section(SectionId section) {
    ImGui::Spacing();
    ImGui::SeparatorText(Index::Get(section).title);
    SectionAnchor(section);
    if (const Rules::Note& note = verdict.SectionNote(section); !note.Empty()) {
        // in a section that can't apply, its link is where the cursor lands
        DrawNote(note, Index::ROW_COUNT + static_cast<int>(section), SectionDisabled(section));
    }
}

bool Menu::SectionDisabled(SectionId section) const {
    for (int i = 0; i < Index::ROW_COUNT; ++i) {
        const auto row = static_cast<RowId>(i);
        if (Index::SectionOf(row) == section && !verdict.Hidden(row) && !verdict.Disabled(row)) {
            return false;
        }
    }
    return true;
}

bool Menu::BeginRow(RowId row) {
    if (verdict.Hidden(row)) {
        return false;
    }
    item = {};
    item.row = row;
    ImGui::TableNextRow();
    FlashRow(row);
    ImGui::BeginDisabled(verdict.Disabled(row));
    ImGui::TableSetColumnIndex(0);
    RowLabel(row);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    return true;
}

void Menu::EndRow(RowId row, bool help) {
    if (help) {
        ItemHelp(row);
    }
    ImGui::EndDisabled();
    if (const Rules::Note* note = verdict.RowNote(row)) {
        DrawNote(*note, static_cast<int>(row), false);
    }
    if (row == capture_place && !capture_note.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(CAPTURE_COLOUR, "%s", capture_note.c_str());
        ImGui::PopTextWrapPos();
    }
    item = {};
}

void Menu::RowLabel(RowId row) {
    // as drawn: the index's label (indented sub-rows: "    Across"), a key row's own
    const std::string& own = key_labels[static_cast<int>(row)];
    const std::string drawn = own.empty() ? Index::Get(row).label : own;
    // (next start): R20, the screens while the touch screen has a window of its own
    const bool next = Index::Get(row).next_start || (row == R::TouchScreens && facts.windows_now);
    ImGui::AlignTextToFramePadding();
    if (!next) {
        ImGui::TextWrapped("%s", drawn.c_str());
        return;
    }
    // amber once it was changed in this opening of the menu (R19)
    const char* marker = "(next start)";
    const ImVec4 colour = ChangedNow(row) ? NOTE_COLOUR : ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
    const ImGuiStyle& style = ImGui::GetStyle();
    if (ImGui::CalcTextSize(drawn.c_str()).x + style.ItemSpacing.x + ImGui::CalcTextSize(marker).x <=
        ImGui::GetContentRegionAvail().x) {
        ImGui::TextUnformatted(drawn.c_str());
        ImGui::SameLine();
        ImGui::TextColored(colour, "%s", marker);
    } else {
        ImGui::TextWrapped("%s", drawn.c_str());
        ImGui::TextColored(colour, "%s", marker);
    }
}

bool Menu::ChangedNow(RowId row) const {
    const auto app = [this](AppSetting setting) { return app_changes.count(setting) > 0; };
    switch (row) {
    case R::DisplayRenderer:
        return app(AppSetting::Renderer);
    case R::DisplayLauncher:
        return app(AppSetting::Launcher);
    case R::GraphicsCustomTextures:
        return app(AppSetting::CustomTextures);
    case R::AudioOutput:
        return app(AppSetting::Output);
    case R::TouchScreens:
        return app(AppSetting::Screens);
    case R::DisplayAspect:
        return game_changes.count("widescreen") > 0;
    case R::DisplayWideRender:
        return game_changes.count("wide_render") > 0;
    case R::OnlineNetSync:
        return game_changes.count("net_sync") > 0;
    default:
        return false;
    }
}

void Menu::FlashRow(RowId row) {
    if (row != flash_row) {
        return;
    }
    const double left = flash_until - ImGui::GetTime();
    if (left <= 0.0) {
        flash_row = R::Count;
        return;
    }
    const int alpha = static_cast<int>(120.0 * std::min(1.0, left));
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(235, 160, 50, alpha));
}

void Menu::ItemHelp(RowId row) {
    layer.Help(HelpOf(row));
}

void Menu::DrawNote(const Rules::Note& note, int key, bool link_takes_first) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec4 colour = note.amber ? NOTE_COLOUR : style.Colors[ImGuiCol_TextDisabled];
    const std::string link = note.link != R::Count ? "Go to " + LabelOf(note.link) : "";
    const bool same_line = !link.empty() && ImGui::CalcTextSize(note.text.c_str()).x + style.ItemSpacing.x +
                                                    ImGui::CalcTextSize(link.c_str()).x <=
                                                ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(note.text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    if (link.empty()) {
        return;
    }
    if (same_line) {
        ImGui::SameLine();
    }
    const ItemContext saved = item;
    item = {};
    item.note = key;
    item.first = link_takes_first;
    if (ImGui::TextLink(fmt::format("{}###note{}", link, key).c_str())) { // (a key row's name changes)
        pending_jump = note.link;
    }
    layer.Help(link_helps[static_cast<int>(note.link)].c_str());
    item = saved;
}

void Menu::LinkRow(const char* text, Tab to, const char* help, const char* id) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", text);
    ImGui::PopTextWrapPos();
    ImGui::TableSetColumnIndex(1);
    ImGui::AlignTextToFramePadding();
    if (ImGui::TextLink(fmt::format("Go to {}##{}", TABS[static_cast<int>(to)], id).c_str())) {
        pending_tab = static_cast<int>(to);
        pending_tab_cursor = ImGui::GetIO().NavVisible;
    }
    layer.Help(help);
}

void Menu::BackendLine() {
    if (backend_notice.empty()) {
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, NOTE_COLOUR);
    ImGui::TextUnformatted("The port's options may not work now: Display says why.");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    const ItemContext saved = item;
    item = {};
    item.first = false;
    if (ImGui::TextLink("Go to Display##backend")) {
        pending_tab = static_cast<int>(Tab::Display);
        pending_tab_cursor = ImGui::GetIO().NavVisible;
    }
    layer.Help("Opens Display, whose top says what happened to the port's code library.");
    item = saved;
}

// ── Lists and sliders: left and right step them (Nav::Step) ──

bool Menu::Choose(const char* id, int& current, const std::vector<std::string>& labels, int disabled) {
    bool changed = false;
    const int count = static_cast<int>(labels.size());
    const char* preview = current >= 0 && current < count ? labels[current].c_str() : "";
    if (ImGui::BeginCombo(id, preview)) {
        list_open = true;
        for (int i = 0; i < count; ++i) {
            const bool selected = i == current;
            ImGui::PushID(i);
            if (ImGui::Selectable(labels[i].c_str(), selected, i == disabled ? ImGuiSelectableFlags_Disabled : 0) &&
                !selected) {
                current = i;
                changed = true;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    } else if (const int step = Nav::Step()) {
        // the entry before or after, no wrap; one that can't be chosen is stepped over
        int next = current + step;
        if (next == disabled) {
            next += step;
        }
        if (next >= 0 && next < count) {
            current = next;
            changed = true;
        }
    }
    return changed;
}

bool Menu::ChooseValue(const char* id, std::string& value, std::vector<std::string> values,
                       std::vector<std::string> labels, int disabled) {
    // a value the list doesn't have (set by hand) shows, and stays, as an entry of its own
    auto it = std::find(values.begin(), values.end(), value);
    if (it == values.end()) {
        values.push_back(value);
        labels.push_back(value);
        it = values.end() - 1;
    }
    int current = static_cast<int>(it - values.begin());
    if (!Choose(id, current, labels, disabled)) {
        return false;
    }
    value = values[current];
    return true;
}

ImGuiSliderFlags Menu::SliderFlags() const {
    // a controller's A held on a slider (Dear ImGui's 0.6 s) asks for a value typed in, with no
    // keyboard at hand: with a controller in use the sliders take the D-pad and LB/RB only
    return layer.PadLast() ? ImGuiSliderFlags_NoInput : ImGuiSliderFlags_None;
}

bool Menu::SliderInt(const char* id, int& value, int min, int max, const char* format, int step) {
    bool changed = ImGui::SliderInt(id, &value, min, max, format, SliderFlags());
    if (ImGui::IsItemActive()) {
        slider_active = true;
    }
    if (const int s = Nav::Step()) {
        const int next = std::clamp(value + s * step, min, max);
        changed = changed || next != value;
        value = next;
    }
    return changed;
}

bool Menu::SliderFloat(const char* id, float& value, float min, float max, const char* format, float step) {
    bool changed = ImGui::SliderFloat(id, &value, min, max, format, SliderFlags());
    if (ImGui::IsItemActive()) {
        slider_active = true;
    }
    if (const int s = Nav::Step()) {
        // to the step's grid, so that 1.0 + 0.1 reads 1.1 in the file
        const float next = std::clamp(std::round((value + s * step) / step) * step, min, max);
        changed = changed || next != value;
        value = next;
    }
    return changed;
}

// ── The map view's notes (Port::Hooks::MapViewNote, TargetViewNote; port/src/map_view.h) ──

void Menu::GatherViewNotes(Rules::Facts& f) const {
    // the hooks say why the quest's map view, or the target camera's, can't show (the player's
    // panels have no map panel, ...); "" when it can. They read the running options and the last
    // quest's panels, on the emulation thread, where the menu draws (window.cpp, Layer::Frame).
    // Menu-level reasons come first (R1-R3, menu_rules.cpp).
    f.map_view_note = Port::Hooks::MapViewNote();
    f.target_view_note = Port::Hooks::TargetViewNote();
}

std::string Menu::EditorViewNote() const {
    // in the editor's toolbar, under its help line: the map's, else the target camera's while on
    std::string note = Port::Hooks::MapViewNote();
    if (note.empty() && Port::Hooks::ParseTargetOverlay(Game("target_overlay")).on) {
        note = Port::Hooks::TargetViewNote();
    }
    return note;
}

} // namespace App::Ui
