#include "ui/touch_taps.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>
#include <utility>
#include <vector>
#include <SDL.h>
#include <fmt/format.h>
#include <imgui.h>

#include "config.h"
#include "hooks.h"
#include "input.h"
#include "taps.h"
#include "ui/layer.h"
#include "window.h"

namespace App::Ui {

namespace {

constexpr int SLOTS = 2;  ///< keys a tap takes in the menu (mh4u-app.ini takes more)

/// What the section keeps between frames.
struct State {
    int selected = 0;       ///< the tap a click on the touch screen moves
    int capture = -1;       ///< the tap whose key waits for a key, -1 none
    int capture_slot = 0;   ///< which of its keys
    std::string note;       ///< why the last key wasn't taken
    bool dragging = false;  ///< a point is being moved on the touch screen
    bool reload = false;    ///< a point moved: the input profile takes it once nothing is held
};

State& SectionState() {
    static State state;
    return state;
}

/// A table's header row as plain text: ImGui's own header cells are buttons, which the keyboard
/// and controller would stop at on their way to the table's controls (as the menu's tables).
void HeaderRow() {
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_TableHeaderBg));
    for (int column = 0; column < ImGui::TableGetColumnCount(); ++column) {
        ImGui::TableSetColumnIndex(column);
        ImGui::TextUnformatted(ImGui::TableGetColumnName(column));
    }
}

/// The input profile again, with the taps as they are now (main.cpp builds it between frames).
void Reload(Layer& layer) {
    layer.window.RequestInputReload();
}

} // namespace

bool DrawTouchTaps(Layer& layer) {
    State& s = SectionState();
    Config& config = layer.config;
    std::vector<Taps::Tap>& taps = config.touch_taps;
    const float em = ImGui::GetFontSize();
    bool changed = false;
    s.selected = std::clamp(s.selected, 0, std::max(static_cast<int>(taps.size()) - 1, 0));

    ImGui::Spacing();
    ImGui::SeparatorText("Touch taps");
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("A key held touches the touch screen at a point, and lets go with the key: an item "
                       "shortcut, the target camera or any other panel, without the mouse. Up to %d.",
                       Taps::MAX);
    ImGui::PopStyleColor();
    if (!s.note.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "%s", s.note.c_str());
    }

    // a row each: which one the touch screen below moves, a name, two keys, the point, Remove
    int remove = -1;
    if (!taps.empty() && ImGui::BeginTable("##touch_taps", 7, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        const float remove_width = ImGui::CalcTextSize("Remove").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::TableSetupColumn("Tap", ImGuiTableColumnFlags_WidthStretch, 0.08f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.24f);
        ImGui::TableSetupColumn("Key or mouse", ImGuiTableColumnFlags_WidthStretch, 0.19f);
        ImGui::TableSetupColumn("Another", ImGuiTableColumnFlags_WidthStretch, 0.19f);
        ImGui::TableSetupColumn("X", ImGuiTableColumnFlags_WidthStretch, 0.15f);
        ImGui::TableSetupColumn("Y", ImGuiTableColumnFlags_WidthStretch, 0.15f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, remove_width);
        HeaderRow();
        for (int i = 0; i < static_cast<int>(taps.size()); ++i) {
            Taps::Tap& tap = taps[i];
            // a control of the row being used makes it the one the touch screen shows on top
            const auto pick = [&s, i] {
                if (ImGui::IsItemActivated()) {
                    s.selected = i;
                }
            };
            ImGui::PushID(i);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            if (ImGui::RadioButton(std::to_string(i + 1).c_str(), s.selected == i)) {
                s.selected = i;
            }
            layer.Help("The tap a click on the touch screen below moves.");

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            // (a longer name from mh4u-app.ini is cut where a character ends, not inside one)
            char name[64] = {};
            std::size_t length = std::min(tap.name.size(), sizeof(name) - 1);
            while (length > 0 && (static_cast<unsigned char>(tap.name[length]) & 0xC0) == 0x80) {
                --length;
            }
            tap.name.copy(name, length);
            if (ImGui::InputTextWithHint("##name", "e.g. Potion", name, sizeof(name))) {
                tap.name = name;
                changed = true;
            }
            pick();
            layer.Help("A name for the key guides (Mega potion, Target camera); without one they show the point.");

            for (int slot = 0; slot < SLOTS; ++slot) {
                ImGui::TableSetColumnIndex(2 + slot);
                const std::vector<std::string> names = Taps::Names(tap.keys);
                const std::string label = s.capture == i && s.capture_slot == slot ? std::string("Press a key...")
                                          : slot < static_cast<int>(names.size()) ? names[slot]
                                                                                   : std::string("-");
                // (### : the button keeps its ID, and a controller's cursor stays on it, as its text changes)
                if (ImGui::Button(fmt::format("{}###key{}", label, slot).c_str(), ImVec2(-FLT_MIN, 0.0f))) {
                    s.capture = i;
                    s.capture_slot = slot;
                    s.selected = i;
                    s.note.clear();
                    layer.SetKeyCapture(true);
                }
                layer.Help("Choose, then press the key or the mouse button that touches this point (Esc: keep the "
                           "one it has, Delete: none). A key taken from another input leaves that one without.");
            }

            for (int axis = 0; axis < 2; ++axis) {
                ImGui::TableSetColumnIndex(4 + axis);
                ImGui::SetNextItemWidth(-FLT_MIN);
                int& value = axis == 0 ? tap.x : tap.y;
                const int last = (axis == 0 ? Taps::WIDTH : Taps::HEIGHT) - 1;
                int v = std::clamp(value, 0, last);
                if (ImGui::SliderInt(axis == 0 ? "##x" : "##y", &v, 0, last, "%d", ImGuiSliderFlags_AlwaysClamp)) {
                    value = v;
                    changed = true;
                    s.reload = true;
                }
                pick();
                layer.Help(axis == 0 ? "Across: 0 at the touch screen's left edge, 319 at its right edge."
                                     : "Down: 0 at the touch screen's top edge, 239 at its bottom edge.");
            }

            ImGui::TableSetColumnIndex(6);
            if (ImGui::Button("Remove")) {
                remove = i;
            }
            layer.Help("This tap goes; its keys do nothing until an input takes them.");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (remove >= 0) {
        taps.erase(taps.begin() + remove);
        if (s.capture == remove) {
            s.capture = -1;
            layer.SetKeyCapture(false);
        } else if (s.capture > remove) {
            --s.capture;
        }
        if (s.selected > remove) {
            --s.selected;
        }
        s.selected = std::clamp(s.selected, 0, std::max(static_cast<int>(taps.size()) - 1, 0));
        changed = true;
        s.reload = true;
    }

    // a new tap, and a point from the game's touch screen
    int touch_x = 0, touch_y = 0;
    const bool touched = Input::LastTouch(touch_x, touch_y);
    const std::string stylus = Input::FirstKey(config.stylus_key);
    ImGui::BeginDisabled(static_cast<int>(taps.size()) >= Taps::MAX);
    if (ImGui::Button("Add a tap")) {
        Taps::Tap tap;
        if (touched) {
            tap.x = touch_x;
            tap.y = touch_y;
        }
        taps.push_back(tap);
        s.selected = static_cast<int>(taps.size()) - 1;
        changed = true;
    }
    ImGui::EndDisabled();
    layer.Help("A new tap at the point last touched in the game (the middle if none yet); then choose its key.");
    ImGui::SameLine();
    const std::string use = touched ? fmt::format("Use the last touch ({}, {})###last_touch", touch_x, touch_y)
                                    : std::string("Use the last touch###last_touch");
    const bool have_tap = s.selected < static_cast<int>(taps.size());
    ImGui::BeginDisabled(!touched || !have_tap);
    if (ImGui::Button(use.c_str()) && touched && have_tap) {
        taps[s.selected].x = touch_x;
        taps[s.selected].y = touch_y;
        changed = true;
        s.reload = true;
    }
    ImGui::EndDisabled();
    static std::string use_help;
    use_help = fmt::format("The selected tap's point, where you last touched the touch screen in the game: touch "
                           "the panel there first ({}), then come back.",
                           stylus.empty() ? "with the mouse while it is the stylus"
                                          : "with the keyboard and mouse, hold " + stylus + " and click it");
    layer.Help(use_help.c_str());

    // the touch screen: the taps' points on a quest's panel slots, and the last touch
    const float room = ImGui::GetContentRegionAvail().x;
    const float width = std::max(1.0f, std::min(room, std::round(em * 15.0f)));
    const bool text_beside = room - width >= em * 10.0f;
    const float scale = width / static_cast<float>(Taps::WIDTH);
    const ImVec2 size(width, std::round(static_cast<float>(Taps::HEIGHT) * scale));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const auto screen = [&at, scale](float x, float y) { return ImVec2(at.x + x * scale, at.y + y * scale); };
    const auto point = [&at, scale](const ImVec2& m) {
        // (kept to the screen before it is a whole number: a mouse that is gone is at -FLT_MAX)
        const float x = std::clamp((m.x - at.x) / scale, 0.0f, static_cast<float>(Taps::WIDTH - 1));
        const float y = std::clamp((m.y - at.y) / scale, 0.0f, static_cast<float>(Taps::HEIGHT - 1));
        return std::pair{static_cast<int>(x), static_cast<int>(y)};
    };
    const float radius = std::max(std::round(em * 0.55f), 6.0f);
    // (the keyboard and the controller set the points with the sliders)
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    ImGui::InvisibleButton("##touch_screen", size);
    ImGui::PopItemFlag();
    if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !taps.empty()) {
        // a tap's dot under the mouse is the one to move; anywhere else the selected tap moves there
        float nearest = radius * radius * 2.0f;
        for (int i = 0; i < static_cast<int>(taps.size()); ++i) {
            const ImVec2 c = screen(static_cast<float>(taps[i].x) + 0.5f, static_cast<float>(taps[i].y) + 0.5f);
            const float d = (mouse.x - c.x) * (mouse.x - c.x) + (mouse.y - c.y) * (mouse.y - c.y);
            if (d < nearest) {
                nearest = d;
                s.selected = i;
            }
        }
        s.dragging = true;
    }
    if (s.dragging && (!ImGui::IsItemActive() || s.selected >= static_cast<int>(taps.size()))) {
        s.dragging = false;
    }
    if (s.dragging) {
        const auto [x, y] = point(mouse);
        Taps::Tap& tap = taps[s.selected];
        if (tap.x != x || tap.y != y) {
            tap.x = x;
            tap.y = y;
            changed = true;
            s.reload = true;
        }
    }
    if (ImGui::IsItemHovered()) {
        const auto [x, y] = point(mouse);
        ImGui::SetTooltip("%d, %d", x, y);
    }
    layer.Help("The touch screen: click to put the selected tap's point there, or drag a tap. The lines are a "
               "quest's panel slots; the blue cross is where the touch screen was last touched in the game.");

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(at.x + size.x, at.y + size.y);
    const float rounding = std::round(em * 0.2f);
    const float thin = std::max(1.0f, std::round(em * 0.06f));
    draw->AddRectFilled(at, end, IM_COL32(22, 22, 26, 255), rounding);
    draw->PushClipRect(at, end, true);
    // a quest's touch panels (port/src/hooks.h): a bar at the top, two columns of three slots under it
    const ImU32 slot_line = IM_COL32(255, 255, 255, 46);
    const float slot_width = static_cast<float>(Port::Hooks::PANEL_SLOT_WIDTH);
    const float slot_height = static_cast<float>(Port::Hooks::PANEL_SLOT_HEIGHT);
    for (int column = 0; column < 2; ++column) {
        for (int row = 0; row < 3; ++row) {
            const float left = static_cast<float>(column) * slot_width;
            const float top = static_cast<float>(Port::Hooks::PANEL_SLOTS_TOP) + static_cast<float>(row) * slot_height;
            draw->AddRect(screen(left + 3.0f, top + 3.0f), screen(left + slot_width - 3.0f, top + slot_height - 3.0f),
                          slot_line, rounding, thin);
        }
    }
    if (touched) {
        const ImVec2 c = screen(static_cast<float>(touch_x) + 0.5f, static_cast<float>(touch_y) + 0.5f);
        const float r = radius * 0.8f;
        const ImU32 blue = IM_COL32(110, 190, 255, 235);
        draw->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), blue, thin * 2.0f);
        draw->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), blue, thin * 2.0f);
    }
    const ImU32 accent = ImGui::GetColorU32(ImGuiCol_CheckMark);
    for (int pass = 0; pass < 2; ++pass) {  // the selected tap last, on top
        for (int i = 0; i < static_cast<int>(taps.size()); ++i) {
            const bool selected = i == s.selected;
            if (selected != (pass == 1)) {
                continue;
            }
            const ImVec2 c = screen(static_cast<float>(taps[i].x) + 0.5f, static_cast<float>(taps[i].y) + 0.5f);
            const int alpha = taps[i].keys.empty() ? 140 : 235;  // without a key, dimmer
            draw->AddCircleFilled(c, radius, selected ? accent : IM_COL32(96, 96, 106, alpha));
            draw->AddCircle(c, radius, IM_COL32(0, 0, 0, 170), 0, thin);
            const std::string number = std::to_string(i + 1);
            const ImVec2 text = ImGui::CalcTextSize(number.c_str());
            draw->AddText(ImVec2(c.x - text.x * 0.5f, c.y - text.y * 0.5f),
                          selected ? IM_COL32(28, 22, 12, 255) : IM_COL32(255, 255, 255, 255), number.c_str());
        }
    }
    draw->PopClipRect();
    draw->AddRect(at, end, IM_COL32(255, 255, 255, 80), rounding, thin);

    // beside it (under it in a narrow window), what the selected tap touches and how to find a point
    if (text_beside) {
        ImGui::SameLine();
    }
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::PushTextWrapPos(0.0f);
    if (taps.empty()) {
        ImGui::TextUnformatted("No taps yet: Add a tap, choose its key and put its point here.");
    } else {
        const Taps::Tap& tap = taps[s.selected];
        ImGui::Text("Tap %d touches %d, %d.", s.selected + 1, tap.x, tap.y);
    }
    const std::string find = fmt::format(
        "To find a panel's point, touch it in the game ({}), come back and press Use the last touch: the blue "
        "cross shows it.",
        stylus.empty() ? "with the mouse while it is the stylus" : "with the keyboard and mouse, hold " + stylus + " and click it");
    ImGui::TextUnformatted(find.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::EndGroup();

    // a point moved takes effect in the game once it is let go
    if (s.reload && !ImGui::IsAnyItemActive()) {
        s.reload = false;
        Reload(layer);
    }
    return changed;
}

bool TouchTapCapturing() {
    return SectionState().capture >= 0;
}

bool OnTouchTapKey(Layer& layer, int code) {
    State& s = SectionState();
    const int tap = s.capture;
    const int slot = s.capture_slot;
    s.capture = -1;
    layer.SetKeyCapture(false);
    Config& config = layer.config;
    if (tap < 0 || tap >= static_cast<int>(config.touch_taps.size())) {
        return false;
    }
    s.note.clear();
    if (code == SDL_SCANCODE_ESCAPE) {
        return false;  // kept as it was
    }
    Taps::Tap& t = config.touch_taps[tap];
    if (code == SDL_SCANCODE_DELETE) {
        t.keys = Taps::WithKey(t.keys, slot, "");
        Reload(layer);
        return true;
    }
    const std::string key = Input::KeyName(code);
    if (key.empty()) {
        s.note = "That key has no name to keep in mh4u-app.ini.";
        return false;
    }
    Taps::Others others = TapOthers(config);
    if (Taps::IsOwn(others, key)) {
        s.note = fmt::format("{} is the app's own key.", key);
        return false;
    }
    // one key, one input: it leaves the one it had
    Taps::Give(config.touch_taps, tap, slot, key, others);
    Reload(layer);
    return true;
}

void ReleaseTouchTapKey(Config& config, const std::string& key) {
    Taps::Release(config.touch_taps, key);
}

void EndTouchTapEdit(Layer& layer) {
    State& s = SectionState();
    if (s.capture >= 0) {
        s.capture = -1;
        layer.SetKeyCapture(false);
    }
    s.note.clear();
    s.dragging = false;
    if (s.reload) {
        s.reload = false;
        Reload(layer);
    }
}

} // namespace App::Ui
