// Derived from Azahar src/core/hle/service/cfg/cfg.cpp, src/core/hle/service/cfg/cfg.h and
// src/core/hle/service/cfg/cfg_defaults.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later: each of them says "GPLv2 or any later version"); see cfg_store.h.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (cfg.cpp, cfg.h)
// Copyright 2023-2025 Citra Emulator Project / Azahar Emulator Project (cfg_defaults.cpp)
#include "cfg_store.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <exception>
#include <iterator>
#include <mutex>
#include <random>

#include "../../common/utf8.h"
#include "mh4n/log.h"

namespace mh4n::cfg {

namespace {

constexpr const char* kLog = "Service.CFG";

// The block header in the file (Azahar's SaveConfigBlockEntry) and the file header (SaveFileConfig):
//   u16 total_entries, u16 data_entries_offset, 1479 x { u32 id, u32 offset_or_data, u16 size,
//   u16 access }, u32 unknown = 0x455C bytes.
constexpr std::size_t kHeaderSize = 4;
constexpr std::size_t kEntrySize = 12;
static_assert(kHeaderSize + kMaxBlockEntries * kEntrySize + 4 == kDataEntriesOffset);

u16 Get16(const u8* p) {
    return static_cast<u16>(p[0] | (p[1] << 8));
}
u32 Get32(const u8* p) {
    return static_cast<u32>(p[0]) | static_cast<u32>(p[1]) << 8 | static_cast<u32>(p[2]) << 16 |
           static_cast<u32>(p[3]) << 24;
}
void Put16(u8* p, u32 v) {
    p[0] = static_cast<u8>(v), p[1] = static_cast<u8>(v >> 8);
}
void Put32(u8* p, u32 v) {
    Put16(p, v & 0xFFFF), Put16(p + 2, v >> 16);
}

u8* Entry(std::array<u8, kSaveFileSize>& buffer, u32 index) {
    return buffer.data() + kHeaderSize + static_cast<std::size_t>(index) * kEntrySize;
}

// ---- default blocks ------------------------------------------------------------------------------------------

void AppendLe(std::vector<u8>& out, u64 value, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<u8>(value >> (8 * i)));
    }
}

void AppendFloat(std::vector<u8>& out, float value) {
    u32 raw;
    std::memcpy(&raw, &value, sizeof(raw));
    AppendLe(out, raw, 4);
}

// `count` entries of 0x40 UTF-16 code units, each holding `text` (Azahar's MakeLocationNames).
std::vector<u8> LocationNames(const char16_t* text) {
    std::vector<u8> out(0x10 * 0x40 * 2, 0);
    for (std::size_t entry = 0; entry < 0x10; ++entry) {
        std::size_t at = entry * 0x40 * 2;
        for (const char16_t* c = text; *c != 0; ++c, at += 2) {
            Put16(&out[at], *c);
        }
    }
    return out;
}

std::vector<DefaultBlock> MakeDefaults() {
    std::vector<DefaultBlock> blocks;
    auto add = [&](u32 id, u16 access, std::vector<u8> data) {
        blocks.push_back({id, access, std::move(data)});
    };
    auto zeros = [](std::size_t n) { return std::vector<u8>(n, 0); };
    auto le = [](u64 value, int bytes) {
        std::vector<u8> v;
        AppendLe(v, value, bytes);
        return v;
    };

    add(kBlockUserTimeOffset, kGlobal, le(0, 8));
    add(kBlockBacklightControls, kSystem, {0, 2});
    {
        // Azahar: "TODO(Subv): Find out what this actually is, these values fix some NaN uniforms in
        // some games, for example Nintendo Zone".
        std::vector<u8> stereo;
        for (const float f : {62.0f, 289.0f, 76.80000305175781f, 46.08000183105469f, 10.0f, 5.0f,
                              55.58000183105469f, 21.56999969482422f}) {
            AppendFloat(stereo, f);
        }
        add(kBlockStereoCameraSettings, kGlobal, std::move(stereo));
    }
    add(kBlockBacklightControlNew3ds, kSystem, zeros(8));
    add(kBlockSoundOutputMode, kGlobal, {1});  // SOUND_STEREO
    // Placeholders in Azahar's table (it randomises them when it formats the file).
    add(kBlockConsoleUniqueId1, kGlobal, le(0, 8));
    add(kBlockConsoleUniqueId2, kGlobal, le(0, 8));
    add(kBlockConsoleUniqueId3, kGlobal, le(0, 4));
    {
        std::vector<u8> name(0x1C, 0);  // 10 UTF-16 units, then two zero words
        const char16_t* text = u"AZAHAR";
        for (std::size_t i = 0; text[i] != 0; ++i) {
            Put16(&name[i * 2], text[i]);
        }
        add(kBlockUsername, kGlobal, std::move(name));
    }
    add(kBlockBirthday, kGlobal, {11, 7});  // November 7th, 2024
    add(kBlockLanguage, kGlobal, {kLanguageEn});
    add(kBlockCountryInfo, kGlobal, {0, 0, 2, 49});  // Washington DC, United States
    add(kBlockCountryName, kGlobal, LocationNames(u"United States"));
    add(kBlockStateName, kGlobal, LocationNames(u"Washington DC"));
    add(kBlockLatitudeLongitude, kGlobal, le(0, 4));
    add(kBlockRestrictedPhotoExchange, kGlobal, zeros(0xC0));
    add(kBlockCoppacsRestriction, kGlobal, zeros(0x14));
    add(kBlockParentalRestrictionEmail, kGlobal, zeros(0x200));
    add(kBlockEulaVersion, kGlobal, {0x7F, 0x7F, 0, 0});
    add(kBlockUnknown000E0000, kGlobal, {0});
    add(kBlockConsoleModel, kSystem, {kNew3dsXl, 0, 0, 0});
    add(kBlockXDeviceToken, kSystem, zeros(0x28));
    add(kBlockSystemSetupRequired, kSystem, le(1, 4));
    add(kBlockDebugMode, kGlobal, le(0, 4));
    add(kBlockClockSequence, kSystem, le(0, 4));
    add(kBlockServerType, kGlobal, {'L', '1', 0, 0});
    add(kBlockUnknown00160000, kGlobal, le(0, 4));
    add(kBlockMiiverseAccessKey, kGlobal, le(0, 4));
    std::sort(blocks.begin(), blocks.end(),
              [](const DefaultBlock& a, const DefaultBlock& b) { return a.id < b.id; });
    return blocks;
}

const DefaultBlock* FindDefault(u32 id) {
    for (const DefaultBlock& block : DefaultBlocks()) {
        if (block.id == id) {
            return &block;
        }
    }
    return nullptr;
}

// The host path of a UTF-8 text (D45): every file call below goes through this, so a user or game folder
// with an accent (or any other character outside the ANSI code page of Windows) is the folder the player
// named. The paths of this file are UTF-8 text everywhere (they are logged and compared as text).
std::filesystem::path HostPath(const std::string& utf8_path) {
    return utf8::PathFromUtf8(utf8_path);
}

std::string UserPath(const std::string& user_folder, const char* relative) {
    std::string path = user_folder;
    if (!path.empty() && path.back() != '/' && path.back() != '\\') {
        path += '/';
    }
    return path + relative;
}

// Writes `bytes` to `path` through a temporary file, so a crash never leaves half a file.
bool WriteFileAtomically(const std::string& path, const void* bytes, std::size_t size) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path target = HostPath(path);
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            MH4N_LOG(Error, kLog, "cannot create the folder of {}: {}", path, ec.message());
            return false;
        }
    }
    const fs::path temporary = HostPath(path + ".tmp");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
        out.close();
        if (!out) {
            MH4N_LOG(Error, kLog, "cannot write {}", utf8::PathToUtf8(temporary));
            fs::remove(temporary, ec);
            return false;
        }
    }
    fs::rename(temporary, target, ec);
    if (ec) {
        MH4N_LOG(Error, kLog, "cannot replace {}: {}", path, ec.message());
        fs::remove(temporary, ec);
        return false;
    }
    return true;
}

}  // namespace

const std::vector<DefaultBlock>& DefaultBlocks() {
    static const std::vector<DefaultBlock> blocks = MakeDefaults();
    return blocks;
}

std::string ConfigPath(const std::string& user_folder) {
    return UserPath(user_folder,
                    "nand/data/00000000000000000000000000000000/sysdata/00010017/00000000/config");
}
std::string McuPath(const std::string& user_folder) {
    return UserPath(user_folder, "sysdata/mcu.dat");
}
std::string MacPath(const std::string& user_folder) {
    return UserPath(user_folder, "sysdata/mac.txt");
}

// ---- ConfigStore -----------------------------------------------------------------------------------------------

ConfigStore::ConfigStore(const std::string& user_folder) : user_folder_(user_folder) {
    // No user folder (no --user): the default blocks, and nothing to save to.
    std::ifstream in;
    if (!user_folder.empty()) {
        in.open(HostPath(ConfigPath(user_folder)), std::ios::binary);
    }
    if (in.is_open() && in) {
        in.read(reinterpret_cast<char*>(buffer_.data()), static_cast<std::streamsize>(buffer_.size()));
        from_file_ = true;  // a short file leaves the rest zero: no blocks there
    } else {
        Format();
    }
    // Azahar's Module constructor: a different EULA version is replaced by the default one, "so users
    // will never get a prompt to accept EULA" (it also saves the file; this does not).
    u32 eula = 0;
    Get(kBlockEulaVersion, 4, kGlobal, &eula);
    const DefaultBlock* want = FindDefault(kBlockEulaVersion);
    if (want != nullptr && ((eula & 0xFF) != want->data[0] || ((eula >> 8) & 0xFF) != want->data[1])) {
        MH4N_LOG(Info, kLog, "Updating accepted EULA version to {}.{}", want->data[1], want->data[0]);
        Set(kBlockEulaVersion, 4, kGlobal, want->data.data());
    }
}

Result ConfigStore::Locate(u32 block_id, u32 size, u16 access, u8** pointer) {
    auto find = [&]() -> u8* {
        // All 1479 entries are searched, set or not, as Azahar's find_if does.
        for (u32 i = 0; i < kMaxBlockEntries; ++i) {
            if (Get32(Entry(buffer_, i)) == block_id) {
                return Entry(buffer_, i);
            }
        }
        return nullptr;
    };
    u8* entry = find();
    if (entry == nullptr) {
        const DefaultBlock* block = FindDefault(block_id);
        if (block == nullptr) {
            MH4N_LOG(Error, kLog,
                     "Config block 0x{:X} with flags {} and size {} was not found, and no default exists.",
                     block_id, access, size);
            return kResultNotFound;
        }
        MH4N_LOG(Warning, kLog,
                 "Config block 0x{:X} with flags {} and size {} was not found, creating from defaults.",
                 block_id, access, size);
        const Result created =
            Create(block_id, static_cast<u16>(block->data.size()), block->access, block->data.data());
        if (IsError(created)) {
            MH4N_LOG(Error, kLog, "Failed to create config block 0x{:X} from defaults: 0x{:08X}",
                     block_id, created);
            return created;
        }
        entry = find();
        if (entry == nullptr) {
            return kResultNotFound;
        }
    }
    const u16 flags = Get16(entry + 10);
    if ((flags & access) == 0) {
        MH4N_LOG(Error, kLog, "Invalid access flag {:X} for config block 0x{:X} with size {}", access,
                 block_id, size);
        return kResultNotAuthorized;
    }
    const u16 block_size = Get16(entry + 8);
    if (block_size != size) {
        MH4N_LOG(Error, kLog, "Invalid size {} for config block 0x{:X} with flags {}", size, block_id,
                 access);
        return kResultInvalidSize;
    }
    // The data is in the block header itself if the size is 4 bytes or less.
    *pointer = block_size <= 4 ? entry + 4 : buffer_.data() + Get32(entry + 4);
    return kResultSuccess;
}

Result ConfigStore::Get(u32 block_id, u32 size, u16 access, void* out) {
    u8* pointer = nullptr;
    const Result located = Locate(block_id, size, access, &pointer);
    if (IsError(located)) {
        return located;
    }
    if (pointer + size > buffer_.data() + buffer_.size()) {  // a damaged file: the offset is outside
        MH4N_LOG(Error, kLog, "config block 0x{:X} points outside the file", block_id);
        return kResultUnknown;
    }
    std::memcpy(out, pointer, size);
    return kResultSuccess;
}

Result ConfigStore::Set(u32 block_id, u32 size, u16 access, const void* in) {
    u8* pointer = nullptr;
    const Result located = Locate(block_id, size, access, &pointer);
    if (IsError(located)) {
        return located;
    }
    if (pointer + size > buffer_.data() + buffer_.size()) {
        MH4N_LOG(Error, kLog, "config block 0x{:X} points outside the file", block_id);
        return kResultUnknown;
    }
    std::memcpy(pointer, in, size);
    return kResultSuccess;
}

Result ConfigStore::Create(u32 block_id, u16 size, u16 access, const void* data) {
    u8* header = buffer_.data();
    const u32 total = Get16(header);
    if (total >= kMaxBlockEntries) {
        return kResultUnknown;
    }
    u8* entry = Entry(buffer_, total);
    u32 offset = 0;
    if (size > 4) {
        offset = Get16(header + 2);
        // The next free offset follows the data of the last block that has a separate data area.
        for (int i = static_cast<int>(total) - 1; i >= 0; --i) {
            const u8* previous = Entry(buffer_, static_cast<u32>(i));
            if (Get16(previous + 8) > 4) {
                offset = Get32(previous + 4) + Get16(previous + 8);
                break;
            }
        }
        if (offset + size > buffer_.size()) {
            return kResultUnknown;
        }
    }
    Put32(entry, block_id);
    Put32(entry + 4, 0);
    Put16(entry + 8, size);
    Put16(entry + 10, access);
    if (size > 4) {
        Put32(entry + 4, offset);
        std::memcpy(buffer_.data() + offset, data, size);
    } else {
        std::memcpy(entry + 4, data, size);
    }
    Put16(header, total + 1);
    return kResultSuccess;
}

Result ConfigStore::Format() {
    buffer_.fill(0);
    Put16(buffer_.data() + 2, kDataEntriesOffset);  // "hardcoded, taken from 3dbrew, verified by hardware"
    for (const DefaultBlock& block : DefaultBlocks()) {
        const Result created =
            Create(block.id, static_cast<u16>(block.data.size()), block.access, block.data.data());
        if (IsError(created)) {
            return created;
        }
    }
    return kResultSuccess;
}

bool ConfigStore::Save() {
    if (user_folder_.empty()) {
        MH4N_LOG(Error, kLog, "there is no user folder (--user) to save the config savegame to");
        return false;
    }
    return WriteFileAtomically(ConfigPath(user_folder_), buffer_.data(), buffer_.size());
}

ConfigFileState GetConfigFileState(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    // symlink_status: a dangling link or a directory is something there, not an absent file.
    const fs::file_status status = fs::symlink_status(HostPath(path), ec);
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory) {
            return ConfigFileState::Absent;
        }
        MH4N_LOG(Error, kLog, "cannot look at {}: {}", path, ec.message());
        return ConfigFileState::LookupFailed;
    }
    if (status.type() == fs::file_type::not_found) {
        return ConfigFileState::Absent;
    }
    return IsValidConfigFile(path) ? ConfigFileState::Valid : ConfigFileState::Invalid;
}

bool IsValidConfigFile(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path host_path = HostPath(path);
    if (!fs::is_regular_file(host_path, ec) || fs::file_size(host_path, ec) != kSaveFileSize || ec) {
        return false;
    }
    std::array<u8, kSaveFileSize> bytes{};
    std::ifstream in(host_path, std::ios::binary);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<std::size_t>(in.gcount()) != bytes.size()) {
        return false;
    }
    const u32 total = Get16(bytes.data());
    if (total > kMaxBlockEntries || Get16(bytes.data() + 2) != kDataEntriesOffset) {
        return false;
    }
    for (u32 i = 0; i < total; ++i) {
        const u8* entry = Entry(bytes, i);
        const u32 size = Get16(entry + 8);
        if (size > 4 && static_cast<u64>(Get32(entry + 4)) + size > kSaveFileSize) {
            return false;
        }
    }
    return true;
}

EnsureResult EnsureConfigFile(const std::string& user_folder, const std::function<u64()>& random64,
                              u64* console_id) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (user_folder.empty() || !fs::is_directory(HostPath(user_folder), ec)) {
        return EnsureResult::NoUserFolder;
    }
    const std::string path = ConfigPath(user_folder);
    switch (GetConfigFileState(path)) {
    case ConfigFileState::Valid:
        return EnsureResult::Existing;
    case ConfigFileState::Invalid:
        MH4N_LOG(Error, kLog,
                 "the config savegame {} is short or damaged: it is left as it is (the blocks that are "
                 "missing read as defaults, in memory); move it away to have a new one made",
                 path);
        return EnsureResult::Invalid;
    case ConfigFileState::LookupFailed:
        MH4N_LOG(Error, kLog, "no config savegame is made in {}", user_folder);
        return EnsureResult::Failed;
    case ConfigFileState::Absent:
        break;
    }
    ConfigStore store(user_folder);  // no file: the default blocks, in memory
    std::pair<u32, u64> drawn;
    if (!TryGenerateConsoleUniqueId(random64, &drawn)) {
        // Nothing is written: this run's console unique ID is 0, and the next start tries again.
        MH4N_LOG(Error, kLog, "no config savegame is made in {}: console id 0 for this run", user_folder);
        return EnsureResult::Failed;
    }
    const auto [random_number, id] = drawn;
    if (IsError(store.SetConsoleUniqueId(random_number, id)) || !store.Save()) {
        MH4N_LOG(Error, kLog, "the config savegame of {} could not be made", user_folder);
        return EnsureResult::Failed;
    }
    if (console_id != nullptr) {
        *console_id = id;
    }
    return EnsureResult::Created;
}

namespace {
std::mutex g_random_override_mutex;
std::function<u64()> g_random_override;  // empty: the host's random_device
}  // namespace

u64 HostRandom64() {
    std::function<u64()> injected;
    {
        std::lock_guard lock(g_random_override_mutex);
        injected = g_random_override;
    }
    if (injected) {
        return injected();
    }
    // The host's secure random (std::random_device: the OS source). This is the one place where
    // randomness is allowed: Azahar draws the console unique ID, the MAC address and the like the same way,
    // and each value is written to the user folder at once, so every later run reads it back and nothing
    // the game sees depends on a fresh draw. The source can throw (a system without it): the callers catch
    // that. A throwing constructor leaves the static unbuilt, so the next call tries again.
    static std::random_device device;
    return (static_cast<u64>(device()) << 32) | device();
}

void SetHostRandom64ForTest(std::function<u64()> source) {
    std::lock_guard lock(g_random_override_mutex);
    g_random_override = std::move(source);
}

bool TryGenerateConsoleUniqueId(const std::function<u64()>& random64, std::pair<u32, u64>* out) {
    try {
        *out = ConfigStore::GenerateConsoleUniqueId(random64);
        return true;
    } catch (const std::exception& e) {
        MH4N_LOG(Error, kLog, "no random numbers for the console unique ID (the host's random source failed: {})",
                 e.what());
        return false;
    }
}

std::pair<u32, u64> ConfigStore::GenerateConsoleUniqueId(const std::function<u64()>& random64) {
    const u32 random_number = static_cast<u32>(random64() % 0x10000);
    const u64 friend_code_seed = random64();
    const u64 console_id = (friend_code_seed & 0x3FFFFFFFFull) | (static_cast<u64>(random_number) << 48);
    return {random_number, console_id};
}

Result ConfigStore::SetConsoleUniqueId(u32 random_number, u64 console_id) {
    u8 id[8];
    for (int i = 0; i < 8; ++i) {
        id[i] = static_cast<u8>(console_id >> (8 * i));
    }
    Result result = Set(kBlockConsoleUniqueId1, 8, kGlobal, id);
    if (IsError(result)) {
        return result;
    }
    result = Set(kBlockConsoleUniqueId2, 8, kGlobal, id);
    if (IsError(result)) {
        return result;
    }
    u8 random[4];
    Put32(random, random_number);
    return Set(kBlockConsoleUniqueId3, 4, kGlobal, random);
}

u64 ConfigStore::GetConsoleUniqueId() {
    u8 id[8] = {};
    Get(kBlockConsoleUniqueId2, 8, kGlobal, id);
    u64 value = 0;
    for (int i = 7; i >= 0; --i) {
        value = value << 8 | id[i];
    }
    return value;
}

std::u16string ConfigStore::GetUsername() {
    u8 block[0x1C] = {};
    Get(kBlockUsername, sizeof(block), kSystemRead, block);
    std::u16string name;
    for (int i = 0; i < 10; ++i) {  // the name is not null-terminated: stop at the first 0
        const u16 unit = Get16(block + i * 2);
        if (unit == 0) {
            break;
        }
        name.push_back(static_cast<char16_t>(unit));
    }
    return name;
}

u8 ConfigStore::GetLanguageRaw() {
    u8 language = 0;
    Get(kBlockLanguage, 1, kSystemRead, &language);
    return language;
}

void ConfigStore::SetLanguage(u8 language) {
    Set(kBlockLanguage, 1, kSystemWrite, &language);
}

// The SMDH of the game's icon: region lockout at 0x2018, the whole structure is 0x36C0 bytes. Azahar's
// AppLoader_NCCH::ParseRegionLockoutInfo turns the bits 0 to 6 into the preferred regions, in order.
std::vector<u32> RegionsFromIcon(const std::string& game_folder) {
    std::vector<u32> regions;
    if (game_folder.empty()) {
        return regions;
    }
    constexpr std::size_t kSmdhSize = 0x36C0;
    constexpr std::size_t kLockoutOffset = 0x2018;
    for (const char* name : {"/icon.bin", "/exefs/icon.bin"}) {
        // A path-based stream (D45): std::fopen takes the ANSI code page on Windows.
        std::ifstream file(HostPath(game_folder + name), std::ios::binary);
        if (!file) {
            continue;
        }
        std::vector<u8> smdh(kSmdhSize);
        file.read(reinterpret_cast<char*>(smdh.data()), static_cast<std::streamsize>(smdh.size()));
        if (static_cast<std::size_t>(file.gcount()) < kSmdhSize) {
            continue;
        }
        u32 lockout = static_cast<u32>(smdh[kLockoutOffset]) |
                      static_cast<u32>(smdh[kLockoutOffset + 1]) << 8 |
                      static_cast<u32>(smdh[kLockoutOffset + 2]) << 16 |
                      static_cast<u32>(smdh[kLockoutOffset + 3]) << 24;
        for (u32 region = 0; region < 7; ++region) {
            if ((lockout & 1) != 0) {
                regions.push_back(region);
            }
            lockout >>= 1;
        }
        return regions;
    }
    return regions;
}

// Azahar's AdjustLanguageInfoBlock: the region and language that fit the game's regions.
std::pair<u32, u8> AdjustLanguage(const std::vector<u32>& regions, u8 language) {
    static const std::vector<u8> by_region[7] = {
        {kLanguageJp},
        {kLanguageEn, kLanguageFr, kLanguageEs, kLanguagePt},
        {kLanguageEn, kLanguageFr, kLanguageDe, kLanguageIt, kLanguageEs, kLanguageNl, kLanguagePt,
         kLanguageRu},
        {kLanguageEn, kLanguageFr, kLanguageDe, kLanguageIt, kLanguageEs, kLanguageNl, kLanguagePt,
         kLanguageRu},
        {kLanguageZh},
        {kLanguageKo},
        {kLanguageTw},
    };
    for (const u32 region : regions) {
        for (const u8 available : by_region[region]) {
            if (available == language) {
                return {region, language};
            }
        }
    }
    return {regions[0], by_region[regions[0]][0]};
}

// ---- MCU data and the MAC address ------------------------------------------------------------------------------

McuData ReadMcuData(const std::string& user_folder) {
    McuData data;
    std::ifstream in;
    if (!user_folder.empty()) {
        in.open(HostPath(McuPath(user_folder)), std::ios::binary);
    }
    if (in.is_open() && in) {
        u8 raw[sizeof(McuData)];
        in.read(reinterpret_cast<char*>(raw), sizeof(raw));
        if (in.gcount() == static_cast<std::streamsize>(sizeof(raw))) {
            McuData read;
            read.magic = Get32(raw);
            read.version = Get32(raw + 4);
            read.reserved = static_cast<u64>(Get32(raw + 8)) | static_cast<u64>(Get32(raw + 12)) << 32;
            read.clock_sequence = Get32(raw + 16);
            if (read.magic == McuData::kMagic && read.version == McuData::kVersion) {
                return read;
            }
        }
    }
    return data;
}

bool WriteMcuData(const std::string& user_folder, const McuData& data) {
    // Never creates a missing user folder (only the folders under it).
    std::error_code ec;
    if (user_folder.empty() || !std::filesystem::is_directory(HostPath(user_folder), ec)) {
        return false;
    }
    u8 raw[sizeof(McuData)] = {};
    Put32(raw, data.magic);
    Put32(raw + 4, data.version);
    Put32(raw + 8, static_cast<u32>(data.reserved));
    Put32(raw + 12, static_cast<u32>(data.reserved >> 32));
    Put32(raw + 16, data.clock_sequence);
    return WriteFileAtomically(McuPath(user_folder), raw, sizeof(raw));
}

std::string MacToString(u64 mac) {
    char text[18];
    std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
                  static_cast<unsigned>((mac >> 40) & 0xFF), static_cast<unsigned>((mac >> 32) & 0xFF),
                  static_cast<unsigned>((mac >> 24) & 0xFF), static_cast<unsigned>((mac >> 16) & 0xFF),
                  static_cast<unsigned>((mac >> 8) & 0xFF), static_cast<unsigned>(mac & 0xFF));
    return text;
}

bool ParseMac(const std::string& text, std::array<u8, 6>* mac) {
    // Azahar's MacToArray: sscanf with %hhx, the whole string consumed.
    std::array<u8, 6> value{};
    int last = -1;
    const int count = std::sscanf(text.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx%n", &value[0], &value[1],
                                  &value[2], &value[3], &value[4], &value[5], &last);
    if (count != 6 || static_cast<int>(text.size()) != last) {
        return false;
    }
    // Azahar keeps a saved address only if writing it back gives the same text.
    u64 packed = 0;
    for (const u8 byte : value) {
        packed = packed << 8 | byte;
    }
    if (MacToString(packed) != text) {
        return false;
    }
    *mac = value;
    return true;
}

namespace {

// The MAC of a run whose host random source failed: the first address of Azahar's first range.
constexpr const char* kFallbackMac = "18:2A:7B:00:00:00";

std::string RandomMac(const std::function<u64()>& random64) {
    // Azahar's GenerateRandomMAC: 16 vendor ranges.
    static const std::array<std::pair<u64, u64>, 16> ranges = {{
        {0x182A7B000000ULL, 0x182A7BFFFFFFULL}, {0x2C10C1000000ULL, 0x2C10C1FFFFFFULL},
        {0x342FBD000000ULL, 0x342FBDFFFFFFULL}, {0x34AF2C000000ULL, 0x34AF2CFFFFFFULL},
        {0x40D28A000000ULL, 0x40D28AFFFFFFULL}, {0x40F407000000ULL, 0x40F407FFFFFFULL},
        {0x48A5E7000000ULL, 0x48A5E7FFFFFFULL}, {0x582F40000000ULL, 0x582F40FFFFFFULL},
        {0x68BD13000000ULL, 0x68BD13FFFFFFULL}, {0x58BDA3000000ULL, 0x58BDA3FFFFFFULL},
        {0x5C521E000000ULL, 0x5C521EFFFFFFULL}, {0x606BFF000000ULL, 0x606BFFFFFFFFULL},
        {0x64B5C6000000ULL, 0x64B5C6FFFFFFULL}, {0x78A2A0000000ULL, 0x78A2A0FFFFFFULL},
        {0x7CBB8A000000ULL, 0x7CBB8AFFFFFFULL}, {0x8CCDE8000000ULL, 0x8CCDE8FFFFFFULL},
    }};
    const auto& range = ranges[random64() % ranges.size()];
    return MacToString(range.first + random64() % (range.second - range.first + 1));
}

}  // namespace

std::string ReadMacAddress(const std::string& user_folder, const std::function<u64()>& random64) {
    std::ifstream in;
    if (!user_folder.empty()) {
        in.open(HostPath(MacPath(user_folder)), std::ios::binary);
    }
    if (in.is_open() && in) {
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::array<u8, 6> ignored{};
        if (text.size() <= 100 && ParseMac(text, &ignored)) {
            return text;
        }
        MH4N_LOG(Warning, kLog, "Invalid saved MAC address, generating a new one");
    } else {
        MH4N_LOG(Info, kLog, "Cannot open mac address file for read, generating a new one");
    }
    std::string mac;
    try {
        mac = RandomMac(random64);
    } catch (const std::exception& e) {
        // The host's random source failed: this run gets the first address of Azahar's first range and
        // nothing is written, so the next start (with a working source) makes the console's own.
        MH4N_LOG(Error, kLog, "no random numbers for the MAC address (the host's random source failed: {}): "
                 "using {} for this run only", e.what(), kFallbackMac);
        return kFallbackMac;
    }
    std::error_code folder_ec;
    if (user_folder.empty() || !std::filesystem::is_directory(HostPath(user_folder), folder_ec)) {
        return mac;  // nowhere to keep it (no folder, and a read never creates one): this run only
    }
    if (!WriteFileAtomically(MacPath(user_folder), mac.data(), mac.size())) {
        MH4N_LOG(Error, kLog, "Cannot open mac address file for write");
    }
    return mac;
}

}  // namespace mh4n::cfg
