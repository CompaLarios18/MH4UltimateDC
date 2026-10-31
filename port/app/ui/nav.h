// The Dear ImGui internals (imgui_internal.h) the in-game menu uses, in one place, each with what
// it relies on: recheck them when Dear ImGui is updated (port/app/third_party/imgui, 1.92.9b).
#pragma once

#include <imgui.h>

namespace App::Ui::Nav {

/// After one of the menu's lists or sliders, while the cursor is on it, it isn't being edited and
/// its list isn't open: left and right (the D-pad's and the arrows) are taken from the navigation
/// for it, so they change its value instead of moving the cursor. -1, 0 or +1, with the keys'
/// repeat while held.
int Step();

/// Moves the cursor to the item just drawn (its window scrolls to show it); `cursor`: the cursor
/// shows, as when a controller or the keyboard moved it there. Not SetKeyboardFocusHere, which
/// would put a slider into typing a value.
void FocusItemNow(bool cursor);

/// The ID of the item the cursor is on (0: none).
ImGuiID FocusedId();

/// The item just drawn can take the cursor: enabled and navigable.
bool LastItemFocusable();

/// The text field being typed in lets go (its text stays).
void LeaveTextField();
/// The field `id` lets go if it is being typed in, and its text is dropped: Dear ImGui keeps its
/// own copy while a field is active, and writes it back over the buffer the frame after the field
/// is let go from outside, which would undo a change made there (the search cleared).
void DropTextField(ImGuiID id);

/// The item active now was activated by a controller (its A), not the mouse or the keyboard.
bool ActivatedByPad();

/// How far an analog key is pushed, 0..1 (a stick's direction, a trigger).
float KeyValue(ImGuiKey key);

/// Ctrl+Tab and Ctrl+Shift+Tab don't switch between Dear ImGui's windows (the menu's tabs take
/// them), and holding a controller's X doesn't either.
void DisableWindowing();

/// `key` belongs to `owner` this frame and the next: the navigation (which reads keys that have
/// no owner) doesn't see it. KeyPressed reads it for that owner.
void TakeKey(ImGuiKey key, ImGuiID owner);
bool KeyPressed(ImGuiKey key, ImGuiID owner, bool repeat = false);

} // namespace App::Ui::Nav
