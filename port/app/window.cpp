#include "window.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <SDL.h>
#include <fmt/format.h>
#include <imgui_impl_sdl2.h>

#include "common/logging/log.h"
#include "config.h"
#include "core/core.h"
#include "frontend.h"
#include "input.h"
#include "runtime.h"
#include "ui/fps_counter.h"
#include "ui/ui.h"

namespace App {

#ifdef ENABLE_OPENGL
std::unique_ptr<Window> MakeOpenGLWindow(Core::System& system, const Config& config, bool is_secondary);
#endif
#ifdef ENABLE_VULKAN
std::unique_ptr<Window> MakeVulkanWindow(Core::System& system, const Config& config, bool is_secondary);
#endif
#ifdef ENABLE_SOFTWARE_RENDERER
std::unique_ptr<Window> MakeSoftwareWindow(Core::System& system, const Config& config, bool is_secondary);
#endif

const SDL_Event* Window::current_event = nullptr;

namespace {
constexpr u32 CURSOR_IDLE_MS = 2000;  // full screen: the mouse cursor hides after this long
constexpr u32 TITLE_EVERY_MS = 2000;
constexpr u32 MEASURE_EVERY_MS = 500;  // the frame rate, for the title and the counter
} // namespace

std::unique_ptr<Window> Window::Create(Core::System& system, Api api, const Config& config,
                                       bool is_secondary) {
    switch (api) {
#ifdef ENABLE_OPENGL
    case Api::OpenGL:
        return MakeOpenGLWindow(system, config, is_secondary);
#endif
#ifdef ENABLE_VULKAN
    case Api::Vulkan:
        return MakeVulkanWindow(system, config, is_secondary);
#endif
#ifdef ENABLE_SOFTWARE_RENDERER
    case Api::Software:
        return MakeSoftwareWindow(system, config, is_secondary);
#endif
    default:
        LOG_ERROR(Frontend, "this build has no such renderer");
        return nullptr;
    }
}

bool Window::InitSDL() {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // Windows: draw at the screen's own pixels on a scaled (high-DPI) display instead of being
    // stretched blurry by the system (the window's coordinates are pixels then, as ToPixels expects)
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    // the game is played with the mouse as a stylus: no touch-to-mouse doubling
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) < 0) {
        LOG_CRITICAL(Frontend, "SDL didn't start: {}", SDL_GetError());
        return false;
    }
    SDL_SetMainReady();
    return true;
}

Window::Window(Core::System& system_, const Config& config_, bool is_secondary_)
    : EmuWindow(is_secondary_), system(system_), config(config_) {
    ReadKeys();
    // auto starts with the controller's way when there is a controller (a Steam Deck's, say)
    bool have_pad = false;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        have_pad = have_pad || SDL_IsGameController(i);
    }
    const auto mode = Input::ParseMode(config.input_mode);
    last_used_keyboard = mode == Input::Mode::KeyboardMouse || (mode == Input::Mode::Auto && !have_pad);
    keyboard_mouse = last_used_keyboard;
}

void Window::ReadKeys() {
    touch_keys = Input::KeyCodes(config.touch_screen_key);
    stylus_keys = Input::KeyCodes(config.stylus_key);
    fullscreen_key = Input::KeyCode(config.fullscreen_key);
    fps_counter_key = Input::KeyCode(config.fps_counter_key);
    key_guides_key = Input::KeyCode(config.key_guides_key);
}

bool Window::EchoOfController() const {
    // Steam Input's desktop layouts send keys (B: Esc, A: Enter, ...) for a controller's buttons
    // while SDL still sees the controller itself; the key comes within a few milliseconds of it
    return last_pad_event != 0 && SDL_GetTicks() - last_pad_event < 150;
}

void Window::UpdateMouseMode() {
    if (is_secondary) {
        return;
    }
    const auto mode = Input::ParseMode(config.input_mode);
    const bool active = mode == Input::Mode::KeyboardMouse ||
                        (mode == Input::Mode::Auto && last_used_keyboard);
    keyboard_mouse = active;
    // the camera only while the game has the mouse: not under the menu, not without the focus,
    // not while the stylus key is held (the touch screen's turn)
    bool stylus = false;
    const Uint8* held = SDL_GetKeyboardState(nullptr);
    for (const int code : stylus_keys) {
        stylus = stylus || (code > 0 && code < SDL_NUM_SCANCODES && held[code]);
    }
    const bool focused = (SDL_GetWindowFlags(render_window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    const bool want = active && focused && !stylus && !Ui::Visible();
    if (want == mouse_camera) {
        return;
    }
    mouse_camera = want;
    SDL_SetRelativeMouseMode(want ? SDL_TRUE : SDL_FALSE);
    if (want) {
        TouchReleased();
    } else {
        // the mouse's buttons let go as it stops being the camera, and the stick centred
        for (const int button : {SDL_BUTTON_LEFT, SDL_BUTTON_MIDDLE, SDL_BUTTON_RIGHT, SDL_BUTTON_X1, SDL_BUTTON_X2}) {
            Runtime().KeyUp(Input::MOUSE_KEY + button);
        }
        Input::MouseReset();
        ShowCursorFor(CURSOR_IDLE_MS);
    }
}

void Window::ImGuiInit() {
    ImGui_ImplSDL2_InitForOther(render_window);
    ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_AutoAll); // any controller, not the first
}

void Window::ImGuiShutdown() {
    ImGui_ImplSDL2_Shutdown();
}

void Window::ImGuiNewFrame() {
    ImGui_ImplSDL2_NewFrame();
}

void Window::ImGuiForwardCurrentEvent() {
    if (current_event) {
        ImGui_ImplSDL2_ProcessEvent(current_event);
    }
}

InputEvent Window::Translate(const SDL_Event& event) {
    using Type = InputEvent::Type;
    InputEvent out;
    switch (event.type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        out.type = event.type == SDL_KEYDOWN ? Type::KeyDown : Type::KeyUp;
        out.scancode = static_cast<int>(event.key.keysym.scancode);
        out.repeat = event.key.repeat != 0;
        out.mods = event.key.keysym.mod;
        break;
    case SDL_TEXTINPUT:
        out.type = Type::Text;
        out.text = event.text.text;
        break;
    case SDL_MOUSEMOTION:
        out.type = Type::MouseMotion;
        out.x = event.motion.x;
        out.y = event.motion.y;
        out.dx = event.motion.xrel;
        out.dy = event.motion.yrel;
        out.from_touch = event.motion.which == SDL_TOUCH_MOUSEID;
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        out.type = Type::MouseButton;
        out.button = event.button.button;
        out.down = event.type == SDL_MOUSEBUTTONDOWN;
        out.x = event.button.x;
        out.y = event.button.y;
        out.from_touch = event.button.which == SDL_TOUCH_MOUSEID;
        break;
    case SDL_MOUSEWHEEL:
        out.type = Type::Wheel;
        out.wheel_x = event.wheel.x;
        out.wheel_y = event.wheel.y;
        out.from_touch = event.wheel.which == SDL_TOUCH_MOUSEID;
        break;
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
        out.type = Type::PadButton;
        out.pad = event.cbutton.which;
        out.button = event.cbutton.button;
        out.down = event.type == SDL_CONTROLLERBUTTONDOWN;
        break;
    case SDL_CONTROLLERAXISMOTION:
        out.type = Type::PadAxis;
        out.pad = event.caxis.which;
        out.axis = event.caxis.axis;
        out.value = event.caxis.value;
        break;
    case SDL_CONTROLLERDEVICEADDED:
        out.type = Type::PadAdded;
        out.pad = event.cdevice.which;
        break;
    case SDL_CONTROLLERDEVICEREMOVED:
        out.type = Type::PadRemoved;
        out.pad = event.cdevice.which;
        break;
    case SDL_FINGERDOWN:
    case SDL_FINGERUP:
    case SDL_FINGERMOTION:
        out.type = Type::Touch;
        break;
    case SDL_WINDOWEVENT:
        switch (event.window.event) {
        case SDL_WINDOWEVENT_SIZE_CHANGED:
        case SDL_WINDOWEVENT_RESIZED:
        case SDL_WINDOWEVENT_MAXIMIZED:
        case SDL_WINDOWEVENT_RESTORED:
        case SDL_WINDOWEVENT_DISPLAY_CHANGED:
            out.type = Type::Resized;
            break;
        case SDL_WINDOWEVENT_CLOSE:
            out.type = Type::CloseRequested;
            break;
        case SDL_WINDOWEVENT_FOCUS_LOST:
            out.type = Type::FocusLost;
            break;
        default:
            break;
        }
        break;
    case SDL_QUIT:
        out.type = Type::Quit;
        break;
    default:
        break;
    }
    return out;
}

Window::~Window() {
    if (render_window) {
        SDL_DestroyWindow(render_window);
    }
}

u32 Window::BaseFlags() {
    return SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
}

void Window::FinishSetup(bool start_fullscreen) {
    window_id = SDL_GetWindowID(render_window);
    Host::SetWindowIcon(*this);
    // a window the size of a 1280x720 picture, or the screen's if that's smaller
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(render_window), &usable) == 0) {
        const int w = std::min(1280, usable.w * 9 / 10);
        const int h = std::min(720, usable.h * 9 / 10);
        SDL_SetWindowSize(render_window, w, h);
        SDL_SetWindowPosition(render_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    if (start_fullscreen) {
        SetFullscreen(true);
    }
    OnResize();
    OnMinimalClientAreaChangeRequest(GetActiveConfig().min_client_area_size);
    SDL_PumpEvents();
}

void Window::SetFullscreen(bool on) {
    // desktop-sized full screen: no mode switch, quick to toggle, the game picks its own size
    if (SDL_SetWindowFullscreen(render_window, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) != 0) {
        LOG_ERROR(Frontend, "full screen {}: {}", on ? "on" : "off", SDL_GetError());
        return;
    }
    fullscreen = on;
    ShowCursorFor(CURSOR_IDLE_MS);
    OnResize();
}

void Window::OnResize() {
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(render_window, &width, &height);
    if (width > 0 && height > 0) {
        UpdateCurrentFramebufferLayout(static_cast<unsigned>(width), static_cast<unsigned>(height));
    }
}

void Window::OnMinimalClientAreaChangeRequest(std::pair<u32, u32> minimal_size) {
    // the core asks for a 3DS-sized minimum; a smaller window is fine for the port's layouts
    SDL_SetWindowMinimumSize(render_window, static_cast<int>(minimal_size.first / 2),
                             static_cast<int>(minimal_size.second / 2));
}

WindowSize Window::SizeInPoints() const {
    WindowSize size;
    SDL_GetWindowSize(render_window, &size.w, &size.h);
    return size;
}

WindowSize Window::SizeInPixels() const {
    WindowSize size;
    SDL_GetWindowSizeInPixels(render_window, &size.w, &size.h);
    return size;
}

std::pair<unsigned, unsigned> Window::ToPixels(s32 x, s32 y) const {
    int w = 1, h = 1, pw = 1, ph = 1;
    SDL_GetWindowSize(render_window, &w, &h);
    SDL_GetWindowSizeInPixels(render_window, &pw, &ph);
    const auto scale = [](s32 v, int from, int to) {
        return static_cast<unsigned>(std::max(0.0, std::round(static_cast<double>(v) * to / std::max(from, 1))));
    };
    return {scale(x, w, pw), scale(y, h, ph)};
}

void Window::ShowCursorFor(u32 ms) {
    if (mouse_camera) {
        return; // hidden while the mouse turns the camera
    }
    SDL_ShowCursor(SDL_ENABLE);
    cursor_hide_at = fullscreen ? SDL_GetTicks() + ms : 0;
}

void Window::OnKey(int scancode, bool down, u16 modifiers) {
    const bool alt_enter = scancode == SDL_SCANCODE_RETURN && (modifiers & KMOD_ALT);
    if (scancode == fullscreen_key || alt_enter) {
        if (down) {
            SetFullscreen(!fullscreen);
        }
        return;
    }
    if (scancode == fps_counter_key && fps_counter_key != 0) {
        if (down && !is_secondary) {
            Ui::ToggleFpsCounter();
        }
        return;
    }
    if (std::find(touch_keys.begin(), touch_keys.end(), scancode) != touch_keys.end()) {
        if (down) {
            Port::ToggleQuestMap();
        }
        return;
    }
    if (scancode == key_guides_key && key_guides_key != 0) {
        if (down && !is_secondary) {
            Ui::ToggleKeyGuides();
        }
        return;
    }
    if (down) {
        Runtime().KeyDown(scancode);
    } else {
        Runtime().KeyUp(scancode);
    }
}

void Window::PollEvents() {
    if (keys_changed.exchange(false)) {
        ReadKeys();
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // a controller's button: what Steam Input may send as a key with it isn't the keyboard's
        if (event.type == SDL_CONTROLLERBUTTONDOWN) {
            last_pad_event = SDL_GetTicks();
            last_used_keyboard = false;
        } else if (event.type == SDL_CONTROLLERAXISMOTION && std::abs(event.caxis.value) > 8000) {
            // (a stick too: Steam Input's desktop layouts send keys for the sticks as well)
            last_pad_event = SDL_GetTicks();
            if (std::abs(event.caxis.value) > 16000) {
                last_used_keyboard = false;
            }
        } else if (event.type == SDL_KEYDOWN && event.key.repeat == 0 && !EchoOfController()) {
            const auto code = event.key.keysym.scancode;
            // (the app's own keys don't count: F11 doesn't make a controller player's mouse the camera)
            if (code != SDL_SCANCODE_ESCAPE && code != fullscreen_key && code != fps_counter_key &&
                code != key_guides_key &&
                std::find(stylus_keys.begin(), stylus_keys.end(), code) == stylus_keys.end()) {
                last_used_keyboard = true;
            }
        } else if (event.type == SDL_MOUSEBUTTONDOWN && mouse_camera) {
            last_used_keyboard = true;
        }
        // the in-game menu's (ui/): its toggle, and while it is up the keys, mouse and controller
        current_event = &event;
        const bool taken = Ui::OnEvent(Translate(event));
        current_event = nullptr;
        if (taken) {
            continue;
        }
        switch (event.type) {
        case SDL_WINDOWEVENT:
            if (event.window.windowID != window_id) {
                break;
            }
            switch (event.window.event) {
            case SDL_WINDOWEVENT_SIZE_CHANGED:
            case SDL_WINDOWEVENT_RESIZED:
            case SDL_WINDOWEVENT_MAXIMIZED:
            case SDL_WINDOWEVENT_RESTORED:
            case SDL_WINDOWEVENT_DISPLAY_CHANGED:
                OnResize();
                break;
            case SDL_WINDOWEVENT_CLOSE:
                RequestClose();
                break;
            case SDL_WINDOWEVENT_FOCUS_LOST:
                // keys held while switching away would stay down
                Runtime().ReleaseAllKeys();
                break;
            }
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            if (event.key.repeat == 0) {
                OnKey(static_cast<int>(event.key.keysym.scancode), event.type == SDL_KEYDOWN,
                      event.key.keysym.mod);
            }
            break;
        case SDL_MOUSEMOTION:
            if (mouse_camera) {
                if (event.motion.which != SDL_TOUCH_MOUSEID) {
                    Input::MouseMotion(config, event.motion.xrel, event.motion.yrel);
                }
                break;
            }
            if (event.motion.which != SDL_TOUCH_MOUSEID && event.motion.windowID == window_id) {
                ShowCursorFor(CURSOR_IDLE_MS);
                if (event.motion.state & SDL_BUTTON_LMASK) {
                    const auto [x, y] = ToPixels(event.motion.x, event.motion.y);
                    TouchMoved(x, y);
                }
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            if (mouse_camera) {
                // the mouse's buttons are keys like any other (Mouse Left, ...)
                if (event.button.which != SDL_TOUCH_MOUSEID) {
                    const int code = Input::MOUSE_KEY + event.button.button;
                    if (event.type == SDL_MOUSEBUTTONDOWN) {
                        Runtime().KeyDown(code);
                    } else {
                        Runtime().KeyUp(code);
                    }
                }
                break;
            }
            if (event.button.which != SDL_TOUCH_MOUSEID && event.button.windowID == window_id &&
                event.button.button == SDL_BUTTON_LEFT) {
                ShowCursorFor(CURSOR_IDLE_MS);
                if (event.type == SDL_MOUSEBUTTONDOWN) {
                    const auto [x, y] = ToPixels(event.button.x, event.button.y);
                    TouchPressed(x, y);
                } else {
                    TouchReleased();
                }
            }
            break;
        case SDL_MOUSEWHEEL:
            if (mouse_camera && event.wheel.which != SDL_TOUCH_MOUSEID) {
                if (event.wheel.y != 0) {
                    Input::Wheel(config, event.wheel.y > 0 ? Input::WHEEL_UP : Input::WHEEL_DOWN);
                }
                if (event.wheel.x != 0) {
                    Input::Wheel(config, event.wheel.x > 0 ? Input::WHEEL_RIGHT : Input::WHEEL_LEFT);
                }
            }
            break;
        case SDL_FINGERDOWN:
        case SDL_FINGERMOTION: {
            int pw = 1, ph = 1;
            SDL_GetWindowSizeInPixels(render_window, &pw, &ph);
            const auto x = static_cast<unsigned>(std::max(0.0f, event.tfinger.x * pw));
            const auto y = static_cast<unsigned>(std::max(0.0f, event.tfinger.y * ph));
            if (event.type == SDL_FINGERDOWN) {
                TouchPressed(x, y);
            } else {
                TouchMoved(x, y);
            }
            break;
        }
        case SDL_FINGERUP:
            TouchReleased();
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            // the 3DS has no stick clicks: the right one is the port's touch-screen switch
            if (event.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSTICK) {
                Port::ToggleQuestMap();
            }
            break;
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED:
            controllers_changed = true;
            break;
        case SDL_QUIT:
            RequestClose();
            break;
        default:
            break;
        }
    }
    UpdateMouseMode();
    if (!is_secondary) {
        Input::Tick(config);
    }
    if (cursor_hide_at && SDL_GetTicks() >= cursor_hide_at && !Ui::Visible()) {
        SDL_ShowCursor(SDL_DISABLE);
        cursor_hide_at = 0;
    }
    if (!is_secondary) {
        const u32 now = SDL_GetTicks();
        if (now - last_measure >= MEASURE_EVERY_MS) {
            last_measure = now;
            const PerfStats stats = Runtime().TakePerfStats();
            fps = stats.game_fps;
            Ui::SetFpsMeasure(fps, fps > 0.0 ? 1000.0 / fps : 0.0, stats.emulation_speed);
        }
        if (now - last_title >= TITLE_EVERY_MS) {
            last_title = now;
            SDL_SetWindowTitle(render_window, fmt::format("MH4U PC port | {:.0f} fps", fps).c_str());
        }
        Ui::Frame(); // the in-game menu's frame, when it is up
    }
}

} // namespace App
