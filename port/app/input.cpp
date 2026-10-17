#include "input.h"

#include <SDL.h>
#include <fmt/format.h>

#include "config.h"
#include "runtime.h"
#include "util/log.h"

namespace App::Input {

namespace {

/// The first game controller SDL knows, and its port among controllers of the same kind (as
/// Azahar's SDL engine numbers them).
bool FirstController(ControllerInfo& out) {
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (!SDL_IsGameController(i)) {
            continue;
        }
        char guid[33];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof(guid));
        out.guid = guid;
        out.port = 0;
        for (int j = 0; j < i; ++j) {
            char other[33];
            SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(j), other, sizeof(other));
            out.port += out.guid == other ? 1 : 0;
        }
        const char* name = SDL_GameControllerNameForIndex(i);
        out.name = name ? name : "controller";
        return true;
    }
    return false;
}

std::string controller_description = "no controller";

} // namespace

int KeyCode(const std::string& name) {
    if (name.empty()) {
        return 0;
    }
    const SDL_Scancode code = SDL_GetScancodeFromName(name.c_str());
    if (code == SDL_SCANCODE_UNKNOWN) {
        APP_LOG_WARNING("no key called \"{}\"", name);
        return 0;
    }
    return static_cast<int>(code);
}

void BuildProfile(const Config& config) {
    ControllerInfo pad;
    const bool have_pad = config.controller_auto && FirstController(pad);
    if (have_pad) {
        controller_description = fmt::format("controller: {} (port {})", pad.name, pad.port);
    } else {
        controller_description = config.controller_auto ? "no controller found" : "controller off";
    }
    Runtime().BuildInputProfile(config, have_pad ? &pad : nullptr);
}

std::string ControllerDescription() {
    return controller_description;
}

void SetMuted(bool on) {
    Runtime().SetInputMuted(on);
}

} // namespace App::Input
