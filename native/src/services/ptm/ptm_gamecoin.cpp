// Derived from Azahar src/core/hle/service/ptm/ptm.cpp and src/core/hle/service/ptm/ptm.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: both say "GPLv2 or any later version"): the
// play coin file that the PTM module makes when the system starts (Module::Module, WriteGameCoinData):
// the SharedExtSaveData archive 0xF000000B, its /gamecoin.dat, the layout of GameCoin and the default
// values (default_game_coin).
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (ptm.cpp, ptm.h)
//
// The play coin file of the user folder (N1-12b). Azahar's PTM module opens the SharedExtSaveData archive
// 0xF000000B on the NAND folder when it is made (system start-up, before the program runs); if the archive
// is not formatted it formats it as an extdata, creates /gamecoin.dat of sizeof(GameCoin) bytes and writes
// the default coins in it. The game opens that archive through fs:USER later, so a folder that never
// started under Azahar must hold it before the first request: the boot step below makes it, at the same
// point (a services step that runs before any guest code or replayed request).
//
// It reuses the file system module's own archive code (the factory of archive 7, its FormatAsExtData and
// its file backends), so the folders, the metadata file and the durable writes (D42, D45) are the ones a
// request of the game makes. The module that owns the step is ptm, as in Azahar; it depends on fs for the
// factory only (the other way round, fs would have to know the coin file).
//
// Departures from Azahar:
//  - Nothing is made without --user, or when the user folder is not an existing folder: the step never makes
//    the user folder itself (Azahar creates it); the archives then answer as an empty disk.
//  - Azahar's start-up also runs in the replay tool's way in (Runtime::Prepare), and so does this step:
//    the replay's user folder is the place where the reference's start-up result has to appear.
//  - Azahar asserts when the archive exists but cannot be opened (an extdata folder without `user/`); here
//    the step logs it and leaves the folder alone.
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../fs/fs_state.h"
#include "../fs/fs_types.h"
#include "../fs/host_files.h"
#include "../fs/save_archives.h"
#include "mh4n/log.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"
#include "mh4n/service.h"

namespace mh4n {

namespace {

constexpr const char* kLog = "Service.PTM";

// ptm_shared_extdata_id (ptm.h): media type 0 (NAND), save id low 0xF000000B, save id high 0. The path here has
// the high word 0x48000 that the SharedExtSaveData factory puts in every id (and the game sends it so), because
// the icon of the format is written under the id as given: with 0 it would not find its folder and the host
// layer would log a failure.
constexpr u32 kPtmSharedExtDataLow = 0xF000000B;
constexpr u32 kSharedExtDataHigh = 0x48000;

// GameCoin (ptm.h), as it is in the file: 20 bytes, no padding.
struct GameCoin {
    u32 magic;
    u16 total_coins;
    u16 total_coins_on_date;
    u32 step_count;
    u32 last_step_count;
    u16 year;
    u8 month;
    u8 day;
};
static_assert(sizeof(GameCoin) == 20);

// default_game_coin (ptm.cpp).
constexpr GameCoin kDefaultGameCoin = {0x4F00, 42, 0, 0, 0, 2014, 12, 29};

fs::Path CharPath(const std::string& text) {
    fs::Path path;
    path.raw_type = static_cast<u32>(fs::LowPathType::Char);
    path.data.assign(text.begin(), text.end());
    path.data.push_back(0);
    return path;
}

// The bytes of the file on a little-endian host, as Azahar writes the structure.
std::array<u8, sizeof(GameCoin)> DefaultBytes() {
    std::array<u8, sizeof(GameCoin)> bytes{};
    std::memcpy(bytes.data(), &kDefaultGameCoin, sizeof(GameCoin));
    return bytes;
}

// WriteGameCoinData, for a folder where the archive was not formatted.
void MakeGameCoinArchive(fs::ArchiveFactory& factory, const fs::Path& archive_path) {
    const fs::ArchiveFormatInfo format_info;  // all zero, as ArchiveFormatInfo() in Azahar
    // no icon, as Azahar
    const Result formatted = factory.FormatAsExtData(archive_path, format_info, std::nullopt);
    if (formatted != kResultSuccess) {
        MH4N_LOG(Error, kLog, "the play coin archive could not be formatted: 0x{:08X}", formatted);
        return;
    }
    std::unique_ptr<fs::ArchiveBackend> archive;
    const Result opened = factory.Open(archive_path, 0, &archive);
    if (opened != kResultSuccess || archive == nullptr) {
        MH4N_LOG(Error, kLog, "the play coin archive does not open after its format: 0x{:08X}", opened);
        return;
    }
    const fs::Path file_path = CharPath("/gamecoin.dat");
    const Result created = archive->FileCreate(file_path, sizeof(GameCoin), 0);
    if (created != kResultSuccess) {
        MH4N_LOG(Error, kLog, "gamecoin.dat could not be created: 0x{:08X}", created);
        return;
    }
    std::shared_ptr<fs::FileBackend> file;
    constexpr u32 kWriteFlag = 2;  // Mode::write_flag
    const Result file_opened = archive->OpenFile(file_path, kWriteFlag, 0, &file);
    if (file_opened != kResultSuccess || file == nullptr) {
        MH4N_LOG(Error, kLog, "gamecoin.dat could not be opened: 0x{:08X}", file_opened);
        return;
    }
    const std::array<u8, sizeof(GameCoin)> bytes = DefaultBytes();
    std::size_t written = 0;
    // flush: the bytes are made durable when the write returns (D42).
    file->Write(0, bytes.size(), true, false, bytes.data(), &written);
    file->Close();
    // The names of the new file are made durable too (the archive's tree).
    archive->Control(0, nullptr, 0, nullptr, 0);
    if (written != bytes.size()) {
        MH4N_LOG(Error, kLog, "gamecoin.dat was written for 0x{:X} of 0x{:X} bytes", written, bytes.size());
        return;
    }
    MH4N_LOG(Info, kLog, "play coin file made: SharedExtSaveData 0x{:08X}, /gamecoin.dat ({} bytes)",
             kPtmSharedExtDataLow, bytes.size());
}

// Module::Module of Azahar's PTM: the archive is opened, and only if it is not formatted is it made.
void StartPtm(Runtime& rt) {
    ServiceManager* manager = FindServiceManager(rt);
    if (manager == nullptr) {
        MH4N_LOG(Error, kLog, "the ptm module needs the service manager");
        return;
    }
    const std::shared_ptr<fs::FsState> state = fs::GetFsState(manager->Context());
    if (state->user_folder.empty()) {
        MH4N_LOG(Info, kLog, "no user folder (--user): the play coin file is not made");
        return;
    }
    if (!fs::host::IsDirectory(state->user_folder)) {
        MH4N_LOG(Info, kLog, "the user folder {} is not an existing folder: the play coin file is not made",
                 state->user_folder);
        return;
    }
    fs::ArchiveFactory* factory = state->archives.Factory(fs::archive_id::kSharedExtSaveData);
    if (factory == nullptr) {
        MH4N_LOG(Error, kLog, "there is no SharedExtSaveData archive: the play coin file is not made");
        return;
    }
    const fs::Path archive_path =
        fs::ConstructExtDataBinaryPath(fs::kMediaNand, kSharedExtDataHigh, kPtmSharedExtDataLow);
    std::unique_ptr<fs::ArchiveBackend> archive;
    const Result result = factory->Open(archive_path, 0, &archive);
    if (result == fs::kResultNotFormatted) {
        MakeGameCoinArchive(*factory, archive_path);
    } else if (result != kResultSuccess) {
        MH4N_LOG(Error, kLog, "the play coin archive does not open: 0x{:08X}; it is left as it is", result);
    }
    // An archive that opens is left alone, with or without gamecoin.dat (Azahar's constructor does nothing).
}

MH4N_REGISTER(BootStep, "services.ipc.ptm", BootStep{Phase::Services, StartPtm});

}  // namespace

}  // namespace mh4n
