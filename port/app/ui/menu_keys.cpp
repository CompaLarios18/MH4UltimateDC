// The in-game menu's Keys tab, and the same bindings' rows elsewhere (Touch screen's Map key,
// Gameplay's Fast forward key and Pause key): a key or mouse button per column, captured from the
// next press, one key for one input. mh4u-app.ini's [Controls] keeps them (config.h).
#include <algorithm>
#include <cfloat>
#include <cstdlib>
#include <iterator>
#include <string>
#include <vector>
#include <SDL.h>
#include <fmt/format.h>
#include <imgui.h>

#include "common/param_package.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "config.h"
#include "input.h"
#include "ui/layer.h"
#include "ui/menu.h"
#include "ui/menu_parts.h"

namespace App::Ui {

using namespace MenuParts;

namespace {

namespace NativeAnalog = Settings::NativeAnalog;
namespace NativeButton = Settings::NativeButton;

using R = Index::RowId;
using K = Menu::Focus::Kind;

/// The app's settings with a binding that aren't a 3DS button or stick.
enum class Extra { None, ItemPrev, ItemNext, Stylus, TouchScreen, Guides, Pause, FastForward };

struct Binding {
    Index::RowId row; ///< its row in Keys
    int button;       ///< Settings::NativeButton, or -1 for a stick's direction or an extra
    int stick;        ///< Settings::NativeAnalog
    int part;         ///< of the stick's keys: up, down, left, right, the half-way key
    Extra extra = Extra::None;
};

// In the order of before (the capture ids, index * SLOTS + slot, stay); Keys draws them in its
// own order, the index's.
constexpr Binding BINDINGS[] = {
    {R::KeysA, NativeButton::A, -1, 0},
    {R::KeysB, NativeButton::B, -1, 0},
    {R::KeysX, NativeButton::X, -1, 0},
    {R::KeysY, NativeButton::Y, -1, 0},
    {R::KeysL, NativeButton::L, -1, 0},
    {R::KeysR, NativeButton::R, -1, 0},
    {R::KeysZl, NativeButton::ZL, -1, 0},
    {R::KeysZr, NativeButton::ZR, -1, 0},
    {R::KeysStart, NativeButton::Start, -1, 0},
    {R::KeysSelect, NativeButton::Select, -1, 0},
    {R::KeysDpadUp, NativeButton::Up, -1, 0},
    {R::KeysDpadDown, NativeButton::Down, -1, 0},
    {R::KeysDpadLeft, NativeButton::Left, -1, 0},
    {R::KeysDpadRight, NativeButton::Right, -1, 0},
    {R::KeysMoveForward, -1, NativeAnalog::CirclePad, 0},
    {R::KeysMoveBack, -1, NativeAnalog::CirclePad, 1},
    {R::KeysMoveLeft, -1, NativeAnalog::CirclePad, 2},
    {R::KeysMoveRight, -1, NativeAnalog::CirclePad, 3},
    {R::KeysWalk, -1, NativeAnalog::CirclePad, 4},
    {R::KeysCameraUp, -1, NativeAnalog::CStick, 0},
    {R::KeysCameraDown, -1, NativeAnalog::CStick, 1},
    {R::KeysCameraLeft, -1, NativeAnalog::CStick, 2},
    {R::KeysCameraRight, -1, NativeAnalog::CStick, 3},
    {R::KeysItemPrev, -1, -1, 0, Extra::ItemPrev},
    {R::KeysItemNext, -1, -1, 0, Extra::ItemNext},
    {R::KeysStylus, -1, -1, 0, Extra::Stylus},
    {R::KeysTouchScreen, -1, -1, 0, Extra::TouchScreen},
    {R::KeysGuides, -1, -1, 0, Extra::Guides},
    {R::KeysPause, -1, -1, 0, Extra::Pause},
    {R::KeysFastForward, -1, -1, 0, Extra::FastForward},
};
constexpr int BINDING_COUNT = static_cast<int>(std::size(BINDINGS));
constexpr int SLOTS = 2; ///< keys a binding takes in the menu (mh4u-app.ini takes more)

/// The 3DS buttons, the first 14 BINDINGS: mh4u.ini's name, the game's action ("" none), and the
/// name in a label.
struct ButtonName {
    const char* ini;
    const char* action;
    const char* name;
};
constexpr ButtonName BUTTON_NAMES[] = {
    {"A", "attack, gather, talk", "A"}, {"B", "evade, crouch", "B"},   {"X", "draw, attack", "X"},
    {"Y", "use item, sheathe", "Y"},     {"L", "camera, focus", "L"},   {"R", "dash, guard", "R"},
    {"ZL", "", "ZL"},                    {"ZR", "", "ZR"},              {"START", "game menu", "START"},
    {"SELECT", "", "SELECT"},            {"UP", "", "D-pad up"},        {"DOWN", "", "D-pad down"},
    {"LEFT", "", "D-pad left"},          {"RIGHT", "", "D-pad right"}};

constexpr const char* PARAMS_HELP = "Set with Azahar's input parameters in mh4u-app.ini (param_...). Reset the keys "
                                    "to the defaults to use keys again.";

/// A stick's keys, "up,down,left,right[,half way]", as five names (empty: none).
std::vector<std::string> StickKeys(const std::string& spec) {
    std::vector<std::string> keys;
    for (const std::string& part : Common::SplitString(spec, ',')) {
        keys.push_back(Common::StripSpaces(part));
    }
    keys.resize(5);
    return keys;
}

std::string JoinStickKeys(const std::vector<std::string>& keys) {
    std::string spec = fmt::format("{},{},{},{}", keys[0], keys[1], keys[2], keys[3]);
    if (keys.size() > 4 && !keys[4].empty()) {
        spec += "," + keys[4];
    }
    return spec;
}

/// The setting a button or an extra keeps its keys in ("F | Mouse Right"); null for a stick.
std::string* SpecOf(Config& config, const Binding& b) {
    if (b.button >= 0) {
        return &config.keys[b.button];
    }
    switch (b.extra) {
    case Extra::ItemPrev:
        return &config.item_prev;
    case Extra::ItemNext:
        return &config.item_next;
    case Extra::Stylus:
        return &config.stylus_key;
    case Extra::TouchScreen:
        return &config.touch_screen_key;
    case Extra::Guides:
        return &config.key_guides_key;
    case Extra::Pause:
        return &config.pause_key;
    case Extra::FastForward:
        return &config.fast_forward_key;
    default:
        return nullptr;
    }
}

bool IsStick(const Binding& b) {
    return b.button < 0 && b.extra == Extra::None;
}

std::string KeyOf(Config& config, const Binding& b, int slot) {
    if (IsStick(b)) {
        return slot == 0 ? StickKeys(config.stick_keys[b.stick])[b.part] : "";
    }
    const auto names = Input::KeyNames(*SpecOf(config, b));
    return slot < static_cast<int>(names.size()) ? names[slot] : "";
}

void SetKey(Config& config, const Binding& b, int slot, const std::string& key) {
    if (IsStick(b)) {
        auto keys = StickKeys(config.stick_keys[b.stick]);
        keys[b.part] = key;
        config.stick_keys[b.stick] = JoinStickKeys(keys);
        return;
    }
    std::string* spec = SpecOf(config, b);
    auto names = Input::KeyNames(*spec);
    if (slot < static_cast<int>(names.size())) {
        names[slot] = key;
    } else {
        names.push_back(key);
    }
    std::string joined;
    for (const std::string& name : names) {
        if (!name.empty()) {
            joined += (joined.empty() ? "" : " | ") + name;
        }
    }
    *spec = joined;
}

std::string ControllerButtonName(int button) {
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A:
        return "A (bottom)";
    case SDL_CONTROLLER_BUTTON_B:
        return "B (right)";
    case SDL_CONTROLLER_BUTTON_X:
        return "X (left)";
    case SDL_CONTROLLER_BUTTON_Y:
        return "Y (top)";
    case SDL_CONTROLLER_BUTTON_BACK:
        return "Back";
    case SDL_CONTROLLER_BUTTON_GUIDE:
        return "Guide";
    case SDL_CONTROLLER_BUTTON_START:
        return "Start";
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:
        return "Left stick click";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
        return "Right stick click";
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        return "LB";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
        return "RB";
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        return "D-pad up";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        return "D-pad down";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        return "D-pad left";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        return "D-pad right";
    default:
        return fmt::format("button {}", button);
    }
}

std::string ControllerAxisName(int axis) {
    switch (axis) {
    case SDL_CONTROLLER_AXIS_LEFTX:
    case SDL_CONTROLLER_AXIS_LEFTY:
        return "Left stick";
    case SDL_CONTROLLER_AXIS_RIGHTX:
    case SDL_CONTROLLER_AXIS_RIGHTY:
        return "Right stick";
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
        return "LT";
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
        return "RT";
    default:
        return fmt::format("axis {}", axis);
    }
}

/// What a controller does for an input in the profile (Settings' parameters), or empty.
std::string ControllerName(const std::string& params) {
    const Common::ParamPackage p{params};
    const std::string engine = p.Get("engine", "");
    if (engine == "mh4u_any") { // the app's keys, mouse and controller together (input.cpp)
        for (int i = p.Get("n", 0) - 1; i >= 0; --i) {
            if (std::string name = ControllerName(p.Get(fmt::format("d{}", i), "")); !name.empty()) {
                return name;
            }
        }
        return "";
    }
    if (engine == "mh4u_mouse") {
        return "";
    }
    if (engine == "analog_from_button") {
        return "";
    }
    if (engine == "keyboard" || engine.empty()) {
        return "";
    }
    if (engine != "sdl" && engine != "gcpad") {
        return engine;
    }
    if (p.Has("button")) {
        return ControllerButtonName(p.Get("button", -1));
    }
    if (p.Has("axis_x")) {
        return ControllerAxisName(p.Get("axis_x", -1));
    }
    if (p.Has("axis")) {
        return ControllerAxisName(p.Get("axis", -1));
    }
    if (p.Has("hat")) {
        return "D-pad (" + p.Get("direction", "") + ")";
    }
    return "controller";
}

/// A key table (Keys): the action, two keys, the controller; the same columns in every group, so
/// that they line up.
bool BeginKeyTable(const char* id) {
    const float unit = ImGui::GetFontSize() / 13.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(ImGui::GetStyle().CellPadding.x, 1.0f * unit));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 4.0f * unit));
    if (!ImGui::BeginTable(id, 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::PopStyleVar(2);
        return false;
    }
    // the action's 30 characters on one line at 1280x800 too
    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 0.37f);
    ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 0.2f);
    ImGui::TableSetupColumn("Second key", ImGuiTableColumnFlags_WidthStretch, 0.2f);
    ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch, 0.23f);
    return true;
}

void EndKeyTable() {
    ImGui::EndTable();
    ImGui::PopStyleVar(2);
}

/// A table's header row as plain text: ImGui's own header cells are buttons, which the keyboard
/// and controller would stop at on their way to the table's controls.
void HeaderRow() {
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_TableHeaderBg));
    for (int column = 0; column < ImGui::TableGetColumnCount(); ++column) {
        ImGui::TableSetColumnIndex(column);
        ImGui::TextUnformatted(ImGui::TableGetColumnName(column));
    }
}

} // namespace

std::string ControllerInputName(const std::string& params) {
    return ControllerName(params);
}

int Menu::BindingOf(RowId row) const {
    for (int i = 0; i < BINDING_COUNT; ++i) {
        if (BINDINGS[i].row == row) {
            return i;
        }
    }
    return -1;
}

const std::string& Menu::PadText(int index) {
    if (pad_cache.size() != static_cast<std::size_t>(BINDING_COUNT)) {
        pad_cache.assign(BINDING_COUNT, PadCache{});
    }
    const Binding& b = BINDINGS[index];
    PadCache& cache = pad_cache[index];
    if (b.button < 0 && !IsStick(b)) {
        // the app's own keys: what a controller does for them doesn't change
        if (!cache.valid) {
            cache.valid = true;
            cache.text = b.extra == Extra::TouchScreen                                      ? "Right stick click"
                         : b.extra == Extra::ItemPrev || b.extra == Extra::ItemNext ? "L + Y / A"
                                                                                            : "";
        }
        return cache.text;
    }
    const auto& profile = Settings::values.current_input_profile;
    const std::string& params = b.button >= 0 ? profile.buttons[b.button] : profile.analogs[b.stick];
    if (cache.valid && cache.params == params) {
        return cache.text; // parsed again only when the profile's parameters change
    }
    cache.valid = true;
    cache.params = params;
    cache.text = ControllerName(params);
    if (IsStick(b) && !cache.text.empty()) {
        static constexpr const char* directions[] = {"up", "down", "left", "right", ""};
        cache.text = b.part < 4 ? cache.text + " " + directions[b.part] : std::string("by how far it is pushed");
    }
    return cache.text;
}

void Menu::RefreshKeyTexts() {
    Config& config = layer.config;
    for (std::string& label : key_labels) {
        label.clear();
    }
    for (std::string& extra : search_extra) {
        extra.clear();
    }
    // a 3DS button's label: its game action, the port's roles given to it, then the button
    const std::string kick = Upper(Game("kick")), target = Upper(Game("target")), skip = Upper(Game("skip"));
    for (int i = 0; i < BINDING_COUNT; ++i) {
        const Binding& b = BINDINGS[i];
        std::string roles;
        if (i < static_cast<int>(std::size(BUTTON_NAMES))) {
            const ButtonName& button = BUTTON_NAMES[i];
            for (const auto& [ini, role] : {std::pair{kick, "kick"}, std::pair{target, "target camera"},
                                            std::pair{skip, "skip"}}) {
                if (ini == button.ini) {
                    roles += (roles.empty() ? "" : ", ") + std::string(role);
                }
            }
            std::string text = button.action;
            if (!roles.empty()) {
                text += (text.empty() ? "" : ", ") + roles;
            }
            if (text.empty()) {
                key_labels[static_cast<int>(b.row)] = button.name;
            } else {
                text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
                key_labels[static_cast<int>(b.row)] = text + " (" + button.name + ")";
            }
        }
        // the search's words for it: the keys and the controller's button bound now, the roles
        std::string extra;
        for (int slot = 0; slot < SLOTS; ++slot) {
            extra += " " + KeyOf(config, b, slot);
        }
        if (!IsStick(b)) {
            for (const std::string& name : Input::KeyNames(*SpecOf(config, b))) {
                extra += " " + name; // a third key and more, which mh4u-app.ini can give
            }
        }
        extra += " " + PadText(i) + " " + roles;
        search_extra[static_cast<int>(b.row)] = extra;
    }
    // the same bindings elsewhere
    search_extra[static_cast<int>(R::TouchMapKey)] = search_extra[static_cast<int>(R::KeysTouchScreen)];
    search_extra[static_cast<int>(R::GameplayFfKey)] = search_extra[static_cast<int>(R::KeysFastForward)];
    search_extra[static_cast<int>(R::GameplayPauseKey)] = search_extra[static_cast<int>(R::KeysPause)];
}

// ── Keys ──

void Menu::DrawKeys() {
    // at the top: the way to Gameplay's buttons, why the last key wasn't taken, R12
    if (BeginSettings("##keys_links")) {
        // #59: "Kick, target camera and touch taps"
        LinkRow("Kick and target camera buttons", Tab::Gameplay,
                "Opens Gameplay: the kick and the target camera on buttons, the end of quests, fast forward.",
                "keys_gameplay");
        EndSettings();
    }
    if (!capture_note.empty() && capture_place != R::Count && Index::TabOf(capture_place) == Tab::Keys) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(CAPTURE_COLOUR, "%s", capture_note.c_str());
        ImGui::PopTextWrapPos();
    }
    if (!verdict.keys_top.Empty()) {
        DrawNote(verdict.keys_top, Index::ROW_COUNT + Index::SECTION_COUNT, false);
    }
    if (BeginKeyTable("##keys_header")) {
        HeaderRow();
        EndKeyTable();
    }
    // a section per group, in the index's order (BINDINGS keep theirs)
    SectionId section = SectionId::Count;
    bool table = false;
    for (int i = 0; i < Index::ROW_COUNT; ++i) {
        const auto row = static_cast<RowId>(i);
        if (Index::TabOf(row) != Tab::Keys) {
            continue;
        }
        if (Index::SectionOf(row) != section) {
            if (table) {
                EndKeyTable();
            }
            section = Index::SectionOf(row);
            Section(section);
            table = BeginKeyTable(Index::Get(section).id);
        }
        if (!table) {
            continue;
        }
        if (Index::Get(row).kind == Index::Kind::Fixed) {
            DrawFixedKeyRow(row);
        } else if (const int binding = BindingOf(row); binding >= 0) {
            DrawKeyRow(row, binding);
        }
    }
    if (table) {
        EndKeyTable();
    }
    DrawResetKeys();
}

void Menu::DrawKeyRow(RowId row, int index) {
    const Binding& b = BINDINGS[index];
    item = {};
    item.row = row;
    ImGui::TableNextRow();
    FlashRow(row);
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextWrapped("%s", LabelOf(row).c_str());
    for (int slot = 0; slot < SLOTS; ++slot) {
        ImGui::TableSetColumnIndex(1 + slot);
        if (slot == 1 && IsStick(b)) {
            continue; // a stick's direction takes one key
        }
        KeyButton(index, slot, "key", -FLT_MIN, row);
    }
    ImGui::TableSetColumnIndex(3);
    ImGui::AlignTextToFramePadding();
    const std::string& pad = PadText(index);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", pad.empty() ? "-" : pad.c_str());
    ImGui::PopTextWrapPos();
    item = {};
}

void Menu::DrawFixedKeyRow(RowId row) {
    const Config& config = layer.config;
    ImGui::TableNextRow();
    FlashRow(row);
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextWrapped("%s", LabelOf(row).c_str());
    ImGui::TableSetColumnIndex(1);
    ImGui::AlignTextToFramePadding();
    std::string keys = "Esc";
    if (row == R::KeysFixedFullscreen) {
        const std::string key = Input::FirstKey(config.fullscreen_key);
        keys = key.empty() ? "Alt+Enter" : key + " or Alt+Enter";
    } else if (row == R::KeysFixedFpsCounter) {
        const std::string key = Input::FirstKey(config.fps_counter_key);
        keys = key.empty() ? "-" : key;
    }
    ImGui::TextUnformatted(keys.c_str());
    layer.Help(HelpOf(row)); // set in mh4u-app.ini, not here: a text, not a button
    ImGui::TableSetColumnIndex(3);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", row == R::KeysFixedMenu ? "Guide, Back+Start" : "-");
    // the search's jump to it: the page scrolls here, then the cursor goes to the next control
    if (focus.kind == K::Next && focus.row == row && ImGui::GetFrameCount() >= focus.ready) {
        if (!focus.scrolled) {
            ImGui::SetScrollHereY(0.35f);
            focus.scrolled = true;
            focus.ready = ImGui::GetFrameCount() + 1;
            focus.frames = 0;
        } else {
            next_armed = true;
        }
    }
}

void Menu::KeyButton(int index, int slot, const char* prefix, float width, RowId place) {
    const Binding& b = BINDINGS[index];
    Config& config = layer.config;
    const bool custom = b.button >= 0 ? !config.button_params[b.button].empty()
                        : IsStick(b)  ? !config.stick_params[b.stick].empty()
                                      : false;
    const int id = index * SLOTS + slot;
    const std::string key = KeyOf(config, b, slot);
    const std::string label = capture == id ? std::string("Press a key...")
                              : custom      ? std::string("(Azahar parameters)")
                              : key.empty() ? std::string("-")
                                            : key;
    ImGui::BeginDisabled(custom);
    // the ID from "###..." only: the label changes (Press a key...) under the cursor
    if (ImGui::Button(fmt::format("{}###{}{}", label, prefix, id).c_str(), ImVec2(width, 0.0f))) {
        StartCapture(id, place);
    }
    ImGui::EndDisabled();
    layer.Help(custom ? PARAMS_HELP : HelpOf(place));
}

void Menu::DrawContextKeyRow(RowId row, int index) {
    // the Keys row's two key buttons, side by side in the control's cell
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float width = std::max(1.0f, (ImGui::GetContentRegionAvail().x - spacing) * 0.5f);
    KeyButton(index, 0, "ctx_key", width, row);
    ImGui::SameLine();
    KeyButton(index, 1, "ctx_key", width, row);
}

void Menu::StartCapture(int id, RowId place) {
    EndTapEdit(); // one wait at a time
    capture = id;
    capture_place = place;
    capture_note.clear();
    layer.SetKeyCapture(true);
}

void Menu::DrawResetKeys() {
    ImGui::Spacing();
    if (ImGui::Button("Reset keys to the defaults")) {
        ImGui::OpenPopup("Reset the keys?");
    }
    layer.Help("Every key as it came, after Monster Hunter Rise on PC, and the controller mapped automatically. It "
               "asks first.");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Reset the keys?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        // #59: "...mapped automatically. Touch taps keep their keys; the mouse settings and Fast
        // forward mode stay."
        ImGui::TextUnformatted("Every key goes back to how it came, after Monster Hunter Rise on PC, and the "
                               "controller is mapped automatically. The mouse settings and Fast forward mode stay.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("Reset")) {
            Config& config = layer.config;
            const Config defaults = App::Defaults();
            config.keys = defaults.keys;
            config.stick_keys = defaults.stick_keys;
            config.button_params = defaults.button_params;
            config.stick_params = defaults.stick_params;
            config.controller_auto = defaults.controller_auto;
            config.item_prev = defaults.item_prev;
            config.item_next = defaults.item_next;
            config.stylus_key = defaults.stylus_key;
            config.touch_screen_key = defaults.touch_screen_key;
            config.key_guides_key = defaults.key_guides_key;
            config.pause_key = defaults.pause_key;
            config.fast_forward_key = defaults.fast_forward_key;
            // fast_forward_hold stays: it is Gameplay's Fast forward mode now (the question says so)
            SettleTaps();
            capture = -1;
            layer.SetKeyCapture(false);
            ControlsChanged(); // and the texts that name keys, next frame
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || CancelPressed()) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
}

void Menu::OnCapturedKey(int code) {
    if (TapTakesKey(code)) {
        return; // a touch tap's wait (#59)
    }
    if (capture < 0 || capture >= BINDING_COUNT * SLOTS) {
        layer.SetKeyCapture(false);
        return;
    }
    const Binding& binding = BINDINGS[capture / SLOTS];
    const int slot = capture % SLOTS;
    Config& config = layer.config;
    capture_note.clear();
    const auto app_key = [&](int c) {
        const auto has = [c](const std::string& spec) {
            const auto codes = Input::KeyCodes(spec);
            return std::find(codes.begin(), codes.end(), c) != codes.end();
        };
        return c == SDL_SCANCODE_ESCAPE || c == Input::KeyCode(config.fullscreen_key) ||
               c == Input::KeyCode(config.fps_counter_key) ||
               (binding.extra != Extra::TouchScreen && has(config.touch_screen_key)) ||
               (binding.extra != Extra::Guides && has(config.key_guides_key)) ||
               (binding.extra != Extra::Pause && has(config.pause_key)) ||
               (binding.extra != Extra::FastForward && has(config.fast_forward_key));
    };
    if (code == SDL_SCANCODE_ESCAPE) {
        // kept as it was
    } else if (code == SDL_SCANCODE_DELETE) {
        SetKey(config, binding, slot, "");
        ControlsChanged();
    } else if (const std::string key = Input::KeyName(code); key.empty()) {
        capture_note = "That key has no name to keep in mh4u-app.ini.";
    } else if (app_key(code)) {
        capture_note = fmt::format("{} is the app's own key.", key);
    } else if (IsStick(binding) && Input::MouseCode(key) != 0) {
        capture_note = "A stick's direction takes a key, not the mouse.";
    } else {
        // one key, one input: it leaves the one it had
        for (const Binding& other : BINDINGS) {
            for (int other_slot = 0; other_slot < SLOTS; ++other_slot) {
                if ((&other != &binding || other_slot != slot) && KeyOf(config, other, other_slot) == key) {
                    SetKey(config, other, other_slot, "");
                }
            }
        }
        SetKey(config, binding, slot, key);
        ReleaseTapKey(key); // and a touch tap that had it (#59)
        ControlsChanged();
    }
    capture = -1;
    layer.SetKeyCapture(false);
}

// ── #59: touch taps (wp/main-59-touch-taps, ui/touch_taps.h), the key capture's hand-off ──

bool Menu::TapCapturing() const {
    // #59: return TouchTapCapturing();
    return false;
}

void Menu::EndTapEdit() {
    // #59: EndTouchTapEdit(layer);
}

bool Menu::TapTakesKey(int code) {
    // #59: a touch tap waiting for its key takes the key pressed (Menu::OnCapturedKey, first):
    //   if (!TouchTapCapturing()) return false;
    //   if (OnTouchTapKey(layer, code)) Changed(AppSetting::Controls);
    //   return true;
    (void)code;
    return false;
}

void Menu::ReleaseTapKey(const std::string& key) {
    // #59: ReleaseTouchTapKey(layer.config, key);
    (void)key;
}

void Menu::SettleTaps() {
    // #59: SettleTouchTaps(layer.config);
}

} // namespace App::Ui
