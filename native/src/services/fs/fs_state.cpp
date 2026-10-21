// Derived from Azahar src/core/hle/service/fs/archive.cpp, src/core/hle/service/fs/archive.h,
// src/core/file_sys/ncch_container.cpp and src/common/settings.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: archive.h and ncch_container.cpp say
// "GPLv3 or any later version"; archive.cpp and settings.h say "GPLv2 or any later version"): the
// archive manager (OpenArchive: an unregistered id is ResultNotFound; handles numbered from 1 and
// skipping the ones in use; CloseArchive of an unknown handle is ResultInvalidArchiveHandle) and where the
// readable RomFS starts in a NCCH section (NCCHContainer::ReadRomFS: 0x1000 bytes in).
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (archive.cpp, archive.h, settings.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (ncch_container.cpp)
#include <filesystem>
#include <fstream>
#include <vector>

#include "fs_state.h"
#include "mh4n/loader.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"

namespace mh4n::fs {

namespace {
constexpr const char* kLog = "Service.FS";

std::shared_ptr<PositionedFile> OpenGameFile(const std::string& folder, const char* name) {
    if (folder.empty()) {
        return nullptr;
    }
    const std::string path = (std::filesystem::path(folder) / name).string();
    std::error_code ignored;
    if (!std::filesystem::exists(path, ignored)) {
        return nullptr;
    }
    std::string error;
    std::shared_ptr<PositionedFile> file = PositionedFile::Open(path, &error);
    if (file == nullptr) {
        MH4N_LOG(Warning, kLog, "{}", error);
    }
    return file;
}

u64 ReadProgramId(const std::string& folder) {
    if (folder.empty()) {
        return 0;
    }
    const std::filesystem::path path = std::filesystem::path(folder) / "exheader.bin";
    std::error_code ignored;
    if (!std::filesystem::is_regular_file(path, ignored)) {
        return 0;
    }
    std::vector<u8> bytes(kExHeaderMinSize);
    std::ifstream in(path, std::ios::binary);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<std::size_t>(in.gcount()) != bytes.size()) {
        return 0;
    }
    ExHeader header;
    std::string error;
    return ParseExHeader(bytes.data(), bytes.size(), &header, &error) ? header.program_id : 0;
}
}  // namespace

// ---- the archive manager -------------------------------------------------------------------------------

bool ArchiveManager::Register(u32 id_code, std::unique_ptr<ArchiveFactory> factory) {
    return factories_.emplace(id_code, std::move(factory)).second;
}

Result ArchiveManager::Open(u32 id_code, const Path& path, u64 program_id, ArchiveHandle* handle) {
    const auto factory = factories_.find(id_code);
    if (factory == factories_.end()) {
        return kResultNotFound;
    }
    std::unique_ptr<ArchiveBackend> archive;
    const Result result = factory->second->Open(path, program_id, &archive);
    if (IsError(result)) {
        return result;
    }
    while (open_.count(next_handle_) != 0) {
        ++next_handle_;
    }
    *handle = next_handle_;
    open_.emplace(next_handle_, std::move(archive));
    ++next_handle_;
    return kResultSuccess;
}

Result ArchiveManager::Close(ArchiveHandle handle) {
    const auto found = open_.find(handle);
    if (found == open_.end()) {
        return kResultInvalidArchiveHandle;
    }
    open_.erase(found);
    return kResultSuccess;
}

ArchiveBackend* ArchiveManager::Get(ArchiveHandle handle) {
    const auto found = open_.find(handle);
    return found == open_.end() ? nullptr : found->second.get();
}

// ---- the state --------------------------------------------------------------------------------------------

void FsState::Init(ServiceContext& context) {
    initialized = true;
    const std::string text = context.GetOptions().GetString("--io-latency", "azahar");
    if (!ParseIoLatency(text, &latency)) {
        // The boot step of options.cpp refuses this before the program runs.
        MH4N_LOG(Error, kLog, "--io-latency {} is not azahar or none; using azahar", text);
        latency = IoLatency::Azahar;
    }
    MakeIoPool(*this, context.GetKernel());

    const std::string& folder = context.GameFolder();
    game.romfs = OpenGameFile(folder, "romfs.bin");
    game.update_romfs = OpenGameFile(folder, "update_romfs.bin");
    game.code = OpenGameFile(folder, "code.bin");
    game.program_id = ReadProgramId(folder);
    MH4N_LOG(Info, kLog, "io latency {}; game folder {}: romfs {}, update romfs {}",
             latency == IoLatency::Azahar ? "azahar" : "none", folder.empty() ? "(none)" : "set",
             game.romfs ? "yes" : "no", game.update_romfs ? "yes" : "no");

    for (const auto& [name, provider] : Registry<ArchiveProvider>::Instance().All()) {
        if (!archives.Register(provider.id_code, provider.make(*this))) {
            MH4N_LOG(Error, kLog, "archive id 0x{:08X} is registered twice ({})", provider.id_code, name);
        }
    }
}

std::shared_ptr<FsState> GetFsState(ServiceContext& context) {
    std::shared_ptr<FsState> state = context.State<FsState>("fs:USER");
    if (!state->initialized) {
        state->Init(context);
    }
    return state;
}

}  // namespace mh4n::fs
