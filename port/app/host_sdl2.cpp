// The host services (app_window.h, Host) of the Azahar app, on SDL2: the shared sources ask for
// time, the clipboard, the cursor, key names, message boxes and the window icon through them
// and include no SDL header. (The controllers are in input_sdl2.cpp.)

#include <SDL.h>

#include "app_window.h"
#include "icon.h"

namespace App::Host {

std::uint32_t Ticks() {
    return SDL_GetTicks();
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
    SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE);
}

Point MousePosition() {
    Point at;
    SDL_GetMouseState(&at.x, &at.y);
    return at;
}

int ScancodeFromName(const std::string& name) {
    return static_cast<int>(SDL_GetScancodeFromName(name.c_str()));
}

std::string ScancodeName(int scancode) {
    if (scancode <= 0 || scancode >= SDL_NUM_SCANCODES) {
        return "";
    }
    return SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
}

void ShowMessageBox(const std::string& title, const std::string& text, bool error, IWindow* parent) {
    SDL_ShowSimpleMessageBox(error ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_INFORMATION, title.c_str(),
                             text.c_str(), parent ? parent->SdlWindow() : nullptr);
}

void SetWindowIcon(IWindow& window) {
    Icon::Apply(window.SdlWindow());
}

} // namespace App::Host
