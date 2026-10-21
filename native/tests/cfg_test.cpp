// Derived from Azahar src/core/hle/service/cfg/cfg.cpp, src/core/hle/service/cfg/cfg_defaults.cpp,
// src/core/hle/service/ptm/ptm.cpp, src/core/hle/service/ndm/ndm_u.cpp, src/core/hle/service/ndm/ndm_u.h and
// src/core/hle/service/ac/ac.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of
// them says "GPLv2 or any later version"): the expected answers (the default blocks, the result codes,
// the command ids and replies, the daemon state, the country code table) are the ones those sources
// give.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (cfg.cpp, ndm_u.cpp, ndm_u.h)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (ptm.cpp)
// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project (ac.cpp)
// Copyright 2023-2025 Citra Emulator Project / Azahar Emulator Project (cfg_defaults.cpp)
//
// Tests of N1-15: the config savegame (the store), the cfg:, ptm:, ndm: and ac: services through the
// SVC layer, and the console unique ID. The file formats and the expected values are Azahar's
// (cfg.cpp, cfg_defaults.cpp at the pinned commit); the config files here are written by this test
// with its own writer, from 3dbrew's description of the format, so reading does not depend on the
// code that writes. Everything is synthetic.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "../src/services/cfg/cfg_store.h"
#include "../src/services/cfg/sha256.h"
#include "mh4n/clock.h"
#include "mh4n/ipc.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"
#include "mh4n/service.h"
#include "mh4n/threads.h"

using namespace mh4n;

namespace {

namespace fs = std::filesystem;

class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = fs::temp_directory_path() /
                ("mh4n_cfg_test_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                 std::to_string(counter++));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    std::string File(const std::string& name) const { return (path_ / name).string(); }
    std::string User() const { return File("user"); }

private:
    fs::path path_;
};

std::vector<u8> ReadAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteAll(const std::string& path, const std::vector<u8>& bytes) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

u32 Le32(const std::vector<u8>& b, std::size_t at) {
    return b[at] | b[at + 1] << 8 | b[at + 2] << 16 | static_cast<u32>(b[at + 3]) << 24;
}
u32 Le16(const std::vector<u8>& b, std::size_t at) {
    return b[at] | b[at + 1] << 8;
}
void Set16(std::vector<u8>& b, std::size_t at, u32 v) {
    b[at] = static_cast<u8>(v), b[at + 1] = static_cast<u8>(v >> 8);
}
void Set32(std::vector<u8>& b, std::size_t at, u32 v) {
    Set16(b, at, v & 0xFFFF), Set16(b, at + 2, v >> 16);
}

// ---- a config savegame as 3dbrew describes it, written independently of the code under test ---------

struct TestBlock {
    u32 id;
    u16 flags;
    std::vector<u8> data;
};

std::vector<u8> MakeConfigFile(const std::vector<TestBlock>& blocks) {
    std::vector<u8> file(0x8000, 0);
    Set16(file, 0, static_cast<u32>(blocks.size()));
    Set16(file, 2, 0x455C);
    u32 next = 0x455C;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const std::size_t entry = 4 + 12 * i;
        Set32(file, entry, blocks[i].id);
        Set16(file, entry + 8, static_cast<u32>(blocks[i].data.size()));
        Set16(file, entry + 10, blocks[i].flags);
        if (blocks[i].data.size() > 4) {
            Set32(file, entry + 4, next);
            std::copy(blocks[i].data.begin(), blocks[i].data.end(), file.begin() + next);
            next += static_cast<u32>(blocks[i].data.size());
        } else {
            std::copy(blocks[i].data.begin(), blocks[i].data.end(), file.begin() + entry + 4);
        }
    }
    return file;
}

std::vector<u8> Utf16(const std::u16string& text, std::size_t units) {
    std::vector<u8> out(units * 2, 0);
    for (std::size_t i = 0; i < text.size(); ++i) {
        Set16(out, i * 2, text[i]);
    }
    return out;
}

constexpr u16 kGlobal = 14, kSystem = 12;

// A savegame like one a user made: Hunter, French, Germany (country 78), a New 3DS XL, console id
// 0x1234'0001'2345'6789, EULA 0x7F7F.
std::vector<u8> UserConfigFile() {
    std::vector<u8> name = Utf16(u"Hunter", 10);
    name.resize(0x1C, 0);
    return MakeConfigFile({
        {0x00090000, kGlobal, {0x89, 0x67, 0x45, 0x23, 0x01, 0x00, 0x34, 0x12}},
        {0x00090001, kGlobal, {0x89, 0x67, 0x45, 0x23, 0x01, 0x00, 0x34, 0x12}},
        {0x00090002, kGlobal, {0x34, 0x12, 0, 0}},
        {0x000A0000, kGlobal, name},
        {0x000A0001, kGlobal, {3, 14}},
        {0x000A0002, kGlobal, {2}},
        {0x000B0000, kGlobal, {0, 0, 1, 78}},
        {0x000D0000, kGlobal, {0x7F, 0x7F, 0, 0}},
        {0x000F0004, kSystem, {2, 0, 0, 0}},  // New Nintendo 3DS
    });
}

std::string Hex(const std::array<u8, 32>& hash) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (const u8 byte : hash) {
        out += digits[byte >> 4];
        out += digits[byte & 15];
    }
    return out;
}

}  // namespace

// ---- SHA-256 -----------------------------------------------------------------------------------------------------

TEST_CASE("cfg: SHA-256 gives the standard digests around the padding boundaries") {
    // hashlib.sha256 of bytes((i * 7 + 1) & 0xff for i in range(n))
    const std::pair<std::size_t, const char*> vectors[] = {
        {0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {3, "ce562c676ea7aba9812f54db146b0096011bf3e423edfb0a6f8fb2b02674a801"},
        {55, "16fa57a0a3423a715d594516339f36189d6b5f93754a9714fef202616a9fabfe"},
        {56, "c37b44e5f1b18554b36966f4f8e08bfbf3164c4b6c10374d12d89850892073c5"},
        {63, "bbba992d2c85af960fb2987a1fd05e0aa82a3db3c740dd8982a9e273b75e36a3"},
        {64, "66bd4633ed6f71c4ecfa4763bf7ba1c8ec7612de9aa6c0578a7b675207c71e0b"},
        {65, "9f7dc47107b750a1f3d35db5d9547f24ef40da5b731b9540d4f43710a154f6c9"},
        {119, "a3ed307b730fa77c07531300c6e4a282330011d4d4caf6bb7b63ae05950f4b66"},
        {120, "8e3b15d9fea7472655aa069620b7f8c2e55ee1499f763200a7515fe826e99d20"},
    };
    for (const auto& [size, expected] : vectors) {
        std::vector<u8> data(size);
        for (std::size_t i = 0; i < size; ++i) {
            data[i] = static_cast<u8>((i * 7 + 1) & 0xFF);
        }
        INFO("length " << size);
        CHECK(Hex(cfg::Sha256(data.data(), data.size())) == expected);
    }
    const char* abc = "abc";
    CHECK(Hex(cfg::Sha256(reinterpret_cast<const u8*>(abc), 3)) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

// ---- the store: defaults ---------------------------------------------------------------------------------------

TEST_CASE("cfg store: without a file the blocks are Azahar's defaults, and nothing is written") {
    TempDir dir;
    cfg::ConfigStore store(dir.User());
    CHECK_FALSE(store.FromFile());
    CHECK(cfg::DefaultBlocks().size() == 28);

    u8 language = 0;
    CHECK(store.Get(0x000A0002, 1, cfg::kUserRead, &language) == kResultSuccess);
    CHECK(language == 1);  // English
    u8 country[4] = {};
    CHECK(store.Get(0x000B0000, 4, cfg::kUserRead, country) == kResultSuccess);
    CHECK(country[0] == 0);
    CHECK(country[1] == 0);
    CHECK(country[2] == 2);    // Washington DC
    CHECK(country[3] == 49);   // United States (conf_services: 0x31)
    CHECK(store.GetUsername() == u"AZAHAR");
    u8 birthday[2] = {};
    CHECK(store.Get(0x000A0001, 2, cfg::kUserRead, birthday) == kResultSuccess);
    CHECK(birthday[0] == 11);
    CHECK(birthday[1] == 7);
    u32 model = 0;
    CHECK(store.Get(0x000F0004, 4, cfg::kSystemRead, &model) == kResultSuccess);
    CHECK(model == 4);  // Azahar's default table: New Nintendo 3DS XL (GetSystemModel reports Old 3DS)
    u8 sound = 0;
    CHECK(store.Get(0x00070001, 1, cfg::kUserRead, &sound) == kResultSuccess);
    CHECK(sound == 1);  // stereo
    float stereo[8];
    CHECK(store.Get(0x00050005, 32, cfg::kUserRead, stereo) == kResultSuccess);
    CHECK(stereo[0] == 62.0f);
    CHECK(stereo[3] == 46.08000183105469f);
    char server[4];
    CHECK(store.Get(0x00150002, 4, cfg::kUserRead, server) == kResultSuccess);
    CHECK(std::string(server) == "L1");
    CHECK(store.GetConsoleUniqueId() == 0);  // the table's placeholder

    CHECK_FALSE(fs::exists(dir.User()));  // reading made nothing
}

TEST_CASE("cfg store: the error results are Azahar's") {
    TempDir dir;
    cfg::ConfigStore store(dir.User());
    u8 buffer[0x400] = {};
    CHECK(store.Get(0x00FF0000, 4, cfg::kUserRead, buffer) == 0xD90103FA);   // no such block, no default
    CHECK(store.Get(0x000A0002, 2, cfg::kUserRead, buffer) == 0xD90103EC);   // wrong size
    CHECK(store.Get(0x000F0004, 4, cfg::kUserRead, buffer) == 0xD90103EA);   // the user may not read it
    CHECK(store.Get(0x000F0004, 4, cfg::kSystemRead, buffer) == kResultSuccess);
    CHECK(store.Set(0x000F0004, 4, cfg::kUserRead, buffer) == 0xD90103EA);
    CHECK(store.Get(0x00000000, 4, cfg::kUserRead, buffer) != kResultSuccess);  // an unset entry has id 0
}

TEST_CASE("cfg store: Save writes the file Azahar's format describes, and it reads back") {
    TempDir dir;
    cfg::ConfigStore store(dir.User());
    const u32 id_low = 0x11223344;
    REQUIRE(store.SetConsoleUniqueId(0xBEEF, (static_cast<u64>(0xBEEF) << 48) | id_low) == kResultSuccess);
    REQUIRE(store.Save());

    const std::string path =
        dir.User() + "/nand/data/00000000000000000000000000000000/sysdata/00010017/00000000/config";
    REQUIRE(fs::exists(path));
    CHECK_FALSE(fs::exists(path + ".tmp"));
    const std::vector<u8> file = ReadAll(path);
    REQUIRE(file.size() == 0x8000);
    CHECK(Le16(file, 0) == 28);       // total entries
    CHECK(Le16(file, 2) == 0x455C);   // data entries offset, "hardcoded"
    // every default block is in the table with its size and flags; blocks of more than 4 bytes follow
    // each other from 0x455C, the others hold their data in the entry
    u32 next = 0x455C;
    std::size_t seen = 0;
    for (std::size_t i = 0; i < 28; ++i) {
        const std::size_t entry = 4 + 12 * i;
        const u32 id = Le32(file, entry);
        const auto it = std::find_if(cfg::DefaultBlocks().begin(), cfg::DefaultBlocks().end(),
                                     [&](const cfg::DefaultBlock& b) { return b.id == id; });
        REQUIRE(it != cfg::DefaultBlocks().end());
        ++seen;
        CHECK(Le16(file, entry + 8) == it->data.size());
        CHECK(Le16(file, entry + 10) == it->access);
        if (it->data.size() > 4) {
            CHECK(Le32(file, entry + 4) == next);
            next += static_cast<u32>(it->data.size());
        }
    }
    CHECK(seen == 28);
    CHECK(next < 0x8000);
    CHECK(Le32(file, 4 + 12 * 1479) == 0);  // the unknown word after the table

    cfg::ConfigStore again(dir.User());
    CHECK(again.FromFile());
    CHECK(again.GetConsoleUniqueId() == ((static_cast<u64>(0xBEEF) << 48) | id_low));
    u32 random = 0;
    CHECK(again.Get(0x00090002, 4, cfg::kUserRead, &random) == kResultSuccess);
    CHECK(random == 0xBEEF);
    CHECK(again.GetUsername() == u"AZAHAR");
}

// ---- the store: a file the user made ---------------------------------------------------------------------------

TEST_CASE("cfg store: blocks are read from a synthetic config file") {
    TempDir dir;
    const std::string path =
        dir.User() + "/nand/data/00000000000000000000000000000000/sysdata/00010017/00000000/config";
    WriteAll(path, UserConfigFile());
    cfg::ConfigStore store(dir.User());
    CHECK(store.FromFile());
    CHECK(store.GetUsername() == u"Hunter");
    CHECK(store.GetLanguageRaw() == 2);
    CHECK(store.GetConsoleUniqueId() == 0x1234000123456789ull);
    u8 country[4];
    CHECK(store.Get(0x000B0000, 4, cfg::kUserRead, country) == kResultSuccess);
    CHECK(country[2] == 1);
    CHECK(country[3] == 78);
    u32 model = 0;
    CHECK(store.Get(0x000F0004, 4, cfg::kSystemRead, &model) == kResultSuccess);
    CHECK(model == 2);
}

TEST_CASE("cfg store: a block missing from the file is made from its default in memory only") {
    TempDir dir;
    const std::string path =
        dir.User() + "/nand/data/00000000000000000000000000000000/sysdata/00010017/00000000/config";
    const std::vector<u8> original = UserConfigFile();
    WriteAll(path, original);
    cfg::ConfigStore store(dir.User());
    u8 sound = 0;
    CHECK(store.Get(0x00070001, 1, cfg::kUserRead, &sound) == kResultSuccess);  // not in the file
    CHECK(sound == 1);
    u8 names[0x800];
    CHECK(store.Get(0x000B0001, 0x800, cfg::kUserRead, names) == kResultSuccess);  // a big one
    CHECK(names[0] == 'U');
    CHECK(names[1] == 0);
    CHECK(Le16(store.Raw().size() ? std::vector<u8>(store.Raw().begin(), store.Raw().begin() + 2)
                                  : std::vector<u8>(2),
               0) == 11);  // 9 + 2 blocks
    CHECK(ReadAll(path) == original);  // the file was not touched
}

TEST_CASE("cfg store: a block that would pass the end of the file is not created") {
    TempDir dir;
    const std::string path =
        dir.User() + "/nand/data/00000000000000000000000000000000/sysdata/00010017/00000000/config";
    // one big block whose data ends 0x10 bytes before the end of the 0x8000-byte file
    const std::size_t big = 0x8000 - 0x455C - 0x10;
    WriteAll(path, MakeConfigFile({{0x00FF0001, kGlobal, std::vector<u8>(big, 0x5A)}}));
    cfg::ConfigStore store(dir.User());
    REQUIRE(store.FromFile());
    // the big block, and the EULA version block the constructor makes (4 bytes, kept in its entry)
    const u32 entries = Le16(std::vector<u8>(store.Raw().begin(), store.Raw().begin() + 2), 0);
    CHECK(entries == 2);
    u8 name[0x1C];
    // the user name (0x1C bytes) has a default, but its data would end at 0x800C
    CHECK(store.Get(0x000A0000, 0x1C, cfg::kUserRead, name) == cfg::kResultUnknown);
    CHECK(Le16(std::vector<u8>(store.Raw().begin(), store.Raw().begin() + 2), 0) == entries);  // none added
    for (std::size_t i = 0x7FF0; i < 0x8000; ++i) {
        CHECK(store.Raw()[i] == 0);  // nothing written at the end of the file
    }
    CHECK(store.Raw()[0x7FEF] == 0x5A);
    // a block of 4 bytes or less keeps its data in the entry, so it still fits
    u8 sound = 0;
    CHECK(store.Get(0x00070001, 1, cfg::kUserRead, &sound) == kResultSuccess);
    CHECK(sound == 1);
}

TEST_CASE("cfg store: a different EULA version is replaced by the default one, in memory") {
    TempDir dir;
    const std::string path =
        dir.User() + "/nand/data/00000000000000000000000000000000/sysdata/00010017/00000000/config";
    std::vector<u8> bytes = MakeConfigFile({{0x000D0000, kGlobal, {0x01, 0x02, 0, 0}}});
    WriteAll(path, bytes);
    cfg::ConfigStore store(dir.User());
    u8 eula[4] = {};
    CHECK(store.Get(0x000D0000, 4, cfg::kUserRead, eula) == kResultSuccess);
    CHECK(eula[0] == 0x7F);
    CHECK(eula[1] == 0x7F);
    CHECK(ReadAll(path) == bytes);
}

TEST_CASE("cfg store: a short or damaged file does not crash") {
    TempDir dir;
    const std::string path =
        dir.User() + "/nand/data/00000000000000000000000000000000/sysdata/00010017/00000000/config";
    WriteAll(path, std::vector<u8>(10, 0xFF));  // total entries 0xFFFF, data offset 0xFFFF
    cfg::ConfigStore store(dir.User());
    u8 language = 0;
    // no block matches and the table is "full" (0xFFFF entries): Azahar cannot make the default either
    CHECK(store.Get(0x000A0002, 1, cfg::kUserRead, &language) == 0xFFFFFFFF);
    // a block whose data offset is outside the file is refused
    std::vector<u8> file = MakeConfigFile({{0x000A0000, kGlobal, std::vector<u8>(0x1C, 0)}});
    Set32(file, 4 + 4, 0x7FF0);
    WriteAll(path, file);
    cfg::ConfigStore damaged(dir.User());
    u8 name[0x1C];
    CHECK(IsError(damaged.Get(0x000A0000, 0x1C, cfg::kUserRead, name)));
}

// ---- the console unique ID -------------------------------------------------------------------------------------

TEST_CASE("cfg store: the console unique ID is Azahar's formula") {
    u64 calls = 0;
    const std::array<u64, 2> values = {0xABCD'1234, 0xFEDC'BA98'7654'3210ull};
    const auto [random_number, console_id] = cfg::ConfigStore::GenerateConsoleUniqueId([&] {
        return values[calls++ % 2];
    });
    CHECK(calls == 2);
    CHECK(random_number == (0xABCD1234 % 0x10000));  // 0..0xFFFF
    CHECK(console_id == ((0xFEDCBA9876543210ull & 0x3FFFFFFFFull) | (static_cast<u64>(random_number) << 48)));
    CHECK(((console_id & 0xFFFF0000'00000000ull) >> 48) == random_number);
    CHECK((console_id & 0x0000FFFC'00000000ull) == 0);
}

TEST_CASE("cfg store: the unique ID blocks 0x90000 and 0x90001 hold the ID and 0x90002 the random number") {
    TempDir dir;
    cfg::ConfigStore store(dir.User());
    REQUIRE(store.SetConsoleUniqueId(0x1234, 0x1234'0000'AABBCCDDull) == kResultSuccess);
    u64 a = 0, b = 0;
    u32 c = 0;
    CHECK(store.Get(0x00090000, 8, cfg::kUserRead, &a) == kResultSuccess);
    CHECK(store.Get(0x00090001, 8, cfg::kUserRead, &b) == kResultSuccess);
    CHECK(store.Get(0x00090002, 4, cfg::kUserRead, &c) == kResultSuccess);
    CHECK(a == 0x1234'0000'AABBCCDDull);
    CHECK(b == a);
    CHECK(c == 0x1234);
}

// ---- the other files of the identity ---------------------------------------------------------------------------

TEST_CASE("cfg: the MAC address is read from mac.txt, or made once and kept") {
    TempDir dir;
    u64 counter = 0;
    const auto random = [&] { return counter += 0x1234567; };
    const std::string made = cfg::ReadMacAddress(dir.User(), random);
    std::array<u8, 6> bytes{};
    CHECK(cfg::ParseMac(made, &bytes));
    CHECK(ReadAll(dir.User() + "/sysdata/mac.txt") == std::vector<u8>(made.begin(), made.end()));
    CHECK(cfg::ReadMacAddress(dir.User(), random) == made);  // kept
    // a file Azahar would reject (lower case: it does not equal what it prints back) is replaced
    WriteAll(dir.User() + "/sysdata/mac.txt", std::vector<u8>{'a', 'a', ':', 'b', 'b', ':', 'c', 'c', ':',
                                                              'd', 'd', ':', 'e', 'e', ':', 'f', 'f'});
    CHECK(cfg::ReadMacAddress(dir.User(), random) != "aa:bb:cc:dd:ee:ff");
    // a good one is used as it is
    const std::string good = "18:2A:7B:01:02:03";
    WriteAll(dir.User() + "/sysdata/mac.txt", std::vector<u8>(good.begin(), good.end()));
    CHECK(cfg::ReadMacAddress(dir.User(), random) == good);
    std::array<u8, 6> parsed{};
    CHECK(cfg::ParseMac(good, &parsed));
    CHECK(parsed == std::array<u8, 6>{0x18, 0x2A, 0x7B, 1, 2, 3});
    CHECK_FALSE(cfg::ParseMac("18:2A:7B:01:02", &parsed));
    CHECK_FALSE(cfg::ParseMac("18:2A:7B:01:02:03 ", &parsed));
    CHECK(cfg::MacToString(0x182A7B010203ull) == good);
}

TEST_CASE("cfg: the MCU data file is valid or the default, and written only on request") {
    TempDir dir;
    CHECK(cfg::ReadMcuData(dir.User()).clock_sequence == 0);
    CHECK_FALSE(fs::exists(dir.User()));
    cfg::McuData data;
    data.clock_sequence = 0x1234;
    REQUIRE(cfg::WriteMcuData(dir.User(), data));
    const std::vector<u8> file = ReadAll(dir.User() + "/sysdata/mcu.dat");
    REQUIRE(file.size() == 0x18);
    CHECK(Le32(file, 0) == 0x4455434D);
    CHECK(Le32(file, 4) == 1);
    CHECK(Le32(file, 16) == 0x1234);
    CHECK(cfg::ReadMcuData(dir.User()).clock_sequence == 0x1234);
    std::vector<u8> bad = file;
    bad[0] ^= 1;
    WriteAll(dir.User() + "/sysdata/mcu.dat", bad);
    CHECK(cfg::ReadMcuData(dir.User()).clock_sequence == 0);
}

// ---- region and language ---------------------------------------------------------------------------------------

TEST_CASE("cfg: the region lockout of the icon and the language that fits it") {
    TempDir dir;
    const std::string game = dir.File("game");
    CHECK(cfg::RegionsFromIcon(game).empty());
    std::vector<u8> smdh(0x36C0, 0);
    Set32(smdh, 0x2018, 0b0000101);  // JPN and EUR
    WriteAll(game + "/icon.bin", smdh);
    CHECK(cfg::RegionsFromIcon(game) == std::vector<u32>{0, 2});
    fs::remove(game + "/icon.bin");
    WriteAll(game + "/exefs/icon.bin", smdh);
    CHECK(cfg::RegionsFromIcon(game) == std::vector<u32>{0, 2});
    WriteAll(game + "/exefs/icon.bin", std::vector<u8>(100, 0));  // too short
    CHECK(cfg::RegionsFromIcon(game).empty());

    // English is not a Japanese language, so a Japan-only game gets Japanese; a game for Japan and
    // Europe keeps English and takes the region where it exists
    CHECK(cfg::AdjustLanguage({0}, cfg::kLanguageEn) == std::pair<u32, u8>{0, cfg::kLanguageJp});
    CHECK(cfg::AdjustLanguage({0, 2}, cfg::kLanguageEn) == std::pair<u32, u8>{2, cfg::kLanguageEn});
    CHECK(cfg::AdjustLanguage({1}, cfg::kLanguageDe) == std::pair<u32, u8>{1, cfg::kLanguageEn});
    CHECK(cfg::AdjustLanguage({4, 5}, cfg::kLanguageKo) == std::pair<u32, u8>{5, cfg::kLanguageKo});
}

TEST_CASE("cfg: country codes and the valid countries of a region") {
    CHECK(cfg::kCountryCodes.size() == 187);
    CHECK(cfg::kCountryCodes[1] == ('J' | 'P' << 8));
    CHECK(cfg::kCountryCodes[49] == ('U' | 'S' << 8));
    CHECK(cfg::kCountryCodes[78] == ('D' | 'E' << 8));
    CHECK(cfg::kCountryCodes[186] == ('B' | 'M' << 8));
    CHECK(cfg::kCountryCodes[0] == 0);
    CHECK(cfg::kDefaultSubregion[49] == 2);  // Washington DC, the default state of the United States
    CHECK(cfg::IsValidRegionCountry(0, 1));
    CHECK_FALSE(cfg::IsValidRegionCountry(0, 49));
    CHECK(cfg::IsValidRegionCountry(1, 49));
    CHECK(cfg::IsValidRegionCountry(2, 78));
    CHECK(cfg::IsValidRegionCountry(3, 78));
    CHECK_FALSE(cfg::IsValidRegionCountry(7, 1));
}

// ---- the services, through the SVC layer ------------------------------------------------------------------------

#if !__has_include("recomp.h")
TEST_CASE("cfg_test's service tests need recomp.h (the repository around native/)" * doctest::skip()) {}
#else

#include "../src/cpu/recomp_c.h"  // last: it defines macros

namespace {

constexpr u32 kText = 0x00100000;

void Put16(std::vector<u8>& b, std::size_t at, u32 v) {
    b[at] = static_cast<u8>(v), b[at + 1] = static_cast<u8>(v >> 8);
}
void Put32(std::vector<u8>& b, std::size_t at, u32 v) {
    Put16(b, at, v & 0xFFFF), Put16(b, at + 2, v >> 16);
}

std::vector<u8> MakeElf() {
    std::vector<u8> b(52 + 32, 0);
    b[0] = 0x7F, b[1] = 'E', b[2] = 'L', b[3] = 'F', b[4] = 1, b[5] = 1, b[6] = 1;
    Put16(b, 16, 2), Put16(b, 18, 40), Put32(b, 20, 1), Put32(b, 24, kText);
    Put32(b, 28, 52), Put16(b, 40, 52), Put16(b, 42, 32), Put16(b, 44, 1);
    Put32(b, 52, 1);  // PT_LOAD
    Put32(b, 52 + 4, 84), Put32(b, 52 + 8, kText), Put32(b, 52 + 12, kText);
    Put32(b, 52 + 16, 0x40), Put32(b, 52 + 20, 0x40), Put32(b, 52 + 24, 5), Put32(b, 52 + 28, 0x1000);
    for (u32 i = 0; i < 0x40; ++i) {
        b.push_back(static_cast<u8>(i));
    }
    return b;
}

std::string WriteElf(const TempDir& dir) {
    const std::string path = dir.File("program.elf");
    WriteAll(path, MakeElf());
    return path;
}

struct Fixture {
    ManualTimeSource* time = nullptr;
    std::unique_ptr<Runtime> runtime;
    std::vector<std::string> logged;
    Fixture() {
        auto source = std::make_unique<ManualTimeSource>();
        time = source.get();
        RuntimeConfig config;
        config.time = std::move(source);
        runtime = std::make_unique<Runtime>(std::move(config));
        SetLogFilter(LogFilter(LogLevel::Info));
        SetLogSink([this](const LogRecord& r) { logged.push_back(std::string(r.message)); });
    }
    ~Fixture() {
        runtime.reset();
        SetLogSink({});
        SetLogFilter(LogFilter(LogLevel::Info));
    }
    bool Logged(const std::string& needle) const {
        return std::any_of(logged.begin(), logged.end(),
                           [&](const std::string& l) { return l.find(needle) != std::string::npos; });
    }
};

Options Parse(const std::vector<std::string>& args) {
    const Options options = ParseOptions("runner", args);
    REQUIRE_MESSAGE(options.ok(), options.error());
    return options;
}

// Boots the ELF whose main thread runs `body` (which must end the process), with the user folder of
// `dir`.
int BootWith(Fixture& f, const TempDir& dir, std::vector<std::string> extra, std::function<void()> body) {
    std::vector<std::string> args = {WriteElf(dir), "--user", dir.User()};
    args.insert(args.end(), extra.begin(), extra.end());
    f.runtime->SetMainThreadEntry([body = std::move(body)](GuestThread&) { body(); });
    return f.runtime->Boot(Parse(args));
}

struct Regs {
    std::array<u32, 16> r{};
};

Regs CallSvc(Runtime& rt, u32 number, std::initializer_list<u32> in) {
    RCtx ctx{};
    int i = 0;
    for (const u32 value : in) {
        ctx.r[i++] = value;
    }
    ctx.r[14] = kText + 0x20;
    rt.Svc().CallSvc(ctx, number, kText + 0x11);
    Regs out;
    for (int k = 0; k < 16; ++k) {
        out.r[static_cast<std::size_t>(k)] = ctx.r[k];
    }
    return out;
}

void ExitProcess(Runtime& rt) {
    CallSvc(rt, 0x03, {});
}

constexpr Result kTimeout = 0x09401BFE;

struct Guest {
    Runtime& rt;
    explicit Guest(Runtime& runtime) : rt(runtime) {}

    u32 Cmd() const { return CommandBufferAddress(*rt.MainThread()); }
    u32 Scratch(u32 offset = 0) const { return rt.MainThread()->stack_top - 0x3000 + offset; }

    void PutWords(u32 address, const std::vector<u32>& words) {
        REQUIRE(rt.Memory().WriteBlock(address, words.data(), static_cast<u32>(words.size() * 4)));
    }
    std::vector<u32> GetWords(u32 address, std::size_t count) {
        std::vector<u32> words(count);
        REQUIRE(rt.Memory().ReadBlock(words.data(), address, static_cast<u32>(count * 4)));
        return words;
    }
    std::vector<u32> Get(std::size_t count) { return GetWords(Cmd(), count); }
    void PutBytes(u32 address, const std::vector<u8>& bytes) {
        REQUIRE(rt.Memory().WriteBlock(address, bytes.data(), static_cast<u32>(bytes.size())));
    }
    std::vector<u8> GetBytes(u32 address, std::size_t count) {
        std::vector<u8> bytes(count);
        REQUIRE(rt.Memory().ReadBlock(bytes.data(), address, static_cast<u32>(count)));
        return bytes;
    }

    Result ConnectToPort(const std::string& name, Handle* handle) {
        std::vector<u8> text(name.begin(), name.end());
        text.push_back(0);
        PutBytes(Scratch(0x2000), text);
        const Regs out = CallSvc(rt, 0x2D, {0, Scratch(0x2000)});
        *handle = out.r[1];
        return out.r[0];
    }
    Result Close(Handle handle) { return CallSvc(rt, 0x23, {handle}).r[0]; }
    Result Wait(Handle handle, s64 ns) {
        return CallSvc(rt, 0x24,
                       {handle, static_cast<u32>(ns), static_cast<u32>(static_cast<u64>(ns) >> 32)})
            .r[0];
    }
    std::vector<u32> Request(Handle session, const std::vector<u32>& words, std::size_t reply_words,
                             Result* result = nullptr) {
        PutWords(Cmd(), words);
        const Result r = CallSvc(rt, 0x32, {session}).r[0];
        if (result != nullptr) {
            *result = r;
        }
        return Get(reply_words);
    }
    Handle NewEvent() { return CallSvc(rt, 0x17, {0}).r[1]; }

    // srv: GetServiceHandle of `name` (flags 1: do not wait): the handle, 0 if refused.
    Handle Service(const std::string& name) {
        Handle srv = 0;
        REQUIRE(ConnectToPort("srv:", &srv) == kResultSuccess);
        std::array<u32, 2> words{};
        std::memcpy(words.data(), name.data(), std::min<std::size_t>(name.size(), 8));
        const std::vector<u32> reply = Request(
            srv, {ipc::MakeHeader(0x0005, 4, 0), words[0], words[1], static_cast<u32>(name.size()), 1}, 4);
        Close(srv);
        return reply[1] == kResultSuccess ? reply[3] : 0;
    }

    // cfg GetConfig-like (size, id, write buffer): the result, and the bytes in *out.
    Result ReadBlock(Handle cfg, u32 command, u32 id, u32 size, std::vector<u8>* out) {
        PutBytes(Scratch(0x100), std::vector<u8>(size, 0xEE));
        const std::vector<u32> reply =
            Request(cfg,
                    {ipc::MakeHeader(static_cast<u16>(command), 2, 2), size, id,
                     ipc::MappedBufferDesc(size, ipc::W), Scratch(0x100)},
                    4);
        REQUIRE(reply[0] == ipc::MakeHeader(static_cast<u16>(command), 1, 2));
        if (out != nullptr) {
            *out = GetBytes(Scratch(0x100), size);
        }
        return reply[1];
    }
    // (header with no parameters) -> the reply's first `words`.
    std::vector<u32> Simple(Handle h, u32 header, std::size_t words) { return Request(h, {header}, words); }
};

fs::path ConfigFilePath(const TempDir& dir) {
    return fs::path(dir.User()) / "nand/data/00000000000000000000000000000000/sysdata/00010017/00000000/config";
}

}  // namespace

// The conformance program conf_services, step by step, against the default blocks: the values are
// the ones port/tests/expected/conf_services.txt records from Azahar.
TEST_CASE("cfg:u answers the defaults of a fresh user folder as Azahar does (conf_services)") {
    Fixture f;
    TempDir dir;
    std::vector<u8> bytes;
    Result language_rc, country_rc, name_rc, unknown_rc, size_rc;
    u8 language = 0;
    std::vector<u8> country, name;
    std::vector<u32> region, model, unknown;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle cfg = g.Service("cfg:u");
        REQUIRE(cfg != 0);
        language_rc = g.ReadBlock(cfg, 1, 0x000A0002, 1, &bytes);
        language = bytes[0];
        country_rc = g.ReadBlock(cfg, 1, 0x000B0000, 4, &country);
        name_rc = g.ReadBlock(cfg, 1, 0x000A0000, 0x1C, &name);
        unknown_rc = g.ReadBlock(cfg, 1, 0x00FF0000, 4, nullptr);
        size_rc = g.ReadBlock(cfg, 1, 0x000A0002, 2, nullptr);
        region = g.Simple(cfg, 0x00020000, 3);
        model = g.Simple(cfg, 0x00050000, 3);
        unknown = g.Simple(cfg, 0x00FF0000, 3);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(language_rc == kResultSuccess);
    CHECK(language == 1);
    CHECK(country_rc == kResultSuccess);
    CHECK(country == std::vector<u8>{0, 0, 2, 0x31});
    CHECK(name_rc == kResultSuccess);
    CHECK(name[0] == 'A');
    CHECK(name[10] == 'R');
    CHECK(name[12] == 0);  // "AZAHAR": 6 characters
    CHECK(unknown_rc == 0xD90103FA);
    CHECK(size_rc == 0xD90103EC);
    CHECK(region[0] == 0x00020080);
    CHECK(region[1] == kResultSuccess);
    CHECK((region[2] & 0xFF) == 0);
    CHECK(model[0] == 0x00050080);
    CHECK(model[1] == kResultSuccess);
    CHECK((model[2] & 0xFF) == 1);  // the Old 3DS XL
    CHECK(unknown[0] == 0x00FF0040);
    CHECK(unknown[1] == kResultSuccess);
    CHECK(f.Logged("unimplemented cfg:u 0x00ff0000"));
    CHECK_FALSE(fs::exists(dir.User()));  // none of it made a file
}

TEST_CASE("cfg:u reads the blocks of the user's config savegame") {
    Fixture f;
    TempDir dir;
    WriteAll(ConfigFilePath(dir).string(), UserConfigFile());
    std::vector<u8> language, name, country, birthday;
    std::vector<u32> region, model, is2ds, coppacs;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle cfg = g.Service("cfg:u");
        g.ReadBlock(cfg, 1, 0x000A0002, 1, &language);
        g.ReadBlock(cfg, 1, 0x000A0000, 0x1C, &name);
        g.ReadBlock(cfg, 1, 0x000B0000, 4, &country);
        g.ReadBlock(cfg, 1, 0x000A0001, 2, &birthday);
        region = g.Simple(cfg, 0x00020000, 3);
        model = g.Simple(cfg, 0x00050000, 3);
        is2ds = g.Simple(cfg, 0x00060000, 3);
        coppacs = g.Simple(cfg, 0x00040000, 3);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(language[0] == 2);
    CHECK(name[0] == 'H');
    CHECK(name[10] == 'r');
    CHECK(name[12] == 0);
    CHECK(country == std::vector<u8>{0, 0, 1, 78});
    CHECK(birthday == std::vector<u8>{3, 14});
    CHECK((region[2] & 0xFF) == 0);          // --region auto, no icon: JPN, as Azahar's ELF runs
    CHECK((model[2] & 0xFF) == 1);           // the file says New 3DS (2); Old 3DS mode reports the XL
    CHECK(is2ds[1] == kResultSuccess);
    CHECK(is2ds[2] == 1);                    // not a 2DS
    CHECK(coppacs[2] == 0);
}

TEST_CASE("cfg:u GetSystemModel and GetModelNintendo2DS follow Azahar's mapping to Old 3DS") {
    for (const auto& [stored, reported, is_not_2ds] :
         {std::tuple<u8, u8, u32>{0, 0, 1}, {1, 1, 1}, {2, 1, 1}, {3, 3, 0}, {4, 1, 1}, {5, 1, 1}}) {
        Fixture f;
        TempDir dir;
        WriteAll(ConfigFilePath(dir).string(),
                 MakeConfigFile({{0x000F0004, kSystem, {stored, 0, 0, 0}}}));
        std::vector<u32> model, is2ds;
        BootWith(f, dir, {"--ms", "100"}, [&] {
            Guest g(*f.runtime);
            const Handle cfg = g.Service("cfg:u");
            model = g.Simple(cfg, 0x00050000, 3);
            is2ds = g.Simple(cfg, 0x00060000, 3);
            ExitProcess(*f.runtime);
        });
        INFO("stored model " << int(stored));
        CHECK((model[2] & 0xFF) == reported);
        CHECK(is2ds[2] == is_not_2ds);
    }
}

TEST_CASE("cfg:u country codes, translation and the transferable ID") {
    Fixture f;
    TempDir dir;
    WriteAll(ConfigFilePath(dir).string(), UserConfigFile());
    std::vector<u32> string_us, string_bad, id_de, id_bad, translate_to_b, translate_to_a, translate_other;
    std::vector<u32> transferable;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle cfg = g.Service("cfg:u");
        string_us = g.Request(cfg, {ipc::MakeHeader(9, 1, 0), 49}, 3);
        string_bad = g.Request(cfg, {ipc::MakeHeader(9, 1, 0), 2}, 3);
        id_de = g.Request(cfg, {ipc::MakeHeader(10, 1, 0), 'D' | 'E' << 8}, 3);
        id_bad = g.Request(cfg, {ipc::MakeHeader(10, 1, 0), 'Z' | 'Z' << 8}, 3);
        // TranslateCountryInfo: {0,0,state 3,country 0x6E} from B to A and from A to B
        translate_to_a = g.Request(cfg, {ipc::MakeHeader(8, 2, 0), 0x6E040000, 0}, 3);
        translate_to_b = g.Request(cfg, {ipc::MakeHeader(8, 2, 0), 0x6E030000, 1}, 3);
        translate_other = g.Request(cfg, {ipc::MakeHeader(8, 2, 0), 0x31020000, 1}, 3);
        transferable = g.Request(cfg, {ipc::MakeHeader(3, 1, 0), 0xFFF12345}, 4);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(string_us[0] == ipc::MakeHeader(9, 2, 0));
    CHECK(string_us[1] == kResultSuccess);
    CHECK(string_us[2] == ('U' | 'S' << 8));
    CHECK(string_bad[1] == 0xD90103FA);
    CHECK(string_bad[2] == 0);
    CHECK(id_de[1] == kResultSuccess);
    CHECK(id_de[2] == 78);
    CHECK(id_bad[1] == 0xD90103FA);
    CHECK(id_bad[2] == 0x00FF);
    CHECK(translate_to_a[1] == kResultSuccess);
    CHECK(translate_to_a[2] == 0x6E030000);
    CHECK(translate_to_b[2] == 0x6E040000);
    CHECK(translate_other[2] == 0x31020000);  // not in the table: unchanged
    // GetTransferableId: the last 8 bytes of SHA-256(console id (8 bytes LE) + the salt's low 20 bits)
    std::array<u8, 12> input = {0x89, 0x67, 0x45, 0x23, 0x01, 0x00, 0x34, 0x12, 0x45, 0x23, 0x01, 0x00};
    const std::array<u8, 32> hash = cfg::Sha256(input.data(), input.size());
    u32 low, high;
    std::memcpy(&low, &hash[24], 4);
    std::memcpy(&high, &hash[28], 4);
    CHECK(transferable[0] == ipc::MakeHeader(3, 3, 0));
    CHECK(transferable[1] == kResultSuccess);
    CHECK(transferable[2] == low);
    CHECK(transferable[3] == high);
}

TEST_CASE("cfg:u GetRegion follows --region, and a bad --region stops the run") {
    for (int region = 0; region <= 6; ++region) {
        Fixture f;
        TempDir dir;
        std::vector<u32> reply, coppacs;
        BootWith(f, dir, {"--ms", "100", "--region", std::to_string(region)}, [&] {
            Guest g(*f.runtime);
            const Handle cfg = g.Service("cfg:u");
            reply = g.Simple(cfg, 0x00020000, 3);
            coppacs = g.Simple(cfg, 0x00040000, 3);
            ExitProcess(*f.runtime);
        });
        INFO("region " << region);
        CHECK((reply[2] & 0xFF) == static_cast<u32>(region));
        CHECK(coppacs[2] == (region == 1 ? 1u : 0u));
    }
    Fixture f;
    TempDir dir;
    bool ran = false;
    const int status = BootWith(f, dir, {"--ms", "100", "--region", "9"}, [&] {
        ran = true;
        ExitProcess(*f.runtime);
    });
    CHECK(status == 2);
    CHECK_FALSE(ran);
    CHECK(f.Logged("bad --region"));
}

TEST_CASE("cfg:s and cfg:i save the config; cfg:u cannot reach their commands") {
    Fixture f;
    TempDir dir;
    WriteAll(ConfigFilePath(dir).string(), UserConfigFile());
    std::vector<u32> as_user, set_reply, read_back, update, system_read_of_system_block, user_read_of_system_block;
    std::vector<u8> language_after, model_system, model_user;
    Result system_rc = 0, user_rc = 0;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle user = g.Service("cfg:u");
        const Handle sys = g.Service("cfg:s");
        const Handle internal = g.Service("cfg:i");
        REQUIRE(sys != 0);
        REQUIRE(internal != 0);
        // cfg:u has no 0x0401
        as_user = g.Request(user, {ipc::MakeHeader(0x0401, 2, 2), 4, 0x000F0004,
                                   ipc::MappedBufferDesc(4, ipc::W), g.Scratch(0x100)}, 3);
        // the model block is system-only: cfg:u cannot read it, cfg:s can
        user_rc = g.ReadBlock(user, 1, 0x000F0004, 4, &model_user);
        system_rc = g.ReadBlock(sys, 0x0401, 0x000F0004, 4, &model_system);
        // SetSystemConfig: language block (id, size, read buffer) := 3; cfg:s keeps it in memory
        g.PutBytes(g.Scratch(0x200), {3});
        set_reply = g.Request(sys, {ipc::MakeHeader(0x0402, 2, 2), 0x000A0002, 1,
                                    ipc::MappedBufferDesc(1, ipc::R), g.Scratch(0x200)}, 4);
        g.ReadBlock(user, 1, 0x000A0002, 1, &language_after);
        // saves
        update = g.Simple(internal, ipc::MakeHeader(0x0803, 0, 0), 2);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(as_user[0] == ipc::MakeHeader(0x0401, 1, 0));
    CHECK(as_user[1] == kResultSuccess);
    CHECK(user_rc == 0xD90103EA);
    CHECK(system_rc == kResultSuccess);
    CHECK(model_system[0] == 2);
    CHECK(set_reply[0] == ipc::MakeHeader(0x0402, 1, 2));
    CHECK(set_reply[1] == kResultSuccess);
    CHECK(language_after[0] == 3);
    CHECK(update[0] == ipc::MakeHeader(0x0803, 1, 0));
    CHECK(update[1] == kResultSuccess);
    cfg::ConfigStore saved(dir.User());
    CHECK(saved.FromFile());
    CHECK(saved.GetLanguageRaw() == 3);
    CHECK(saved.GetUsername() == u"Hunter");
}

// A second service manager over the same runtime whose game folder is `game` (a Runtime started from
// an ELF has none): the SVC layer then reaches the services made from it.
void UseGameFolder(Runtime& rt, const std::string& game) {
    ServiceEnvironment env;
    env.game_folder = game;
    env.user_folder = rt.UserFolder();
    env.options = &rt.GetOptions();
    env.kernel = &rt.GetKernel();
    env.scheduler = &rt.Scheduler();
    env.threads = &rt.Threads();
    env.interfaces = &rt.Interfaces();
    env.request_exit = [&rt](int status, const std::string& reason) { rt.RequestExit(status, reason); };
    auto manager = std::make_shared<ServiceManager>(std::move(env));
    manager->InstallRegistered();
    manager->Install(rt.Svc());
    rt.Own(manager);
}

std::vector<u8> SmdhWithLockout(u32 lockout) {
    std::vector<u8> smdh(0x36C0, 0);
    Set32(smdh, 0x2018, lockout);
    return smdh;
}

TEST_CASE("cfg:u and cfg:s answer an invalid size for a block size above 0x8000, and leave the buffer alone") {
    Fixture f;
    TempDir dir;
    WriteAll(ConfigFilePath(dir).string(), UserConfigFile());
    std::vector<u32> user_known, user_missing, sys_known, sys_missing, set_big, set_beyond;
    std::vector<u8> buffers[4], country_after;
    const int status = BootWith(f, dir, {"--ms", "100", "--region", "1"}, [&] {
        Guest g(*f.runtime);
        const Handle user = g.Service("cfg:u");
        const Handle sys = g.Service("cfg:s");
        REQUIRE(user != 0);
        REQUIRE(sys != 0);
        auto get = [&](Handle h, u32 command, u32 size, u32 id, std::vector<u8>* buffer) {
            g.PutBytes(g.Scratch(0x100), std::vector<u8>(8, 0xEE));
            auto reply = g.Request(h, {ipc::MakeHeader(static_cast<u16>(command), 2, 2), size, id,
                                       ipc::MappedBufferDesc(8, ipc::W), g.Scratch(0x100)}, 4);
            *buffer = g.GetBytes(g.Scratch(0x100), 8);
            return reply;
        };
        user_known = get(user, 1, 0x8001, 0x000A0002, &buffers[0]);
        user_missing = get(user, 1, 0xFFFFFFFF, 0x00FF0000, &buffers[1]);  // not NotFound
        sys_known = get(sys, 0x0401, 0x8001, 0x000F0004, &buffers[2]);
        sys_missing = get(sys, 0x0401, 0x10000, 0x00FF0000, &buffers[3]);
        // SetSystemConfig of the country block (4 bytes): a size above the buffer, then above 0x8000
        g.PutBytes(g.Scratch(0x200), std::vector<u8>(8, 0x55));
        set_beyond = g.Request(sys, {ipc::MakeHeader(0x0402, 2, 2), 0x000B0000, 4,
                                     ipc::MappedBufferDesc(2, ipc::R), g.Scratch(0x200)}, 4);
        set_big = g.Request(sys, {ipc::MakeHeader(0x0402, 2, 2), 0x000B0000, 0x8001,
                                  ipc::MappedBufferDesc(8, ipc::R), g.Scratch(0x200)}, 4);
        g.ReadBlock(user, 1, 0x000B0000, 4, &country_after);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    const std::vector<u32>* gets[] = {&user_known, &user_missing, &sys_known, &sys_missing};
    const u32 commands[] = {1, 1, 0x0401, 0x0401};
    for (int i = 0; i < 4; ++i) {
        INFO("get " << i);
        CHECK((*gets[i])[0] == ipc::MakeHeader(static_cast<u16>(commands[i]), 1, 2));
        CHECK((*gets[i])[1] == 0xD90103EC);
        CHECK(buffers[i] == std::vector<u8>(8, 0xEE));
    }
    for (const std::vector<u32>* set : {&set_beyond, &set_big}) {
        CHECK((*set)[0] == ipc::MakeHeader(0x0402, 1, 2));
        CHECK((*set)[1] == 0xD90103EC);
    }
    CHECK(country_after == std::vector<u8>{0, 0, 1, 78});  // the block is unchanged
}

TEST_CASE("cfg:u and cfg:s answer InvalidBufferDescriptor when the buffer argument is not a mapped buffer") {
    Fixture f;
    TempDir dir;
    WriteAll(ConfigFilePath(dir).string(), UserConfigFile());
    std::vector<u32> get, get_system, set;
    std::vector<u8> country_after;
    const int status = BootWith(f, dir, {"--ms", "100", "--region", "1"}, [&] {
        Guest g(*f.runtime);
        const Handle user = g.Service("cfg:u");
        const Handle sys = g.Service("cfg:s");
        REQUIRE(user != 0);
        REQUIRE(sys != 0);
        // the two translate words are 0 (an immediate-handle-less, not mapped-buffer, descriptor)
        get = g.Request(user, {ipc::MakeHeader(0x0001, 2, 2), 4, 0x000B0000, 0, 0}, 2);
        get_system = g.Request(sys, {ipc::MakeHeader(0x0401, 2, 2), 4, 0x000B0000, 0, 0}, 2);
        set = g.Request(sys, {ipc::MakeHeader(0x0402, 2, 2), 0x000B0000, 4, 0, 0}, 2);
        g.ReadBlock(user, 1, 0x000B0000, 4, &country_after);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(get[0] == ipc::MakeHeader(0x0001, 1, 0));
    CHECK(get[1] == kResultInvalidBufferDescriptor);
    CHECK(get_system[0] == ipc::MakeHeader(0x0401, 1, 0));
    CHECK(get_system[1] == kResultInvalidBufferDescriptor);
    CHECK(set[0] == ipc::MakeHeader(0x0402, 1, 0));
    CHECK(set[1] == kResultInvalidBufferDescriptor);
    CHECK(country_after == std::vector<u8>{0, 0, 1, 78});
}

TEST_CASE("cfg:u --region auto takes the region lockout of the game's icon.bin") {
    // USA only: GetRegion 1, IsCoppacsSupported 1, and French (a USA language) stays
    {
        Fixture f;
        TempDir dir;
        const std::string game = dir.File("game");
        WriteAll(game + "/icon.bin", SmdhWithLockout(0b10));
        WriteAll(ConfigFilePath(dir).string(), UserConfigFile());
        const std::vector<u8> before = ReadAll(ConfigFilePath(dir).string());
        std::vector<u32> region, coppacs;
        std::vector<u8> language;
        const int status = BootWith(f, dir, {"--ms", "100", "--region", "auto"}, [&] {
            UseGameFolder(*f.runtime, game);
            Guest g(*f.runtime);
            const Handle cfg = g.Service("cfg:u");
            REQUIRE(cfg != 0);
            region = g.Simple(cfg, 0x00020000, 3);
            coppacs = g.Simple(cfg, 0x00040000, 3);
            g.ReadBlock(cfg, 1, 0x000A0002, 1, &language);
            ExitProcess(*f.runtime);
        });
        CHECK(status == 0);
        CHECK(region[0] == 0x00020080);
        CHECK(region[1] == kResultSuccess);
        CHECK((region[2] & 0xFF) == 1);
        CHECK(coppacs[1] == kResultSuccess);
        CHECK(coppacs[2] == 1);
        CHECK(language == std::vector<u8>{2});
        CHECK(f.Logged("Preferred region code set to 1"));
        CHECK_FALSE(f.Logged("does not fit the region"));
        CHECK(ReadAll(ConfigFilePath(dir).string()) == before);
    }
    // JPN only: GetRegion 0, and French is not a Japan language, so the language becomes Japanese (0)
    // in memory; the file is not written
    {
        Fixture f;
        TempDir dir;
        const std::string game = dir.File("game");
        WriteAll(game + "/exefs/icon.bin", SmdhWithLockout(0b1));
        WriteAll(ConfigFilePath(dir).string(), UserConfigFile());
        const std::vector<u8> before = ReadAll(ConfigFilePath(dir).string());
        std::vector<u32> region, coppacs;
        std::vector<u8> language;
        const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
            UseGameFolder(*f.runtime, game);
            Guest g(*f.runtime);
            const Handle cfg = g.Service("cfg:u");
            REQUIRE(cfg != 0);
            region = g.Simple(cfg, 0x00020000, 3);
            coppacs = g.Simple(cfg, 0x00040000, 3);
            g.ReadBlock(cfg, 1, 0x000A0002, 1, &language);
            ExitProcess(*f.runtime);
        });
        CHECK(status == 0);
        CHECK((region[2] & 0xFF) == 0);
        CHECK(coppacs[2] == 0);
        CHECK(language == std::vector<u8>{0});
        CHECK(f.Logged("System language 2 does not fit the region. Adjusted to 0"));
        CHECK(ReadAll(ConfigFilePath(dir).string()) == before);
    }
}

TEST_CASE("cfg:u --region auto without an icon.bin warns once that GetRegion answers JPN") {
    Fixture f;
    TempDir dir;
    const std::string game = dir.File("game");
    fs::create_directories(game);
    std::vector<u32> first, second;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        UseGameFolder(*f.runtime, game);
        Guest g(*f.runtime);
        const Handle cfg = g.Service("cfg:u");
        REQUIRE(cfg != 0);
        first = g.Simple(cfg, 0x00020000, 3);
        second = g.Simple(cfg, 0x00020000, 3);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK((first[2] & 0xFF) == 0);
    CHECK((second[2] & 0xFF) == 0);
    const std::string warning = "give --region or provide icon.bin";
    CHECK(std::count_if(f.logged.begin(), f.logged.end(), [&](const std::string& l) {
              return l.find(warning) != std::string::npos;
          }) == 1);
    // with --region given there is nothing to warn about
    Fixture g2;
    TempDir dir2;
    BootWith(g2, dir2, {"--ms", "100", "--region", "2"}, [&] {
        UseGameFolder(*g2.runtime, game);
        Guest g(*g2.runtime);
        g.Simple(g.Service("cfg:u"), 0x00020000, 3);
        ExitProcess(*g2.runtime);
    });
    CHECK_FALSE(g2.Logged(warning));
}

TEST_CASE("cfg:i FormatConfig puts the defaults back with a new console unique ID") {
    Fixture f;
    TempDir dir;
    WriteAll(ConfigFilePath(dir).string(), UserConfigFile());
    std::vector<u32> reply;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle internal = g.Service("cfg:i");
        reply = g.Simple(internal, ipc::MakeHeader(0x0806, 0, 0), 2);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(reply[1] == kResultSuccess);
    cfg::ConfigStore saved(dir.User());
    CHECK(saved.FromFile());
    CHECK(saved.GetUsername() == u"AZAHAR");
    CHECK(saved.GetLanguageRaw() == 1);
    const u64 id = saved.GetConsoleUniqueId();
    CHECK(id != 0x1234000123456789ull);
    u32 random = 0;
    CHECK(saved.Get(0x00090002, 4, cfg::kUserRead, &random) == kResultSuccess);
    CHECK(random == (id >> 48));
}

TEST_CASE("cfg:s keeps the UUID clock sequence in mcu.dat") {
    Fixture f;
    TempDir dir;
    std::vector<u32> before, set, after;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle sys = g.Service("cfg:s");
        before = g.Simple(sys, ipc::MakeHeader(0x040E, 0, 0), 3);
        REQUIRE(!fs::exists(fs::path(dir.User()) / "sysdata/mcu.dat"));  // reading made nothing
        set = g.Request(sys, {ipc::MakeHeader(0x040D, 1, 0), 0x1234}, 2);
        after = g.Simple(sys, ipc::MakeHeader(0x040E, 0, 0), 3);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(before[0] == ipc::MakeHeader(0x040E, 2, 0));
    CHECK(before[2] == 0);
    CHECK(set[1] == kResultSuccess);
    CHECK(after[2] == 0x1234);
    CHECK(cfg::ReadMcuData(dir.User()).clock_sequence == 0x1234);
}

// ---- --new-console-id ---------------------------------------------------------------------------------------------

TEST_CASE("--new-console-id gives the console a new random ID, kept in the user folder") {
    TempDir dir;
    std::vector<u64> ids;
    for (int run = 0; run < 2; ++run) {
        Fixture f;
        std::vector<u8> id_block, random_block;
        u32 transferable = 0;
        const int status = BootWith(f, dir, {"--ms", "100", "--new-console-id"}, [&] {
            Guest g(*f.runtime);
            const Handle cfg = g.Service("cfg:u");
            g.ReadBlock(cfg, 1, 0x00090001, 8, &id_block);
            g.ReadBlock(cfg, 1, 0x00090002, 4, &random_block);
            transferable = g.Request(cfg, {ipc::MakeHeader(3, 1, 0), 0}, 4)[1];
            ExitProcess(*f.runtime);
        });
        CHECK(status == 0);
        CHECK(transferable == kResultSuccess);
        u64 id = 0;
        std::memcpy(&id, id_block.data(), 8);
        u32 random = 0;
        std::memcpy(&random, random_block.data(), 4);
        CHECK(random == (id >> 48));                      // Azahar: the random number is the top 16 bits
        CHECK((id & 0x0000FFFC'00000000ull) == 0);        // and the seed has 34 bits
        CHECK(f.Logged("console id "));
        ids.push_back(id);
        // the file has it, with the other blocks at their defaults
        cfg::ConfigStore saved(dir.User());
        CHECK(saved.FromFile());
        CHECK(saved.GetConsoleUniqueId() == id);
        CHECK(saved.GetUsername() == u"AZAHAR");
    }
    // and without the option a run keeps what the file says
    Fixture f;
    std::vector<u8> id_block;
    BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        g.ReadBlock(g.Service("cfg:u"), 1, 0x00090001, 8, &id_block);
        ExitProcess(*f.runtime);
    });
    u64 kept = 0;
    std::memcpy(&kept, id_block.data(), 8);
    CHECK(kept == ids[1]);
    CHECK(ids[0] != ids[1]);
}

TEST_CASE("--new-console-id keeps the rest of an existing config savegame") {
    Fixture f;
    TempDir dir;
    WriteAll(ConfigFilePath(dir).string(), UserConfigFile());
    const int status = BootWith(f, dir, {"--ms", "100", "--new-console-id"}, [&] { ExitProcess(*f.runtime); });
    CHECK(status == 0);
    cfg::ConfigStore saved(dir.User());
    CHECK(saved.GetUsername() == u"Hunter");
    CHECK(saved.GetLanguageRaw() == 2);
    CHECK(saved.GetConsoleUniqueId() != 0x1234000123456789ull);
}

// ---- ptm -----------------------------------------------------------------------------------------------------------

TEST_CASE("ptm:u answers Azahar's defaults (conf_services)") {
    Fixture f;
    TempDir dir;
    std::vector<u32> level, charging, adapter, shell, pedometer, steps_total, unknown, history, new3ds;
    std::vector<u32> wrong_history;
    std::vector<u8> history_bytes, wrong_bytes;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle ptm = g.Service("ptm:u");
        REQUIRE(ptm != 0);
        level = g.Simple(ptm, 0x00070000, 3);
        charging = g.Simple(ptm, 0x00080000, 3);
        adapter = g.Simple(ptm, 0x00050000, 3);
        shell = g.Simple(ptm, 0x00060000, 3);
        pedometer = g.Simple(ptm, 0x00090000, 3);
        steps_total = g.Simple(ptm, 0x000C0000, 3);
        unknown = g.Simple(ptm, 0x00FF0000, 3);
        g.PutBytes(g.Scratch(0x100), std::vector<u8>(8, 0xEE));
        history = g.Request(ptm, {ipc::MakeHeader(0x000B, 3, 2), 4, 0, 0, ipc::MappedBufferDesc(8, ipc::W),
                                  g.Scratch(0x100)}, 4);
        history_bytes = g.GetBytes(g.Scratch(0x100), 8);
        g.PutBytes(g.Scratch(0x100), std::vector<u8>(8, 0xEE));
        wrong_history = g.Request(ptm, {ipc::MakeHeader(0x000B, 3, 2), 5, 0, 0,
                                        ipc::MappedBufferDesc(8, ipc::W), g.Scratch(0x100)}, 4);
        wrong_bytes = g.GetBytes(g.Scratch(0x100), 8);
        new3ds = g.Simple(ptm, ipc::MakeHeader(0x040A, 0, 0), 3);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(level[0] == 0x00070080);
    CHECK(level[1] == kResultSuccess);
    CHECK(level[2] == 5);  // completely full
    CHECK(charging[0] == 0x00080080);
    CHECK(charging[2] == 1);
    CHECK(adapter[0] == 0x00050080);
    CHECK(adapter[2] == 1);
    CHECK(shell[0] == 0x00060080);
    CHECK(shell[2] == 1);
    CHECK(pedometer[2] == 0);
    CHECK(steps_total[0] == 0x000C0080);
    CHECK(steps_total[1] == kResultSuccess);
    CHECK(steps_total[2] == 0);
    CHECK(unknown[0] == 0x00FF0040);
    CHECK(unknown[1] == kResultSuccess);
    CHECK(history[0] == ipc::MakeHeader(0x000B, 1, 2));
    CHECK(history[1] == kResultSuccess);
    CHECK(history_bytes == std::vector<u8>(8, 0));
    // 5 hours do not fit 8 bytes: the invalid size result, the buffer handed back and not written
    REQUIRE(wrong_history.size() == 4);
    CHECK(wrong_history[0] == ipc::MakeHeader(0x000B, 1, 2));
    CHECK(wrong_history[1] == MakeResult(1004, 53, 8, 27));
    CHECK(wrong_history[2] == ipc::MappedBufferDesc(8, ipc::W));
    CHECK(wrong_bytes == std::vector<u8>(8, 0xEE));
    CHECK(new3ds[0] == 0x040A0040);       // ptm:u has no 0x040A in Azahar: the answer to an unknown command
}

TEST_CASE("ptm:sysm answers CheckNew3DS as Old 3DS") {
    Fixture f;
    TempDir dir;
    std::vector<u32> new3ds, closed, shell_sysm, shell_s;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle sysm = g.Service("ptm:sysm");
        REQUIRE(sysm != 0);
        new3ds = g.Simple(sysm, ipc::MakeHeader(0x040A, 0, 0), 3);
        closed = g.Simple(sysm, ipc::MakeHeader(0x080F, 0, 0), 3);
        shell_sysm = g.Simple(sysm, ipc::MakeHeader(0x0811, 0, 0), 3);
        const Handle ptm_s = g.Service("ptm:s");
        REQUIRE(ptm_s != 0);
        shell_s = g.Simple(ptm_s, ipc::MakeHeader(0x0811, 0, 0), 3);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(new3ds[0] == ipc::MakeHeader(0x040A, 2, 0));
    CHECK(new3ds[1] == kResultSuccess);
    CHECK(new3ds[2] == 0);
    CHECK(closed[2] == 0);
    for (const std::vector<u32>* shell : {&shell_sysm, &shell_s}) {  // GetShellState: the shell is open
        CHECK((*shell)[0] == ipc::MakeHeader(0x0811, 2, 0));
        CHECK((*shell)[1] == kResultSuccess);
        CHECK((*shell)[2] == 1);
    }
}

// ---- ndm -----------------------------------------------------------------------------------------------------------

TEST_CASE("ndm:u keeps the daemon state Azahar's stub keeps") {
    Fixture f;
    TempDir dir;
    std::vector<std::vector<u32>> r;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle ndm = g.Service("ndm:u");
        REQUIRE(ndm != 0);
        r.push_back(g.Simple(ndm, ipc::MakeHeader(0x0016, 0, 0), 3));             // 0 default daemons
        r.push_back(g.Request(ndm, {ipc::MakeHeader(0x000D, 1, 0), 0}, 3));      // 1 status of cec: idle
        r.push_back(g.Request(ndm, {ipc::MakeHeader(0x0007, 1, 0), 2}, 2));      // 2 resume without suspend
        r.push_back(g.Request(ndm, {ipc::MakeHeader(0x0006, 1, 0), 2}, 2));      // 3 suspend boss
        r.push_back(g.Request(ndm, {ipc::MakeHeader(0x000D, 1, 0), 1}, 3));      // 4 boss: suspended
        r.push_back(g.Request(ndm, {ipc::MakeHeader(0x000E, 1, 0), 1}, 4));      // 5 disable count 1
        r.push_back(g.Request(ndm, {ipc::MakeHeader(0x0007, 1, 0), 2}, 2));      // 6 resume
        r.push_back(g.Request(ndm, {ipc::MakeHeader(0x000D, 1, 0), 1}, 3));      // 7 idle again
        r.push_back(g.Simple(ndm, ipc::MakeHeader(0x0011, 0, 0), 3));             // 8 scan interval 30
        r.push_back(g.Request(ndm, {ipc::MakeHeader(0x0012, 1, 0), 77}, 2));     // 9 set retry
        r.push_back(g.Simple(ndm, ipc::MakeHeader(0x0013, 0, 0), 3));             // 10 retry 77
        r.push_back(g.Request(ndm, {ipc::MakeHeader(0x0014, 1, 0), 0xF}, 2));    // 11 override defaults
        r.push_back(g.Simple(ndm, ipc::MakeHeader(0x0016, 0, 0), 3));             // 12 0xF
        r.push_back(g.Simple(ndm, ipc::MakeHeader(0x0015, 0, 0), 2));             // 13 reset
        r.push_back(g.Simple(ndm, ipc::MakeHeader(0x0016, 0, 0), 3));             // 14 back to 9
        r.push_back(g.Request(ndm, {ipc::MakeHeader(0x0001, 1, 2), 2, ipc::CallingPidDesc(), 0}, 2));  // 15
        r.push_back(g.Simple(ndm, ipc::MakeHeader(0x0003, 0, 0), 3));             // 16 exclusive state 2
        r.push_back(g.Request(ndm, {ipc::MakeHeader(0x000D, 1, 0), 9}, 3));      // 17 daemon 9: out of range
        r.push_back(g.Simple(ndm, ipc::MakeHeader(0x000F, 0, 0), 4));             // 18 scheduler count
        r.push_back(g.Simple(ndm, 0x000A0000, 2));                                // 19 GetCurrentState: unimplemented
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    REQUIRE(r.size() == 20);
    CHECK(r[0][2] == 9);   // Cec | Friend
    CHECK(r[1][1] == kResultSuccess);
    CHECK(r[1][2] == 1);   // idle
    CHECK(r[2][1] == MakeResult(13, 26, 5, 25));
    CHECK(r[3][1] == kResultSuccess);
    CHECK(r[4][2] == 3);   // suspended
    CHECK(r[5][2] == 1);
    CHECK(r[5][3] == 1);
    CHECK(r[6][1] == kResultSuccess);
    CHECK(r[7][2] == 1);
    CHECK(r[8][2] == 30);
    CHECK(r[10][2] == 77);
    CHECK(r[12][2] == 0xF);
    CHECK(r[14][2] == 9);
    CHECK(r[15][0] == ipc::MakeHeader(0x0001, 1, 0));
    CHECK(r[15][1] == kResultSuccess);
    CHECK(r[16][2] == 2);
    CHECK(r[17][1] == MakeResult(1021, 26, 8, 27));
    CHECK(r[18][1] == kResultSuccess);
    CHECK(r[18][2] == 0);
    CHECK(r[18][3] == 0);
    CHECK(r[19][0] == 0x000A0040);  // answered as an unknown command
    CHECK(f.Logged("unimplemented ndm:u 0x000a0000"));
}

// ---- ac ------------------------------------------------------------------------------------------------------------

TEST_CASE("ac:u reports the internet, wifi slot 1, and connects at once (conf_services)") {
    Fixture f;
    TempDir dir;
    std::vector<u32> status_r, wifi, is_connected_before, connect, is_connected_after, connect_result, close,
        is_connected_closed, cancel_closed, proxy, infra, unknown;
    Result connect_event_wait = 0, connect_event_wait_again = 0, close_event_wait = 0, disconnect_event_wait = 0;
    std::vector<u32> cancel_connected;
    const int exit_status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle ac = g.Service("ac:u");
        REQUIRE(ac != 0);
        status_r = g.Simple(ac, 0x000C0000, 3);
        wifi = g.Simple(ac, 0x000D0000, 3);
        is_connected_before = g.Request(ac, {ipc::MakeHeader(0x003E, 1, 2), 0, ipc::MoveHandleDesc(1), 0}, 3);
        const Handle connect_event = g.NewEvent();
        const Handle disconnect_event = g.NewEvent();
        const Handle close_event = g.NewEvent();
        REQUIRE(connect_event != 0);
        g.PutBytes(g.Scratch(0x300), std::vector<u8>(0x200, 0));
        // RegisterDisconnectEvent (pid, event)
        g.Request(ac, {ipc::MakeHeader(0x0030, 0, 4), ipc::CallingPidDesc(), 0, ipc::CopyHandleDesc(1), disconnect_event}, 2);
        // ConnectAsync (pid, event, config buffer)
        connect = g.Request(ac, {ipc::MakeHeader(0x0004, 0, 6), ipc::CallingPidDesc(), 0, ipc::CopyHandleDesc(1),
                                 connect_event, ipc::StaticBufferDesc(0x200, 0), g.Scratch(0x300)}, 2);
        connect_event_wait = g.Wait(connect_event, 0);
        connect_event_wait_again = g.Wait(connect_event, 0);  // one shot: consumed
        is_connected_after = g.Request(ac, {ipc::MakeHeader(0x003E, 1, 2), 0, ipc::MoveHandleDesc(1), 0}, 3);
        connect_result = g.Request(ac, {ipc::MakeHeader(0x0005, 0, 2), ipc::CallingPidDesc(), 0}, 2);
        cancel_connected = g.Request(ac, {ipc::MakeHeader(0x0007, 0, 2), ipc::CallingPidDesc(), 0}, 2);
        close = g.Request(ac, {ipc::MakeHeader(0x0008, 0, 4), ipc::CallingPidDesc(), 0, ipc::CopyHandleDesc(1),
                               close_event}, 2);
        close_event_wait = g.Wait(close_event, 0);
        disconnect_event_wait = g.Wait(disconnect_event, 0);
        is_connected_closed = g.Request(ac, {ipc::MakeHeader(0x003E, 1, 2), 0, ipc::MoveHandleDesc(1), 0}, 3);
        cancel_closed = g.Request(ac, {ipc::MakeHeader(0x0007, 0, 2), ipc::CallingPidDesc(), 0}, 2);
        proxy = g.Simple(ac, 0x00360000, 3);
        infra = g.Request(ac, {ipc::MakeHeader(0x0027, 0, 2), ipc::StaticBufferDesc(0x200, 0), g.Scratch(0x300)}, 3);
        unknown = g.Simple(ac, 0x00FF0000, 3);
        ExitProcess(*f.runtime);
    });
    CHECK(exit_status == 0);
    CHECK(status_r[0] == 0x000C0080);
    CHECK(status_r[2] == 3);
    CHECK(wifi[0] == 0x000D0080);
    CHECK(wifi[2] == 1);
    CHECK(is_connected_before[0] == 0x003E0080);
    CHECK(is_connected_before[1] == kResultSuccess);
    CHECK(is_connected_before[2] == 0);
    CHECK(connect[1] == kResultSuccess);
    CHECK(connect_event_wait == kResultSuccess);  // signalled by ConnectAsync
    CHECK(connect_event_wait_again == kTimeout);
    CHECK(is_connected_after[2] == 1);
    CHECK(connect_result[1] == kResultSuccess);
    CHECK(cancel_connected[1] == MakeResult(301, 39, 5, 28));
    CHECK(close[1] == kResultSuccess);
    CHECK(close_event_wait == kResultSuccess);
    CHECK(disconnect_event_wait == kResultSuccess);  // connected when closed
    CHECK(is_connected_closed[2] == 0);
    CHECK(cancel_closed[1] == MakeResult(302, 39, 5, 28));
    CHECK(proxy[2] == 0);
    CHECK(infra[1] == kResultSuccess);
    CHECK(infra[2] == 0);
    CHECK(unknown[0] == 0x00FF0040);
    CHECK(unknown[1] == kResultSuccess);
}

TEST_CASE("ac:u is shared by its sessions, and CreateDefaultConfig gives 0x200 zero bytes") {
    Fixture f;
    TempDir dir;
    std::vector<u32> first_status, after_connect_in_second, create;
    std::vector<u8> create_bytes;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle one = g.Service("ac:u");
        const Handle two = g.Service("ac:u");
        const Handle event = g.NewEvent();
        g.PutBytes(g.Scratch(0x300), std::vector<u8>(0x200, 0));
        g.Request(one, {ipc::MakeHeader(0x0004, 0, 6), ipc::CallingPidDesc(), 0, ipc::CopyHandleDesc(1), event,
                        ipc::StaticBufferDesc(0x200, 0), g.Scratch(0x300)}, 2);
        after_connect_in_second = g.Request(two, {ipc::MakeHeader(0x003E, 1, 2), 0, ipc::MoveHandleDesc(1), 0}, 3);
        // CreateDefaultConfig: the reply's static buffer 0 is received at Scratch(0x500)
        g.PutWords(g.Cmd() + 4 * static_cast<u32>(ipc::StaticBufferDescriptorWord(0)),
                   {ipc::StaticBufferDesc(0x200, 0), g.Scratch(0x500)});
        g.PutBytes(g.Scratch(0x500), std::vector<u8>(0x200, 0xEE));
        create = g.Request(one, {ipc::MakeHeader(0x0001, 0, 0)}, 4);
        create_bytes = g.GetBytes(g.Scratch(0x500), 0x200);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(after_connect_in_second[2] == 1);
    REQUIRE(create.size() == 4);
    CHECK(create[0] == ipc::MakeHeader(0x0001, 1, 2));
    CHECK(create[1] == kResultSuccess);
    CHECK(create[2] == ipc::StaticBufferDesc(0x200, 0));
    CHECK(create_bytes == std::vector<u8>(0x200, 0));
}

TEST_CASE("ac:u SetRequestEulaVersion returns the config, GetNZoneBeaconNotFoundEvent signals, and ac:i shares the state") {
    Fixture f;
    TempDir dir;
    std::vector<u32> eula, beacon, beacon_without_event, internal_connected, unknown_internal;
    std::vector<u8> eula_bytes;
    Result beacon_wait = 0;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle user = g.Service("ac:u");
        const Handle internal = g.Service("ac:i");
        REQUIRE(user != 0);
        REQUIRE(internal != 0);
        // SetRequestEulaVersion (major, minor, config buffer): the config comes back in static buffer 0
        std::vector<u8> config(0x200);
        for (std::size_t i = 0; i < config.size(); ++i) {
            config[i] = static_cast<u8>(i * 7 + 1);
        }
        g.PutBytes(g.Scratch(0x300), config);
        g.PutWords(g.Cmd() + 4 * static_cast<u32>(ipc::StaticBufferDescriptorWord(0)),
                   {ipc::StaticBufferDesc(0x200, 0), g.Scratch(0x500)});
        g.PutBytes(g.Scratch(0x500), std::vector<u8>(0x200, 0));
        eula = g.Request(user, {ipc::MakeHeader(0x002D, 2, 2), 3, 4, ipc::StaticBufferDesc(0x200, 0),
                                g.Scratch(0x300)}, 4);
        eula_bytes = g.GetBytes(g.Scratch(0x500), 0x200);
        // GetNZoneBeaconNotFoundEvent (pid, event): the event is signalled
        const Handle event = g.NewEvent();
        beacon = g.Request(user, {ipc::MakeHeader(0x002F, 0, 4), ipc::CallingPidDesc(), 0, ipc::CopyHandleDesc(1),
                                  event}, 2);
        beacon_wait = g.Wait(event, 0);
        // without an event handle the command still answers success (the guard in the service)
        beacon_without_event = g.Request(user, {ipc::MakeHeader(0x002F, 0, 2), ipc::CallingPidDesc(), 0}, 2);
        // ConnectAsync through ac:u is seen by ac:i (one shared state)
        const Handle connect_event = g.NewEvent();
        g.PutBytes(g.Scratch(0x300), std::vector<u8>(0x200, 0));
        g.Request(user, {ipc::MakeHeader(0x0004, 0, 6), ipc::CallingPidDesc(), 0, ipc::CopyHandleDesc(1),
                         connect_event, ipc::StaticBufferDesc(0x200, 0), g.Scratch(0x300)}, 2);
        internal_connected = g.Request(internal, {ipc::MakeHeader(0x003E, 1, 2), 0, ipc::MoveHandleDesc(1), 0}, 3);
        unknown_internal = g.Simple(internal, 0x00FF0000, 3);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    REQUIRE(eula.size() == 4);
    CHECK(eula[0] == ipc::MakeHeader(0x002D, 1, 2));
    CHECK(eula[1] == kResultSuccess);
    CHECK(eula[2] == ipc::StaticBufferDesc(0x200, 0));
    std::vector<u8> expected(0x200);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        expected[i] = static_cast<u8>(i * 7 + 1);
    }
    CHECK(eula_bytes == expected);
    CHECK(beacon[0] == ipc::MakeHeader(0x002F, 1, 0));
    CHECK(beacon[1] == kResultSuccess);
    CHECK(beacon_wait == kResultSuccess);
    CHECK(beacon_without_event[1] == kResultSuccess);
    CHECK(internal_connected[1] == kResultSuccess);
    CHECK(internal_connected[2] == 1);
    CHECK(unknown_internal[0] == 0x00FF0040);
    CHECK(unknown_internal[1] == kResultSuccess);
}

#endif  // recomp.h
