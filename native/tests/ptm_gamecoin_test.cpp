// Derived from Azahar src/core/hle/service/ptm/ptm.cpp and src/core/hle/service/ptm/ptm.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: both say "GPLv2 or any later version"): the
// default play coin values and the layout of GameCoin, written out here as bytes.
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (ptm.cpp, ptm.h)
//
// The play coin file that the ptm boot step leaves in the user folder (N1-12b), in temporary folders.
// The step runs through Runtime::Prepare, the replay tool's way in. That fs:USER then opens the archive
// with the id the game sends and reads the coins is a test of fs_save_test.cpp (it has the guest harness).
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "mh4n/clock.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/runtime.h"

using namespace mh4n;
namespace stdfs = std::filesystem;

namespace {

// GameCoin {0x4F00, 42, 0, 0, 0, 2014, 12, 29}, little endian, 20 bytes (written out, not computed).
const std::vector<u8> kDefaultCoin = {0x00, 0x4F, 0x00, 0x00, 0x2A, 0x00, 0x00, 0x00, 0x00, 0x00,
                                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xDE, 0x07, 0x0C, 0x1D};

class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = stdfs::temp_directory_path() /
                ("mh4n_ptm_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                 std::to_string(counter++));
        stdfs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        stdfs::remove_all(path_, ignored);
    }
    std::string User() const { return (path_ / "user").string(); }
    stdfs::path UserPath() const { return path_ / "user"; }
    // nand/data/<32 zeros>/extdata/00048000/F000000B
    stdfs::path Archive() const {
        return UserPath() / "nand" / "data" / "00000000000000000000000000000000" / "extdata" / "00048000" / "F000000B";
    }

private:
    stdfs::path path_;
};

std::vector<u8> ReadAll(const stdfs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
void WriteAll(const stdfs::path& path, const std::vector<u8>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

struct Records {
    std::vector<std::pair<LogLevel, std::string>> lines;
    Records() {
        SetLogFilter(LogFilter(LogLevel::Info));
        SetLogSink([this](const LogRecord& r) { lines.emplace_back(r.level, std::string(r.message)); });
    }
    ~Records() { SetLogSink({}); }
    int Count(LogLevel level, const std::string& needle) const {
        return static_cast<int>(std::count_if(lines.begin(), lines.end(), [&](const auto& line) {
            return line.first == level && line.second.find(needle) != std::string::npos;
        }));
    }
};

// One start-up of the replay's kind: Devices and Services, no program.
void Start(const std::vector<std::string>& args) {
    const Options options = ParseOptions("runner", args);
    REQUIRE_MESSAGE(options.ok(), options.error());
    RuntimeConfig config;
    config.time = std::make_unique<ManualTimeSource>();
    Runtime runtime(std::move(config));
    REQUIRE(runtime.Prepare(options, {Phase::Devices, Phase::Services}));
}

// Every regular file under `root` with its size, sorted: the layout the step leaves.
std::vector<std::string> Layout(const stdfs::path& root) {
    std::vector<std::string> out;
    for (const auto& entry : stdfs::recursive_directory_iterator(root)) {
        std::string line = stdfs::relative(entry.path(), root).generic_string();
        line += entry.is_directory() ? "/" : " " + std::to_string(entry.file_size());
        out.push_back(line);
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

TEST_CASE("an empty user folder gets Azahar's play coin archive: folders, metadata, gamecoin.dat") {
    TempDir dir;
    stdfs::create_directories(dir.UserPath());
    Records log;
    Start({"--user", dir.User()});

    const stdfs::path archive = dir.Archive();
    CHECK(stdfs::is_directory(archive / "user"));
    CHECK(stdfs::is_directory(archive / "boss"));
    // FormatAsExtData with ArchiveFormatInfo(): sixteen zero bytes
    CHECK(ReadAll(archive / "metadata") == std::vector<u8>(16, 0));
    CHECK(ReadAll(archive / "user" / "gamecoin.dat") == kDefaultCoin);
    CHECK(stdfs::file_size(archive / "user" / "gamecoin.dat") == 20);
    // Azahar passes no icon for this archive: none is left
    CHECK_FALSE(stdfs::exists(archive / "icon"));
    // nothing but the nand folder and what is in it
    const std::string ids = "nand/data/00000000000000000000000000000000/";
    const std::string home = ids + "extdata/00048000/F000000B/";
    CHECK(Layout(dir.UserPath()) == std::vector<std::string>{"nand/", "nand/data/", ids, ids + "extdata/",
                                                             ids + "extdata/00048000/", home, home + "boss/",
                                                             home + "metadata 16", home + "user/",
                                                             home + "user/gamecoin.dat 20"});
    CHECK(log.Count(LogLevel::Info, "play coin file made") == 1);
    CHECK(log.Count(LogLevel::Error, "") == 0);
    CHECK(log.Count(LogLevel::Warning, "") == 0);
}

TEST_CASE("a second start changes nothing: bytes and times") {
    TempDir dir;
    stdfs::create_directories(dir.UserPath());
    Start({"--user", dir.User()});
    const stdfs::path archive = dir.Archive();
    const std::vector<std::string> before = Layout(dir.UserPath());
    const auto coin_time = stdfs::last_write_time(archive / "user" / "gamecoin.dat");
    const auto metadata_time = stdfs::last_write_time(archive / "metadata");
    const auto user_time = stdfs::last_write_time(archive / "user");

    Records log;
    Start({"--user", dir.User()});
    CHECK(Layout(dir.UserPath()) == before);
    CHECK(ReadAll(archive / "user" / "gamecoin.dat") == kDefaultCoin);
    CHECK(ReadAll(archive / "metadata") == std::vector<u8>(16, 0));
    CHECK(stdfs::last_write_time(archive / "user" / "gamecoin.dat") == coin_time);
    CHECK(stdfs::last_write_time(archive / "metadata") == metadata_time);
    CHECK(stdfs::last_write_time(archive / "user") == user_time);
    CHECK(log.Count(LogLevel::Info, "play coin file made") == 0);
    CHECK(log.Count(LogLevel::Error, "") == 0);
}

TEST_CASE("an archive that exists without gamecoin.dat is left alone") {
    TempDir dir;
    const stdfs::path archive = dir.Archive();
    stdfs::create_directories(archive / "user");
    stdfs::create_directories(archive / "boss");
    const std::vector<u8> metadata = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 0, 0, 0};
    WriteAll(archive / "metadata", metadata);
    Start({"--user", dir.User()});
    CHECK_FALSE(stdfs::exists(archive / "user" / "gamecoin.dat"));
    CHECK(ReadAll(archive / "metadata") == metadata);
    CHECK(stdfs::is_empty(archive / "user"));
}

TEST_CASE("a gamecoin.dat that the console already has is kept, byte for byte") {
    TempDir dir;
    const stdfs::path archive = dir.Archive();
    stdfs::create_directories(archive / "user");
    stdfs::create_directories(archive / "boss");
    std::vector<u8> mine = kDefaultCoin;
    mine[4] = 0x63;  // 99 coins
    WriteAll(archive / "user" / "gamecoin.dat", mine);
    Start({"--user", dir.User()});
    CHECK(ReadAll(archive / "user" / "gamecoin.dat") == mine);
    // no metadata is made either: Azahar's constructor does nothing for an archive that opens
    CHECK_FALSE(stdfs::exists(archive / "metadata"));
}

TEST_CASE("without --user nothing is made, and it is said") {
    TempDir dir;
    Records log;
    Start({});
    CHECK(log.Count(LogLevel::Info, "no user folder (--user): the play coin file is not made") == 1);
    CHECK(log.Count(LogLevel::Error, "") == 0);
    CHECK(stdfs::is_empty(stdfs::path(dir.UserPath()).parent_path()));
}

TEST_CASE("a user folder that does not exist is not made") {
    TempDir dir;
    Records log;
    Start({"--user", dir.User()});
    CHECK_FALSE(stdfs::exists(dir.UserPath()));
    CHECK(log.Count(LogLevel::Info, "is not an existing folder") == 1);
}
