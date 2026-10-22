// The controllers the Azahar app's SDL2 knows, for the input profile (Host::ListControllers,
// app_window.h).

#include <utility>

#include <SDL.h>

#include "app_window.h"
#include "input.h"

namespace App::Host {

std::vector<ControllerInfo> ListControllers() {
    // every joystick's GUID, controllers or not: the port counts devices of the same kind
    std::vector<std::string> guids;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        char guid[33];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof(guid));
        guids.push_back(guid);
    }
    std::vector<ControllerInfo> pads;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (!SDL_IsGameController(i)) {
            continue;
        }
        ControllerInfo pad;
        pad.guid = guids[static_cast<std::size_t>(i)];
        pad.port = Input::ControllerPort(guids, static_cast<std::size_t>(i));
        const char* name = SDL_GameControllerNameForIndex(i);
        pad.name = name ? name : "";
        pads.push_back(std::move(pad));
    }
    return pads;
}

} // namespace App::Host
