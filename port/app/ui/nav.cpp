#include "ui/nav.h"

#include <imgui_internal.h>

namespace App::Ui::Nav {

int Step() {
    if (!ImGui::IsItemFocused() || ImGui::IsItemActive()) {
        return 0;
    }
    const ImGuiID id = ImGui::GetItemID();
    if (id == 0) {
        return 0;
    }
    // The navigation reads the D-pad and the arrows with ImGuiKeyOwner_NoOwner (imgui.cpp,
    // NavUpdateCreateMoveRequest), so while this item owns them the cursor stays on it. SetKeyOwner
    // takes effect at once and for the next frame; ownership ends the frame after a key is let go.
    static constexpr ImGuiKey LESS[] = {ImGuiKey_GamepadDpadLeft, ImGuiKey_LeftArrow};
    static constexpr ImGuiKey MORE[] = {ImGuiKey_GamepadDpadRight, ImGuiKey_RightArrow};
    int step = 0;
    for (const ImGuiKey key : LESS) {
        ImGui::SetKeyOwner(key, id);
        if (ImGui::IsKeyPressed(key, ImGuiInputFlags_Repeat, id)) {
            step = -1;
        }
    }
    for (const ImGuiKey key : MORE) {
        ImGui::SetKeyOwner(key, id);
        if (ImGui::IsKeyPressed(key, ImGuiInputFlags_Repeat, id)) {
            step = 1;
        }
    }
    return step;
}

void FocusItemNow(bool cursor) {
    // FocusItem resolves a navigation request with the last item: the cursor moves there in the
    // next frame's NewFrame, after any init request (a window focused on opening), and the window
    // scrolls to keep it in view.
    ImGui::FocusItem();
    if (cursor) {
        ImGui::SetNavCursorVisible(true);
        // the cursor's item counts as the hovered one while it shows (IsItemHovered)
        GImGui->NavHighlightItemUnderNav = true;
    }
}

ImGuiID FocusedId() {
    return ImGui::GetFocusID();
}

bool LastItemFocusable() {
    const ImGuiLastItemData& item = GImGui->LastItemData;
    return item.ID != 0 && (item.ItemFlags & (ImGuiItemFlags_Disabled | ImGuiItemFlags_NoNav)) == 0;
}

void LeaveTextField() {
    ImGui::ClearActiveID();
}

void DropTextField(ImGuiID id) {
    ImGuiContext* g = ImGui::GetCurrentContext();
    if (!g || id == 0) {
        return;
    }
    if (g->ActiveId == id) {
        ImGui::ClearActiveID(); // (InputTextDeactivateHook keeps the text it had)
    }
    if (g->InputTextDeactivatedState.ID == id) {
        g->InputTextDeactivatedState.ID = 0; // imgui_widgets.cpp, InputTextEx: not applied next frame
    }
}

bool ActivatedByPad() {
    // the source is the navigation's last device, so a shortcut (Ctrl+F) or tabbing after the
    // controller was used reads Gamepad too: those come with their own activation flags
    const ImGuiContext& g = *GImGui;
    return g.ActiveIdSource == ImGuiInputSource_Gamepad &&
           (g.NavActivateFlags & (ImGuiActivateFlags_FromShortcut | ImGuiActivateFlags_FromTabbing)) == 0;
}

bool ActivatedByTouch() {
    // the layer gives a touchscreen's finger as the mouse, its source TouchScreen (ui.cpp, Finger)
    const ImGuiContext& g = *GImGui;
    return g.ActiveIdSource == ImGuiInputSource_Mouse && g.IO.MouseSource == ImGuiMouseSource_TouchScreen;
}

bool TypingValue(const char* id) {
    // imgui_widgets.cpp, SliderScalar: its text field (TempInputScalar) has the slider's own ID
    return ImGui::TempInputIsActive(ImGui::GetID(id));
}

bool Shortcut(ImGuiKeyChord chord, ImGuiInputFlags flags) {
    // imgui.cpp, SetShortcutRouting: the active item's own shortcut isn't culled as a possible
    // character (IsKeyChordPotentiallyCharInput), and takes the route (score 300); with none
    // active, the owner 0 is the window's, as ImGui::Shortcut's own
    return ImGui::Shortcut(chord, flags, ImGui::GetActiveID());
}

float KeyValue(ImGuiKey key) {
    return ImGui::GetKeyData(key)->AnalogValue;
}

void DisableWindowing() {
    // set once in the context's constructor (imgui.cpp, ImGuiContext::ImGuiContext); 0 turns them off
    ImGuiContext& g = *GImGui;
    g.ConfigNavWindowingKeyNext = 0;
    g.ConfigNavWindowingKeyPrev = 0;
    g.ConfigNavWindowingWithGamepad = false;
}

void TakeKey(ImGuiKey key, ImGuiID owner) {
    ImGui::SetKeyOwner(key, owner);
}

bool KeyPressed(ImGuiKey key, ImGuiID owner, bool repeat) {
    return ImGui::IsKeyPressed(key, repeat ? ImGuiInputFlags_Repeat : ImGuiInputFlags_None, owner);
}

} // namespace App::Ui::Nav
