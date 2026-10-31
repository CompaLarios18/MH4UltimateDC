// The in-game menu's HUD layout editor (Touch screen > In quests > Edit HUD layout): the quest's map
// and target camera views over the game, moved and resized with the mouse (or a finger), a
// controller or the keyboard; Done keeps them in mh4u.ini, Cancel puts them back.
#include <algorithm>
#include <cmath>
#include <string>
#include <fmt/format.h>
#include <imgui.h>

#include "common/logging/log.h"
#include "hooks.h"
#include "ui/layer.h"
#include "ui/menu.h"
#include "ui/menu_parts.h"
#include "ui/nav.h"

namespace App::Ui {

using namespace MenuParts;

namespace {

using Port::Hooks::PanelView;
using Port::Hooks::Placement;
using K = Menu::Focus::Kind;

constexpr const char* PANEL_VIEW_KEYS[] = {"quest_overlay", "target_overlay"};
constexpr const char* VIEW_NAMES[] = {"Map", "Target camera"};

bool IsMapKey(const std::string& key) {
    return key == "quest_overlay";
}

std::string FormatView(const std::string& key, const PanelView& view) {
    return IsMapKey(key) ? Port::Hooks::FormatQuestOverlay(view) : Port::Hooks::FormatTargetOverlay(view);
}

/// A view's width over its height: the map is two touch-panel slots tall, the target camera one.
float ViewAspect(const std::string& key) {
    return static_cast<float>(Port::Hooks::PANEL_SLOT_WIDTH) /
           static_cast<float>(Port::Hooks::PANEL_SLOT_HEIGHT * (IsMapKey(key) ? 2 : 1));
}

/// Where a view is in a `width` x `height` window, as Azahar's overlay layout puts it.
ImVec4 ViewRect(const Placement& p, float aspect, float width, float height) {
    const float h = std::clamp(std::round(height * p.height), 1.0f, height);
    const float w = std::min(width, std::floor(h * aspect));
    return ImVec4(std::round(p.x * (width - w)), std::round(p.y * (height - h)), w, h);
}

/// A view placed with its top-left corner at `left`, `top` and `h` pixels tall, as PanelView keeps
/// it (fractions of the window's height and of the room it leaves beside and above the view).
PanelView PlaceView(PanelView view, float aspect, float width, float height, float left, float top, float h) {
    const float w = std::min(width, h * aspect);
    left = std::clamp(left, 0.0f, width - w);
    top = std::clamp(top, 0.0f, height - h);
    view.on = true;
    view.placed = true;
    view.height = h / height;
    view.x = width - w > 0.5f ? left / (width - w) : 0.0f;
    view.y = height - h > 0.5f ? top / (height - h) : 0.0f;
    return view;
}

} // namespace

void Menu::StartLayoutEdit() {
    layout_edit = true;
    drag = -1;
    pick = 0;
    layout_before.clear();
    layout_before_missing.clear();
    for (const char* key : PANEL_VIEW_KEYS) {
        if (const auto it = game_changes.find(key); it != game_changes.end()) {
            layout_before[key] = it->second;
        } else {
            layout_before_missing.insert(key);
        }
    }
    layer.Post([] { Port::Hooks::SetLayoutEditing(true); });
    LOG_INFO(Frontend, "menu: the HUD layout editor");
}

void Menu::EndLayoutEdit() {
    layout_edit = false;
    drag = -1;
    layer.Post([] { Port::Hooks::SetLayoutEditing(false); });
}

void Menu::CancelLayoutEdit() {
    if (!layout_edit) {
        return;
    }
    for (const char* key : PANEL_VIEW_KEYS) {
        if (layout_before_missing.count(key)) {
            game_changes.erase(key);
        } else {
            game_changes[key] = layout_before[key];
        }
        layer.Post([key = std::string(key), value = Game(key)] { Port::Hooks::SetOption(key, value); });
    }
    EndLayoutEdit();
    // back to Touch screen, on Edit HUD layout
    tab = Tab::TouchScreen;
    focus = {};
    focus.kind = K::Row;
    focus.row = RowId::TouchHudEditor;
    focus.cursor = true;
}

std::string Menu::EditorNumbers() const {
    const char* name = VIEW_NAMES[pick];
    const PanelView view = pick == 0 ? Port::Hooks::ParseQuestOverlay(Game("quest_overlay"))
                                     : Port::Hooks::ParseTargetOverlay(Game("target_overlay"));
    if (!view.on) {
        return fmt::format("{}: not shown", name);
    }
    const std::string size = view.height > 0.0f ? fmt::format("{:.0f}% high", view.height * 100.0f) : "at its scale";
    if (!view.placed) {
        return fmt::format("{}: {}, {}", name, pick == 0 ? "in the touch screen's corner" : "under the map", size);
    }
    return fmt::format("{}: {}, {:.0f}% across, {:.0f}% down", name, size, view.x * 100.0f, view.y * 100.0f);
}

void Menu::EditorKeys(float width, float height) {
    const ImGuiIO& io = ImGui::GetIO();
    const auto pressed = [](ImGuiKey key, bool repeat = false) { return ImGui::IsKeyPressed(key, repeat); };
    // LB/RB or Tab: the map or the target camera
    if (pressed(ImGuiKey_GamepadL1) || pressed(ImGuiKey_GamepadR1) || (pressed(ImGuiKey_Tab) && !io.KeyCtrl)) {
        pick = 1 - pick;
    }
    const std::string key = PANEL_VIEW_KEYS[pick];
    PanelView views[2] = {Port::Hooks::ParseQuestOverlay(Game("quest_overlay")),
                          Port::Hooks::ParseTargetOverlay(Game("target_overlay"))};
    // Y or Space: shows or hides it (the target camera only with the map), as its checkbox
    if (pressed(ImGuiKey_GamepadFaceUp) || pressed(ImGuiKey_Space)) {
        if (pick == 0 || views[0].on) {
            views[pick].on = !views[pick].on;
            SetGame(key, FormatView(key, views[pick]));
        }
        return;
    }
    // X or Backspace: Reset; A or Enter: Done
    if (pressed(ImGuiKey_GamepadFaceLeft) || pressed(ImGuiKey_Backspace)) {
        for (int i = 0; i < 2; ++i) {
            SetGame(PANEL_VIEW_KEYS[i], views[i].on ? (i == 0 ? "map" : "on") : "off");
        }
        return;
    }
    if (pressed(ImGuiKey_GamepadFaceDown) || pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) {
        EndLayoutEdit();
        layer.CloseMenu();
        return;
    }
    if (!views[0].on || !views[pick].on || width <= 0.0f || height <= 0.0f) {
        return; // nothing shown to move
    }
    // the D-pad or the arrows: 1% of the room a press, with repeat; the left stick up to 50% a second
    float dx = 0.0f, dy = 0.0f;
    if (pressed(ImGuiKey_GamepadDpadLeft, true) || pressed(ImGuiKey_LeftArrow, true)) {
        dx -= 0.01f;
    }
    if (pressed(ImGuiKey_GamepadDpadRight, true) || pressed(ImGuiKey_RightArrow, true)) {
        dx += 0.01f;
    }
    if (pressed(ImGuiKey_GamepadDpadUp, true) || pressed(ImGuiKey_UpArrow, true)) {
        dy -= 0.01f;
    }
    if (pressed(ImGuiKey_GamepadDpadDown, true) || pressed(ImGuiKey_DownArrow, true)) {
        dy += 0.01f;
    }
    const float speed = 0.5f * io.DeltaTime;
    dx += (Nav::KeyValue(ImGuiKey_GamepadLStickRight) - Nav::KeyValue(ImGuiKey_GamepadLStickLeft)) * speed;
    dy += (Nav::KeyValue(ImGuiKey_GamepadLStickDown) - Nav::KeyValue(ImGuiKey_GamepadLStickUp)) * speed;
    // LT/RT or - and +: 1% of the window's height smaller or larger, with repeat
    int grow = 0;
    if (pressed(ImGuiKey_GamepadL2, true) || pressed(ImGuiKey_Minus, true) || pressed(ImGuiKey_KeypadSubtract, true)) {
        grow -= 1;
    }
    if (pressed(ImGuiKey_GamepadR2, true) || pressed(ImGuiKey_Equal, true) || pressed(ImGuiKey_KeypadAdd, true)) {
        grow += 1;
    }
    if (dx == 0.0f && dy == 0.0f && grow == 0) {
        return;
    }
    // from where it is now (a view not placed yet too), as the drag does
    const auto touch = Port::Hooks::ParseOverlay(Game("touch_overlay"));
    const Placement now = pick == 0 ? Port::Hooks::MapPlacement(touch, views[0])
                                    : Port::Hooks::TargetPlacement(touch, views[0], views[1]);
    const float aspect = ViewAspect(key);
    const ImVec4 r = ViewRect(now, aspect, width, height);
    float h = r.w;
    if (grow != 0) {
        // the top-left corner stays; 3% to 100% of the window's height, inside the window
        h = std::clamp(h + static_cast<float>(grow) * 0.01f * height, std::max(height * 0.03f, 1.0f),
                       std::max(1.0f, std::min(height - r.y, (width - r.x) / aspect)));
    }
    PanelView view = PlaceView(views[pick], aspect, width, height, r.x, r.y, h);
    view.x = std::clamp(view.x + dx, 0.0f, 1.0f);
    view.y = std::clamp(view.y + dy, 0.0f, 1.0f);
    if (const std::string text = FormatView(key, view); text != Game(key)) {
        SetGame(key, text);
    }
}

void Menu::DrawLayoutEditor() {
    const ImGuiIO& io = ImGui::GetIO();
    const float em = ImGui::GetFontSize();
    const float width = io.DisplaySize.x, height = io.DisplaySize.y;

    // the toolbar, at the top in the middle: the mouse and a finger click it; the controller's and
    // the keyboard's keys move the views instead of its cursor (NoNav)
    ImGui::SetNextWindowPos(ImVec2(width * 0.5f, em * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    // a fixed width, so that its buttons stay put whatever the help line says
    ImGui::SetNextWindowSize(ImVec2(std::min(width * 0.9f, em * 26.0f), 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (just_opened) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::Begin("##layout_editor", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav);
    ImGui::TextUnformatted("Edit HUD layout");
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Drag a view to move it, its corner to resize it.");
    ImGui::PopStyleColor();
    ImGui::TextWrapped("%s", EditorNumbers().c_str());
    if (const std::string note = EditorViewNote(); !note.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, NOTE_COLOUR);
        ImGui::TextWrapped("%s", note.c_str());
        ImGui::PopStyleColor();
    }
    // the keys of the controller or the keyboard, whichever was used last (EditorKeys), in lines
    // that keep each key next to what it does
    const char* keys =
        layer.PadLast()
            ? "LB/RB: map or target camera   D-pad: move\nLT/RT: size   Y: show/hide\nX: reset   A: done   B: cancel"
            : "Tab: map or target camera   arrows: move\n-/+: size   Space: show/hide\n"
              "Backspace: reset   Enter: done   Esc: cancel";
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", keys);
    ImGui::PopStyleColor();
    PanelView views[2] = {Port::Hooks::ParseQuestOverlay(Game("quest_overlay")),
                          Port::Hooks::ParseTargetOverlay(Game("target_overlay"))};
    for (int i = 0; i < 2; ++i) {
        if (i == 1) {
            ImGui::SameLine();
        }
        bool on = views[i].on;
        ImGui::BeginDisabled(i == 1 && !views[0].on);
        if (ImGui::Checkbox(VIEW_NAMES[i], &on)) {
            views[i].on = on;
            SetGame(PANEL_VIEW_KEYS[i], FormatView(PANEL_VIEW_KEYS[i], views[i]));
        }
        ImGui::EndDisabled();
    }
    if (ImGui::Button("Done")) {
        EndLayoutEdit();
        layer.CloseMenu();
    }
    layer.Help("Keeps the views where they are (saved in mh4u.ini) and goes back to the game (A or Enter).");
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        // the touch screen's corner at its scale, the target camera under the map
        for (int i = 0; i < 2; ++i) {
            SetGame(PANEL_VIEW_KEYS[i], views[i].on ? (i == 0 ? "map" : "on") : "off");
        }
    }
    layer.Help("The map in the touch screen's corner at its scale, the target camera under it (X or Backspace).");
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        CancelLayoutEdit();
    }
    layer.Help("Puts them back as they were (Esc, or a controller's B).");
    const char* help = layer.HelpText();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", help ? help : "Esc or a controller's B: cancel. Sliders for both: Touch screen > In quests.");
    ImGui::PopStyleColor();
    ImGui::End();
    if (!layout_edit) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
        CancelLayoutEdit();
        return;
    }
    EditorKeys(width, height);
    if (!layout_edit) {
        return;
    }
    views[0] = Port::Hooks::ParseQuestOverlay(Game("quest_overlay"));
    views[1] = Port::Hooks::ParseTargetOverlay(Game("target_overlay"));
    const auto touch = Port::Hooks::ParseOverlay(Game("touch_overlay"));
    ImVec4 rects[2];
    bool shown[2];
    for (int i = 0; i < 2; ++i) {
        const Placement p = i == 0 ? Port::Hooks::MapPlacement(touch, views[0])
                                   : Port::Hooks::TargetPlacement(touch, views[0], views[1]);
        rects[i] = ViewRect(p, ViewAspect(PANEL_VIEW_KEYS[i]), width, height);
        shown[i] = views[0].on && views[i].on; // the target camera only with the map
    }

    // the mouse on the game: pick a view (the target camera is drawn over the map) or its corner
    const ImVec2 mouse = io.MousePos;
    const float handle = em * 1.1f;
    if (drag < 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
        for (int i = 1; i >= 0; --i) {
            const ImVec4& r = rects[i];
            if (!shown[i] || mouse.x < r.x || mouse.y < r.y || mouse.x >= r.x + r.z || mouse.y >= r.y + r.w) {
                continue;
            }
            drag = i;
            pick = i; // the controller's and the keyboard's keys move it next
            drag_resize = mouse.x >= r.x + r.z - handle && mouse.y >= r.y + r.w - handle;
            drag_mouse_x = mouse.x;
            drag_mouse_y = mouse.y;
            drag_left = r.x;
            drag_top = r.y;
            drag_height = r.w;
            break;
        }
    }
    if (drag >= 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        drag = -1;
    }
    if (drag >= 0 && width > 0 && height > 0) {
        const std::string key = PANEL_VIEW_KEYS[drag];
        const float aspect = ViewAspect(key);
        float left = drag_left, top = drag_top, h = drag_height;
        if (drag_resize) {
            // the top-left corner stays; the size follows the mouse, inside the window
            h = std::max((mouse.x - left) / aspect, mouse.y - top);
            h = std::clamp(h, std::max(height * 0.03f, 1.0f),
                           std::max(1.0f, std::min(height - top, (width - left) / aspect)));
        } else {
            left += mouse.x - drag_mouse_x;
            top += mouse.y - drag_mouse_y;
        }
        const float w = std::min(width, h * aspect);
        left = std::clamp(left, 0.0f, width - w);
        top = std::clamp(top, 0.0f, height - h);
        // the window's edges hold it a little, so that it sits right against them
        const float snap = em * 0.6f;
        left = left < snap ? 0.0f : width - w - left < snap ? width - w : left;
        top = top < snap ? 0.0f : height - h - top < snap ? height - h : top;
        const PanelView view = PlaceView(views[drag], aspect, width, height, left, top, h);
        if (const std::string text = FormatView(key, view); text != Game(key)) {
            SetGame(key, text);
        }
    }

    // the frames, under the toolbar; the one the keys move outlined in the accent colour
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ImU32 accent = ImGui::GetColorU32(ImGuiCol_CheckMark);
    const float line = std::max(2.0f, em * 0.12f);
    for (int i = 0; i < 2; ++i) {
        if (!shown[i]) {
            continue;
        }
        const ImVec4& r = rects[i];
        const ImVec2 a(r.x, r.y), b(r.x + r.z, r.y + r.w);
        const bool hot = drag == i || (drag < 0 && mouse.x >= a.x && mouse.y >= a.y && mouse.x < b.x && mouse.y < b.y);
        const bool chosen = i == pick;
        draw->AddRectFilled(a, b, IM_COL32(255, 200, 120, hot ? 48 : 24));
        draw->AddRect(a, b, hot || chosen ? accent : IM_COL32(255, 255, 255, 200), 0.0f, chosen ? line * 1.5f : line);
        const ImVec2 text = ImGui::CalcTextSize(VIEW_NAMES[i]);
        const ImVec2 at(a.x + em * 0.3f, a.y + em * 0.2f);
        draw->PushClipRect(a, b, true); // a small view cuts its name short
        draw->AddRectFilled(ImVec2(at.x - em * 0.2f, at.y - em * 0.1f),
                            ImVec2(at.x + text.x + em * 0.2f, at.y + text.y + em * 0.1f), IM_COL32(0, 0, 0, 170),
                            em * 0.2f);
        draw->AddText(at, IM_COL32(255, 255, 255, 255), VIEW_NAMES[i]);
        draw->PopClipRect();
        draw->AddTriangleFilled(ImVec2(b.x, b.y - handle), b, ImVec2(b.x - handle, b.y), accent);
    }
    if (!shown[0]) {
        const char* text = "Tick Map in the box above to place the map.";
        const ImVec2 size = ImGui::CalcTextSize(text);
        draw->AddText(ImVec2((width - size.x) * 0.5f, (height - size.y) * 0.5f), IM_COL32(255, 255, 255, 230), text);
    }
}

} // namespace App::Ui
