// Derived from Azahar src/core/hle/service/cfg/cfg.cpp, src/core/hle/service/cfg/cfg.h and
// src/core/hle/service/cfg/cfg_defaults.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later: each of them says "GPLv2 or any later version"): the config savegame (its header
// and block entries, the 0x8000-byte file, where it lives in the user folder, how a block is found, read,
// written and created, the access flags, the result codes), the default blocks and their values, the
// console unique ID (GenerateConsoleUniqueId, SetConsoleUniqueId), the MCU data file and the MAC address
// file.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (cfg.cpp, cfg.h)
// Copyright 2023-2025 Citra Emulator Project / Azahar Emulator Project (cfg_defaults.cpp)
//
// The config savegame of the console (N1-15), in Azahar's file format and place, so one user folder
// works with Azahar and with the native runner:
//
//   <user>/nand/data/00000000000000000000000000000000/sysdata/00010017/00000000/config   (0x8000 bytes)
//   <user>/sysdata/mcu.dat                                                               (UUID clock sequence)
//   <user>/sysdata/mac.txt                                                               (the console's MAC)
//
// Departures from Azahar, all so that a read never touches the disk:
//  - Azahar's Module constructor formats and writes the file when there is none (with a random console
//    unique ID), writes the EULA version it changes, and writes the file again when a block that a
//    program asks for is missing. Here the blocks exist in memory and a read never writes: the file is
//    written only by Save() (the cfg:s/cfg:i commands that save, and `--new-console-id`) and by the
//    start-up step of the runner (EnsureConfigFile, D37), which makes it once, with a random console
//    unique ID, in a user folder that exists and holds none. The replay tool (scope ipc-replay,
//    Runtime::Prepare) never runs that step: a replay takes the folder of its recording, which holds
//    a config, and must stay repeatable. Without --user, or when the folder does not exist, nothing is
//    written and the console unique ID blocks hold the default table's value, 0 (Azahar's table says
//    they are placeholders that its formatting replaces with a random value).
//  - A config file that is short or damaged is never replaced by the start-up step or by
//    `--new-console-id` (Azahar reads it without checking): an Error is logged and the blocks are
//    read as they are, the missing ones from the defaults in memory.
//  - Formatting adds the blocks in ascending block id order (Azahar iterates an unordered_map). Readers
//    find a block by its id, so the files are interchangeable; the data offsets differ.
//  - Create (a block added to the file) refuses a block whose data would pass the end of the 0x8000-byte
//    file; Azahar writes it without that check.
//  - The file is replaced by writing a temporary file next to it and renaming it.
#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "mh4n/kernel.h"
#include "mh4n/types.h"

namespace mh4n::cfg {

// Azahar's ConfigBlockID, the ones used here.
enum BlockId : u32 {
    kBlockUserTimeOffset = 0x00030001,
    kBlockBacklightControls = 0x00050001,
    kBlockStereoCameraSettings = 0x00050005,
    kBlockBacklightControlNew3ds = 0x00050009,
    kBlockSoundOutputMode = 0x00070001,
    kBlockConsoleUniqueId1 = 0x00090000,
    kBlockConsoleUniqueId2 = 0x00090001,
    kBlockConsoleUniqueId3 = 0x00090002,
    kBlockUsername = 0x000A0000,
    kBlockBirthday = 0x000A0001,
    kBlockLanguage = 0x000A0002,
    kBlockCountryInfo = 0x000B0000,
    kBlockCountryName = 0x000B0001,
    kBlockStateName = 0x000B0002,
    kBlockLatitudeLongitude = 0x000B0003,
    kBlockRestrictedPhotoExchange = 0x000C0000,
    kBlockCoppacsRestriction = 0x000C0001,
    kBlockParentalRestrictionEmail = 0x000C0002,
    kBlockEulaVersion = 0x000D0000,
    kBlockUnknown000E0000 = 0x000E0000,
    kBlockConsoleModel = 0x000F0004,
    kBlockXDeviceToken = 0x000F0006,
    kBlockSystemSetupRequired = 0x00110000,
    kBlockDebugMode = 0x00130000,
    kBlockClockSequence = 0x00150000,
    kBlockServerType = 0x00150002,
    kBlockUnknown00160000 = 0x00160000,
    kBlockMiiverseAccessKey = 0x00170000,
};

// Azahar's AccessFlag.
enum Access : u16 {
    kUserRead = 1 << 1,
    kSystemWrite = 1 << 2,
    kSystemRead = 1 << 3,
    kSystem = kSystemRead | kSystemWrite,
    kGlobal = kUserRead | kSystemRead | kSystemWrite,
};

enum SystemModel : u8 {
    kNintendo3ds = 0,
    kNintendo3dsXl = 1,
    kNew3ds = 2,
    kNintendo2ds = 3,
    kNew3dsXl = 4,
    kNew2dsXl = 5,
};

enum SystemLanguage : u8 {
    kLanguageJp = 0,
    kLanguageEn = 1,
    kLanguageFr = 2,
    kLanguageDe = 3,
    kLanguageIt = 4,
    kLanguageEs = 5,
    kLanguageZh = 6,
    kLanguageKo = 7,
    kLanguageNl = 8,
    kLanguagePt = 9,
    kLanguageRu = 10,
    kLanguageTw = 11,
};

inline constexpr u32 kSaveFileSize = 0x8000;
inline constexpr u32 kMaxBlockEntries = 1479;
inline constexpr u32 kDataEntriesOffset = 0x455C;

// Result codes (Azahar: ErrorModule::Config = 64, ErrorLevel::Permanent = 27, WrongArgument = 8).
inline constexpr Result kResultNotFound = MakeResult(1018, 64, 8, 27);       // 0xD90103FA
inline constexpr Result kResultNotAuthorized = MakeResult(1002, 64, 8, 27);  // 0xD90103EA
inline constexpr Result kResultInvalidSize = MakeResult(1004, 64, 8, 27);    // 0xD90103EC
inline constexpr Result kResultUnknown = 0xFFFFFFFF;                         // Azahar's ResultUnknown
static_assert(kResultNotFound == 0xD90103FA);
static_assert(kResultNotAuthorized == 0xD90103EA);
static_assert(kResultInvalidSize == 0xD90103EC);

struct DefaultBlock {
    u32 id;
    u16 access;
    std::vector<u8> data;
};
// Azahar's default blocks (cfg_defaults.cpp), in ascending block id order.
const std::vector<DefaultBlock>& DefaultBlocks();

// 187 country codes (two characters in a u16, 0 for none) and the default state of each country.
extern const std::array<u16, 187> kCountryCodes;
extern const std::array<u8, 187> kDefaultSubregion;

// Region numbers: 0 JPN, 1 USA, 2 EUR, 3 AUS, 4 CHN, 5 KOR, 6 TWN.
bool IsValidRegionCountry(u32 region, u8 country_code);

// The place of the config savegame file and of the other two, in a user folder.
std::string ConfigPath(const std::string& user_folder);
std::string McuPath(const std::string& user_folder);
std::string MacPath(const std::string& user_folder);

// The regions a game is for, in the order of its icon's region lockout bits (Azahar's
// AppLoader_NCCH::ParseRegionLockoutInfo): `<game folder>/icon.bin` or `<game folder>/exefs/icon.bin`,
// the SMDH of the game. Empty when there is no game folder or no icon.
std::vector<u32> RegionsFromIcon(const std::string& game_folder);
// Azahar's AdjustLanguageInfoBlock: the (region, language) among `regions` (not empty) that fits
// `language`, else the first region and its first language.
std::pair<u32, u8> AdjustLanguage(const std::vector<u32>& regions, u8 language);

// The start-up step's outcome (D37).
enum class EnsureResult {
    NoUserFolder,  // --user not given, or the folder does not exist: nothing written (never created)
    Existing,      // a valid config is there: untouched
    Created,       // there was none: written once, with a new random console unique ID
    Invalid,       // there is a file that is short or damaged (or not a file): untouched, Error logged
    Failed,        // there was none and it could not be written: Error logged
};
// True when `path`'s content is a config savegame this module can use: 0x8000 bytes, at most 1479
// entries, data offset 0x455C, and every entry's data inside the file.
bool IsValidConfigFile(const std::string& path);
// Makes the config savegame of an existing user folder that holds none: the default blocks and a console
// unique ID from `random64` (Azahar's GenerateConsoleUniqueId), written atomically in Azahar's path. The
// folder itself is never created. `*console_id` receives the new ID when the result is Created.
EnsureResult EnsureConfigFile(const std::string& user_folder, const std::function<u64()>& random64,
                              u64* console_id = nullptr);

class ConfigStore {
public:
    // Reads the config savegame of `user_folder`. Without a readable file, the default blocks (in
    // memory; `FromFile()` is false). Never writes. Applies Azahar's EULA version fix-up in memory.
    explicit ConfigStore(const std::string& user_folder);

    bool FromFile() const { return from_file_; }
    const std::string& UserFolder() const { return user_folder_; }
    const std::array<u8, kSaveFileSize>& Raw() const { return buffer_; }

    // Azahar's GetConfigBlock / SetConfigBlock: the block's size must be exactly `size` and its access
    // flags must share a bit with `access`. A block missing from the table that has a default is made
    // (in memory) first.
    Result Get(u32 block_id, u32 size, u16 access, void* out);
    Result Set(u32 block_id, u32 size, u16 access, const void* in);

    // Azahar's FormatConfig without the file: all blocks back to their defaults (a random console ID is
    // not made: see the top).
    Result Format();
    // Writes the 0x8000 bytes to the config file, creating the folders. False (and a log) on failure.
    bool Save();

    // The console unique ID: GenerateConsoleUniqueId takes its randomness from `random64` (called
    // twice), SetConsoleUniqueId writes the three blocks.
    static std::pair<u32, u64> GenerateConsoleUniqueId(const std::function<u64()>& random64);
    Result SetConsoleUniqueId(u32 random_number, u64 console_id);
    u64 GetConsoleUniqueId();

    // Typed reads the services and other modules need (SystemRead; the defaults on an error).
    std::u16string GetUsername();
    u8 GetLanguageRaw();
    void SetLanguage(u8 language);

private:
    Result Locate(u32 block_id, u32 size, u16 access, u8** pointer);
    Result Create(u32 block_id, u16 size, u16 access, const void* data);

    std::string user_folder_;
    std::array<u8, kSaveFileSize> buffer_{};
    bool from_file_ = false;
};

// The 0x4455434D "MCUD" file (Azahar's MCUData): the UUID clock sequence the cfg:s commands keep.
struct McuData {
    static constexpr u32 kMagic = 0x4455434D;
    static constexpr u32 kVersion = 1;
    u32 magic = kMagic;
    u32 version = kVersion;
    u64 reserved = 0;
    u32 clock_sequence = 0;
};
static_assert(sizeof(McuData) == 0x18);
McuData ReadMcuData(const std::string& user_folder);
bool WriteMcuData(const std::string& user_folder, const McuData& data);

// The console's MAC address as "AA:BB:CC:DD:EE:FF": the contents of mac.txt when it holds one, else a
// random one in Azahar's ranges, which is written to mac.txt so the console keeps it.
std::string ReadMacAddress(const std::string& user_folder, const std::function<u64()>& random64);
std::string MacToString(u64 mac);
// 6 bytes of a well-formed address; false if `text` is not one.
bool ParseMac(const std::string& text, std::array<u8, 6>* mac);

}  // namespace mh4n::cfg
