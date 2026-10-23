// The controllers of mh4u-native on SDL3's gamepad API (N3-07): the IGamepadLayer the mapper asks
// which controllers there are. Port/app's host_sdl3.cpp lists them the same way (ListControllers)
// for the shared sources; this one also opens them, since SDL3 sends a gamepad's events only to
// the program that opened it.
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "input.h"
#include "input/input_mapper.h"
#include "util/log.h"

namespace mh4n::app {

namespace {

class SdlGamepads final : public IGamepadLayer {
public:
    ~SdlGamepads() override {
        for (const auto& [id, gamepad] : open_) {
            SDL_CloseGamepad(gamepad);
        }
    }

    std::vector<PadInfo> List() override {
        std::vector<PadInfo> pads;
        std::set<SDL_JoystickID> present;
        int count = 0;
        SDL_JoystickID* ids = SDL_GetJoysticks(&count);
        if (ids) {
            // every joystick's GUID, gamepads or not: the port counts devices of the same kind
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
                if (open_.find(ids[i]) == open_.end()) {
                    SDL_Gamepad* gamepad = SDL_OpenGamepad(ids[i]);
                    if (!gamepad) {
                        APP_LOG_WARNING("a controller did not open: {}", SDL_GetError());
                        continue;
                    }
                    open_[ids[i]] = gamepad;
                }
                present.insert(ids[i]);
                PadInfo pad;
                pad.id = static_cast<std::int32_t>(ids[i]);
                pad.guid = guids[static_cast<std::size_t>(i)];
                pad.port = App::Input::ControllerPort(guids, static_cast<std::size_t>(i));
                const char* name = SDL_GetGamepadNameForID(ids[i]);
                pad.name = name ? name : "";
                pads.push_back(std::move(pad));
            }
            SDL_free(ids);
        }
        for (auto it = open_.begin(); it != open_.end();) {
            if (present.count(it->first) == 0) {
                SDL_CloseGamepad(it->second);
                it = open_.erase(it);
            } else {
                ++it;
            }
        }
        return pads;
    }

private:
    std::map<SDL_JoystickID, SDL_Gamepad*> open_;
};

}  // namespace

std::unique_ptr<IGamepadLayer> MakeSdlGamepads() {
    return std::make_unique<SdlGamepads>();
}

}  // namespace mh4n::app
