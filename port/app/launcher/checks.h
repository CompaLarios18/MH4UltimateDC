// What the launcher's setup checks: the game file (Monster Hunter 4 Ultimate USA or Europe,
// decrypted, its RomFS where its headers say), its update in Azahar's virtual SD card (USA v1.3.0,
// Europe v1.1.0; and installing it from the player's .cia), and the code library (installed, and
// built from the code the game will run).
#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "common/common_types.h"
#include "launcher/update_code.h"

namespace Launcher::Checks {

constexpr u64 MH4U_USA = 0x0004000000126300;
constexpr u64 MH4U_UPDATE = 0x0004000E00126300;
constexpr u16 UPDATE_VERSION = UpdateCode::TARGET_VERSION;  ///< v1.3.0 (config/versions.json)

/// A version the port runs (config/versions.json): the game, the update whose code it runs,
/// that update's version and its code's SHA-1. Europe's v1.1.0 is USA v1.3.0's program a few
/// bytes along (config/shifts/eur_v110.txt), so build_game.py builds its library from the same
/// tables.
struct Region {
    u64 program_id;
    u64 update_id;
    u16 update_version;
    const char* code_sha1;
    const char* name;
};
constexpr Region REGIONS[] = {
    {MH4U_USA, MH4U_UPDATE, UPDATE_VERSION, UpdateCode::TARGET_CODE_SHA1, "USA"},
    {0x0004000000126100, 0x0004000E00126100, UpdateCode::EUR_VERSION, UpdateCode::EUR_CODE_SHA1, "Europe"},
};

/// The region of a game's program id, or nullptr for another game.
const Region* FindRegion(u64 program_id);

/// The region the checks use for a game: its own, or USA's when there is no game yet.
const Region& RegionOf(u64 program_id);

struct Game {
    enum class State {
        Unset,
        Missing,
        NotA3ds,
        Encrypted,
        OtherGame,
        BadRomFs,   ///< decrypted, its flags say, but the RomFS doesn't start as a decrypted one does
        Truncated,  ///< the file ends before its RomFS does
        Ok,
    };
    State state = State::Unset;
    u64 program_id = 0;
};

/// Reads the game file's NCCH header (Azahar's NCCHContainer): which program, decrypted or not;
/// then, for MH4U, the start of its RomFS (launcher/game_file.h).
Game CheckGame(const std::string& path);

/// A sentence for the player about the game file.
std::string Describe(const Game& game);

struct Update {
    enum class State { Missing, OtherVersion, Ok };
    State state = State::Missing;
    u16 version = 0;      ///< its title metadata's version label
    std::string content;  ///< the installed update's program (an NCCH that build_game.py reads)
    /// The setup's line about it when its label isn't the update's: its code all the same (Ok),
    /// or other code, or code that couldn't be read (OtherVersion)
    std::string note;
};

/// The game's update in Azahar's virtual SD card (Service::AM's title paths). A version label
/// other than the region's (v1.3.0, v1.1.0) is judged by the installed code
/// (launcher/update_code.h): some decryption tools reset the label, and Azahar's installer keeps
/// the .cia's.
Update CheckUpdate(u64 program_id = MH4U_USA);

/// "1.3.0" for a title version.
std::string VersionText(u16 version);

/// "v1.3.0": the update the port runs for that game (USA's when there is no game yet).
std::string UpdateName(u64 program_id);

/// A .cia for the setup's step 2: installed when `problem` is empty (`note`, if any, said once
/// it is in), else not, for that reason under `title`.
struct UpdateCia {
    std::string title;
    std::string problem;
    std::string note;
};

/// Whether a .cia is the game's update (USA v1.3.0, Europe v1.1.0), decrypted; a version label
/// other than that update's is judged by the .cia's code (launcher/update_code.h).
UpdateCia CheckUpdateCia(const std::string& path, u64 program_id = MH4U_USA);

/// Where the port looks for the game's code library (recomp/<program id>.<dll|so|dylib>).
std::string LibraryPath(u64 program_id = MH4U_USA);

/// The code library as the game's start would find it, from the app's code check (CheckCode,
/// run as a process of its own so that the library isn't loaded here).
struct Code {
    enum class State {
        Unknown,   ///< not checked (no game yet)
        Missing,   ///< no library
        Checking,
        Ok,        ///< the game runs on it
        Partial,   ///< it runs, but the library is older than the app (some options missing)
        NotUsed,   ///< it won't run: notice says why (the update, a code mod, an old library)
        Failed,    ///< the check itself didn't work (notice says why)
    };
    State state = State::Unknown;
    std::string notice;
};

/// The verdict from the code check's exit status and output.
Code CodeFromCheck(int exit_code, const std::string& output);

/// Installs a CIA (the update) on a thread of its own, with its progress.
class Install {
public:
    ~Install();
    void Start(const std::string& path);
    bool Running() const {
        return running;
    }
    float Progress() const {
        return progress;
    }
    /// After the end: empty if it worked, else what went wrong.
    const std::string& Error() const {
        return error;
    }

private:
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<float> progress{0.0f};
    std::string error;
};

} // namespace Launcher::Checks
