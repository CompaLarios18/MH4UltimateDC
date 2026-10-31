#include "launcher/checks.h"

#include <memory>
#include <fmt/format.h>

#include "common/file_derived.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "core/file_sys/ncch_container.h"
#include "core/file_sys/title_metadata.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/fs/archive.h"
#include "core/loader/loader.h"

namespace Launcher::Checks {

namespace {

/// UpdateCode's reads, from one of Azahar's files.
UpdateCode::Read Reader(FileUtil::IOFileBase& file) {
    return [&file](u64 offset, void* out, std::size_t size) {
        return file.ReadAtBytes(static_cast<u8*>(out), size, static_cast<std::size_t>(offset)) == size;
    };
}

UpdateCode::Code Unreadable() {
    UpdateCode::Code code;
    code.error = "it can't be opened";
    return code;
}

/// The code in a .cia, the file read as Azahar's installer reads it (also compressed).
UpdateCode::Code CiaCode(const std::string& path, bool compressed) {
    std::unique_ptr<FileUtil::IOFileBase> file = std::make_unique<FileUtil::IOFile>(path, "rb");
    if (compressed) {
        file = std::make_unique<FileUtil::Z3DSReadIOFile>(std::move(file));
    }
    if (!file->IsOpen()) {
        return Unreadable();
    }
    return UpdateCode::ReadCiaCode(Reader(*file));
}

/// The installed update's own code: its program as Azahar's loader opens it, but not a code.bin
/// from Azahar's mods folder (which NCCHContainer::LoadSectionExeFS would read instead).
UpdateCode::Code InstalledCode(const std::string& path) {
    const auto file = FileSys::NCCHContainer::AutoOpenNCCHNCSD(path);
    if (!file || !file->IsOpen()) {
        return Unreadable();
    }
    return UpdateCode::ReadCode(Reader(*file), 0);
}

} // namespace

Game CheckGame(const std::string& path) {
    Game game;
    if (path.empty()) {
        return game;
    }
    if (!FileUtil::Exists(path) || FileUtil::IsDirectory(path)) {
        game.state = Game::State::Missing;
        return game;
    }
    FileSys::NCCHContainer ncch(path);
    const auto status = ncch.LoadHeader();
    switch (status) {
    case Loader::ResultStatus::Success:
        game.program_id = ncch.ncch_header.program_id;
        game.state = game.program_id == MH4U_USA ? Game::State::Ok : Game::State::OtherGame;
        break;
    case Loader::ResultStatus::ErrorEncrypted:
        // the header itself is readable: it says which game it is
        game.program_id = ncch.ncch_header.program_id;
        game.state = Game::State::Encrypted;
        break;
    default:
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
                           game.program_id == MH4U_USA ? "copy of Monster Hunter 4 Ultimate"
                                                       : "game file");
    case State::OtherGame:
        return fmt::format("This is another 3DS game (program id {:016X}), not Monster Hunter 4 "
                           "Ultimate for the USA ({:016X}), the version the port is made for.",
                           game.program_id, MH4U_USA);
    case State::Ok:
        return "Monster Hunter 4 Ultimate (USA), decrypted.";
    }
    return "";
}

Update CheckUpdate() {
    Update update;
    const auto media = Service::FS::MediaType::SDMC;
    FileSys::TitleMetadata tmd;
    if (tmd.Load(Service::AM::GetTitleMetadataPath(media, MH4U_UPDATE)) !=
        Loader::ResultStatus::Success) {
        return update;
    }
    update.version = tmd.GetTitleVersion();
    update.content = Service::AM::GetTitleContentPath(media, MH4U_UPDATE, 0);
    if (update.content.empty() || !FileUtil::Exists(update.content)) {
        update.content.clear();
        return update;
    }
    if (update.version == UPDATE_VERSION) {
        update.state = Update::State::Ok;
        return update;
    }
    // another label (Azahar's installer keeps the .cia's, and some decryption tools reset it on a
    // v1.3.0 update): the installed code says which update it is
    const auto code = InstalledCode(update.content);
    const auto verdict = UpdateCode::Judge(update.version, code);
    update.state = verdict == UpdateCode::Verdict::Relabeled ? Update::State::Ok : Update::State::OtherVersion;
    update.note = UpdateCode::InstalledText(verdict, update.version, code.error);
    LOG_INFO(Frontend, "launcher: the update: {}", update.note);
    return update;
}

std::string VersionText(u16 version) {
    return UpdateCode::VersionText(version);
}

UpdateCia CheckUpdateCia(const std::string& path) {
    UpdateCia result;
    bool compressed = false;
    switch (Service::AM::CheckCIAToInstall(path, compressed, true)) {
    case Service::AM::InstallStatus::Success:
        break;
    case Service::AM::InstallStatus::ErrorEncrypted:
        result.problem = "This .cia is encrypted. The port needs a decrypted one (GodMode9 can build one "
                         "from the update installed on your 3DS).";
        return result;
    case Service::AM::InstallStatus::ErrorFileNotFound:
    case Service::AM::InstallStatus::ErrorFailedToOpenFile:
        result.problem = "The file can't be read.";
        return result;
    default:
        result.problem = "This file isn't a CIA the app can read.";
        return result;
    }
    if (const auto infos = Service::AM::GetCIAInfos(path)) {
        const auto& info = infos->first;
        if (info.tid != MH4U_UPDATE) {
            result.problem = fmt::format("This .cia is title {:016X}, not Monster Hunter 4 Ultimate's "
                                         "update ({:016X}).",
                                         info.tid, MH4U_UPDATE);
            return result;
        }
        if (info.version != UPDATE_VERSION) {
            // another label: some decryption tools reset it on a v1.3.0 update, so the code says
            // which update it is
            const auto code = CiaCode(path, compressed);
            const auto verdict = UpdateCode::Judge(info.version, code);
            auto& text = verdict == UpdateCode::Verdict::Relabeled ? result.note : result.problem;
            text = UpdateCode::CiaText(verdict, info.version, code.error);
            LOG_INFO(Frontend, "launcher: {}: {}", path, text);
        }
    }
    return result;
}

std::string LibraryPath() {
#if defined(_WIN32)
    constexpr const char* ext = "dll";
#elif defined(__APPLE__)
    constexpr const char* ext = "dylib";
#else
    constexpr const char* ext = "so";
#endif
    return fmt::format("{}recomp/{:016X}.{}", FileUtil::GetUserPath(FileUtil::UserPath::UserDir),
                       MH4U_USA, ext);
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
            Service::AM::InstallCIA(path, [this](std::size_t done, std::size_t total) {
                if (total > 0) {
                    progress = static_cast<float>(static_cast<double>(done) / static_cast<double>(total));
                }
            });
        switch (status) {
        case Service::AM::InstallStatus::Success:
            break;
        case Service::AM::InstallStatus::ErrorEncrypted:
            error = "The .cia is encrypted: the port needs a decrypted one.";
            break;
        case Service::AM::InstallStatus::ErrorFileNotFound:
        case Service::AM::InstallStatus::ErrorFailedToOpenFile:
            error = "The .cia can't be read.";
            break;
        case Service::AM::InstallStatus::ErrorAborted:
            error = "Installing stopped half-way (is the disk full?). The log has the details.";
            break;
        default:
            error = "This file isn't a CIA that can be installed.";
            break;
        }
        LOG_INFO(Frontend, "installing {}: {}", path, error.empty() ? "done" : error);
        progress = 1.0f;
        running = false;
    });
}

} // namespace Launcher::Checks
