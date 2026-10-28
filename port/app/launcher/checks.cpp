#include "launcher/checks.h"

#include <optional>

#include <fmt/format.h>

#include "runtime.h"
#include "util/file.h"
#include "util/log.h"

namespace Launcher::Checks {

const Region* FindRegion(u64 program_id) {
    for (const Region& region : REGIONS) {
        if (region.program_id == program_id) {
            return &region;
        }
    }
    return nullptr;
}

const Region& RegionOf(u64 program_id) {
    const Region* region = FindRegion(program_id);
    return region ? *region : REGIONS[0];
}

Game CheckGame(const std::string& path) {
    Game game;
    if (path.empty()) {
        return game;
    }
    if (!App::Util::Exists(path) || App::Util::IsDirectory(path)) {
        game.state = Game::State::Missing;
        return game;
    }
    const App::GameHeader header = App::Runtime().ReadGameHeader(path);
    switch (header.state) {
    case App::GameHeader::State::Ok:
        game.program_id = header.program_id;
        game.state = FindRegion(game.program_id) ? Game::State::Ok : Game::State::OtherGame;
        break;
    case App::GameHeader::State::Encrypted:
        // the header itself is readable: it says which game it is
        game.program_id = header.program_id;
        game.state = Game::State::Encrypted;
        break;
    case App::GameHeader::State::Unreadable:
        game.state = Game::State::NotA3ds;
        break;
    }
    return game;
}

std::string Describe(const Game& game) {
    using State = Game::State;
    switch (game.state) {
    case State::Unset:
        return "Choose your decrypted Monster Hunter 4 Ultimate (.3ds, .cci or .cxi).";
    case State::Missing:
        return "The game file isn't there any more: choose it again.";
    case State::NotA3ds:
        return "This file isn't a 3DS game the app can read: choose your decrypted .3ds, .cci or "
               ".cxi.";
    case State::Encrypted:
        return fmt::format("This {} is encrypted. The port needs a decrypted dump of your own copy "
                           "(GodMode9 on your 3DS can make one).",
                           FindRegion(game.program_id) ? "copy of Monster Hunter 4 Ultimate"
                                                       : "game file");
    case State::OtherGame:
        return fmt::format("This is another 3DS game (program id {:016X}), not Monster Hunter 4 "
                           "Ultimate for the USA ({:016X}) or Europe ({:016X}), the versions the "
                           "port is made for.",
                           game.program_id, REGIONS[0].program_id, REGIONS[1].program_id);
    case State::Ok:
        return fmt::format("Monster Hunter 4 Ultimate ({}), decrypted.", RegionOf(game.program_id).name);
    }
    return "";
}

Update CheckUpdate(u64 program_id) {
    const Region& region = RegionOf(program_id);
    Update update;
    const App::InstalledTitle title = App::Runtime().FindInstalledTitle(region.update_id);
    if (!title.found) {
        return update;
    }
    update.version = title.version;
    update.content = title.content_path;
    if (update.content.empty() || !App::Util::Exists(update.content)) {
        update.content.clear();
        return update;
    }
    update.state = update.version == region.update_version ? Update::State::Ok : Update::State::OtherVersion;
    return update;
}

std::string VersionText(u16 version) {
    // the 3DS's title versions: major in bits 10-15, minor in 4-9, micro in 0-3
    return fmt::format("{}.{}.{}", version >> 10, (version >> 4) & 0x3F, version & 0xF);
}

std::string UpdateName(u64 program_id) {
    return "v" + VersionText(RegionOf(program_id).update_version);
}

std::string CheckUpdateCia(const std::string& path, u64 program_id) {
    const Region& region = RegionOf(program_id);
    std::optional<App::CiaInfo> info;
    switch (App::Runtime().CheckCia(path, info)) {
    case App::CiaResult::Ok:
        break;
    case App::CiaResult::Encrypted:
        return "This .cia is encrypted. The port needs a decrypted one (GodMode9 can build one from "
               "the update installed on your 3DS).";
    case App::CiaResult::Unreadable:
        return "The file can't be read.";
    default:
        return "This file isn't a CIA the app can read.";
    }
    if (info) {
        if (info->title_id != region.update_id) {
            for (const Region& other : REGIONS) {
                if (info->title_id == other.update_id) {
                    return fmt::format("This .cia is the update for {}, but your game is the {} one: "
                                       "choose the update of the same region.",
                                       other.name, region.name);
                }
            }
            return fmt::format("This .cia is title {:016X}, not Monster Hunter 4 Ultimate's update "
                               "({:016X}).",
                               info->title_id, region.update_id);
        }
        // another version number is no reason to refuse it: a decrypted .cia often says v1.0.0
        // (see Update::Installed); building the code library checks the code itself
    }
    return "";
}

std::string LibraryPath(u64 program_id) {
#if defined(_WIN32)
    constexpr const char* ext = "dll";
#elif defined(__APPLE__)
    constexpr const char* ext = "dylib";
#else
    constexpr const char* ext = "so";
#endif
    return fmt::format("{}recomp/{:016X}.{}", App::Runtime().UserDir(),
                       RegionOf(program_id).program_id, ext);
}

Code CodeFromCheck(int exit_code, const std::string& output) {
    Code code;
    std::string notice;
    if (const auto at = output.find("notice:\n"); at != std::string::npos) {
        notice = output.substr(at + 8);
        // the first paragraph: what happened (the rest says how to build the library, which
        // the launcher's own button does)
        if (const auto end = notice.find("\n\n"); end != std::string::npos) {
            notice.resize(end);
        }
        while (!notice.empty() && (notice.back() == '\n' || notice.back() == '\r')) {
            notice.pop_back();
        }
    }
    switch (exit_code) {
    case 0:
        code.state = Code::State::Ok;
        break;
    case 3:
        code.state = Code::State::Partial;
        code.notice = notice;
        break;
    case 2:
        code.state = Code::State::NotUsed;
        code.notice = notice.empty() ? "The game would run without the port's code library."
                                     : notice;
        break;
    default:
        code.state = Code::State::Failed;
        code.notice = output.empty() ? fmt::format("The check ended with status {}.", exit_code)
                                     : output.substr(0, 400);
        break;
    }
    return code;
}

Install::~Install() {
    if (thread.joinable()) {
        thread.join();
    }
}

void Install::Start(const std::string& path) {
    if (thread.joinable()) {
        thread.join();
    }
    error.clear();
    progress = 0.0f;
    running = true;
    thread = std::thread([this, path] {
        const auto status =
            App::Runtime().InstallCia(path, [this](std::size_t done, std::size_t total) {
                if (total > 0) {
                    progress = static_cast<float>(static_cast<double>(done) / static_cast<double>(total));
                }
            });
        switch (status) {
        case App::CiaResult::Ok:
            break;
        case App::CiaResult::Encrypted:
            error = "The .cia is encrypted: the port needs a decrypted one.";
            break;
        case App::CiaResult::Unreadable:
            error = "The .cia can't be read.";
            break;
        case App::CiaResult::Aborted:
            error = "Installing stopped half-way (is the disk full?). The log has the details.";
            break;
        default:
            error = "This file isn't a CIA that can be installed.";
            break;
        }
        APP_LOG_INFO("installing {}: {}", path, error.empty() ? "done" : error);
        progress = 1.0f;
        running = false;
    });
}

} // namespace Launcher::Checks
