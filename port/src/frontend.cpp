#include "frontend.h"

#include <cstdlib>
#include <memory>
#include <vector>
#include <fmt/format.h>

#include "arm_recomp.h"
#include "hooks.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/file_sys/ncch_container.h"
#include "core/hle/service/am/am.h"
#include "core/loader/loader.h"

namespace Port {

namespace {

constexpr u64 MH4U_USA = 0x0004000000126300;
std::string notice;

constexpr const char* BUILD_HINT =
    "Build it from your own copy of the v1.3.0 update with \"Build code library\" next to "
    "azahar.exe on Windows, or tools/build_game.py (docs/PLAYING.md in the port's repository).";

/// The code Azahar's loader will put in memory for the game at `rom_path`: the installed
/// update's when there is one, else the game's own, with the code mods in Azahar's mods folder
/// applied (as Loader::AppLoader_NCCH does). Empty if it can't be read.
std::vector<u8> CodeToBeLoaded(const std::string& rom_path, u64 program_id) {
    constexpr u64 UPDATE_TID_HIGH = 0x0004000e00000000;
    FileSys::NCCHContainer update(Service::AM::GetTitleContentPath(
        Service::FS::MediaType::SDMC, (program_id & 0xFFFFFFFF) | UPDATE_TID_HIGH));
    FileSys::NCCHContainer base(rom_path);
    FileSys::NCCHContainer* ncch = update.Load() == Loader::ResultStatus::Success ? &update : &base;
    std::vector<u8> code;
    if (ncch->Load() != Loader::ResultStatus::Success ||
        ncch->LoadSectionExeFS(".code", code) != Loader::ResultStatus::Success) {
        return {};
    }
    const auto patched = ncch->ApplyCodePatch(code);
    if (patched != Loader::ResultStatus::Success && patched != Loader::ResultStatus::ErrorNotUsed) {
        return {};
    }
    return code;
}

} // namespace

std::string BackendNotice() {
    return notice;
}

void ToggleQuestMap() {
    Hooks::ToggleQuestMap();
}

std::string OptionsPath() {
    return FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "mh4u.ini";
}

std::string OptionsTemplate() {
    return R"(# MH4U PC port options. Changes apply the next time the game starts.
# One "key = value" per line; a line starting with # is ignored. All of them, and what was
# tried: docs/PLAYING.md in the port's repository.

# frame-rate cap (the game runs at 30 on a 3DS; its speed stays right at any rate), e.g. your
# monitor's refresh rate; above 60 a busy scene may need Emulation > Configure > Debug > CPU
# Clock Speed raised
fps = 60

# the top screen's shape: 16:9, 16:10, 21:9, 32:9, 4:3 or off. wide_render = 1 draws the
# wider picture itself (0 stretches a 400x240 one); hud_edges = 1 moves the hunting HUD's
# sides (health, stamina, party list; the item selection) out to the screen's edges
widescreen = 16:9
wide_render = 1
hud_edges = 1

# View > Screen Layout > Custom Layout: the game fills the window and the touch screen sits
# in a corner (top-left, top-right, bottom-left, bottom-right), its height in % of the
# window's, its opacity in %. off = Azahar's own custom layout
touch_overlay = top-right 30 85
# in quests that corner shows only the map (map 40 = 40% of the window's height; off = the
# whole touch screen); F12 switches between the two while you play
quest_overlay = map

# buttons for the kick and the target camera, touch-screen panels on a 3DS
# (A B X Y L R ZL ZR START UP DOWN LEFT RIGHT; ZL and ZR are a controller's triggers)
kick = ZR
target = ZL

# the game's dither pattern: off for a clean picture at an Internal Resolution above 1x
dither = off

# multiplayer: wait a moment for a player with the port whose PC stalls, instead of the game
# dropping them (on, off; all = with every port player in the room)
net_sync = on
)";
}

std::string PrepareBackendFor(Core::System& system, const std::string& rom_path) {
    system.SetCPUFactory(nullptr);
    notice.clear();

    u64 program_id = 0;
    if (const auto loader = Loader::GetLoader(rom_path)) {
        if (loader->ReadProgramId(program_id) != Loader::ResultStatus::Success) {
            program_id = 0;
        }
    }

    std::string library;
    if (const char* env = std::getenv("MH4U_CODE"); env && *env) {
        library = env;
    } else if (program_id != 0) {
#if defined(_WIN32)
        constexpr const char* ext = "dll";
#elif defined(__APPLE__)
        constexpr const char* ext = "dylib";
#else
        constexpr const char* ext = "so";
#endif
        library = fmt::format("{}recomp/{:016X}.{}", FileUtil::GetUserPath(FileUtil::UserPath::UserDir),
                              program_id, ext);
        if (!FileUtil::Exists(library)) {
            if (program_id == MH4U_USA) {
                notice = fmt::format("The MH4U PC port's code library isn't installed ({}), so the game "
                                     "runs on Azahar's own CPU emulation, without the port's options "
                                     "(frame rate, widescreen, touch screen overlay, net sync).\n\n{}",
                                     library, BUILD_HINT);
            }
            return fmt::format("no recompiled code for {:016X}, using the emulated CPU", program_id);
        }
    } else {
        return "no recompiled code, using the emulated CPU";
    }

    std::string error;
    if (!ARM_Recomp::LoadCode(library, error)) {
        if (program_id == MH4U_USA) {
            notice = fmt::format("The MH4U PC port's code library couldn't be used ({}), so the game "
                                 "runs on Azahar's own CPU emulation, without the port's options. A "
                                 "library made for an older version of this Azahar has to be built "
                                 "again.\n\n{}",
                                 error, BUILD_HINT);
        }
        return fmt::format("{}; using the emulated CPU", error);
    }
    const u64 code_title = ARM_Recomp::CodeTitleId();
    if (code_title != 0 && program_id != 0 && code_title != program_id) {
        return fmt::format("{} is for {:016X}, not {:016X}; using the emulated CPU", library,
                           code_title, program_id);
    }
    // The recompiled code only runs the code it came from; any other (the game without its
    // update, a code mod added since) would run entirely in the interpreter, far too slowly.
    // Azahar's own CPU emulation plays it at full speed instead.
    if (const auto code = program_id != 0 ? CodeToBeLoaded(rom_path, program_id) : std::vector<u8>{};
        !code.empty() && !ARM_Recomp::IsRecompiledCode(code)) {
        if (program_id == MH4U_USA) {
            notice = fmt::format(
                "The game's code isn't the code the MH4U PC port's library was built from, so the "
                "game runs on Azahar's own CPU emulation, without the port's options. Either the "
                "v1.3.0 update isn't installed in Azahar (File > Install CIA... with your update), or "
                "a code mod was added, changed or removed since the library was built.\n\n{}",
                BUILD_HINT);
        }
        return "the program's code isn't the code that was recompiled; using the emulated CPU";
    }
    if (const int missing = Hooks::MissingHookPoints(); missing > 0 && program_id == MH4U_USA) {
        notice = fmt::format("The MH4U PC port's code library is older than this Azahar: {} of the "
                             "port's hook points are missing, so some of its options won't work "
                             "until the library is built again.\n\n{}",
                             missing, BUILD_HINT);
    }
    system.SetCPUFactory([](Core::System& sys, Memory::MemorySystem& memory, u32 core_id,
                            std::shared_ptr<Core::Timing::Timer> timer)
                             -> std::shared_ptr<Core::ARM_Interface> {
        return std::make_shared<ARM_Recomp>(sys, memory, core_id, std::move(timer));
    });
    return fmt::format("running recompiled code from {} ({} functions)", library,
                       ARM_Recomp::FunctionCount());
}

} // namespace Port
