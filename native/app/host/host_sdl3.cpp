// The host services (app_window.h, Host) of mh4u-native, on SDL3: the shared sources ask for
// time, the clipboard, the cursor, key names, message boxes, controllers and the window icon
// through them and include no SDL header. The Azahar app's SDL2 version is port/app/host_sdl2.cpp
// (and input_sdl2.cpp for the controllers).
//
// Scancodes are SDL2's numbering (app_window.h): SDL3 keeps those values for the keys the app
// names (letters, digits, arrows, F-keys, modifiers), and the window translates the rest.
#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <utility>

#include "app_window.h"
#include "icon/icon_data.h"
#include "input.h"

namespace App::Host {

std::uint32_t Ticks() {
    return static_cast<std::uint32_t>(SDL_GetTicks());  // wraps after 49 days, as documented
}

std::string GetClipboard() {
    std::string text;
    if (char* clipboard = SDL_GetClipboardText()) {
        text = clipboard;
        SDL_free(clipboard);
    }
    return text;
}

void SetClipboard(const std::string& text) {
    SDL_SetClipboardText(text.c_str());
}

void ShowCursor(bool show) {
    if (show) {
        SDL_ShowCursor();
    } else {
        SDL_HideCursor();
    }
}

Point MousePosition() {
    float x = 0.0f;
    float y = 0.0f;
    SDL_GetMouseState(&x, &y);
    return {static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y))};
}

int ScancodeFromName(const std::string& name) {
    const SDL_Scancode code = SDL_GetScancodeFromName(name.c_str());
    return code == SDL_SCANCODE_UNKNOWN ? 0 : static_cast<int>(code);
}

std::string ScancodeName(int scancode) {
    if (scancode <= 0 || scancode >= Scancode::Count || scancode >= SDL_SCANCODE_COUNT) {
        return "";
    }
    return SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
}

void ShowMessageBox(const std::string& title, const std::string& text, bool error, IWindow* parent) {
    const SDL_MessageBoxFlags flags = error ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_INFORMATION;
    if (!SDL_ShowSimpleMessageBox(flags, title.c_str(), text.c_str(), parent ? parent->SdlWindow() : nullptr)) {
        // no message box could be shown (no display): the player still gets the words
        std::fprintf(stderr, "%s: %s\n", title.c_str(), text.c_str());
    }
}

std::vector<ControllerInfo> ListControllers() {
    std::vector<ControllerInfo> pads;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    if (!ids) {
        return pads;
    }
    // every joystick's GUID, controllers or not: the port counts devices of the same kind
    std::vector<std::string> guids;
    for (int i = 0; i < count; ++i) {
        char guid[33];
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(ids[i]), guid, sizeof(guid));
        guids.push_back(guid);
    }
    for (int i = 0; i < count; ++i) {
        if (!SDL_IsGamepad(ids[i])) {
            continue;
        }
        ControllerInfo pad;
        pad.guid = guids[static_cast<std::size_t>(i)];
        pad.port = Input::ControllerPort(guids, static_cast<std::size_t>(i));
        const char* name = SDL_GetGamepadNameForID(ids[i]);
        pad.name = name ? name : "";
        pads.push_back(std::move(pad));
    }
    SDL_free(ids);
    return pads;
}

void SetWindowIcon(IWindow& window) {
    SDL_Window* sdl_window = window.SdlWindow();
    if (!sdl_window) {
        return;
    }
    // the pixels are RGBA, row by row, top to bottom
    SDL_Surface* surface =
        SDL_CreateSurfaceFrom(Icon::kSize, Icon::kSize, SDL_PIXELFORMAT_RGBA32,
                              const_cast<unsigned char*>(Icon::kPixels), Icon::kSize * 4);
    if (!surface) {
        return;
    }
    SDL_SetWindowIcon(sdl_window, surface);
    SDL_DestroySurface(surface);
}

} // namespace App::Host
