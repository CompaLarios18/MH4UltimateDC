#include "ui/key_guides.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <SDL.h>
#include <imgui.h>

#include "common/settings.h"
#include "common/string_util.h"
#include "config.h"
#include "input.h"

namespace App::Ui {

namespace {

namespace NativeAnalog = Settings::NativeAnalog;
namespace NativeButton = Settings::NativeButton;

constexpr const char* MOUSE_MOVE = "#mouse-move";  ///< the mouse moving: the camera

struct GuideRow {
    const char* button;              ///< the 3DS button the game's prompts show, "" for none
    std::vector<std::string> keys;   ///< key names as mh4u-app.ini has them (or MOUSE_MOVE)
    const char* label;
};

/// A short name on a key cap.
std::string CapText(const std::string& name) {
    static constexpr std::pair<const char*, const char*> shorter[] = {
        {"Left Shift", "Shift"}, {"Right Shift", "Shift"}, {"Left Ctrl", "Ctrl"}, {"Right Ctrl", "Ctrl"},
        {"Left Alt", "Alt"},     {"Right Alt", "Alt"},     {"Return", "Enter"},   {"Backspace", "Bksp"},
        {"Escape", "Esc"},       {"Space", "Space"},       {"Keypad Enter", "Enter"}};
    for (const auto& [from, to] : shorter) {
        if (name == from) {
            return to;
        }
    }
    return name;
}

bool IsMouse(const std::string& name) {
    const int code = Input::MouseCode(name);
    return name == MOUSE_MOVE || (code >= Input::MOUSE_KEY && code < Input::MACRO_KEY);
}

/// The 3DS button as the game draws it: A, B, X, Y in their colours, the shoulders and START as
/// grey caps.
void DrawButton(ImDrawList* dl, ImVec2 at, float h, const char* button) {
    const std::string b = button;
    const ImU32 white = IM_COL32(255, 255, 255, 255);
    if (b.size() == 1 && std::string("ABXY").find(b) != std::string::npos) {
        static constexpr ImU32 colours[] = {IM_COL32(214, 64, 58, 255), IM_COL32(230, 186, 38, 255),
                                            IM_COL32(52, 108, 214, 255), IM_COL32(56, 160, 78, 255)};
        const ImU32 colour = colours[std::string("ABXY").find(b)];
        const ImVec2 c(at.x + h * 0.5f, at.y + h * 0.5f);
        dl->AddCircleFilled(c, h * 0.48f, colour);
        const ImVec2 size = ImGui::CalcTextSize(button);
        dl->AddText(ImVec2(c.x - size.x * 0.5f, c.y - size.y * 0.5f), white, button);
        return;
    }
    const ImVec2 size = ImGui::CalcTextSize(button);
    const float w = std::max(h, size.x + h * 0.5f);
    dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h), IM_COL32(70, 72, 78, 255), h * 0.3f);
    dl->AddText(ImVec2(at.x + (w - size.x) * 0.5f, at.y + (h - size.y) * 0.5f), white, button);
}

float ButtonWidth(float h, const char* button) {
    const std::string b = button;
    if (b.empty()) {
        return 0.0f;
    }
    if (b.size() == 1 && std::string("ABXY").find(b) != std::string::npos) {
        return h;
    }
    return std::max(h, ImGui::CalcTextSize(button).x + h * 0.5f);
}

/// A mouse, the part that does it lit: a button, the wheel (with which way it turns), the side
/// buttons, or arrows beside it for moving it.
float DrawMouse(ImDrawList* dl, ImVec2 at, float h, const std::string& name) {
    const float w = h * 0.66f;
    const float side = name == MOUSE_MOVE ? h * 0.32f : 0.0f;  // room for the arrows
    const ImVec2 o(at.x + side, at.y);
    const ImU32 line = IM_COL32(235, 235, 235, 255);
    const ImU32 lit = IM_COL32(236, 163, 52, 255);
    const ImU32 body = IM_COL32(40, 42, 48, 230);
    const float r = w * 0.5f;
    const float split = o.y + h * 0.45f;
    dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), body, r);
    const int code = Input::MouseCode(name);
    const ImVec2 wheel_min(o.x + w * 0.42f, o.y + h * 0.13f), wheel_max(o.x + w * 0.58f, o.y + h * 0.33f);
    if (code == Input::MOUSE_KEY + SDL_BUTTON_LEFT) {
        dl->AddRectFilled(o, ImVec2(o.x + w * 0.5f, split), lit, r, ImDrawFlags_RoundCornersTopLeft);
    } else if (code == Input::MOUSE_KEY + SDL_BUTTON_RIGHT) {
        dl->AddRectFilled(ImVec2(o.x + w * 0.5f, o.y), ImVec2(o.x + w, split), lit, r,
                          ImDrawFlags_RoundCornersTopRight);
    } else if (code == Input::MOUSE_KEY + SDL_BUTTON_X1 || code == Input::MOUSE_KEY + SDL_BUTTON_X2) {
        const float y = code == Input::MOUSE_KEY + SDL_BUTTON_X1 ? 0.62f : 0.48f;
        dl->AddRectFilled(ImVec2(o.x - w * 0.06f, o.y + h * y), ImVec2(o.x + w * 0.14f, o.y + h * (y + 0.13f)), lit,
                          w * 0.05f);
    }
    dl->AddRect(o, ImVec2(o.x + w, o.y + h), line, r, 0, std::max(1.0f, h * 0.06f));
    dl->AddLine(ImVec2(o.x, split), ImVec2(o.x + w, split), line, std::max(1.0f, h * 0.05f));
    dl->AddLine(ImVec2(o.x + w * 0.5f, o.y), ImVec2(o.x + w * 0.5f, split), line, std::max(1.0f, h * 0.05f));
    const bool wheel = code == Input::MOUSE_KEY + SDL_BUTTON_MIDDLE || code == Input::WHEEL_UP ||
                       code == Input::WHEEL_DOWN;
    dl->AddRectFilled(wheel_min, wheel_max, wheel ? lit : line, w * 0.08f);
    if (code == Input::WHEEL_UP || code == Input::WHEEL_DOWN) {
        // which way it turns, beside the mouse
        const float x = o.x + w + h * 0.16f, cy = o.y + h * 0.25f, t = h * 0.13f;
        const float s = code == Input::WHEEL_UP ? -1.0f : 1.0f;
        dl->AddTriangleFilled(ImVec2(x - t, cy - s * t * 0.6f), ImVec2(x + t, cy - s * t * 0.6f),
                              ImVec2(x, cy + s * t * 0.8f), lit);
        return side + w + h * 0.32f;
    }
    if (name == MOUSE_MOVE) {
        const float cy = o.y + h * 0.5f, t = h * 0.12f;
        dl->AddTriangleFilled(ImVec2(at.x, cy), ImVec2(at.x + t * 1.6f, cy - t), ImVec2(at.x + t * 1.6f, cy + t), lit);
        const float rx = o.x + w + side;
        dl->AddTriangleFilled(ImVec2(rx, cy), ImVec2(rx - t * 1.6f, cy - t), ImVec2(rx - t * 1.6f, cy + t), lit);
        return side * 2 + w;
    }
    return w;
}

float MouseWidth(float h, const std::string& name) {
    const int code = Input::MouseCode(name);
    if (name == MOUSE_MOVE) {
        return h * 0.66f + h * 0.64f;
    }
    if (code == Input::WHEEL_UP || code == Input::WHEEL_DOWN) {
        return h * 0.66f + h * 0.32f;
    }
    return h * 0.66f;
}

/// A key cap with the key's short name.
float DrawCap(ImDrawList* dl, ImVec2 at, float h, const std::string& name) {
    const std::string text = CapText(name);
    const ImVec2 size = ImGui::CalcTextSize(text.c_str());
    const float w = std::max(h, size.x + h * 0.55f);
    const float r = h * 0.2f;
    dl->AddRectFilled(ImVec2(at.x, at.y + h * 0.06f), ImVec2(at.x + w, at.y + h), IM_COL32(120, 120, 126, 255), r);
    dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h * 0.9f), IM_COL32(238, 238, 240, 255), r);
    dl->AddText(ImVec2(at.x + (w - size.x) * 0.5f, at.y + (h * 0.9f - size.y) * 0.5f), IM_COL32(24, 24, 28, 255),
                text.c_str());
    return w;
}

float CapWidth(float h, const std::string& name) {
    return IsMouse(name) ? MouseWidth(h, name) : std::max(h, ImGui::CalcTextSize(CapText(name).c_str()).x + h * 0.55f);
}

/// The row's keys, its 3DS button and what it does, at the cursor; the width it took.
void DrawRow(const GuideRow& row, float h, float keys_width, float button_width) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float gap = h * 0.18f;
    float x = at.x;
    for (std::size_t i = 0; i < row.keys.size(); ++i) {
        const std::string& key = row.keys[i];
        x += IsMouse(key) ? DrawMouse(dl, ImVec2(x, at.y), h, key) : DrawCap(dl, ImVec2(x, at.y), h, key);
        x += gap;
    }
    x = at.x + keys_width + gap;
    if (row.button[0] != '\0') {
        DrawButton(dl, ImVec2(x, at.y + h * 0.1f), h * 0.8f, row.button);
    }
    x += button_width + gap * 1.5f;
    const ImVec2 size = ImGui::CalcTextSize(row.label);
    dl->AddText(ImVec2(x, at.y + (h - size.y) * 0.5f), IM_COL32(235, 232, 225, 255), row.label);
    ImGui::Dummy(ImVec2(x + size.x - at.x, h));
}

std::vector<std::string> Keys(const std::string& spec) {
    return Input::KeyNames(spec);
}

/// The circle pad's keys: "W A S D" as four caps (up, left, down, right), or none.
std::vector<std::string> StickKeys(const std::string& spec) {
    std::vector<std::string> parts;
    for (const std::string& part : Common::SplitString(spec, ',')) {
        parts.push_back(Common::StripSpaces(part));
    }
    if (parts.size() < 4) {
        return {};
    }
    // up, left, down, right: the order a hand reads them in (W A S D)
    std::vector<std::string> keys;
    for (const int i : {0, 2, 1, 3}) {
        if (!parts[i].empty()) {
            keys.push_back(parts[i]);
        }
    }
    return keys;
}

std::vector<GuideRow> Rows(const Config& c) {
    const auto key = [&](int button) {
        return c.button_params[button].empty() ? Keys(c.keys[button]) : std::vector<std::string>{};
    };
    std::vector<std::string> items = Keys(c.item_prev);
    for (const auto& k : Keys(c.item_next)) {
        items.push_back(k);
    }
    std::vector<GuideRow> rows = {
        {"", StickKeys(c.stick_keys[NativeAnalog::CirclePad]), "Move"},
        {"", {MOUSE_MOVE}, "Camera"},
        {"X", key(NativeButton::X), "Draw / attack"},
        {"A", key(NativeButton::A), "Attack / gather / talk"},
        {"B", key(NativeButton::B), "Evade / crouch"},
        {"Y", key(NativeButton::Y), "Use item / sheathe"},
        {"R", key(NativeButton::R), "Dash / guard"},
        {"L", key(NativeButton::L), "Reset camera / focus"},
        {"", items, "Item bar"},
        {"ZL", key(NativeButton::ZL), "Target monster"},
        {"ZR", key(NativeButton::ZR), "Kick / skip"},
        {"START", key(NativeButton::Start), "Game menu"},
        {"", Keys(c.touch_screen_key), "Map / touch screen"},
        {"", Keys(c.stylus_key), "Hold: mouse on the touch screen"},
        {"", {"Escape"}, "Port menu"},
        {"", Keys(c.key_guides_key), "Hide these keys"},
    };
    // only rows with a key
    rows.erase(std::remove_if(rows.begin(), rows.end(), [](const GuideRow& r) { return r.keys.empty(); }),
               rows.end());
    return rows;
}

} // namespace

void DrawKeyGuides(const Config& config, const std::string& place) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool right = place == "top-right" || place == "bottom-right";
    const bool bottom = place == "bottom-left" || place == "bottom-right";
    // small: about 2.4% of the window's height a row
    const float font = std::clamp(std::round(io.DisplaySize.y / 52.0f), 11.0f, 40.0f);
    ImGui::PushFont(nullptr, font);
    const float h = std::round(font * 1.35f);
    const float margin = std::round(font * 0.7f);
    ImGui::SetNextWindowPos(ImVec2(right ? io.DisplaySize.x - margin : margin,
                                   bottom ? io.DisplaySize.y - margin : margin),
                            ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(0.42f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(margin * 0.8f, margin * 0.6f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(margin, font * 0.28f));
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##key_guides", nullptr, flags)) {
        const std::vector<GuideRow> rows = Rows(config);
        // two columns, the keys of each lined up
        const std::size_t half = (rows.size() + 1) / 2;
        const float gap = h * 0.18f;
        float keys_width[2] = {0, 0}, button_width[2] = {0, 0};
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const int col = i < half ? 0 : 1;
            float w = 0;
            for (const auto& k : rows[i].keys) {
                w += CapWidth(h, k) + gap;
            }
            keys_width[col] = std::max(keys_width[col], w - gap);
            button_width[col] = std::max(button_width[col], ButtonWidth(h * 0.8f, rows[i].button));
        }
        if (ImGui::BeginTable("##guides", 2, ImGuiTableFlags_SizingFixedFit)) {
            for (std::size_t r = 0; r < half; ++r) {
                ImGui::TableNextRow();
                for (int col = 0; col < 2; ++col) {
                    const std::size_t i = r + (col == 0 ? 0 : half);
                    ImGui::TableSetColumnIndex(col);
                    if (i < rows.size()) {
                        DrawRow(rows[i], h, keys_width[col], button_width[col]);
                    }
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopFont();
}

} // namespace App::Ui
