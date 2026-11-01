#include "ui/ui.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>

#include "common/logging/log.h"
#include "config.h"
#include "core/core.h"
#include "hooks.h"
#include "input.h"
#include "input_common/keyboard.h"
#include "input_common/main.h"
#include "speed.h"
#include "ui/fps_counter.h"
#include "ui/hint.h"
#include "ui/key_guides.h"
#include "ui/keyboard.h"
#include "ui/layer.h"
#include "ui/menu.h"
#include "ui/nav.h"
#include "window.h"

namespace App::Ui {

namespace {

std::atomic<Layer*> instance{nullptr};

constexpr u32 BACK_HELD = 1, START_HELD = 2;
/// A key that comes this soon after a controller's button, or before it (Esc, and a key the button
/// follows in the same pump: Layer::ForwardHeld), is the echo Steam Input's desktop layouts send
/// for that button.
constexpr u32 ECHO_MS = 150;

/// The keys Steam Input sends for a controller's buttons: Esc (B), Enter (A), Space, Tab and the
/// arrows (the D-pad).
bool EchoKey(SDL_Scancode code) {
    switch (code) {
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
    case SDL_SCANCODE_SPACE:
    case SDL_SCANCODE_TAB:
    case SDL_SCANCODE_UP:
    case SDL_SCANCODE_DOWN:
    case SDL_SCANCODE_LEFT:
    case SDL_SCANCODE_RIGHT:
        return true;
    default:
        return false;
    }
}

/// Pause and fast forward (speed.h) at the top middle of the screen, small; it takes no input.
void DrawSpeedBadge(const std::string& text, bool paused) {
    const ImGuiIO& io = ImGui::GetIO();
    const float margin = std::round(ImGui::GetFontSize() * 0.5f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, margin), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(margin * 1.2f, margin * 0.5f));
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
    if (ImGui::Begin("##speed", nullptr, flags)) {
        ImGui::TextColored(paused ? ImVec4(1.0f, 0.75f, 0.45f, 1.0f) : ImVec4(0.55f, 0.90f, 0.55f, 1.0f), "%s",
                           text.c_str());
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

/// The menu's look: dark, translucent, an amber accent; sizes for a 13-pixel font, scaled with
/// the font (UpdateScale).
void SetStyle(ImGuiStyle& s) {
    ImGui::StyleColorsDark(&s);
    s.WindowPadding = ImVec2(14, 12);
    s.FramePadding = ImVec2(8, 5);
    s.CellPadding = ImVec2(6, 4);
    s.ItemSpacing = ImVec2(10, 7);
    s.ItemInnerSpacing = ImVec2(7, 5);
    s.ScrollbarSize = 12;
    s.GrabMinSize = 12;
    s.WindowRounding = 8;
    s.ChildRounding = 6;
    s.FrameRounding = 5;
    s.PopupRounding = 6;
    s.GrabRounding = 4;
    s.TabRounding = 5;
    s.ScrollbarRounding = 6;
    s.WindowBorderSize = 1;
    s.PopupBorderSize = 1;
    s.WindowTitleAlign = ImVec2(0.5f, 0.5f);

    const ImVec4 amber(0.92f, 0.63f, 0.20f, 1.00f);
    const auto mix = [](float r, float g, float b, float a = 1.0f) { return ImVec4(r, g, b, a); };
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = mix(0.93f, 0.92f, 0.89f);
    c[ImGuiCol_TextDisabled] = mix(0.58f, 0.56f, 0.52f);
    c[ImGuiCol_WindowBg] = mix(0.07f, 0.07f, 0.08f, 0.95f);
    c[ImGuiCol_ChildBg] = mix(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_PopupBg] = mix(0.09f, 0.09f, 0.10f, 0.98f);
    c[ImGuiCol_Border] = mix(0.92f, 0.63f, 0.20f, 0.30f);
    c[ImGuiCol_FrameBg] = mix(0.16f, 0.16f, 0.17f);
    c[ImGuiCol_FrameBgHovered] = mix(0.27f, 0.22f, 0.15f);
    c[ImGuiCol_FrameBgActive] = mix(0.36f, 0.28f, 0.15f);
    c[ImGuiCol_TitleBg] = mix(0.10f, 0.09f, 0.08f);
    c[ImGuiCol_TitleBgActive] = mix(0.20f, 0.15f, 0.09f);
    c[ImGuiCol_CheckMark] = amber;
    c[ImGuiCol_SliderGrab] = amber;
    c[ImGuiCol_SliderGrabActive] = mix(1.00f, 0.76f, 0.36f);
    c[ImGuiCol_Button] = mix(0.21f, 0.19f, 0.17f);
    c[ImGuiCol_ButtonHovered] = mix(0.46f, 0.33f, 0.15f);
    c[ImGuiCol_ButtonActive] = mix(0.62f, 0.43f, 0.15f);
    c[ImGuiCol_Header] = mix(0.31f, 0.24f, 0.14f);
    c[ImGuiCol_HeaderHovered] = mix(0.46f, 0.33f, 0.15f);
    c[ImGuiCol_HeaderActive] = mix(0.62f, 0.43f, 0.15f);
    c[ImGuiCol_Separator] = mix(0.92f, 0.63f, 0.20f, 0.25f);
    c[ImGuiCol_Tab] = mix(0.15f, 0.14f, 0.13f);
    c[ImGuiCol_TabHovered] = mix(0.52f, 0.37f, 0.15f);
    c[ImGuiCol_TabSelected] = mix(0.40f, 0.29f, 0.13f);
    c[ImGuiCol_TabSelectedOverline] = amber;
    c[ImGuiCol_TableRowBgAlt] = mix(1.0f, 1.0f, 1.0f, 0.03f);
    c[ImGuiCol_TextSelectedBg] = mix(0.92f, 0.63f, 0.20f, 0.35f);
    c[ImGuiCol_NavCursor] = amber;
    c[ImGuiCol_ModalWindowDimBg] = mix(0.0f, 0.0f, 0.0f, 0.45f);
    c[ImGuiCol_SeparatorHovered] = mix(0.92f, 0.63f, 0.20f, 0.78f);
    c[ImGuiCol_SeparatorActive] = amber;
    c[ImGuiCol_ResizeGrip] = mix(0.92f, 0.63f, 0.20f, 0.20f);
    c[ImGuiCol_ResizeGripHovered] = mix(0.92f, 0.63f, 0.20f, 0.67f);
    c[ImGuiCol_ResizeGripActive] = mix(0.92f, 0.63f, 0.20f, 0.95f);
    c[ImGuiCol_TextLink] = amber;
    // the ones the dark style works out from its own (blue) colours, from these instead
    const auto lerp = [](const ImVec4& a, const ImVec4& b, float t) {
        return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                      a.w + (b.w - a.w) * t);
    };
    c[ImGuiCol_CheckboxSelectedBg] = lerp(c[ImGuiCol_FrameBg], c[ImGuiCol_FrameBgHovered], 0.65f);
    c[ImGuiCol_TabDimmed] = lerp(c[ImGuiCol_Tab], c[ImGuiCol_TitleBg], 0.80f);
    c[ImGuiCol_TabDimmedSelected] = lerp(c[ImGuiCol_TabSelected], c[ImGuiCol_TitleBg], 0.40f);
}

} // namespace

// ── The draw-data copy ──

DrawCopy::~DrawCopy() {
    for (ImDrawList* list : lists) {
        IM_DELETE(list);
    }
}

void DrawCopy::CopyFrom(const ImDrawData& source) {
    // the lists are kept from frame to frame, so a frame like the last one allocates nothing
    while (lists.Size < source.CmdLists.Size) {
        lists.push_back(IM_NEW(ImDrawList)(ImGui::GetDrawListSharedData()));
    }
    data.Clear();
    for (int i = 0; i < source.CmdLists.Size; ++i) {
        const ImDrawList* from = source.CmdLists[i];
        ImDrawList* to = lists[i];
        to->CmdBuffer = from->CmdBuffer;
        to->IdxBuffer = from->IdxBuffer;
        to->VtxBuffer = from->VtxBuffer;
        to->Flags = from->Flags;
        data.CmdLists.push_back(to);
    }
    data.Valid = true;
    data.FrameCount = source.FrameCount;
    data.TotalIdxCount = source.TotalIdxCount;
    data.TotalVtxCount = source.TotalVtxCount;
    data.DisplayPos = source.DisplayPos;
    data.DisplaySize = source.DisplaySize;
    data.FramebufferScale = source.FramebufferScale;
    data.Textures = source.Textures; // the context's: the renderer uploads what changed
}

void DrawCopy::Clear() {
    data.Clear();
    for (ImDrawList* list : lists) {
        IM_DELETE(list);
    }
    lists.clear();
}

// ── The layer ──

Layer::Layer(Core::System& system_, Window& window_, Config& config_)
    : system{system_}, window{window_}, config{config_} {
    IMGUI_CHECKVERSION();
    context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // nothing of the menu's is kept between runs but the settings
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    // Esc and B with nothing of Dear ImGui's to close leave the cursor where it is: they are the
    // menu's back steps (a search cleared, then the menu closed), and a controller needs a cursor
    io.ConfigNavEscapeClearFocusItem = false;
    // Ctrl+Tab is the menu's next tab, not Dear ImGui's window switcher
    Nav::DisableWindowing();
    // the window shows and hides the cursor itself (hidden after a while in full screen)
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    // a font that scales: drawn at the size the window's height asks for (UpdateScale)
    io.Fonts->AddFontDefaultVector();
    SetStyle(ImGui::GetStyle());
    base_style = ImGui::GetStyle();
    ImGui_ImplSDL2_InitForOther(window.GetSDLWindow());
    ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_AutoAll); // any controller, not the first

    menu = std::make_unique<Menu>(*this);
    keyboard = std::make_shared<Keyboard>();
    system.RegisterSoftwareKeyboard(keyboard);
#ifdef ENABLE_VULKAN
    RegisterVulkanOverlay(*this);
#endif
    LOG_INFO(Frontend, "in-game menu: Esc, or a controller's Guide or Back+Start");
}

Layer::~Layer() {
#ifdef ENABLE_VULKAN
    UnregisterVulkanOverlay();
#endif
    if (menu_open) {
        menu->Closed(); // the window closed with the menu up: its changes are kept all the same
    }
    SetCapture(false);
    menu.reset();
    std::scoped_lock lock{mutex};
    MakeCurrent();
    copy.Clear();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext(context);
}

void Layer::MakeCurrent() {
    ImGui::SetCurrentContext(context);
}

void Layer::Post(std::function<void()> action) {
    posted.push_back(std::move(action));
}

void Layer::Help(const char* text) {
    if (ImGui::IsItemFocused()) {
        help_focused = text;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        help_hovered = text;
    }
    if (menu_open) {
        menu->AfterItem(); // the cursor it is waiting to put somewhere, the tab's last row
    }
}

const char* Layer::HelpText() const {
    // the cursor's item while the cursor shows (a controller or the keys moved it), otherwise the
    // mouse's: the item a player looks at
    if (ImGui::GetIO().NavVisible && help_focused) {
        return help_focused;
    }
    return help_hovered ? help_hovered : help_focused;
}

bool Layer::PadJustUsed(u32 now) const {
    const auto recent = [now](u32 at) { return at != 0 && now - at < ECHO_MS; };
    return recent(pad_button_at) || recent(stick_at);
}

void Layer::Open() {
    if (menu_open) {
        return;
    }
    menu_open = true;
    menu->Opened();
    SetCapture(true);
}

void Layer::Close() {
    if (!menu_open) {
        return;
    }
    menu_open = false;
    key_capture = false;
    menu->Closed();
    keyboard->Refocus();
    SetCapture(keyboard->Active());
}

void Layer::SetCapture(bool on) {
    if (on == capturing) {
        return;
    }
    capturing = on;
    finger = -1; // a finger down as the menu comes or goes is forgotten (its lift may be the game's)
    Input::SetMuted(on);
    if (on) {
        // what the game had held is let go: the menu has the keys and the mouse now
        InputCommon::GetKeyboard()->ReleaseAllKeys();
        window.TouchReleased();
        fresh_input = true;
        SDL_ShowCursor(SDL_ENABLE);
    }
}

bool Layer::OnEvent(const SDL_Event& event) {
    const u32 now = SDL_GetTicks();
    // the menu's toggle: Esc, a controller's Guide, or its Back and Start together (Guide is
    // often Steam's or the Game Bar's)
    switch (event.type) {
    case SDL_KEYDOWN: {
        const SDL_Scancode code = event.key.keysym.scancode;
        if (event.key.repeat == 0 && code == SDL_SCANCODE_ESCAPE) {
            // a moment later, unless a controller's button came with it: Steam Input's desktop
            // layouts (a Steam Deck's, say) send Esc for B while the menu sees the controller too,
            // and one B would open the menu, or take two of its back steps (Frame decides)
            if (menu_open && PadJustUsed(now)) {
                LogEcho();
                return true;
            }
            escape_at = std::max<u32>(now, 1);
            escape_busy = menu_open && was_busy;
            return true;
        }
        if (code == SDL_SCANCODE_ESCAPE && capturing) {
            // its auto-repeat: Esc's press is the layer's (Frame), and Dear ImGui, which didn't see the
            // key go down, would take a repeat for a press of its own (over the game's keyboard, its
            // text field let go)
            return true;
        }
        if (event.key.repeat == 0) {
            echoes_down.erase(code); // pressed again: an echo or not (here, or in ForwardHeld)
        }
        if (menu_open && EchoKey(code) && (PadJustUsed(now) || echoes_down.count(code) != 0)) {
            // Enter for A, the arrows for the D-pad: the button itself does it (once); and while it
            // is held, the key's auto-repeat goes as its down went (Dear ImGui, which didn't see the
            // key go down, would take a repeat for a press)
            if (event.key.repeat == 0) {
                echoes_down.insert(code);
            }
            drop_space_text = code == SDL_SCANCODE_SPACE;
            return true;
        }
        drop_space_text = false;
        if (menu_open && event.key.repeat == 0) {
            pad_last = false;
        }
        break;
    }
    case SDL_TEXTINPUT:
        if (drop_space_text && std::string(event.text.text) == " ") {
            drop_space_text = false; // the text of a dropped Space
            return true;
        }
        pad_last = false;
        break;
    case SDL_MOUSEBUTTONDOWN:
        if (event.button.which != SDL_TOUCH_MOUSEID) {
            pad_last = false;
        }
        break;
    case SDL_MOUSEWHEEL:
        pad_last = false;
        break;
    case SDL_CONTROLLERAXISMOTION: {
        const int value = std::abs(static_cast<int>(event.caxis.value));
        if (value > 8000) {
            stick_at = std::max<u32>(now, 1);
        }
        if (value > 16000) {
            pad_last = true;
        }
        break;
    }
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
        const bool down = event.type == SDL_CONTROLLERBUTTONDOWN;
        if (down) {
            pad_button_at = std::max<u32>(now, 1);
            pad_last = true;
        }
        const u8 button = event.cbutton.button;
        if (button == SDL_CONTROLLER_BUTTON_GUIDE) {
            if (down) {
                menu_open ? Close() : Open();
            }
            return true;
        }
        const u32 bit = button == SDL_CONTROLLER_BUTTON_BACK    ? BACK_HELD
                        : button == SDL_CONTROLLER_BUTTON_START ? START_HELD
                                                                : 0;
        if (bit) {
            const u32 before = controller_buttons;
            controller_buttons = down ? controller_buttons | bit : controller_buttons & ~bit;
            if (down && before != controller_buttons &&
                controller_buttons == (BACK_HELD | START_HELD)) {
                menu_open ? Close() : Open();
                return true;
            }
        }
        break;
    }
    case SDL_CONTROLLERDEVICEADDED:
    case SDL_CONTROLLERDEVICEREMOVED: {
        // the menu's list of controllers, kept whether it is up or not (and the window's)
        if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
            controller_buttons = 0;
        }
        std::scoped_lock lock{mutex};
        MakeCurrent();
        ImGui_ImplSDL2_ProcessEvent(&event);
        return false;
    }
    default:
        break;
    }

    if (!capturing) {
        return false;
    }
    // a touchscreen's first finger is the menu's mouse (a Steam Deck's: SDL's touch-to-mouse is
    // off for the game, window.cpp, and Dear ImGui's SDL backend takes no fingers); the others do
    // nothing, and while a key is awaited a finger only lifts
    if (event.type == SDL_FINGERDOWN || event.type == SDL_FINGERUP || event.type == SDL_FINGERMOTION) {
        if (!key_capture || event.type == SDL_FINGERUP) {
            Finger(event);
        }
        return true;
    }
    // Keys and the key rows: the next key pressed is the one to bind (Dear ImGui still sees keys
    // let go, the one that started the capture among them)
    if (key_capture && event.type == SDL_KEYDOWN) {
        if (event.key.repeat == 0) {
            menu->OnCapturedKey(static_cast<int>(event.key.keysym.scancode));
        }
        return true;
    }
    // (and a mouse button or the wheel: Mouse Right, Wheel Up, ...)
    if (key_capture && event.type == SDL_MOUSEBUTTONDOWN && event.button.which != SDL_TOUCH_MOUSEID) {
        menu->OnCapturedKey(Input::MOUSE_KEY + event.button.button);
        return true;
    }
    if (key_capture && event.type == SDL_MOUSEWHEEL && event.wheel.y != 0) {
        menu->OnCapturedKey(event.wheel.y > 0 ? Input::WHEEL_UP : Input::WHEEL_DOWN);
        return true;
    }
    // an echo key's down, and the keyboard's events after it, wait for the frame (ForwardHeld): the
    // controller's button it may be the echo of can come right after it, in the same pump (SDL
    // reads the keyboard before the controllers)
    const bool key_event = event.type == SDL_KEYDOWN || event.type == SDL_KEYUP || event.type == SDL_TEXTINPUT ||
                           event.type == SDL_TEXTEDITING;
    if (menu_open && key_event &&
        (!held.empty() ||
         (event.type == SDL_KEYDOWN && event.key.repeat == 0 && EchoKey(event.key.keysym.scancode)))) {
        held.push_back(event);
    } else {
        std::scoped_lock lock{mutex};
        MakeCurrent();
        ImGui_ImplSDL2_ProcessEvent(&event);
    }
    switch (event.type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        // full screen stays the window's (F11, Alt+Enter)
        const int code = static_cast<int>(event.key.keysym.scancode);
        const bool alt_enter = code == SDL_SCANCODE_RETURN && (event.key.keysym.mod & KMOD_ALT);
        return !(alt_enter || code == Input::KeyCode(config.fullscreen_key));
    }
    case SDL_TEXTINPUT:
    case SDL_TEXTEDITING:
    case SDL_MOUSEMOTION:
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEWHEEL:
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
    case SDL_CONTROLLERAXISMOTION:
        return true;
    default:
        return false; // the window's too: resizing, focus, controllers coming and going, quitting
    }
}

void Layer::Finger(const SDL_Event& event) {
    const SDL_TouchFingerEvent& touch = event.tfinger;
    if (event.type == SDL_FINGERDOWN && finger < 0) {
        finger = static_cast<s64>(touch.fingerId);
    }
    if (static_cast<s64>(touch.fingerId) != finger) {
        return;
    }
    // the finger's place is a fraction of the window's size (SDL's touch events are normalized)
    int w = 0, h = 0;
    SDL_GetWindowSize(window.GetSDLWindow(), &w, &h);
    {
        std::scoped_lock lock{mutex};
        MakeCurrent();
        ImGuiIO& io = ImGui::GetIO();
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(touch.x * static_cast<float>(w), touch.y * static_cast<float>(h));
        if (event.type != SDL_FINGERMOTION) {
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, event.type == SDL_FINGERDOWN);
        }
    }
    if (event.type == SDL_FINGERUP) {
        finger = -1;
    }
    // (pad_last stays: a finger on a Steam Deck says nothing of whether its controller or a
    // keyboard is in hand, and the hints are those)
}

void Layer::ForwardHeld() {
    if (held.empty()) {
        return;
    }
    std::vector<SDL_Event> events;
    events.swap(held);
    if (!menu_open) {
        return; // closed meanwhile: its next opening starts from a clean input state
    }
    // an echo key that a controller's button (or a stick pushed) came with, at once or a moment
    // after: the button does it, once; a dropped Space's text goes with it, and the key's
    // auto-repeat with its down (echoes_down)
    const auto echo = [this](u32 at) {
        const auto after = [at](u32 pad) { return pad != 0 && pad >= at && pad - at < ECHO_MS; };
        return after(pad_button_at) || after(stick_at);
    };
    bool drop_text = false;
    std::scoped_lock lock{mutex};
    MakeCurrent();
    for (const SDL_Event& e : events) {
        if (e.type == SDL_KEYDOWN && EchoKey(e.key.keysym.scancode) &&
            (e.key.repeat == 0 ? echo(e.key.timestamp) : echoes_down.count(e.key.keysym.scancode) != 0)) {
            if (e.key.repeat == 0) {
                echoes_down.insert(e.key.keysym.scancode);
            }
            drop_text = e.key.keysym.scancode == SDL_SCANCODE_SPACE;
            continue;
        }
        if (e.type == SDL_TEXTINPUT && drop_text && std::string(e.text.text) == " ") {
            drop_text = false;
            continue;
        }
        ImGui_ImplSDL2_ProcessEvent(&e);
    }
    drop_space_text = drop_space_text || drop_text; // (its text in a later pump)
}

void Layer::LogEcho() {
    if (!echo_logged) {
        echo_logged = true;
        LOG_INFO(Frontend, "an Esc that came with a controller's button was left out (Steam Input sends keys for "
                           "a controller's buttons in its desktop layouts)");
    }
}

void Layer::UpdateScale(float display_height) {
    // a font about 1/34 of the window's height: 21 pixels at 720p, 32 at 1080p, 64 at 4K
    const float size = std::clamp(std::round(display_height / 34.0f), 14.0f, 96.0f);
    if (size == font_size) {
        return;
    }
    font_size = size;
    ImGuiStyle& style = ImGui::GetStyle();
    style = base_style;
    style.ScaleAllSizes(size / 13.0f);
    style.FontSizeBase = size;
}

void Layer::ToggleFpsCounter() {
    if (FpsCounterOn(config.fps_counter)) {
        last_fps_corner = config.fps_counter;
        config.fps_counter = "off";
    } else {
        config.fps_counter = last_fps_corner;
    }
    // the file's settings with this one changed: not the command line's
    Config file = App::Load();
    file.fps_counter = config.fps_counter;
    App::Save(file);
    LOG_INFO(Frontend, "frame-rate counter: {}", config.fps_counter);
}

void Layer::ToggleKeyGuides() {
    if (FpsCounterOn(config.key_guides)) {
        last_guides_corner = config.key_guides;
        config.key_guides = "off";
    } else {
        config.key_guides = last_guides_corner;
    }
    Config file = App::Load();
    file.key_guides = config.key_guides;
    App::Save(file);
    LOG_INFO(Frontend, "key guides: {}", config.key_guides);
}

void Layer::Frame() {
    ForwardHeld(); // the keyboard's events of the last pump that waited for its controller's
    if (escape_at != 0) {
        // the Esc held back in OnEvent: an echo if a controller's button came within ECHO_MS of it,
        // before or after; otherwise, once it has waited a moment, one step back
        const u32 now = SDL_GetTicks();
        const bool echo = pad_button_at != 0 && (pad_button_at >= escape_at ? pad_button_at - escape_at
                                                                              : escape_at - pad_button_at) < ECHO_MS;
        if (echo) {
            escape_at = 0;
            LogEcho();
        } else if (now - escape_at >= 80) {
            escape_at = 0;
            pad_last = false;
            if (!menu_open) {
                Open();
            } else if (key_capture) {
                menu->OnCapturedKey(SDL_SCANCODE_ESCAPE); // the key awaited stays as it was
            } else if (escape_busy) {
                give_escape = true; // a list, a question or a text field takes it (Dear ImGui's)
            } else if (menu->EditingLayout()) {
                menu->CancelLayoutEdit(); // the layout editor's Esc: back to the menu as it was
            } else if (!menu->OnBack()) {
                Close(); // nothing of the menu's to step back from: back to the game
            }
        }
    }
    if (close_requested) {
        close_requested = false;
        Close();
    }
    const bool keyboard_up = keyboard->Active();
    SetCapture(menu_open || keyboard_up);
    // a hint over the game (the skip after a quest) while nothing else of the menu's is up
    const Hint hint = capturing ? Hint{} : CurrentHint(config);
    const bool fps_counter = FpsCounterOn(config.fps_counter);
    // the key guides while the keyboard and mouse are in use, under nothing else of the menu's
    const bool guides = !capturing && window.KeyboardMouseActive() && FpsCounterOn(config.key_guides);
    // pause and fast forward, under nothing of the menu's
    const std::string speed = capturing ? std::string() : Speed::Get().Badge(SDL_GetTicks());
    // why the touch-screen key changed nothing (ui/hint.h), for a few seconds
    const std::string notice = capturing ? std::string() : TouchScreenNotice(SDL_GetTicks());
    const bool draw = capturing || keyboard->HasNotice() || !hint.Empty() || fps_counter || guides ||
                      !speed.empty() || !notice.empty();
    if (!draw || !renderer_ready.load(std::memory_order_acquire)) {
        if (visible.load(std::memory_order_relaxed)) {
            std::scoped_lock lock{mutex};
            draw_ready = false;
            visible.store(false, std::memory_order_release);
        }
        return;
    }

    std::unique_lock lock{mutex};
    MakeCurrent();
    ImGuiIO& io = ImGui::GetIO();
    if (fresh_input) {
        // whatever the menu had pressed when it closed is over
        io.ClearEventsQueue();
        io.ClearInputKeys();
        io.ClearInputMouse();
        int x = 0, y = 0;
        SDL_GetMouseState(&x, &y);
        io.AddMousePosEvent(static_cast<float>(x), static_cast<float>(y));
        // and a controller's buttons held now (the backend reads them each frame) count as held,
        // not as pressed: a frame that takes them in and shows nothing (B held as the menu
        // opened would close it at once, A would choose its first button)
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        ImGui::EndFrame();
        fresh_input = false;
    }
    if (give_escape) {
        give_escape = false;
        io.AddKeyEvent(ImGuiKey_Escape, true);
        io.AddKeyEvent(ImGuiKey_Escape, false); // (a frame later: Dear ImGui trickles the queue)
    }
    ImGui_ImplSDL2_NewFrame();
    // pixels per window unit as the window counts them (the backend asks OpenGL, which a
    // Vulkan or software window doesn't have)
    int w = 0, h = 0, pw = 0, ph = 0;
    SDL_GetWindowSize(window.GetSDLWindow(), &w, &h);
    SDL_GetWindowSizeInPixels(window.GetSDLWindow(), &pw, &ph);
    if (w > 0 && h > 0 && pw > 0 && ph > 0) {
        io.DisplayFramebufferScale = ImVec2(static_cast<float>(pw) / w, static_cast<float>(ph) / h);
    }
    UpdateScale(io.DisplaySize.y);
    if (capturing) {
        SDL_ShowCursor(SDL_ENABLE);
    }

    ImGui::NewFrame();
    help_focused = help_hovered = nullptr;
    if (menu_open) {
        menu->Draw();
    } else if (keyboard_up) {
        keyboard->Draw();
    }
    if (!keyboard_up && keyboard->HasNotice()) {
        keyboard->DrawNotice();
    }
    if (!hint.Empty()) {
        DrawHint(hint, Port::Hooks::SkipHint() == 2);
    }
    if (fps_counter) {
        DrawFpsCounter(config.fps_counter);
    }
    if (guides) {
        DrawKeyGuides(config, config.key_guides);
    }
    if (!speed.empty()) {
        DrawSpeedBadge(speed, Speed::Get().Paused());
    }
    if (!notice.empty()) {
        DrawNotice(notice, !speed.empty());
    }
    was_busy = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup) || ImGui::IsAnyItemActive();
    ImGui::Render();
    copy.CopyFrom(*ImGui::GetDrawData());
    draw_ready = true;
    visible.store(true, std::memory_order_release);
    lock.unlock();

    // what the frame asked for, without the lock
    auto actions = std::move(posted);
    posted.clear();
    for (auto& action : actions) {
        action();
    }
}

ImDrawData* Layer::DrawData(u32 width, u32 height) {
    ImDrawData& data = copy.data;
    if (!draw_ready || width == 0 || height == 0 || data.DisplaySize.x <= 0.0f ||
        data.DisplaySize.y <= 0.0f || data.CmdLists.Size == 0) {
        return nullptr;
    }
    // drawn at the size of the target, which can differ from the window's for a frame while it
    // is resized
    data.FramebufferScale = ImVec2(static_cast<float>(width) / data.DisplaySize.x,
                                   static_cast<float>(height) / data.DisplaySize.y);
    return &data;
}

Layer* Get() {
    return instance.load(std::memory_order_acquire);
}

// ── ui.h ──

void Start(Core::System& system, Window& window, Config& config) {
    if (!Get()) {
        instance.store(new Layer(system, window, config), std::memory_order_release);
    }
}

void Stop() {
    delete instance.exchange(nullptr);
}

bool OnEvent(const SDL_Event& event) {
    Layer* layer = Get();
    return layer && layer->OnEvent(event);
}

void Frame() {
    if (Layer* layer = Get()) {
        layer->Frame();
    }
}

bool Visible() {
    const Layer* layer = Get();
    return layer && layer->Capturing();
}

void ToggleFpsCounter() {
    if (Layer* layer = Get()) {
        layer->ToggleFpsCounter();
    }
}

void ToggleKeyGuides() {
    if (Layer* layer = Get()) {
        layer->ToggleKeyGuides();
    }
}

} // namespace App::Ui
