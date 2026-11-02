#include "frontend.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
#include <fmt/format.h>
#if defined(__ANDROID__)
#include <sys/stat.h>
#include <unistd.h>
#endif

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
constexpr u64 MH4U_EUROPE = 0x0004000000126100;
std::string notice;

/// MH4U for the USA or Europe: the games the port's code library is built for (the notices below)
bool IsMh4u(u64 program_id) {
    return program_id == MH4U_USA || program_id == MH4U_EUROPE;
}

/// The update whose code the library is built from
const char* UpdateName(u64 program_id) {
    return program_id == MH4U_EUROPE ? "v1.1.0" : "v1.3.0";
}

// How to build the library: with this version's tools (one from another version's tools can be
// refused: ARM_Recomp::LoadCode).
#if defined(__ANDROID__)
constexpr const char* BUILD_HINT =
    "Build it on your PC from your own copy of the update (USA v1.3.0, Europe v1.1.0) with "
    "tools/build_game.py --android from the PC package of the same release as this APK, then copy "
    "it into Azahar's user folder on the device as recomp/<the game's program id>.so: "
    "0004000000126300.so for the USA, 0004000000126100.so for Europe (docs/PLAYING.md in the "
    "port's repository).";
#else
constexpr const char* BUILD_HINT =
    "Build it from your own copy of the update (USA v1.3.0, Europe v1.1.0) with this version's "
    "tools: the launcher's Setup, \"Build code library\" next to azahar.exe on Windows, or "
    "tools/build_game.py (docs/PLAYING.md in the port's repository).";
#endif

#if defined(__ANDROID__)
/// The app's own files folder (/data/user/<user>/<package>/files/): the system's linker loads
/// code from there, not from shared storage, where the player puts the library.
std::string PrivateDir() {
    std::string package;
    if (std::FILE* cmdline = std::fopen("/proc/self/cmdline", "rb")) {
        for (int c; (c = std::fgetc(cmdline)) != EOF && c != '\0' && c != ':';) {
            package += static_cast<char>(c);
        }
        std::fclose(cmdline);
    }
    if (package.empty()) {
        return "";
    }
    return fmt::format("/data/user/{}/{}/files/", getuid() / 100000, package);
}

/// The code library the player put in Azahar's user folder (recomp/<title>.so, as on a PC),
/// copied into the app's own folder where it can be loaded: its path there, or "" when there is
/// none (an APK built with -Pmh4uCodeLibs carries one of its own) or it couldn't be copied.
std::string InstallUserLibrary(u64 program_id, std::string& error) {
    const std::string source =
        fmt::format("{}recomp/{:016X}.so", FileUtil::GetUserPath(FileUtil::UserPath::UserDir), program_id);
    if (!FileUtil::Exists(source)) {
        return "";
    }
    std::string bytes;
    if (FileUtil::ReadFileToString(false, source, bytes) == 0 || bytes.empty()) {
        error = fmt::format("couldn't read {}", source);
        return "";
    }
    const std::string dir = PrivateDir();
    if (dir.empty()) {
        error = "couldn't find the app's own folder";
        return "";
    }
    const std::string target = dir + "mh4u_code.so";
    // the same library as last time: kept (it is tens of megabytes)
    std::string current;
    if (std::FILE* f = std::fopen(target.c_str(), "rb")) {
        current.resize(bytes.size() + 1);
        current.resize(std::fread(current.data(), 1, current.size(), f));
        std::fclose(f);
        if (current == bytes) {
            return target;
        }
    }
    const std::string temporary = target + ".new";
    std::remove(temporary.c_str());
    std::FILE* out = std::fopen(temporary.c_str(), "wb");
    if (!out) {
        error = fmt::format("couldn't write {}", temporary);
        return "";
    }
    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), out) == bytes.size();
    if (std::fclose(out) != 0 || !written) {
        std::remove(temporary.c_str());
        error = fmt::format("couldn't write {}", temporary);
        return "";
    }
    // read-only, as Android 14 wants code loaded at run time to be
    chmod(temporary.c_str(), 0400);
    std::remove(target.c_str());
    if (std::rename(temporary.c_str(), target.c_str()) != 0) {
        error = fmt::format("couldn't put the library in place at {}", target);
        return "";
    }
    LOG_INFO(Frontend, "copied {} to {}", source, target);
    return target;
}
#endif

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

void SetWindowSize(u32 width, u32 height) {
    Hooks::SetWindowSize(width, height);
}

std::string OptionsPath() {
    return FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "mh4u.ini";
}

std::string OptionsTemplate(bool match_screen) {
    std::string text = R"(# MH4U PC port options. Changes apply the next time the game starts.
# One "key = value" per line; a line starting with # is ignored. All of them, and what was
# tried: docs/PLAYING.md in the port's repository.

# frame-rate cap (the game runs at 30 on a 3DS; its speed stays right at any rate), e.g. your
# monitor's refresh rate; above 60 a busy scene may need Emulation > Configure > Debug > CPU
# Clock Speed raised
fps = 60

# the top screen's shape: 16:9, 16:10, 21:9, 32:9, 4:3, match (the window's shape: the whole
# screen on Android; callers pass their size to Port::SetWindowSize) or off.
# wide_render = 1 draws the wider picture itself (0 stretches a 400x240 one); hud_edges = 1
# moves the hunting HUD's sides (health, stamina, party list; the item selection) out to the
# screen's edges
widescreen = 16:9
wide_render = 1
hud_edges = 1

# View > Screen Layout > Custom Layout: the game fills the window and the touch screen sits
# in a corner (top-left, top-right, bottom-left, bottom-right), its height in % of the
# window's, its opacity in %. off = Azahar's own custom layout
touch_overlay = top-right 30 85
# in quests that corner shows only the map, the map panel of your panel layout (a layout
# without it keeps the whole touch screen; off = always the whole touch screen); F12 switches
# between the two while you play. map 40 = 40% of the window's height (without a size: the
# touch screen's scale); map 40 at 100 0 also places it: across and down in % of the room the
# window leaves (0 0 = top left, 100 0 = top right, 50 100 = bottom middle), so it stays in the
# window at any size. The in-game menu (Esc) > Touch screen > Edit HUD layout places it
quest_overlay = map
# with the map, the target camera panel (whose monster the camera follows; a tap on it changes
# the target) in a small view of its own, if your panel layout has it: on = under the map at
# the touch screen's scale, on 12 at 100 30 = 12% of the window's height, placed as above; off
target_overlay = on

# buttons for the kick and the target camera, touch-screen panels on a 3DS
# (A B X Y L R ZL ZR START UP DOWN LEFT RIGHT; ZL and ZR are a controller's triggers)
kick = ZR
target = ZL
# a button for the touch screen's Skip, which replaces the kick button once a quest is
# complete and skips the scene of the monster's end (the kick does nothing then, so ZR suits
# both); buttons held together for skip_end_hold seconds to end the wait before the return,
# the carving time, as if it had run out (online, once every player with the port has held
# them; the screen says who hasn't); skip_hint: lines on the screen say which keys do what
# (on, flash or off)
skip = ZR
skip_end = ZL+ZR
skip_end_hold = 1.0
skip_hint = on

# the game's dither pattern: off for a clean picture at an Internal Resolution above 1x
dither = off
# the game's stereoscopic 3D: it draws the top screen twice (one pass per eye) even with the 3D
# slider at 0, which costs a full scene pass and can show as a doubled, soft picture; off draws
# it once, as the community's "3D off" cheat does. on = as on a 3DS
stereo_3d = off
# the game's own anti-aliasing (on, as on a 3DS): off saves its pass, which at a high Internal
# Resolution costs frames for little; the edges get a little jagged
antialiasing = on

# the game's screen filters (on = as on a 3DS, off = skipped): the glow over bright parts, which
# softens the picture; the focus blur of cutscenes, which emulation gets wrong (it can blur the
# whole picture); the blur some effects blend in; the zoom blur of roars; heat shimmer
bloom = off
depth_of_field = off
motion_blur = on
radial_blur = on
haze = on

# multiplayer: wait a moment for a player with the port whose PC stalls, instead of the game
# dropping them (on, off; all = with every port player in the room)
net_sync = on
)";
    if (match_screen) {
        // the Android app's first file: fill the phone's screen instead of a fixed 16:9
        if (const auto at = text.find("widescreen = 16:9"); at != std::string::npos) {
            text.replace(at, std::string("widescreen = 16:9").size(), "widescreen = match");
        }
    }
    return text;
}

std::string PrepareBackendFor(Core::System& system, const std::string& rom_path) {
    system.SetCPUFactory(nullptr);
    notice.clear();

#if defined(__ANDROID__)
    // The desktop packages write mh4u.ini on first run (60 fps, widescreen, the touch screen
    // in a corner); do the same here, with widescreen matching the phone's screen. Written
    // before Hooks::Install reads it, so the first boot already starts with them.
    if (!FileUtil::Exists(OptionsPath())) {
        FileUtil::WriteStringToFile(true, OptionsPath(), OptionsTemplate(true));
        LOG_INFO(Frontend, "wrote the first mh4u.ini to {}", OptionsPath());
    }
#endif

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
#if defined(__ANDROID__)
        // the player's own library from Azahar's user folder (build_game.py --android), else
        // lib/arm64-v8a/libmh4u_code.so in the APK, which dlopen finds by soname in the app's
        // native library directory (-Pmh4uCodeLibs, see android/app/build.gradle.kts)
        std::string copy_error;
        library = InstallUserLibrary(program_id, copy_error);
        if (!copy_error.empty()) {
            LOG_ERROR(Frontend, "the code library in the user folder: {}", copy_error);
        }
        if (library.empty()) {
            library = "libmh4u_code.so";
        }
#else
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
            if (IsMh4u(program_id)) {
                notice = fmt::format("The MH4U PC port's code library isn't installed ({}), so the game "
                                     "runs on Azahar's own CPU emulation, without the port's options "
                                     "(frame rate, widescreen, touch screen overlay, net sync).\n\n{}",
                                     library, BUILD_HINT);
            }
            return fmt::format("no recompiled code for {:016X}, using the emulated CPU", program_id);
        }
#endif
    } else {
        return "no recompiled code, using the emulated CPU";
    }

    std::string error;
    if (!ARM_Recomp::LoadCode(library, error)) {
#if defined(__ANDROID__)
        if (IsMh4u(program_id) && library == "libmh4u_code.so") {
            notice = fmt::format("The MH4U port's code library isn't on this device, so the game runs on "
                                 "Azahar's own CPU emulation, without the port's options.\n\n{}",
                                 BUILD_HINT);
            return fmt::format("no recompiled code for {:016X} ({}), using the emulated CPU", program_id, error);
        }
#endif
        if (IsMh4u(program_id)) {
            // (the error says what to do about a library of another version: build it again)
            notice = fmt::format("The MH4U PC port's code library couldn't be used ({}), so the game "
                                 "runs on Azahar's own CPU emulation, without the port's options.\n\n{}",
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
        if (IsMh4u(program_id)) {
            notice = fmt::format(
                "The game's code isn't the code the MH4U PC port's library was built from, so the "
                "game runs on Azahar's own CPU emulation, without the port's options. Either the "
                "{} update isn't installed in Azahar (File > Install CIA... with your update), or "
                "a code mod was added, changed or removed since the library was built.\n\n{}",
                UpdateName(program_id), BUILD_HINT);
        }
        return "the program's code isn't the code that was recompiled; using the emulated CPU";
    }
    if (const int missing = Hooks::MissingHookPoints(); missing > 0 && IsMh4u(program_id)) {
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
