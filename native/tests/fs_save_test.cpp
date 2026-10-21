// Derived from Azahar src/core/file_sys/savedata_archive.cpp, src/core/file_sys/archive_extsavedata.cpp,
// src/core/file_sys/archive_source_sd_savedata.cpp, src/core/file_sys/archive_other_savedata.cpp,
// src/core/file_sys/disk_archive.cpp, src/common/file_util.cpp (ScanDirectoryTree, SplitFilename83, the order of
// ForeachDirectoryEntry: the expected listing bytes), src/core/file_sys/errors.h, src/core/hle/service/fs/fs_user.cpp,
// src/core/hle/service/fs/directory.cpp, src/core/hle/service/fs/archive.cpp and
// src/core/hle/service/fs/archive.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: archive.h
// says "GPLv3 or any later version", the source of the layout of ExtSaveDataInfo; the others say "GPLv2 or any
// later version"): the result codes written out as numbers, the paths of the folders under the user folder,
// the commands' parameters and replies, and what each operation answers for a missing or an existing path.
// Copyright 2016-2024 Citra Emulator Project / Azahar Emulator Project (savedata_archive.cpp,
//   archive_other_savedata.cpp)
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (archive_source_sd_savedata.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (disk_archive.cpp, file_util.cpp,
//   fs_user.cpp, archive.cpp, archive.h)
// Copyright 2018-2026 Citra Emulator Project / Azahar Emulator Project (directory.cpp)
// Copyright 2014-2025 Citra Emulator Project / Azahar Emulator Project (archive_extsavedata.cpp)
// Copyright 2016 Citra Emulator Project (errors.h)
//
// Save data and extdata through fs:USER (N1-13): the guest sends the requests the game sends, in a temporary
// user folder, and the test looks at the folder as Azahar would leave it. Time is a manual clock, so the
// delays are numbers of ticks and not measurements.
#include <doctest/doctest.h>

#if !__has_include("recomp.h")
TEST_CASE("fs_save_test needs recomp.h (the repository around native/)" * doctest::skip()) {}
#else

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <dirent.h>
#endif

#include "../src/services/fs/disk_file.h"
#include "../src/services/fs/fs_state.h"
#include "../src/services/fs/host_files.h"
#include "../src/services/fs/save_archives.h"
#include "ipc_replay/tool.h"
#include "mh4n/clock.h"
#include "mh4n/ipc.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"
#include "mh4n/service.h"
#include "mh4n/threads.h"
#include "../src/cpu/recomp_c.h"  // last: it defines macros

using namespace mh4n;

namespace {

constexpr u32 kText = 0x00100000;

// The results of Azahar's errors.h as numbers (description | module 17 << 10 | summary << 21 | level << 27),
// written out here and not computed with the code under test.
constexpr Result kFileNotFound = 0xC8804470;          // 112
constexpr Result kPathNotFound = 0xC8804471;          // 113
constexpr Result kNotFoundInvalidState = 0xC8A04478;  // 120, summary InvalidState
constexpr Result kNotFormatted = 0xC8A04554;          // 340
constexpr Result kFileAlreadyExists = 0xC82044B4;     // 180
constexpr Result kDirectoryAlreadyExists = 0xC82044B9;  // 185
constexpr Result kInvalidOpenFlags = 0xC92044E6;      // 230
constexpr Result kDirectoryNotEmpty = 0xC92044F0;     // 240
constexpr Result kUnexpectedFileOrDirectory = 0xE0C04702;  // 770
constexpr Result kUnsupportedOpenFlags = 0xE0C046F8;  // 760
constexpr Result kInvalidPath = 0xE0E046BE;           // 702
constexpr Result kWriteBeyondEnd = 0xE0E046C1;        // 705
constexpr Result kGamecardNotInserted = 0xC880448D;   // 141
constexpr Result kInvalidArchiveHandle = 0xC8804465;  // 101
constexpr Result kUnimplementedFs = 0xD8C047F4;       // 1012, NotSupported, Permanent
constexpr Result kHostFailure = MakeResult(1000, 17, 11, 27);
constexpr Result kUnknown = 0xFFFFFFFF;  // Azahar's ResultUnknown
constexpr Result kNoDataCanceled = 0xC92047EF;        // 1007, Canceled, Status (IVFCArchive::DeleteFile)
constexpr Result kNotAuthorized = 0xD8C047EA;         // 1002, NotSupported, Permanent (IVFCArchive::CreateFile)

// ---- harness: an ELF whose main thread runs a C++ body --------------------------------------------------

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

// The UTF-8 text of a path (what --user and the archive paths carry), whatever the platform's own spelling.
std::string Utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

// The tests read and write the files that the code under test makes, so these two must open any name the
// code can make: on Windows through the wide-character C library call, which does not depend on how the
// standard library opens a stream.
void WriteBytesTo(const std::filesystem::path& path, const std::vector<u8>& bytes) {
#if defined(_WIN32)
    if (std::FILE* file = _wfopen(path.c_str(), L"wb")) {
        std::fwrite(bytes.data(), 1, bytes.size(), file);
        std::fclose(file);
    }
#else
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
#endif
}

std::vector<u8> ReadBytesFrom(const std::filesystem::path& path) {
#if defined(_WIN32)
    std::vector<u8> bytes;
    if (std::FILE* file = _wfopen(path.c_str(), L"rb")) {
        u8 chunk[4096];
        for (std::size_t got; (got = std::fread(chunk, 1, sizeof chunk, file)) != 0;) {
            bytes.insert(bytes.end(), chunk, chunk + got);
        }
        std::fclose(file);
    }
    return bytes;
#else
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
#endif
}

// "José-ñ-日本", as the name of a folder and as the UTF-8 bytes that --user carries: written as escapes so
// that no source or compiler setting changes it.
const std::u8string kUnicodeName = u8"Jos\u00e9-\u00f1-\u65e5\u672c";
const std::string kUnicodeBytes = "Jos\xC3\xA9-\xC3\xB1-\xE6\x97\xA5\xE6\x9C\xAC";

// A temporary folder with a game folder and a user folder; the user folder is the one the tests look at.
class TempDir {
public:
    // `user_name`: the name of the user folder under the temporary folder (the D45 tests give it characters
    // outside ASCII).
    explicit TempDir(const std::u8string& user_name = u8"user") : user_name_(user_name) {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("mh4n_save_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                 std::to_string(counter++));
        std::filesystem::create_directories(path_ / "game");
        std::filesystem::create_directories(path_ / user_name_);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    std::filesystem::path Path() const { return path_; }
    std::filesystem::path User() const { return path_ / user_name_; }
    std::string Game() const { return (path_ / "game").string(); }
    std::string UserText() const { return Utf8(User()); }
    std::string WriteElf() const {
        const std::string path = (path_ / "program.elf").string();
        WriteBytesTo(path, MakeElf());
        return path;
    }
    // The folder of the title the tests run as (no exheader: program id 1).
    std::filesystem::path SaveFolder() const {
        return User() / "sdmc" / "Nintendo 3DS" / "00000000000000000000000000000000" /
               "00000000000000000000000000000000" / "title" / "00000000" / "00000001" / "data" / "00000001";
    }
    std::filesystem::path ExtFolder(u32 high, u32 low) const {
        char name[32];
        std::snprintf(name, sizeof(name), "%08X", high);
        std::string h = name;
        std::snprintf(name, sizeof(name), "%08X", low);
        return User() / "sdmc" / "Nintendo 3DS" / "00000000000000000000000000000000" /
               "00000000000000000000000000000000" / "extdata" / h / name;
    }
    std::filesystem::path SharedExtFolder(u32 high, u32 low) const {
        char name[32];
        std::snprintf(name, sizeof(name), "%08X", high);
        std::string h = name;
        std::snprintf(name, sizeof(name), "%08X", low);
        return User() / "nand" / "data" / "00000000000000000000000000000000" / "extdata" / h / name;
    }

private:
    std::filesystem::path path_;
    std::u8string user_name_;
};

std::vector<u8> Pattern(std::size_t size, u8 seed) {
    std::vector<u8> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<u8>(seed + i * 7 + (i >> 8));
    }
    return bytes;
}

// ---- the clock: it waits for the I/O threads before it skips idle time ----------------------------------------------

class IoAwareTime final : public TimeSource {
public:
    std::function<int()> in_flight = [] { return 0; };

    s64 NowNs() override { return now_.load(std::memory_order_acquire); }
    bool WaitUntil(std::condition_variable& cv, std::unique_lock<std::mutex>& lock, std::optional<s64> deadline_ns,
                   const std::function<bool()>& pred) override {
        while (!pred()) {
            if (!deadline_ns) {
                cv.wait(lock, pred);
                return true;
            }
            if (in_flight() == 0) {
                s64 now = now_.load(std::memory_order_acquire);
                while (now < *deadline_ns && !now_.compare_exchange_weak(now, *deadline_ns)) {
                }
                return pred();
            }
            cv.wait_for(lock, std::chrono::microseconds(200), pred);
        }
        return true;
    }

private:
    std::atomic<s64> now_{0};
};

struct Fixture {
    IoAwareTime* time = nullptr;
    std::unique_ptr<Runtime> runtime;
    std::vector<std::string> logged;
    Fixture() {
        auto source = std::make_unique<IoAwareTime>();
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
                           [&](const std::string& line) { return line.find(needle) != std::string::npos; });
    }
};

Options Parse(const std::vector<std::string>& args) {
    const Options options = ParseOptions("runner", args);
    REQUIRE_MESSAGE(options.ok(), options.error());
    return options;
}

// Boots the ELF with `--user <dir>/user` and runs `body` as the main thread.
int BootWith(Fixture& f, const TempDir& dir, std::vector<std::string> extra, std::function<void()> body) {
    std::vector<std::string> args = {dir.WriteElf(), "--user", dir.UserText()};
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

enum LowPath : u32 { kEmpty = 1, kBinary = 2, kChar = 3, kWchar = 4 };
const std::vector<u8> kEmptyPath = {0};

std::vector<u8> CharPath(const std::string& text) {
    std::vector<u8> bytes(text.begin(), text.end());
    bytes.push_back(0);
    return bytes;
}
std::vector<u8> WcharPath(const std::u16string& text) {
    std::vector<u8> bytes;
    for (const char16_t c : text) {
        bytes.push_back(static_cast<u8>(c)), bytes.push_back(static_cast<u8>(c >> 8));
    }
    bytes.push_back(0), bytes.push_back(0);
    return bytes;
}
std::vector<u8> ExtPath(u32 media, u32 low, u32 high) {
    std::vector<u8> path(12, 0);
    std::memcpy(path.data(), &media, 4);
    std::memcpy(path.data() + 4, &low, 4);
    std::memcpy(path.data() + 8, &high, 4);
    return path;
}

struct Guest {
    Runtime& rt;
    Handle fs = 0;
    u32 next_path = 0;

    // `own_manager`: make a second service manager that has the game folder of the test. Without it the
    // runtime's own is used, which is the one that writes an IPC trace (--trace-ipc).
    Guest(Fixture& f, const TempDir& dir, bool own_manager = true) : rt(*f.runtime) {
        Runtime& runtime = *f.runtime;
        ServiceManager* manager_ptr = FindServiceManager(runtime);
        if (own_manager) {
            ServiceEnvironment env;
            env.game_folder = dir.Game();
            env.user_folder = runtime.UserFolder();
            env.options = &runtime.GetOptions();
            env.kernel = &runtime.GetKernel();
            env.scheduler = &runtime.Scheduler();
            env.threads = &runtime.Threads();
            env.interfaces = &runtime.Interfaces();
            env.request_exit = [&runtime](int status, const std::string& reason) { runtime.RequestExit(status, reason); };
            auto manager = std::make_shared<ServiceManager>(std::move(env));
            manager->InstallRegistered();
            manager->Install(runtime.Svc());
            runtime.Own(manager);
            manager_ptr = manager.get();
        }
        REQUIRE(manager_ptr != nullptr);
        const std::shared_ptr<fs::FsState> state = manager_ptr->Context().State<fs::FsState>("fs:USER");
        f.time->in_flight = [state] { return state->io_in_flight.load(); };
        Handle srv = 0;
        REQUIRE(ConnectToPort("srv:", &srv) == kResultSuccess);
        std::array<u32, 2> name{};
        std::memcpy(name.data(), "fs:USER", 7);
        const std::vector<u32> got = Request(srv, {ipc::MakeHeader(0x0005, 4, 0), name[0], name[1], 7, 0}, 4);
        REQUIRE(got[1] == kResultSuccess);
        fs = got[3];
        Request(fs, {ipc::MakeHeader(0x0861, 1, 2), 0x090B00C8, ipc::CallingPidDesc(), 0}, 2);  // InitializeWithSdkVersion
    }

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
    Result Send(Handle session) { return CallSvc(rt, 0x32, {session}).r[0]; }
    std::vector<u32> Request(Handle session, const std::vector<u32>& words, std::size_t reply_words) {
        PutWords(Cmd(), words);
        REQUIRE(Send(session) == kResultSuccess);
        return GetWords(Cmd(), reply_words);
    }
    u64 Now() { return rt.Clock().Now(); }

    // A path in guest memory, one after the other.
    u32 Place(const std::vector<u8>& bytes) {
        const u32 at = Scratch(0x100 + next_path);
        next_path += (static_cast<u32>(bytes.size()) + 15) & ~15u;
        PutBytes(at, bytes);
        return at;
    }

    // ---- archives ----
    std::vector<u32> OpenArchive(u32 archive_id, u32 path_type, const std::vector<u8>& path) {
        next_path = 0;
        const u32 at = Place(path);
        const u32 size = static_cast<u32>(path.size());
        return Request(fs, {ipc::MakeHeader(0x080C, 3, 2), archive_id, path_type, size, ipc::StaticBufferDesc(size, 0), at}, 4);
    }
    // The handle of an archive that opens (the test fails otherwise).
    u64 MustOpen(u32 archive_id, u32 path_type, const std::vector<u8>& path) {
        const std::vector<u32> got = OpenArchive(archive_id, path_type, path);
        REQUIRE(got[1] == kResultSuccess);
        return got[2] | (static_cast<u64>(got[3]) << 32);
    }
    Result CloseArchive(u64 handle) {
        return Request(fs, {ipc::MakeHeader(0x080E, 2, 0), static_cast<u32>(handle), static_cast<u32>(handle >> 32)}, 2)[1];
    }
    // OpenFileDirectly of a char path in an archive (the file is read-only unless `mode` says more).
    std::vector<u32> OpenFileDirectly(u32 archive_id, u32 archive_path_type, const std::vector<u8>& archive_path,
                                      const std::string& file, u32 mode = 1) {
        next_path = 0;
        const std::vector<u8> file_path = CharPath(file);
        const u32 archive_at = Place(archive_path), file_at = Place(file_path);
        const u32 asize = static_cast<u32>(archive_path.size()), fsize = static_cast<u32>(file_path.size());
        return Request(fs,
                       {ipc::MakeHeader(0x0803, 8, 4), 0, archive_id, archive_path_type, asize, kChar, fsize, mode, 0,
                        ipc::StaticBufferDesc(asize, 0), archive_at, ipc::StaticBufferDesc(fsize, 2), file_at},
                       4);
    }
    // FormatSaveData of archive 4 with an empty archive path.
    Result Format(u32 block_size = 8, u32 directories = 10, u32 files = 20, bool duplicate = true) {
        next_path = 0;
        const u32 at = Place(kEmptyPath);
        return Request(fs,
                       {ipc::MakeHeader(0x084C, 9, 2), 4, kEmpty, 1, block_size, directories, files, 4, 5,
                        duplicate ? 1u : 0u, ipc::StaticBufferDesc(1, 0), at},
                       2)[1];
    }
    Result CreateExtSaveData(u32 media, u32 high, u32 low, const std::vector<u8>& icon, u32 directories = 3,
                             u32 files = 4, u64 size_limit = 0x10000) {
        const u32 buffer = Scratch(0x1000);
        PutBytes(buffer, icon);
        const std::vector<u32> got =
            Request(fs,
                    {ipc::MakeHeader(0x0851, 9, 2), media | 0x0100u, low, high, 0, directories, files,
                     static_cast<u32>(size_limit), static_cast<u32>(size_limit >> 32), static_cast<u32>(icon.size()),
                     ipc::MappedBufferDesc(static_cast<u32>(icon.size()), ipc::R), buffer},
                    4);
        REQUIRE(got[0] == ipc::MakeHeader(0x0851, 1, 2));
        return got[1];
    }
    Result DeleteExtSaveData(u32 media, u32 high, u32 low) {
        return Request(fs, {ipc::MakeHeader(0x0852, 4, 0), media, low, high, 0}, 2)[1];
    }

    // ---- operations in an archive (paths are char paths) ----
    std::vector<u32> PathCommand(u16 command, u64 handle, const std::string& path, const std::vector<u32>& extra_before_path,
                                 std::size_t reply_words, bool transaction = true) {
        next_path = 0;
        const std::vector<u8> bytes = CharPath(path);
        const u32 at = Place(bytes);
        const u32 size = static_cast<u32>(bytes.size());
        std::vector<u32> words = {0};
        if (transaction) {
            words.push_back(0);
        }
        words.push_back(static_cast<u32>(handle)), words.push_back(static_cast<u32>(handle >> 32));
        words.push_back(kChar), words.push_back(size);
        words.insert(words.end(), extra_before_path.begin(), extra_before_path.end());
        words.push_back(ipc::StaticBufferDesc(size, 0)), words.push_back(at);
        const u32 normal = static_cast<u32>(words.size()) - 3;
        words[0] = ipc::MakeHeader(command, normal, 2);
        return Request(fs, words, reply_words);
    }
    Result DeleteFile(u64 h, const std::string& p) { return PathCommand(0x0804, h, p, {}, 2)[1]; }
    Result DeleteDirectory(u64 h, const std::string& p) { return PathCommand(0x0806, h, p, {}, 2)[1]; }
    Result DeleteDirectoryRecursively(u64 h, const std::string& p) { return PathCommand(0x0807, h, p, {}, 2)[1]; }
    Result CreateFile(u64 h, const std::string& p, u64 size, u32 attributes = 0) {
        return PathCommand(0x0808, h, p, {attributes, static_cast<u32>(size), static_cast<u32>(size >> 32)}, 2)[1];
    }
    Result CreateDirectory(u64 h, const std::string& p) { return PathCommand(0x0809, h, p, {0}, 2)[1]; }
    struct Opened {
        std::vector<u32> reply;
        Result result = 0;
        Handle session = 0;
    };
    Opened OpenFile(u64 h, const std::string& p, u32 mode) {
        Opened out;
        out.reply = PathCommand(0x0802, h, p, {mode, 0}, 4);
        out.result = out.reply[1];
        out.session = out.reply[3];
        return out;
    }
    Opened OpenDirectory(u64 h, const std::string& p) {
        Opened out;
        out.reply = PathCommand(0x080B, h, p, {}, 4, false);
        out.result = out.reply[1];
        out.session = out.reply[3];
        return out;
    }
    Result Control(u64 h, u32 action) {
        const u32 in = Scratch(0x1000), out = Scratch(0x1100);
        PutBytes(in, std::vector<u8>(8, 0));
        return Request(fs,
                       {ipc::MakeHeader(0x080D, 5, 4), static_cast<u32>(h), static_cast<u32>(h >> 32), action, 8, 8,
                        ipc::MappedBufferDesc(8, ipc::R), in, ipc::MappedBufferDesc(8, ipc::W), out},
                       2)[1];
    }
    // ControlArchive with the sizes the client declares and the sizes of the mapped buffers; the output is
    // pre-filled with 0xEE and returned.
    std::pair<Result, std::vector<u8>> ControlSized(u64 h, u32 action, u32 in_declared, u32 out_declared, u32 in_mapped,
                                                    u32 out_mapped) {
        const u32 in = Scratch(0x1000), out = Scratch(0x1100);
        PutBytes(in, std::vector<u8>(std::max<u32>(in_mapped, 1), 0x55));
        PutBytes(out, std::vector<u8>(std::max<u32>(out_mapped, 1), 0xEE));
        const Result result =
            Request(fs,
                    {ipc::MakeHeader(0x080D, 5, 4), static_cast<u32>(h), static_cast<u32>(h >> 32), action, in_declared,
                     out_declared, ipc::MappedBufferDesc(in_mapped, ipc::R), in, ipc::MappedBufferDesc(out_mapped, ipc::W),
                     out},
                    2)[1];
        return {result, GetBytes(out, out_mapped)};
    }
    std::pair<Result, u64> GetFreeBytes(u64 h) {
        const std::vector<u32> got = Request(fs, {ipc::MakeHeader(0x0812, 2, 0), static_cast<u32>(h), static_cast<u32>(h >> 32)}, 4);
        return {got[1], got[2] | (static_cast<u64>(got[3]) << 32)};
    }

    // ---- file sessions ----
    struct ReadResult {
        Result result = 0;
        u32 count = 0;
        std::vector<u8> bytes;
    };
    ReadResult Read(Handle file, u64 offset, u32 length) {
        const u32 target = Scratch(0x1000);
        PutBytes(target, std::vector<u8>(std::max<u32>(length, 1), 0xEE));
        const std::vector<u32> reply =
            Request(file,
                    {ipc::MakeHeader(0x0802, 3, 2), static_cast<u32>(offset), static_cast<u32>(offset >> 32), length,
                     ipc::MappedBufferDesc(length, ipc::W), target},
                    5);
        ReadResult out;
        out.result = reply[1];
        out.count = reply[2];
        out.bytes = GetBytes(target, out.count);
        return out;
    }
    struct WriteResult {
        Result result = 0;
        u32 count = 0;
    };
    // flags: 0 none, 1 flush
    WriteResult Write(Handle file, u64 offset, const std::vector<u8>& data, u32 flags = 0) {
        const u32 source = Scratch(0x1000);
        PutBytes(source, data);
        const std::vector<u32> reply =
            Request(file,
                    {ipc::MakeHeader(0x0803, 4, 2), static_cast<u32>(offset), static_cast<u32>(offset >> 32),
                     static_cast<u32>(data.size()), flags, ipc::MappedBufferDesc(static_cast<u32>(data.size()), ipc::R), source},
                    5);
        REQUIRE(reply[0] == ipc::MakeHeader(0x0803, 2, 2));
        return {reply[1], reply[2]};
    }
    u64 GetSize(Handle file) {
        const std::vector<u32> reply = Request(file, {ipc::MakeHeader(0x0804, 0, 0)}, 4);
        REQUIRE(reply[1] == kResultSuccess);
        return reply[2] | (static_cast<u64>(reply[3]) << 32);
    }
    Result SetSize(Handle file, u64 size) {
        return Request(file, {ipc::MakeHeader(0x0805, 2, 0), static_cast<u32>(size), static_cast<u32>(size >> 32)}, 2)[1];
    }
    Result Flush(Handle file) { return Request(file, {ipc::MakeHeader(0x0809, 0, 0)}, 2)[1]; }
    Result CloseFile(Handle file) { return Request(file, {ipc::MakeHeader(0x0808, 0, 0)}, 2)[1]; }

    // ---- directory sessions: the entries read, 0x228 bytes each ----
    std::vector<std::vector<u8>> ReadDirectory(Handle dir, u32 count, Result* result = nullptr, u32 buffer_entries = 0) {
        if (buffer_entries == 0) {
            buffer_entries = count;
        }
        const u32 target = Scratch(0x1000);
        const u32 size = buffer_entries * 0x228;
        PutBytes(target, std::vector<u8>(size, 0xEE));
        const std::vector<u32> reply =
            Request(dir, {ipc::MakeHeader(0x0801, 1, 2), count, ipc::MappedBufferDesc(size, ipc::W), target}, 5);
        REQUIRE(reply[0] == ipc::MakeHeader(0x0801, 2, 2));
        if (result != nullptr) {
            *result = reply[1];
        }
        std::vector<std::vector<u8>> entries;
        for (u32 i = 0; i < reply[2]; ++i) {
            entries.push_back(GetBytes(target + i * 0x228, 0x228));
        }
        return entries;
    }
    Result CloseDirectory(Handle dir) { return Request(dir, {ipc::MakeHeader(0x0802, 0, 0)}, 2)[1]; }
    // A directory read into a buffer of `buffer_entries` entries that was filled with `fill` first: the
    // number of entries the reply says and the whole buffer afterwards.
    std::pair<u32, std::vector<u8>> ReadDirectoryBuffer(Handle dir, u32 count, u32 buffer_entries, u8 fill) {
        const u32 target = Scratch(0x1000);
        const u32 size = buffer_entries * 0x228;
        PutBytes(target, std::vector<u8>(size, fill));
        const std::vector<u32> reply =
            Request(dir, {ipc::MakeHeader(0x0801, 1, 2), count, ipc::MappedBufferDesc(size, ipc::W), target}, 5);
        REQUIRE(reply[0] == ipc::MakeHeader(0x0801, 2, 2));
        REQUIRE(reply[1] == kResultSuccess);
        return {reply[2], GetBytes(target, size)};
    }
};

// A decoded directory entry.
struct Entry {
    std::string name;
    std::string short_name, extension;
    bool is_directory, is_hidden, is_archive, is_read_only;
    u64 file_size;
};
Entry Decode(const std::vector<u8>& raw) {
    REQUIRE(raw.size() == 0x228);
    std::u16string wide;
    for (std::size_t i = 0; i < 0x106; ++i) {
        const char16_t c = static_cast<char16_t>(raw[i * 2] | (raw[i * 2 + 1] << 8));
        if (c == 0) {
            break;
        }
        wide.push_back(c);
    }
    Entry e;
    e.name = fs::Utf16ToUtf8(wide);
    e.short_name = std::string(reinterpret_cast<const char*>(&raw[0x20C]));
    e.extension = std::string(reinterpret_cast<const char*>(&raw[0x216]));
    e.is_directory = raw[0x21C] != 0;
    e.is_hidden = raw[0x21D] != 0;
    e.is_archive = raw[0x21E] != 0;
    e.is_read_only = raw[0x21F] != 0;
    std::memcpy(&e.file_size, &raw[0x220], 8);
    return e;
}


}  // namespace

// ---- the folder layout --------------------------------------------------------------------------------------------

TEST_CASE("the folders are Azahar's: title ids in lower case, extdata ids in upper case, 32 zeros for the ids") {
    CHECK(fs::SaveDataPath("/u/sdmc/", 0x0004000000155400ULL) ==
          "/u/sdmc/Nintendo 3DS/00000000000000000000000000000000/00000000000000000000000000000000/title/"
          "00040000/00155400/data/00000001/");
    CHECK(fs::SaveDataMetadataPath("/u/sdmc/", 0x0004000000155400ULL) ==
          "/u/sdmc/Nintendo 3DS/00000000000000000000000000000000/00000000000000000000000000000000/title/"
          "00040000/00155400/data/00000001.metadata");
    CHECK(fs::SaveDataPath("/u/sdmc/", 0x000400000ABCDEF0ULL).find("0004000/") == std::string::npos);
    CHECK(fs::SaveDataPath("/u/sdmc/", 0x000400000ABCDEF0ULL).find("/00040000/0abcdef0/data/00000001/") != std::string::npos);
    const std::array<u8, 16> info = fs::EncodeFormatInfo({0x1000, 10, 20, 1});
    CHECK(info == std::array<u8, 16>{0x00, 0x10, 0, 0, 10, 0, 0, 0, 20, 0, 0, 0, 1, 0, 0, 0});
}

// ---- save data (archive 4) ------------------------------------------------------------------------------------------------

TEST_CASE("SaveData: not formatted, FormatSaveData, create, write, read back, list, delete") {
    Fixture f;
    TempDir dir;
    Result not_formatted = 0, format = 0, create = 0, create_again = 0, close_archive = 0;
    Guest::Opened opened;
    Guest::WriteResult written;
    Guest::ReadResult read;
    u64 size = 0, free_bytes = 0;
    Result free_result = 0, control = 0;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none"}, [&] {
        Guest g(f, dir);
        not_formatted = g.OpenArchive(4, kEmpty, kEmptyPath)[1];
        format = g.Format();
        const u64 archive = g.MustOpen(4, kEmpty, kEmptyPath);
        create = g.CreateFile(archive, "/save.bin", 0x100);
        create_again = g.CreateFile(archive, "/save.bin", 0x100);
        opened = g.OpenFile(archive, "/save.bin", 3);
        written = g.Write(opened.session, 0x10, Pattern(0x20, 5), 1);
        read = g.Read(opened.session, 0x10, 0x20);
        size = g.GetSize(opened.session);
        std::tie(free_result, free_bytes) = g.GetFreeBytes(archive);
        control = g.Control(archive, 0);
        g.CloseFile(opened.session);
        close_archive = g.CloseArchive(archive);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(not_formatted == kNotFormatted);
    CHECK(format == kResultSuccess);
    CHECK(create == kResultSuccess);
    CHECK(create_again == kFileAlreadyExists);
    CHECK(opened.result == kResultSuccess);
    CHECK(opened.reply[0] == ipc::MakeHeader(0x0802, 1, 2));
    CHECK(written.result == kResultSuccess);
    CHECK(written.count == 0x20);
    CHECK(read.result == kResultSuccess);
    CHECK(read.bytes == Pattern(0x20, 5));
    CHECK(size == 0x100);  // the file was created with the size the game said
    CHECK(free_result == kResultSuccess);
    CHECK(free_bytes == 32u * 1024 * 1024);  // Azahar stubs 32 MiB
    CHECK(control == kResultSuccess);
    CHECK(close_archive == kResultSuccess);

    // on the disk: the metadata (16 bytes) next to the folder, and the file with the bytes in it
    const std::vector<u8> metadata = ReadBytesFrom(dir.SaveFolder().parent_path() / "00000001.metadata");
    CHECK(metadata == std::vector<u8>{0x00, 0x10, 0, 0, 10, 0, 0, 0, 20, 0, 0, 0, 1, 0, 0, 0});  // 8 blocks of 512
    const std::vector<u8> content = ReadBytesFrom(dir.SaveFolder() / "save.bin");
    REQUIRE(content.size() == 0x100);
    CHECK(std::vector<u8>(content.begin() + 0x10, content.begin() + 0x30) == Pattern(0x20, 5));
    CHECK(content[0] == 0);
}

TEST_CASE("SaveData: the error cases are Azahar's") {
    Fixture f;
    TempDir dir;
    std::map<std::string, Result> r;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none"}, [&] {
        Guest g(f, dir);
        g.Format();
        const u64 a = g.MustOpen(4, kEmpty, kEmptyPath);
        g.CreateDirectory(a, "/dir");
        g.CreateFile(a, "/dir/inner.bin", 4);
        g.CreateFile(a, "/file.bin", 4);
        r["delete missing file"] = g.DeleteFile(a, "/missing.bin");
        r["delete file in missing dir"] = g.DeleteFile(a, "/nodir/x.bin");
        r["delete a directory as a file"] = g.DeleteFile(a, "/dir");
        r["delete a file under a file"] = g.DeleteFile(a, "/file.bin/x");
        r["create in a missing dir"] = g.CreateFile(a, "/nodir/x.bin", 4);
        r["create under a file"] = g.CreateFile(a, "/file.bin/x", 4);
        r["create over a directory"] = g.CreateFile(a, "/dir", 4);
        r["create directory again"] = g.CreateDirectory(a, "/dir");
        r["create directory over a file"] = g.CreateDirectory(a, "/file.bin");
        r["create directory in a missing dir"] = g.CreateDirectory(a, "/nodir/sub");
        r["delete a non-empty directory"] = g.DeleteDirectory(a, "/dir");
        r["delete the root"] = g.DeleteDirectory(a, "/");
        r["delete a missing directory"] = g.DeleteDirectory(a, "/nodir");
        r["delete a file as a directory"] = g.DeleteDirectory(a, "/file.bin");
        r["relative path"] = g.DeleteFile(a, "file.bin");
        r["invalid character"] = g.CreateFile(a, "/a:b", 4);
        r["leaves the archive"] = g.DeleteFile(a, "/../x");
        r["open missing"] = g.OpenFile(a, "/missing.bin", 1).result;
        r["open in a missing dir"] = g.OpenFile(a, "/nodir/x", 1).result;
        r["open a directory"] = g.OpenFile(a, "/dir", 1).result;
        r["open under a file"] = g.OpenFile(a, "/file.bin/x", 1).result;
        r["open with no mode"] = g.OpenFile(a, "/file.bin", 0).result;
        r["create flag without write"] = g.OpenFile(a, "/new.bin", 5).result;
        r["open directory missing"] = g.OpenDirectory(a, "/nodir").result;
        r["open directory on a file"] = g.OpenDirectory(a, "/file.bin").result;
        r["bad handle"] = g.DeleteFile(0x1234, "/file.bin");
        r["recursive delete"] = g.DeleteDirectoryRecursively(a, "/dir");
        r["after the recursive delete"] = g.OpenDirectory(a, "/dir").result;
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(r["delete missing file"] == kFileNotFound);
    CHECK(r["delete file in missing dir"] == kPathNotFound);
    CHECK(r["delete a directory as a file"] == kFileNotFound);
    CHECK(r["delete a file under a file"] == kFileNotFound);
    CHECK(r["create in a missing dir"] == kPathNotFound);
    CHECK(r["create under a file"] == kUnexpectedFileOrDirectory);
    CHECK(r["create over a directory"] == kFileAlreadyExists);
    CHECK(r["create directory again"] == kDirectoryAlreadyExists);
    CHECK(r["create directory over a file"] == kDirectoryAlreadyExists);
    CHECK(r["create directory in a missing dir"] == kPathNotFound);
    CHECK(r["delete a non-empty directory"] == kDirectoryNotEmpty);
    CHECK(r["delete the root"] == kDirectoryNotEmpty);
    CHECK(r["delete a missing directory"] == kPathNotFound);
    CHECK(r["delete a file as a directory"] == kUnexpectedFileOrDirectory);
    CHECK(r["relative path"] == kInvalidPath);
    CHECK(r["invalid character"] == kInvalidPath);
    CHECK(r["leaves the archive"] == kInvalidPath);
    CHECK(r["open missing"] == kFileNotFound);
    CHECK(r["open in a missing dir"] == kPathNotFound);
    CHECK(r["open a directory"] == kUnexpectedFileOrDirectory);
    CHECK(r["open under a file"] == kUnexpectedFileOrDirectory);
    CHECK(r["open with no mode"] == kUnsupportedOpenFlags);
    CHECK(r["create flag without write"] == kUnsupportedOpenFlags);
    CHECK(r["open directory missing"] == kPathNotFound);
    CHECK(r["open directory on a file"] == kUnexpectedFileOrDirectory);
    CHECK(r["bad handle"] == kInvalidArchiveHandle);
    CHECK(r["recursive delete"] == kResultSuccess);
    CHECK(r["after the recursive delete"] == kPathNotFound);
    CHECK(!std::filesystem::exists(dir.SaveFolder() / "dir"));
    CHECK(std::filesystem::exists(dir.SaveFolder() / "file.bin"));
}

TEST_CASE("SaveData: OpenFile with the create flag, read-only and write-only opens, a zero-size file, wide paths") {
    Fixture f;
    TempDir dir;
    Guest::Opened created, read_only, write_only;
    Guest::WriteResult write_to_read_only;
    Guest::ReadResult read_from_write_only, read_from_created;
    Result zero_size = 0, wide = 0, wide_again = 0;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none"}, [&] {
        Guest g(f, dir);
        g.Format();
        const u64 a = g.MustOpen(4, kEmpty, kEmptyPath);
        created = g.OpenFile(a, "/made.bin", 6);  // write + create
        g.Write(created.session, 0, Pattern(8, 1));
        read_from_created = g.Read(created.session, 0, 8);  // not opened for reading
        g.CloseFile(created.session);
        read_only = g.OpenFile(a, "/made.bin", 1);
        write_to_read_only = g.Write(read_only.session, 0, Pattern(8, 2));
        write_only = g.OpenFile(a, "/made.bin", 2);
        read_from_write_only = g.Read(write_only.session, 0, 8);
        zero_size = g.CreateFile(a, "/zero.bin", 0);
        // a wide char path
        g.next_path = 0;
        const std::vector<u8> bytes = WcharPath(u"/wide.bin");
        const u32 at = g.Place(bytes);
        const u32 size = static_cast<u32>(bytes.size());
        const auto request = [&](u32 file_size) {
            return g.Request(g.fs,
                             {ipc::MakeHeader(0x0808, 8, 2), 0, static_cast<u32>(a), static_cast<u32>(a >> 32), kWchar, size, 0,
                              file_size, 0, ipc::StaticBufferDesc(size, 0), at},
                             2)[1];
        };
        wide = request(4);
        wide_again = request(4);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(created.result == kResultSuccess);
    CHECK(read_from_created.result == kInvalidOpenFlags);
    CHECK(read_from_created.count == 0);
    CHECK(read_only.result == kResultSuccess);
    CHECK(write_to_read_only.result == kInvalidOpenFlags);
    CHECK(write_to_read_only.count == 0);
    CHECK(write_only.result == kResultSuccess);
    CHECK(read_from_write_only.result == kInvalidOpenFlags);
    CHECK(zero_size == kResultSuccess);  // a save data archive allows it
    CHECK(wide == kResultSuccess);
    CHECK(wide_again == kFileAlreadyExists);
    CHECK(ReadBytesFrom(dir.SaveFolder() / "made.bin") == Pattern(8, 1));
    CHECK(std::filesystem::file_size(dir.SaveFolder() / "zero.bin") == 0);
    CHECK(std::filesystem::file_size(dir.SaveFolder() / "wide.bin") == 4);
}

TEST_CASE("SaveData: a directory listing has Azahar's entries, in the host's order") {
    Fixture f;
    TempDir dir;
    std::vector<Entry> entries;
    std::vector<std::vector<u8>> second_read, third_read;
    Result read_result = 0, close = 0;
    Guest::Opened missing;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none"}, [&] {
        Guest g(f, dir);
        g.Format();
        const u64 a = g.MustOpen(4, kEmpty, kEmptyPath);
        g.CreateFile(a, "/longfilename.txt", 7);
        g.CreateFile(a, "/.hidden", 3);
        g.CreateDirectory(a, "/Folder");
        g.CreateFile(a, "/b.bin", 1);
        const Guest::Opened opened = g.OpenDirectory(a, "/");
        REQUIRE(opened.result == kResultSuccess);
        REQUIRE(opened.reply[0] == ipc::MakeHeader(0x080B, 1, 2));
        REQUIRE(opened.reply[2] == ipc::MoveHandleDesc(1));
        for (const auto& raw : g.ReadDirectory(opened.session, 3, &read_result)) {
            entries.push_back(Decode(raw));
        }
        second_read = g.ReadDirectory(opened.session, 3);  // what is left: one entry
        third_read = g.ReadDirectory(opened.session, 3);   // nothing
        close = g.CloseDirectory(opened.session);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(read_result == kResultSuccess);
    REQUIRE(entries.size() == 3);
    REQUIRE(second_read.size() == 1);
    CHECK(third_read.empty());
    CHECK(close == kResultSuccess);
    entries.push_back(Decode(second_read[0]));

    // the host's own order: readdir's, without "." and ".."
    std::vector<std::string> host_order;
#if !defined(_WIN32)
    DIR* d = opendir(dir.SaveFolder().c_str());
    REQUIRE(d != nullptr);
    while (const dirent* e = readdir(d)) {
        if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0) {
            host_order.push_back(e->d_name);
        }
    }
    closedir(d);
#else
    for (const auto& e : std::filesystem::directory_iterator(dir.SaveFolder())) {
        host_order.push_back(e.path().filename().string());
    }
#endif
    std::vector<std::string> names;
    for (const Entry& e : entries) {
        names.push_back(e.name);
    }
    CHECK(names == host_order);

    for (const Entry& e : entries) {
        if (e.name == "longfilename.txt") {
            CHECK(e.short_name == "LONGFI~1");
            CHECK(e.extension == "TXT");
            CHECK(!e.is_directory);
            CHECK(e.is_archive);  // the archive bit of every file
            CHECK(!e.is_hidden);
            CHECK(!e.is_read_only);
            CHECK(e.file_size == 7);
        } else if (e.name == ".hidden") {
            CHECK(e.is_hidden);  // a leading dot
            CHECK(e.file_size == 3);
            CHECK(e.short_name == "        ");
            CHECK(e.extension == "HID");
        } else if (e.name == "Folder") {
            CHECK(e.is_directory);
            CHECK(!e.is_archive);
            CHECK(e.file_size == 0);
            CHECK(e.short_name == "FOLDER  ");
            CHECK(e.extension == "   ");
        } else {
            CHECK(e.name == "b.bin");
            CHECK(e.short_name == "B       ");
            CHECK(e.extension == "BIN");
            CHECK(e.file_size == 1);
        }
    }
}

TEST_CASE("a directory read never writes past the client's buffer") {
    Fixture f;
    TempDir dir;
    std::vector<std::vector<u8>> entries;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none"}, [&] {
        Guest g(f, dir);
        g.Format();
        const u64 a = g.MustOpen(4, kEmpty, kEmptyPath);
        for (int i = 0; i < 4; ++i) {
            g.CreateFile(a, "/f" + std::to_string(i), 1);
        }
        const Guest::Opened opened = g.OpenDirectory(a, "/");
        entries = g.ReadDirectory(opened.session, 100, nullptr, 2);  // asks for 100, room for 2
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(entries.size() == 2);
    CHECK(f.Logged("Read of 100 entries"));
}

TEST_CASE("FormatSaveData deletes what the save held (Azahar), and answers its errors") {
    Fixture f;
    TempDir dir;
    Result format = 0, wrong_archive = 0, other_path = 0;
    bool exists_after = true;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none"}, [&] {
        Guest g(f, dir);
        g.Format();
        const u64 a = g.MustOpen(4, kEmpty, kEmptyPath);
        g.CreateFile(a, "/keep.bin", 4);
        g.CloseArchive(a);
        format = g.Format(16, 1, 2, false);
        // archive 6 instead of 4
        g.next_path = 0;
        const u32 at = g.Place(kEmptyPath);
        wrong_archive = g.Request(g.fs,
                                  {ipc::MakeHeader(0x084C, 9, 2), 6, kEmpty, 1, 8, 1, 1, 1, 1, 0,
                                   ipc::StaticBufferDesc(1, 0), at},
                                  2)[1];
        // a binary path instead of an empty one
        g.next_path = 0;
        const std::vector<u8> binary(8, 0);
        const u32 at2 = g.Place(binary);
        other_path = g.Request(g.fs,
                               {ipc::MakeHeader(0x084C, 9, 2), 4, kBinary, 8, 8, 1, 1, 1, 1, 0,
                                ipc::StaticBufferDesc(8, 0), at2},
                               2)[1];
        exists_after = std::filesystem::exists(dir.SaveFolder() / "keep.bin");
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(format == kResultSuccess);
    CHECK(!exists_after);
    CHECK(wrong_archive == kInvalidPath);
    CHECK(other_path == kUnimplementedFs);
    CHECK(ReadBytesFrom(dir.SaveFolder().parent_path() / "00000001.metadata") ==
          std::vector<u8>{0x00, 0x20, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0});
}

// ---- file semantics ------------------------------------------------------------------------------------------------

TEST_CASE("writes are on the disk when the reply comes; Flush and Close; a closed file") {
    Fixture f;
    TempDir dir;
    std::vector<u8> after_write, after_flush_write, after_set_size;
    Guest::WriteResult write_after_close;
    Guest::ReadResult read_after_close;
    Result flush = 0, close = 0, close_again = 0, set_size = 0;
    u64 size_after_set = 0, size_beyond = 0;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none"}, [&] {
        Guest g(f, dir);
        g.Format();
        const u64 a = g.MustOpen(4, kEmpty, kEmptyPath);
        g.CreateFile(a, "/s.bin", 0x40);
        const Guest::Opened o = g.OpenFile(a, "/s.bin", 3);
        g.Write(o.session, 0, Pattern(0x10, 1));  // no flush flag
        after_write = ReadBytesFrom(dir.SaveFolder() / "s.bin");
        g.Write(o.session, 0x10, Pattern(0x10, 2), 1);  // with the flush flag
        after_flush_write = ReadBytesFrom(dir.SaveFolder() / "s.bin");
        flush = g.Flush(o.session);
        set_size = g.SetSize(o.session, 0x80);
        size_after_set = g.GetSize(o.session);
        after_set_size = ReadBytesFrom(dir.SaveFolder() / "s.bin");
        // beyond the end grows the file of a save data archive
        g.Write(o.session, 0x100, Pattern(4, 3));
        size_beyond = g.GetSize(o.session);
        close = g.CloseFile(o.session);
        write_after_close = g.Write(o.session, 0, Pattern(4, 9));
        read_after_close = g.Read(o.session, 0, 4);
        close_again = g.CloseFile(o.session);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    REQUIRE(after_write.size() == 0x40);
    CHECK(std::vector<u8>(after_write.begin(), after_write.begin() + 0x10) == Pattern(0x10, 1));  // visible at once
    CHECK(std::vector<u8>(after_flush_write.begin() + 0x10, after_flush_write.begin() + 0x20) == Pattern(0x10, 2));
    CHECK(flush == kResultSuccess);
    CHECK(set_size == kResultSuccess);
    CHECK(size_after_set == 0x80);
    CHECK(after_set_size.size() == 0x80);
    CHECK(std::all_of(after_set_size.begin() + 0x40, after_set_size.end(), [](u8 b) { return b == 0; }));
    CHECK(size_beyond == 0x104);
    CHECK(close == kResultSuccess);
    CHECK(write_after_close.result == kInvalidOpenFlags);
    CHECK(write_after_close.count == 0);
    CHECK(read_after_close.result == kResultSuccess);
    CHECK(read_after_close.count == 0);
    CHECK(close_again == kResultSuccess);
    const std::vector<u8> final_content = ReadBytesFrom(dir.SaveFolder() / "s.bin");
    CHECK(final_content.size() == 0x104);
    CHECK(std::vector<u8>(final_content.begin(), final_content.begin() + 0x10) == Pattern(0x10, 1));  // the write after Close did nothing
}

TEST_CASE("a game that exits without closing its file keeps what it wrote") {
    Fixture f;
    TempDir dir;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none"}, [&] {
        Guest g(f, dir);
        g.Format();
        const u64 a = g.MustOpen(4, kEmpty, kEmptyPath);
        g.CreateFile(a, "/s.bin", 8);
        const Guest::Opened o = g.OpenFile(a, "/s.bin", 3);
        g.Write(o.session, 0, Pattern(8, 7), 1);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(ReadBytesFrom(dir.SaveFolder() / "s.bin") == Pattern(8, 7));
}

// ---- extdata --------------------------------------------------------------------------------------------------------------

TEST_CASE("ExtSaveData: create, open, fixed-size files, delete") {
    Fixture f;
    TempDir dir;
    std::map<std::string, Result> r;
    Guest::ReadResult read;
    Guest::WriteResult beyond, crossing, at_end;
    u64 size_after_set = 0;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none"}, [&] {
        Guest g(f, dir);
        r["missing"] = g.OpenArchive(6, kBinary, ExtPath(1, 0x1234, 0))[1];
        r["create"] = g.CreateExtSaveData(1, 0, 0x1234, Pattern(0x30, 1));
        const u64 a = g.MustOpen(6, kBinary, ExtPath(1, 0x1234, 0));
        r["zero size"] = g.CreateFile(a, "/zero.bin", 0);
        r["create file"] = g.CreateFile(a, "/data.bin", 0x20);
        r["create dir"] = g.CreateDirectory(a, "/sub");
        r["open with create"] = g.OpenFile(a, "/new.bin", 7).result;
        r["open missing"] = g.OpenFile(a, "/new.bin", 1).result;
        const Guest::Opened o = g.OpenFile(a, "/data.bin", 1);  // read only asked: read and write given
        r["open"] = o.result;
        g.Write(o.session, 0, Pattern(0x20, 4));
        read = g.Read(o.session, 0, 0x20);
        beyond = g.Write(o.session, 0x21, Pattern(4, 5));
        at_end = g.Write(o.session, 0x20, Pattern(4, 5));
        crossing = g.Write(o.session, 0x1E, Pattern(8, 6));
        r["set size"] = g.SetSize(o.session, 0x10);
        size_after_set = g.GetSize(o.session);
        r["open a directory"] = g.OpenFile(a, "/sub", 1).result;
        r["open in a missing dir"] = g.OpenFile(a, "/nodir/x", 1).result;
        r["delete"] = g.DeleteExtSaveData(1, 0, 0x1234);
        r["gone"] = g.OpenArchive(6, kBinary, ExtPath(1, 0x1234, 0))[1];
        r["delete again"] = g.DeleteExtSaveData(1, 0, 0x1234);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(r["missing"] == kNotFoundInvalidState);
    CHECK(r["create"] == kResultSuccess);
    CHECK(r["zero size"] == kUnsupportedOpenFlags);
    CHECK(r["create file"] == kResultSuccess);
    CHECK(r["create dir"] == kResultSuccess);
    CHECK(r["open with create"] == kUnsupportedOpenFlags);
    CHECK(r["open missing"] == kFileNotFound);
    CHECK(r["open"] == kResultSuccess);
    CHECK(read.bytes == Pattern(0x20, 4));
    CHECK(beyond.result == kWriteBeyondEnd);
    CHECK(at_end.result == kResultSuccess);
    CHECK(at_end.count == 0);
    CHECK(crossing.result == kResultSuccess);
    CHECK(crossing.count == 2);  // cut at the end of the file
    CHECK(r["set size"] == kResultSuccess);  // Azahar answers success and records the size
    CHECK(size_after_set == 0x10);           // the session keeps the size it was told (Azahar's File::SetSize)
    CHECK(r["open a directory"] == kUnexpectedFileOrDirectory);
    CHECK(r["open in a missing dir"] == kPathNotFound);
    CHECK(r["delete"] == kResultSuccess);
    CHECK(r["gone"] == kNotFoundInvalidState);
    CHECK(r["delete again"] == kResultSuccess);
    CHECK(!std::filesystem::exists(dir.ExtFolder(0, 0x1234)));
}

TEST_CASE("FormatAsExtData takes an optional icon: none makes no icon file, one makes it") {
    Fixture f;
    TempDir dir;
    Result none = kUnknown, with_icon = kUnknown, empty_icon = kUnknown;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none"}, [&] {
        Guest g(f, dir, false);
        ServiceManager* manager = FindServiceManager(*f.runtime);
        REQUIRE(manager != nullptr);
        const std::shared_ptr<fs::FsState> state = manager->Context().State<fs::FsState>("fs:USER");
        fs::ArchiveFactory* factory = state->archives.Factory(fs::archive_id::kExtSaveData);
        REQUIRE(factory != nullptr);
        const fs::ArchiveFormatInfo info;
        none = factory->FormatAsExtData(fs::ConstructExtDataBinaryPath(fs::kMediaSdmc, 0, 0x71), info, std::nullopt);
        const std::vector<u8> icon = Pattern(0x18, 3);
        with_icon = factory->FormatAsExtData(fs::ConstructExtDataBinaryPath(fs::kMediaSdmc, 0, 0x72), info,
                                             std::span<const u8>(icon));
        empty_icon = factory->FormatAsExtData(fs::ConstructExtDataBinaryPath(fs::kMediaSdmc, 0, 0x73), info,
                                              std::span<const u8>());
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(none == kResultSuccess);
    CHECK(with_icon == kResultSuccess);
    CHECK(empty_icon == kResultSuccess);
    CHECK(std::filesystem::is_directory(dir.ExtFolder(0, 0x71) / "user"));
    CHECK(std::filesystem::exists(dir.ExtFolder(0, 0x71) / "metadata"));
    CHECK_FALSE(std::filesystem::exists(dir.ExtFolder(0, 0x71) / "icon"));
    CHECK(ReadBytesFrom(dir.ExtFolder(0, 0x72) / "icon") == Pattern(0x18, 3));
    CHECK(std::filesystem::exists(dir.ExtFolder(0, 0x73) / "icon"));  // an empty icon is a (empty) file
    CHECK(std::filesystem::file_size(dir.ExtFolder(0, 0x73) / "icon") == 0);
}

TEST_CASE("CreateExtSaveData leaves Azahar's folders, metadata and icon") {
    Fixture f;
    TempDir dir;
    Result created = 0, bad_media = 0, delete_bad_media = 0;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none"}, [&] {
        Guest g(f, dir);
        created = g.CreateExtSaveData(1, 0x00000000, 0x0000ABCD, Pattern(0x24, 8), 7, 9, 0x20000);
        bad_media = g.CreateExtSaveData(2, 0, 0x55, Pattern(4, 1));  // a media type with no extdata
        delete_bad_media = g.DeleteExtSaveData(2, 0, 0x55);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(created == kResultSuccess);
    const std::filesystem::path folder = dir.ExtFolder(0, 0xABCD);
    CHECK(std::filesystem::is_directory(folder / "user"));
    CHECK(std::filesystem::is_directory(folder / "boss"));
    CHECK(ReadBytesFrom(folder / "icon") == Pattern(0x24, 8));
    // total size (the size limit as 32 bits), directories, files, duplicate data
    CHECK(ReadBytesFrom(folder / "metadata") ==
          std::vector<u8>{0x00, 0x00, 0x02, 0x00, 7, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0});
    // the normal factory takes every media type but NAND, and makes it under the SD card
    CHECK(bad_media == kResultSuccess);
    CHECK(delete_bad_media == kUnknown);
}

TEST_CASE("SharedExtSaveData lives in the NAND folder and its high id is always 0x48000") {
    Fixture f;
    TempDir dir;
    std::map<std::string, Result> r;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none"}, [&] {
        Guest g(f, dir);
        // (0xF000000B is the play coin archive that the ptm boot step makes, N1-12b: this case uses fresh ids)
        r["missing"] = g.OpenArchive(7, kBinary, ExtPath(0, 0xF000000D, 0x12345))[1];
        r["create"] = g.CreateExtSaveData(0, 0x00048000, 0xF000000C, Pattern(8, 1));
        // the id the game asks with has any high word: Azahar overwrites it
        const u64 a = g.MustOpen(7, kBinary, ExtPath(0, 0xF000000C, 0xDEAD));
        r["create file"] = g.CreateFile(a, "/f.bin", 4);
        r["unknown"] = g.OpenArchive(7, kBinary, ExtPath(0, 0xF0000099, 0))[1];
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(r["missing"] == kNotFormatted);
    CHECK(r["create"] == kResultSuccess);
    CHECK(r["create file"] == kResultSuccess);
    CHECK(r["unknown"] == kNotFormatted);
    const std::filesystem::path folder = dir.SharedExtFolder(0x48000, 0xF000000C);
    CHECK(std::filesystem::is_directory(folder / "user"));
    CHECK(std::filesystem::is_directory(folder / "boss"));
    CHECK(std::filesystem::exists(folder / "metadata"));
    CHECK(std::filesystem::file_size(folder / "user" / "f.bin") == 4);
    CHECK(std::filesystem::exists(folder / "icon"));  // the game's high word was the shared one
}

TEST_CASE("N1-12b: the play coin archive that the ptm step made opens through fs:USER and holds Azahar's default coins") {
    Fixture f;
    TempDir dir;
    // GameCoin {0x4F00, 42, 0, 0, 0, 2014, 12, 29}, little endian (written out, not computed)
    const std::vector<u8> expected = {0x00, 0x4F, 0x00, 0x00, 0x2A, 0x00, 0x00, 0x00, 0x00, 0x00,
                                      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xDE, 0x07, 0x0C, 0x1D};
    std::map<std::string, Result> r;
    std::map<std::string, std::vector<u8>> bytes;
    std::map<std::string, u64> sizes;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none"}, [&] {
        Guest g(f, dir);
        // the 12-byte binary path of the game: media NAND, save id low 0xF000000B, high 0x48000; and the id as
        // Azahar's ptm builds it (high 0): the shared factory replaces the high word
        for (const auto& [name, high] : std::vector<std::pair<std::string, u32>>{{"game", 0x00048000}, {"ptm", 0}}) {
            const std::vector<u32> opened = g.OpenArchive(7, kBinary, ExtPath(0, 0xF000000B, high));
            r[name + " archive"] = opened[1];
            if (opened[1] != kResultSuccess) {
                continue;
            }
            const u64 archive = opened[2] | (static_cast<u64>(opened[3]) << 32);
            const Guest::Opened file = g.OpenFile(archive, "/gamecoin.dat", 1);
            r[name + " file"] = file.result;
            if (file.result == kResultSuccess) {
                sizes[name] = g.GetSize(file.session);
                const Guest::ReadResult read = g.Read(file.session, 0, 20);
                r[name + " read"] = read.result;
                bytes[name] = read.bytes;
                g.CloseFile(file.session);
            }
            g.CloseArchive(archive);
        }
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    for (const char* name : {"game", "ptm"}) {
        const std::string n = name;
        CHECK(r[n + " archive"] == kResultSuccess);
        CHECK(r[n + " file"] == kResultSuccess);
        CHECK(r[n + " read"] == kResultSuccess);
        CHECK(sizes[n] == 20);
        CHECK(bytes[n] == expected);
    }
    const std::filesystem::path folder = dir.SharedExtFolder(0x48000, 0xF000000B);
    CHECK(std::filesystem::file_size(folder / "user" / "gamecoin.dat") == 20);
    CHECK(std::filesystem::file_size(folder / "metadata") == 16);
}

// ---- OtherSaveData ----------------------------------------------------------------------------------------------------------

TEST_CASE("OtherSaveDataPermitted opens another title's save by the id the path names") {
    Fixture f;
    TempDir dir;
    std::map<std::string, Result> r;
    const u32 word = 0x00155400 >> 8;  // 0x0004000000155400's low word is word << 8
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none"}, [&] {
        Guest g(f, dir);
        const auto path = [&](u32 media) { return ExtPath(media, word, 0); };
        r["not formatted"] = g.OpenArchive(0x567890B4, kBinary, path(1))[1];
        // make that title's save as the game would: the folder and the file the archive 4 would have
        const std::string folder = fs::SaveDataPath(dir.UserText() + "/sdmc/", 0x0004000000155400ULL);
        REQUIRE(fs::host::CreateFullPath(folder));
        WriteBytesTo(folder + "other.bin", Pattern(6, 3));
        const u64 a = g.MustOpen(0x567890B4, kBinary, path(1));
        const Guest::Opened o = g.OpenFile(a, "/other.bin", 1);
        r["open"] = o.result;
        r["read"] = g.Read(o.session, 0, 6).bytes == Pattern(6, 3) ? kResultSuccess : kUnknown;
        r["nand"] = g.OpenArchive(0x567890B4, kBinary, path(0))[1];
        r["game card"] = g.OpenArchive(0x567890B4, kBinary, path(2))[1];
        r["wrong path type"] = g.OpenArchive(0x567890B4, kEmpty, kEmptyPath)[1];
        r["short path"] = g.OpenArchive(0x567890B4, kBinary, std::vector<u8>(8, 0))[1];
        // General: the whole 64 bits are in the path
        std::vector<u8> general = ExtPath(1, 0x00155400, 0x00040000);
        r["general"] = g.OpenArchive(0x567890B2, kBinary, general)[1];
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(r["not formatted"] == kNotFormatted);
    CHECK(r["open"] == kResultSuccess);
    CHECK(r["read"] == kResultSuccess);
    CHECK(r["nand"] == kUnsupportedOpenFlags);  // "strange, but verified with a real 3DS"
    CHECK(r["game card"] == kGamecardNotInserted);
    CHECK(r["wrong path type"] == kInvalidPath);
    CHECK(r["short path"] == kInvalidPath);
    CHECK(r["general"] == kResultSuccess);
}

// ---- the other archives keep their behaviour ------------------------------------------------------------------------------

// A test archive (id 0x7E57) that overrides nothing but OpenFile, so it answers the file operations with the
// defaults of ArchiveBackend (Azahar's IVFCArchive). Its files record the thread each Read and Write ran on.
namespace {
struct ProbeThreads {
    std::mutex mutex;
    std::thread::id read, write;
    int reads = 0;
};
ProbeThreads g_probe;

class ProbeFile final : public fs::FileBackend {
public:
    explicit ProbeFile(bool on_scheduler) : on_scheduler_(on_scheduler) {}
    Result Read(u64, std::size_t length, u8* buffer, std::size_t* read) override {
        {
            std::lock_guard lock(g_probe.mutex);
            g_probe.read = std::this_thread::get_id();
            ++g_probe.reads;
        }
        std::memset(buffer, 0x42, length);
        *read = length;
        return kResultSuccess;
    }
    Result Write(u64, std::size_t, bool, bool, const u8*, std::size_t* written) override {
        std::lock_guard lock(g_probe.mutex);
        g_probe.write = std::this_thread::get_id();
        *written = 0;
        return kResultSuccess;
    }
    u64 GetSize() override { return 0x100; }
    bool ReadsOnSchedulerThread() const override { return on_scheduler_; }

private:
    bool on_scheduler_;
};

class ProbeArchive final : public fs::ArchiveBackend {
public:
    std::string GetName() const override { return "ProbeArchive"; }
    Result OpenFile(const fs::Path& path, u32, u32, std::shared_ptr<fs::FileBackend>* file) override {
        *file = std::make_shared<ProbeFile>(path.data.size() > 1 && path.data[1] == 's');
        return kResultSuccess;
    }
};

class ProbeFactory final : public fs::ArchiveFactory {
public:
    std::string GetName() const override { return "Probe"; }
    Result Open(const fs::Path&, u64, std::unique_ptr<fs::ArchiveBackend>* archive) override {
        *archive = std::make_unique<ProbeArchive>();
        return kResultSuccess;
    }
};
MH4N_REGISTER(fs::ArchiveProvider, "7e57.probe",
              fs::ArchiveProvider{0x7E57, [](fs::FsState&) { return std::make_unique<ProbeFactory>(); }});
}  // namespace

TEST_CASE("an archive that overrides nothing answers the file operations as Azahar's IVFCArchive does") {
    Fixture f;
    TempDir dir;
    std::map<std::string, Result> r;
    std::vector<std::vector<u8>> entries;
    std::pair<Result, u64> free_bytes;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none"}, [&] {
        Guest g(f, dir);
        const u64 a = g.MustOpen(0x7E57, kEmpty, kEmptyPath);
        r["delete file"] = g.DeleteFile(a, "/x");
        r["delete directory"] = g.DeleteDirectory(a, "/x");
        r["delete recursively"] = g.DeleteDirectoryRecursively(a, "/x");
        r["create file"] = g.CreateFile(a, "/x", 4);
        r["create directory"] = g.CreateDirectory(a, "/x");
        const Guest::Opened d = g.OpenDirectory(a, "/");
        r["open directory"] = d.result;
        r["directory session"] = d.session != 0 ? kResultSuccess : kUnknown;
        Result read_result = kUnknown;
        entries = g.ReadDirectory(d.session, 4, &read_result);
        r["read directory"] = read_result;
        free_bytes = g.GetFreeBytes(a);
        r["control"] = g.Control(a, 0);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(r["delete file"] == kNoDataCanceled);
    CHECK(r["delete directory"] == kUnknown);
    CHECK(r["delete recursively"] == kUnknown);
    CHECK(r["create file"] == kNotAuthorized);
    CHECK(r["create directory"] == kUnknown);
    CHECK(r["open directory"] == kResultSuccess);  // IVFCArchive::OpenDirectory: an empty directory
    CHECK(r["directory session"] == kResultSuccess);
    CHECK(r["read directory"] == kResultSuccess);
    CHECK(entries.empty());
    CHECK(free_bytes.first == kResultSuccess);
    CHECK(free_bytes.second == 0);
    CHECK(r["control"] == kResultSuccess);  // ArchiveBackend::Control logs and answers success
}

TEST_CASE("SelfNCCH answers the file operations with UnsupportedOpenFlags, as Azahar's SelfNCCHArchive does") {
    Fixture f;
    TempDir dir;
    std::map<std::string, Result> r;
    std::pair<Result, u64> free_bytes;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none"}, [&] {
        Guest g(f, dir);
        // SelfNCCH opens (without RomFS files it still opens: it is the file that is missing)
        const u64 a = g.MustOpen(3, kEmpty, kEmptyPath);
        r["delete file"] = g.DeleteFile(a, "/x");
        r["delete directory"] = g.DeleteDirectory(a, "/x");
        r["delete recursively"] = g.DeleteDirectoryRecursively(a, "/x");
        r["create file"] = g.CreateFile(a, "/x", 4);
        r["create directory"] = g.CreateDirectory(a, "/x");
        r["open directory"] = g.OpenDirectory(a, "/").result;
        free_bytes = g.GetFreeBytes(a);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    for (const char* name : {"delete file", "delete directory", "delete recursively", "create file", "create directory",
                             "open directory"}) {
        CHECK_MESSAGE(r[name] == kUnsupportedOpenFlags, name);
    }
    CHECK(free_bytes.first == kResultSuccess);
    CHECK(free_bytes.second == 0);
}

TEST_CASE("a file that a Write can change is read on the scheduler thread; the others on an I/O thread") {
    Fixture f;
    TempDir dir;
    std::thread::id read_sync, write_sync, read_async, write_async;
    u32 count_sync = 0;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none"}, [&] {
        Guest g(f, dir);
        const u64 a = g.MustOpen(0x7E57, kEmpty, kEmptyPath);
        const Guest::Opened sync = g.OpenFile(a, "/s", 1);  // the probe reads this one on the scheduler thread
        g.Write(sync.session, 0, Pattern(4, 1));
        count_sync = g.Read(sync.session, 0, 8).count;
        {
            std::lock_guard lock(g_probe.mutex);
            read_sync = g_probe.read, write_sync = g_probe.write;
        }
        const Guest::Opened async = g.OpenFile(a, "/a", 1);
        g.Write(async.session, 0, Pattern(4, 1));
        g.Read(async.session, 0, 8);
        {
            std::lock_guard lock(g_probe.mutex);
            read_async = g_probe.read, write_async = g_probe.write;
        }
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(count_sync == 8);
    CHECK(read_sync == write_sync);   // a Write runs on the scheduler thread: so did this Read
    CHECK(read_async != write_async);  // this one went to the I/O pool
}

TEST_CASE("--io-latency azahar: a file that a Write can change is read in the request's tick, the reply waits") {
    Fixture f;
    TempDir dir;
    u64 read_ticks = 0;
    int reads_at_reply = 0;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "azahar"}, [&] {
        Guest g(f, dir);
        {
            std::lock_guard lock(g_probe.mutex);
            g_probe.reads = 0;
        }
        const u64 a = g.MustOpen(0x7E57, kEmpty, kEmptyPath);
        const Guest::Opened o = g.OpenFile(a, "/s", 1);
        const u64 before = g.Now();
        g.Read(o.session, 0, 0x40);
        read_ticks = g.Now() - before;
        {
            std::lock_guard lock(g_probe.mutex);
            reads_at_reply = g_probe.reads;
        }
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(read_ticks == NsToTicks(fs::RomFsReadDelayNs(0x40)));  // the probe keeps the default delay, to the tick
    CHECK(reads_at_reply == 1);
}

TEST_CASE("BossExtSaveData opens the boss/ folder of the extdata, apart from user/") {
    Fixture f;
    TempDir dir;
    std::map<std::string, Result> r;
    Guest::ReadResult read;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none"}, [&] {
        Guest g(f, dir);
        r["missing"] = g.OpenArchive(0x12345678, kBinary, ExtPath(1, 0x77, 0))[1];
        g.CreateExtSaveData(1, 0, 0x77, Pattern(4, 1));
        const u64 b = g.MustOpen(0x12345678, kBinary, ExtPath(1, 0x77, 0));
        r["create"] = g.CreateFile(b, "/boss.bin", 0x10);
        const Guest::Opened o = g.OpenFile(b, "/boss.bin", 1);
        r["open"] = o.result;
        g.Write(o.session, 0, Pattern(0x10, 9));
        read = g.Read(o.session, 0, 0x10);
        const u64 u = g.MustOpen(6, kBinary, ExtPath(1, 0x77, 0));
        r["not in user"] = g.OpenFile(u, "/boss.bin", 1).result;
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(r["missing"] == kNotFoundInvalidState);
    CHECK(r["create"] == kResultSuccess);
    CHECK(r["open"] == kResultSuccess);
    CHECK(read.bytes == Pattern(0x10, 9));
    CHECK(r["not in user"] == kFileNotFound);
    CHECK(ReadBytesFrom(dir.ExtFolder(0, 0x77) / "boss" / "boss.bin") == Pattern(0x10, 9));
    CHECK(!std::filesystem::exists(dir.ExtFolder(0, 0x77) / "user" / "boss.bin"));
}

TEST_CASE("ControlArchive with an action and sizes that differ: the buffers are cut to what is mapped") {
    Fixture f;
    TempDir dir;
    std::map<std::string, std::pair<Result, std::vector<u8>>> r;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none"}, [&] {
        Guest g(f, dir);
        g.Format();
        const u64 a = g.MustOpen(4, kEmpty, kEmptyPath);
        r["action 1, in 4, out 2"] = g.ControlSized(a, 1, 4, 2, 8, 8);
        r["action 0x7F, in 0, out 0"] = g.ControlSized(a, 0x7F, 0, 0, 8, 8);
        r["declared beyond mapped"] = g.ControlSized(a, 2, 0x100, 0x100, 8, 8);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    for (auto& [name, got] : r) {
        CHECK_MESSAGE(got.first == kResultSuccess, name);
    }
    // the output buffer is written as far as the client declared it (zeros), never past
    CHECK(r["action 1, in 4, out 2"].second == std::vector<u8>{0, 0, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xEE});
    CHECK(r["action 0x7F, in 0, out 0"].second == std::vector<u8>(8, 0xEE));
    CHECK(r["declared beyond mapped"].second == std::vector<u8>(8, 0));
}

TEST_CASE("without --user the save archives are not formatted and cannot be formatted") {
    Fixture f;
    TempDir dir;
    Result open = 0, format = 0, ext_create = 0, ext_open = 0, ext_delete = 0;
    std::vector<std::string> args = {dir.WriteElf(), "--ms", "1000", "--io-latency", "none"};
    f.runtime->SetMainThreadEntry([&](GuestThread&) {
        Guest g(f, dir);
        open = g.OpenArchive(4, kEmpty, kEmptyPath)[1];
        format = g.Format();
        ext_create = g.CreateExtSaveData(1, 0, 5, Pattern(4, 1));
        ext_open = g.OpenArchive(6, kBinary, ExtPath(1, 5, 0))[1];
        ext_delete = g.DeleteExtSaveData(1, 0, 5);
        ExitProcess(*f.runtime);
    });
    CHECK(f.runtime->Boot(Parse(args)) == 0);
    CHECK(open == kNotFormatted);
    CHECK(format == kHostFailure);
    CHECK(ext_create == kHostFailure);
    CHECK(ext_open == kNotFoundInvalidState);
    CHECK(ext_delete == kResultSuccess);
}

// ---- when replies arrive ----------------------------------------------------------------------------------------------------

TEST_CASE("--io-latency azahar: save data opens and reads wait for Azahar's delays; writes and the rest do not") {
    Fixture f;
    TempDir dir;
    u64 save_open = 0, ext_open = 0, small_read = 0, big_read = 0, write = 0, create = 0, close = 0, flush = 0, delete_ticks = 0,
        open_archive = 0;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "azahar"}, [&] {
        Guest g(f, dir);
        g.Format();
        u64 before = g.Now();
        const u64 a = g.MustOpen(4, kEmpty, kEmptyPath);
        open_archive = g.Now() - before;
        before = g.Now();
        g.CreateFile(a, "/s.bin", 0x2000);
        create = g.Now() - before;
        before = g.Now();
        const Guest::Opened o = g.OpenFile(a, "/s.bin", 3);
        save_open = g.Now() - before;
        before = g.Now();
        g.Read(o.session, 0, 0x40);
        small_read = g.Now() - before;
        before = g.Now();
        g.Read(o.session, 0, 0x1000);
        big_read = g.Now() - before;
        before = g.Now();
        g.Write(o.session, 0, Pattern(0x1000, 1), 1);
        write = g.Now() - before;
        before = g.Now();
        g.Flush(o.session);
        flush = g.Now() - before;
        before = g.Now();
        g.CloseFile(o.session);
        close = g.Now() - before;
        before = g.Now();
        g.DeleteFile(a, "/s.bin");
        delete_ticks = g.Now() - before;
        g.CreateExtSaveData(1, 0, 9, Pattern(4, 1));
        const u64 e = g.MustOpen(6, kBinary, ExtPath(1, 9, 0));
        g.CreateFile(e, "/e.bin", 8);
        before = g.Now();
        g.OpenFile(e, "/e.bin", 3);
        ext_open = g.Now() - before;
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    // a SaveDataArchive sets no delay generator in Azahar: its open takes the default one, 9438006 ns
    CHECK(save_open == NsToTicks(9438006));
    CHECK(ext_open == NsToTicks(3085068));
    CHECK(small_read == NsToTicks(631826));  // the minimum
    CHECK(big_read == NsToTicks(0x1000 * 183 + 524879));
    CHECK(fs::SaveReadDelayNs(0x1000) == 0x1000 * 183 + 524879);
    // answered in the request's tick: the IPC reply delay and nothing else
    CHECK(write == NsToTicks(kIpcReplyDelayNs));
    CHECK(create == NsToTicks(kIpcReplyDelayNs));
    CHECK(flush == NsToTicks(kIpcReplyDelayNs));
    CHECK(close == NsToTicks(kIpcReplyDelayNs));
    CHECK(delete_ticks == NsToTicks(kIpcReplyDelayNs));
    CHECK(open_archive == NsToTicks(kIpcReplyDelayNs));
}

// ---- the backends and helpers on their own ------------------------------------------------------------------------------

TEST_CASE("WritableFile: positioned reads and writes, resize, close") {
    TempDir dir;
    const std::string path = (dir.Path() / "f.bin").string();
    CHECK(fs::host::CreateEmptyFile(path));
    std::string error;
    CHECK(fs::WritableFile::Open((dir.Path() / "missing.bin").string(), true, &error) == nullptr);
    CHECK(!error.empty());

    auto file = fs::WritableFile::Open(path, true, &error);
    REQUIRE(file != nullptr);
    CHECK(file->Size() == 0);
    const std::vector<u8> data = Pattern(100, 1);
    CHECK(file->WriteAt(10, data.data(), data.size()) == 100);  // past the end: the gap is zeros
    CHECK(file->Size() == 110);
    std::vector<u8> back(110, 0xEE);
    CHECK(file->ReadAt(0, back.data(), back.size()) == 110);
    CHECK(std::all_of(back.begin(), back.begin() + 10, [](u8 b) { return b == 0; }));
    CHECK(std::vector<u8>(back.begin() + 10, back.end()) == data);
    bool failed = true;
    CHECK(file->ReadAt(105, back.data(), 50, &failed) == 5);  // short at the end, and not a failure
    CHECK(!failed);
    CHECK(file->Resize(20));
    CHECK(file->Size() == 20);
    CHECK(file->Resize(40));
    std::vector<u8> grown(40, 0xEE);
    CHECK(file->ReadAt(0, grown.data(), 40) == 40);
    CHECK(std::all_of(grown.begin() + 20, grown.end(), [](u8 b) { return b == 0; }));
    CHECK(file->Sync());
    CHECK(file->Close());
    CHECK(!file->IsOpen());
    CHECK(file->WriteAt(0, data.data(), 4) == 0);
    CHECK(file->ReadAt(0, back.data(), 4) == 0);
    CHECK(!file->Resize(1));
    CHECK(file->Close());  // closing twice is fine

    auto read_only = fs::WritableFile::Open(path, false, &error);
    REQUIRE(read_only != nullptr);
    bool write_failed = false;
    CHECK(read_only->WriteAt(0, data.data(), 4, &write_failed) == 0);
    CHECK(write_failed);
    CHECK(!read_only->Resize(1));
    CHECK(read_only->Sync());

    // a directory is not a file
    CHECK(fs::WritableFile::Open(dir.Path().string(), false, &error) == nullptr);
}

TEST_CASE("WritableFile: a reader and a writer use one file at the same time") {
    TempDir dir;
    const std::string path = (dir.Path() / "f.bin").string();
    REQUIRE(fs::host::WriteFileDurably(path, std::vector<u8>(4096, 0).data(), 4096));
    auto file = fs::WritableFile::Open(path, true, nullptr);
    REQUIRE(file != nullptr);
    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};
    std::thread reader([&] {
        std::vector<u8> chunk(64);
        while (!stop) {
            if (file->ReadAt(0, chunk.data(), chunk.size()) != chunk.size()) {
                ++bad;
            }
            // every 8-byte word is written whole by one pwrite, and all the bytes of a word are equal
            for (std::size_t w = 0; w < 64; w += 8) {
                if (!std::all_of(chunk.begin() + static_cast<std::ptrdiff_t>(w), chunk.begin() + static_cast<std::ptrdiff_t>(w + 8),
                                 [&](u8 b) { return b == chunk[w]; })) {
                    ++bad;
                }
            }
        }
    });
    for (int round = 0; round < 2000; ++round) {
        const std::vector<u8> word(8, static_cast<u8>(round));
        for (std::size_t w = 0; w < 64; w += 8) {
            CHECK(file->WriteAt(w, word.data(), 8) == 8);
        }
    }
    stop = true;
    reader.join();
    CHECK(bad == 0);
}

TEST_CASE("DiskFile and FixSizeDiskFile: the modes and the fixed size") {
    TempDir dir;
    const std::string path = (dir.Path() / "f.bin").string();
    REQUIRE(fs::host::WriteFileDurably(path, Pattern(16, 1).data(), 16));
    std::size_t n = 99;
    {
        fs::DiskFile rw(fs::WritableFile::Open(path, true, nullptr), 3);
        u8 buffer[16];
        CHECK(rw.Read(0, 16, buffer, &n) == kResultSuccess);
        CHECK(n == 16);
        CHECK(rw.Read(100, 16, buffer, &n) == kResultSuccess);  // past the end: nothing
        CHECK(n == 0);
        CHECK(rw.Write(16, 4, false, false, buffer, &n) == kResultSuccess);  // a save grows
        CHECK(n == 4);
        CHECK(rw.GetSize() == 20);
        CHECK(rw.SetSize(8));
        CHECK(rw.GetSize() == 8);
        CHECK(rw.Close());
        CHECK(rw.Write(0, 4, false, false, buffer, &n) == kInvalidOpenFlags);
        CHECK(n == 0);
        CHECK(rw.Read(0, 4, buffer, &n) == kResultSuccess);
        CHECK(n == 0);
        CHECK(rw.GetReadDelayNs(0) == 631826);
    }
    {
        fs::DiskFile ro(fs::WritableFile::Open(path, false, nullptr), 1);
        u8 buffer[4] = {};
        CHECK(ro.Write(0, 4, false, false, buffer, &n) == kInvalidOpenFlags);
        fs::DiskFile wo(fs::WritableFile::Open(path, true, nullptr), 2);
        CHECK(wo.Read(0, 4, buffer, &n) == kInvalidOpenFlags);
    }
    {
        fs::FixSizeDiskFile fixed(fs::WritableFile::Open(path, true, nullptr), 3);  // 8 bytes now
        const std::vector<u8> bytes = Pattern(8, 9);
        CHECK(!fixed.SetSize(100));
        CHECK(fixed.Write(9, 1, false, false, bytes.data(), &n) == kWriteBeyondEnd);
        CHECK(fixed.Write(8, 1, false, false, bytes.data(), &n) == kResultSuccess);
        CHECK(n == 0);
        CHECK(fixed.Write(6, 8, false, false, bytes.data(), &n) == kResultSuccess);
        CHECK(n == 2);
        CHECK(fixed.GetSize() == 8);
    }
}

TEST_CASE("PathParser: Azahar's rules for the path of a file in an archive") {
    const auto char_path = [](const std::string& text) { return fs::Path{3, CharPath(text)}; };
    CHECK(fs::PathParser(char_path("/a/b.txt")).IsValid());
    CHECK(fs::PathParser(fs::Path{4, WcharPath(u"/a/b")}).IsValid());
    CHECK(!fs::PathParser(fs::Path{1, {0}}).IsValid());              // empty
    CHECK(!fs::PathParser(fs::Path{2, {'/', 'a', 0}}).IsValid());    // binary
    CHECK(!fs::PathParser(char_path("")).IsValid());
    CHECK(!fs::PathParser(char_path("a/b")).IsValid());              // no leading slash
    for (const char c : std::string("<>\\|:\"*?")) {
        CHECK(!fs::PathParser(char_path(std::string("/a") + c + "b")).IsValid());
    }
    CHECK(!fs::PathParser(char_path("/..")).IsValid());
    CHECK(!fs::PathParser(char_path("/a/../..")).IsValid());
    CHECK(fs::PathParser(char_path("/a/../b")).IsValid());
    CHECK(fs::PathParser(char_path("/")).IsRootDirectory());
    CHECK(fs::PathParser(char_path("/a/..")).IsRootDirectory());
    CHECK(fs::PathParser(char_path("/./")).IsRootDirectory());
    CHECK(!fs::PathParser(char_path("/a")).IsRootDirectory());
    CHECK(fs::PathParser(char_path("//a///./b/")).BuildHostPath("/m/") == "/m/a/b");
    CHECK(fs::PathParser(char_path("/")).BuildHostPath("/m/") == "/m/");
    // a wide char path with other than ASCII text
    CHECK(fs::PathParser(fs::Path{4, WcharPath(u"/café")}).BuildHostPath("/m/") == "/m/caf\xC3\xA9");
}

TEST_CASE("PathParser::GetHostStatus on a real folder") {
    TempDir dir;
    const std::string mount = (dir.Path() / "mount").string() + "/";
    CHECK(fs::PathParser(fs::Path{3, CharPath("/a")}).GetHostStatus(mount) == fs::PathParser::InvalidMountPoint);
    std::filesystem::create_directories(dir.Path() / "mount" / "d");
    WriteBytesTo(dir.Path() / "mount" / "f", {1});
    const auto status = [&](const std::string& p) { return fs::PathParser(fs::Path{3, CharPath(p)}).GetHostStatus(mount); };
    CHECK(status("/") == fs::PathParser::DirectoryFound);
    CHECK(status("/d") == fs::PathParser::DirectoryFound);
    CHECK(status("/f") == fs::PathParser::FileFound);
    CHECK(status("/nothing") == fs::PathParser::NotFound);
    CHECK(status("/nothing/x") == fs::PathParser::PathNotFound);
    CHECK(status("/f/x") == fs::PathParser::FileInPath);
    CHECK(status("/d/x") == fs::PathParser::NotFound);
}

TEST_CASE("SplitFilename83 and the UTF conversions") {
    const auto split = [](const std::string& name) {
        char s[9], e[4];
        fs::SplitFilename83(name, s, e);
        return std::make_pair(std::string(s), std::string(e));
    };
    CHECK(split("longfilename.txt") == std::make_pair(std::string("LONGFI~1"), std::string("TXT")));
    CHECK(split("a.b") == std::make_pair(std::string("A       "), std::string("B  ")));
    CHECK(split("noext") == std::make_pair(std::string("NOEXT   "), std::string("   ")));
    CHECK(split("a.tar.gz") == std::make_pair(std::string("ATAR    "), std::string("GZ ")));  // the last dot; dots dropped
    CHECK(split("eightchr.dat") == std::make_pair(std::string("EIGHTCHR"), std::string("DAT")));
    CHECK(split("a b.c") == std::make_pair(std::string("AB      "), std::string("C  ")));
    CHECK(split("name.") == std::make_pair(std::string("NAME    "), std::string("   ")));
    CHECK(split("x.abcdef") == std::make_pair(std::string("X       "), std::string("ABC")));

    CHECK(fs::Utf8ToUtf16("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80") == std::u16string(u"aé€\U0001F600"));
    CHECK(fs::Utf16ToUtf8(u"aé€\U0001F600") == "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");
    CHECK(fs::Utf8ToUtf16("\xFF") == std::u16string(u"�"));
    CHECK(fs::Utf16ToUtf8(std::u16string(1, char16_t(0xD800))) == "\xEF\xBF\xBD");
}

// The old loop of CreateFullPath on Windows: the next "/", and when there is none the first "\" from index 0
// (not from the position). Returns the prefixes it would have made before its guard of 100 stopped it.
static std::vector<std::string> OldWindowsPrefixes(const std::string& path, bool* hit_guard) {
    std::vector<std::string> out;
    std::size_t position = 0;
    int guard = 100;
    *hit_guard = false;
    for (;;) {
        position = path.find('/', position);
        if (position == std::string::npos) {
            position = path.find('\\', 0);
        }
        if (position == std::string::npos) {
            return out;
        }
        out.push_back(path.substr(0, position + 1));
        if (--guard <= 0) {
            *hit_guard = true;
            return out;
        }
        ++position;
    }
}

TEST_CASE("CreateFullPath: the directories of a path with the Windows separators (run on every platform)") {
    using Prefixes = std::vector<std::string>;
    const std::string_view win = "/\\";
    bool deep = true;
    // a user folder with backslashes, then the forward slashes of the archive paths (the failure of CI)
    CHECK(fs::host::DirectoryPrefixes("C:\\a\\b/c/d/", win, &deep) ==
          Prefixes{"C:\\", "C:\\a\\", "C:\\a\\b/", "C:\\a\\b/c/", "C:\\a\\b/c/d/"});
    CHECK(!deep);
    // only backslashes; the last part has no separator after it, so it is a file
    CHECK(fs::host::DirectoryPrefixes("C:\\a\\b\\c", win, &deep) == Prefixes{"C:\\", "C:\\a\\", "C:\\a\\b\\"});
    // a UNC root is not made; the parts after the share are
    CHECK(fs::host::DirectoryPrefixes("\\\\server\\share\\x/y/", win, &deep) ==
          Prefixes{"\\\\server\\share\\x/", "\\\\server\\share\\x/y/"});
    CHECK(fs::host::DirectoryPrefixes("\\\\?\\C:\\a\\b/", win, &deep) == Prefixes{"\\\\?\\C:\\a\\", "\\\\?\\C:\\a\\b/"});
    // with only "/" a backslash is part of a name (what the platforms other than Windows do, unchanged)
    CHECK(fs::host::DirectoryPrefixes("/a\\b/c/d", "/", &deep) == Prefixes{"/", "/a\\b/", "/a\\b/c/"});
    CHECK(fs::host::DirectoryPrefixes("//x//y/", "/", &deep) == Prefixes{"/", "//", "//x/", "//x//", "//x//y/"});
    CHECK(fs::host::DirectoryPrefixes("name", win, &deep).empty());
    CHECK(fs::host::DirectoryPrefixes("", win, &deep).empty());
    CHECK(fs::host::FindSeparator("a\\b/c", 0, win) == 1);
    CHECK(fs::host::FindSeparator("a\\b/c", 2, win) == 3);
    CHECK(fs::host::FindSeparator("a\\b/c", 4, win) == std::string::npos);
    CHECK(fs::host::FindSeparator("a\\b/c", 0, "/") == 3);

    // the guard counts directory levels: 100 are the limit, as before, and a long path of 99 is fine
    std::string levels99 = "/", levels100 = "/";
    for (int i = 0; i < 98; ++i) {
        levels99 += "d/";
    }
    for (int i = 0; i < 99; ++i) {
        levels100 += "d/";
    }
    deep = true;
    CHECK(fs::host::DirectoryPrefixes(levels99, win, &deep).size() == 99);
    CHECK(!deep);
    CHECK(fs::host::DirectoryPrefixes(levels100, win, &deep).size() == 100);
    CHECK(deep);
    std::string mixed = "C:\\";
    for (int i = 0; i < 40; ++i) {
        mixed += (i % 2 == 0 ? "d\\" : "d/");
    }
    CHECK(fs::host::DirectoryPrefixes(mixed, win, &deep).size() == 41);
    CHECK(!deep);

    // the old Windows loop: a path with a backslash before its last "/" ran into its guard (it went back to
    // the first backslash again and again), and so did one with backslashes only
    bool old_guard = false;
    OldWindowsPrefixes("C:\\a\\b/c/d/", &old_guard);
    CHECK(old_guard);
    OldWindowsPrefixes("C:\\a\\b\\c", &old_guard);
    CHECK(old_guard);
    OldWindowsPrefixes("C:/a/b/", &old_guard);
    CHECK(!old_guard);
}

TEST_CASE("host helpers: delete, recursive delete, listing, create full path") {
    TempDir dir;
    const std::string root = (dir.Path() / "t").string();
    CHECK(fs::host::CreateFullPath(root + "/a/b/c/"));
    CHECK(fs::host::IsDirectory(root + "/a/b/c"));
    CHECK(fs::host::CreateFullPath(root + "/a/b/c/"));  // already there
    CHECK(fs::host::CreateDir(root + "/a"));            // an existing directory is success
    WriteBytesTo(root + "/a/b/f.bin", {1, 2, 3});
    WriteBytesTo(root + "/a/g.bin", {1});
    const auto children = fs::host::ListDirectory(root + "/a");
    REQUIRE(children.has_value());
    CHECK(children->size() == 2);
    CHECK(!fs::host::ListDirectory(root + "/nothing").has_value());
    CHECK(fs::host::FileSize(root + "/a/b/f.bin") == 3);
    CHECK(fs::host::FileSize(root + "/a/b") == 0);
    CHECK(fs::host::RemoveFile(root + "/nothing"));   // a missing file counts as removed
    CHECK(!fs::host::RemoveFile(root + "/a"));        // a directory does not
    CHECK(!fs::host::RemoveEmptyDirectory(root + "/a"));  // not empty
    CHECK(fs::host::RemoveEmptyDirectory(root + "/a/b/c"));
    CHECK(fs::host::DeleteDirectoryRecursively(root + "/a"));
    CHECK(!fs::host::Exists(root + "/a"));
    CHECK(!fs::host::DeleteDirectoryRecursively(root + "/a"));  // not there: Azahar's false
#if !defined(_WIN32)
    // a link in the tree is removed, not followed
    std::filesystem::create_directories(dir.Path() / "outside");
    WriteBytesTo(dir.Path() / "outside" / "keep.bin", {9});
    std::filesystem::create_directories(root + "/l");
    std::filesystem::create_directory_symlink(dir.Path() / "outside", root + "/l/link");
    CHECK(fs::host::DeleteDirectoryRecursively(root + "/l"));
    CHECK(std::filesystem::exists(dir.Path() / "outside" / "keep.bin"));
#endif
    const std::vector<u8> bytes = Pattern(33, 2);
    CHECK(fs::host::WriteFileDurably(root + "/meta", bytes.data(), bytes.size()));
    CHECK(std::filesystem::file_size(root + "/meta") == 33);
    CHECK(!fs::host::WriteFileDurably(root + "/no/such/dir/file", bytes.data(), bytes.size()));
}

// ---- D45: UTF-8 paths ------------------------------------------------------------------------------------------------

TEST_CASE("D45: the host file calls take UTF-8 paths with accents and other non-ASCII characters") {
    TempDir dir(kUnicodeName);  // the user folder is <temporary>/José-ñ-日本
    const std::string user = dir.UserText();
    REQUIRE(user.find(kUnicodeBytes) != std::string::npos);
    CHECK(fs::host::ToPath(user) == dir.User());
    CHECK(fs::host::ToUtf8(dir.User()) == user);
    // fs::path's operator/ writes the preferred separator and ToPath keeps the separators of its input, so on
    // Windows the text of the two is the same folder with '\\' against '/': compare it in one style (the accents
    // and every byte of the names stay exact).
    const auto one_style = [](std::string text) {
        std::replace(text.begin(), text.end(), '\\', '/');
        return text;
    };
    CHECK(one_style(fs::host::ToUtf8(fs::host::ToPath(user + "/sdmc/"))) ==
          one_style(fs::host::ToUtf8(dir.User() / "sdmc" / "")));
    CHECK(fs::host::IsDirectory(user));
    CHECK(fs::host::Exists(user));

    // the folders of a save under it (CreateFullPath makes each one), with names of their own that are not ASCII
    const std::string deep = user + "/sdmc/Nintendo 3DS/" + kUnicodeBytes + "/";
    CHECK(fs::host::CreateFullPath(deep));
    CHECK(fs::host::IsDirectory(deep));
    CHECK(fs::host::IsDirectory(Utf8(dir.User() / "sdmc" / "Nintendo 3DS" / kUnicodeName)));
    const std::string accent_name = "caf\xC3\xA9-\xE6\x97\xA5.bin";
    const std::string file = deep + accent_name;
    CHECK(fs::host::CreateEmptyFile(file));
    CHECK(fs::host::Exists(file));
    const std::vector<u8> bytes = Pattern(33, 4);
    CHECK(fs::host::WriteFileDurably(deep + "meta-\xE6\x97\xA5", bytes.data(), bytes.size()));
    CHECK(ReadBytesFrom(dir.User() / "sdmc" / "Nintendo 3DS" / kUnicodeName / std::filesystem::path(u8"meta-日")) == bytes);

    // a listing gives the names as UTF-8, whole
    const auto children = fs::host::ListDirectory(deep);
    REQUIRE(children.has_value());
    std::vector<std::string> names;
    for (const fs::host::Child& child : *children) {
        names.push_back(child.name);
        if (child.name == accent_name) {
            CHECK(!child.is_directory);
            CHECK(child.size == 0);
        } else {
            CHECK(child.size == 33);
        }
    }
    std::sort(names.begin(), names.end());
    CHECK(names == std::vector<std::string>{accent_name, "meta-\xE6\x97\xA5"});

    // a file is opened for writing and for reading by its UTF-8 name
    std::string error;
    {
        const auto writable = fs::WritableFile::Open(file, true, &error);
        REQUIRE_MESSAGE(writable != nullptr, error);
        CHECK(writable->WriteAt(0, bytes.data(), bytes.size()) == bytes.size());
        CHECK(writable->Close());
    }
    const auto positioned = fs::PositionedFile::Open(file, &error);
    REQUIRE_MESSAGE(positioned != nullptr, error);
    CHECK(positioned->Size() == bytes.size());
    std::vector<u8> back(bytes.size());
    CHECK(positioned->ReadAt(0, back.data(), back.size()) == back.size());
    CHECK(back == bytes);
    CHECK(fs::host::FileSize(file) == bytes.size());
    fs::host::SyncDirectoryTree(user);

    // delete one file, then the whole tree (the recursive delete lists each folder and removes by UTF-8 name)
    CHECK(fs::host::RemoveFile(deep + "meta-\xE6\x97\xA5"));
    CHECK(!fs::host::Exists(deep + "meta-\xE6\x97\xA5"));
    CHECK(fs::host::DeleteDirectoryRecursively(user + "/sdmc"));
    CHECK(!fs::host::Exists(user + "/sdmc"));
    CHECK(fs::host::IsDirectory(user));
}

TEST_CASE("D45: a save, an extdata and a listing in a user folder with non-ASCII characters") {
    Fixture f;
    // The fixture keeps only the message text, so another module's "cannot ..." line (the cfg module's start-up
    // step, say) would trip a check on `f.logged`. The fs module's own lines are told apart by their category
    // (all of them are "Service.FS"), and any line of it that says "cannot" still fails the test.
    std::vector<std::string> fs_trouble;
    SetLogSink([&](const LogRecord& r) {
        f.logged.push_back(std::string(r.message));
        if (r.category.rfind("Service.FS", 0) == 0 && r.message.find("cannot") != std::string_view::npos) {
            fs_trouble.push_back(std::string(r.message));
        }
    });
    TempDir dir(kUnicodeName);
    const std::string accent_name = "caf\xC3\xA9-\xE6\x97\xA5.bin";
    Result format = 0, create_save_file = 0, create_named = 0, create_ext = 0, create_ext_file = 0, close_save = 0,
           close_ext = 0;
    Guest::Opened save_file, ext_file, listing_save, listing_ext;
    Guest::ReadResult save_read, ext_read;
    std::vector<std::string> save_names, ext_names;
    std::string user_in_runtime;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none"}, [&] {
        Guest g(f, dir);
        user_in_runtime = f.runtime->UserFolder();
        format = g.Format();
        const u64 save = g.MustOpen(4, kEmpty, kEmptyPath);
        create_save_file = g.CreateFile(save, "/save.bin", 0x40);
        create_named = g.CreateFile(save, "/" + accent_name, 0x10);
        save_file = g.OpenFile(save, "/save.bin", 3);
        g.Write(save_file.session, 0, Pattern(0x40, 6), 1);
        save_read = g.Read(save_file.session, 0, 0x40);
        close_save = g.CloseFile(save_file.session);
        listing_save = g.OpenDirectory(save, "/");
        for (const auto& raw : g.ReadDirectory(listing_save.session, 8)) {
            save_names.push_back(Decode(raw).name);
        }
        g.CloseDirectory(listing_save.session);

        create_ext = g.CreateExtSaveData(1, 0, 0x1234, Pattern(0x30, 1));
        const u64 ext = g.MustOpen(6, kBinary, ExtPath(1, 0x1234, 0));
        create_ext_file = g.CreateFile(ext, "/data.bin", 0x20);
        g.CreateFile(ext, "/" + accent_name, 0x8);
        ext_file = g.OpenFile(ext, "/data.bin", 1);
        g.Write(ext_file.session, 0, Pattern(0x20, 4), 1);
        ext_read = g.Read(ext_file.session, 0, 0x20);
        close_ext = g.CloseFile(ext_file.session);
        listing_ext = g.OpenDirectory(ext, "/");
        for (const auto& raw : g.ReadDirectory(listing_ext.session, 8)) {
            ext_names.push_back(Decode(raw).name);
        }
        g.CloseDirectory(listing_ext.session);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    // --user reached the runtime as the UTF-8 bytes it was given
    CHECK(user_in_runtime == dir.UserText());
    CHECK(user_in_runtime.find(kUnicodeBytes) != std::string::npos);
    CHECK(format == kResultSuccess);
    CHECK(create_save_file == kResultSuccess);
    CHECK(create_named == kResultSuccess);
    CHECK(save_file.result == kResultSuccess);
    CHECK(save_read.bytes == Pattern(0x40, 6));
    CHECK(close_save == kResultSuccess);
    std::sort(save_names.begin(), save_names.end());
    CHECK(save_names == std::vector<std::string>{accent_name, "save.bin"});
    CHECK(create_ext == kResultSuccess);
    CHECK(create_ext_file == kResultSuccess);
    CHECK(ext_file.result == kResultSuccess);
    CHECK(ext_read.bytes == Pattern(0x20, 4));
    CHECK(close_ext == kResultSuccess);
    std::sort(ext_names.begin(), ext_names.end());
    CHECK(ext_names == std::vector<std::string>{accent_name, "data.bin"});
    // the folders are where Azahar would leave them, under the user folder with the accents
    CHECK(fs_trouble == std::vector<std::string>{});
    CHECK(ReadBytesFrom(dir.SaveFolder() / "save.bin") == Pattern(0x40, 6));
    CHECK(ReadBytesFrom(dir.SaveFolder() / std::filesystem::path(u8"café-日.bin")).size() == 0x10);
    CHECK(ReadBytesFrom(dir.SaveFolder().parent_path() / "00000001.metadata") ==
          std::vector<u8>{0x00, 0x10, 0, 0, 10, 0, 0, 0, 20, 0, 0, 0, 1, 0, 0, 0});
    CHECK(ReadBytesFrom(dir.ExtFolder(0, 0x1234) / "user" / "data.bin") == Pattern(0x20, 4));
    CHECK(ReadBytesFrom(dir.ExtFolder(0, 0x1234) / "icon") == Pattern(0x30, 1));
    CHECK(ReadBytesFrom(dir.ExtFolder(0, 0x1234) / "user" / std::filesystem::path(u8"café-日.bin")).size() == 8);
}

// ---- D42: no needless sync ---------------------------------------------------------------------------------------------

TEST_CASE("D42: a file that was not written since its last sync is not synced again") {
    TempDir dir;
    const std::string path = (dir.Path() / "f.bin").string();
    REQUIRE(fs::host::WriteFileDurably(path, Pattern(16, 1).data(), 16));
    std::string error;
    const auto file = fs::WritableFile::Open(path, true, &error);
    REQUIRE_MESSAGE(file != nullptr, error);
    const u64 total_before = fs::WritableFile::TotalHostSyncs();

    // opened and only read: clean, and Sync, Close and the destructor never ask the host
    CHECK(!file->Dirty());
    std::vector<u8> buffer(16);
    CHECK(file->ReadAt(0, buffer.data(), 16) == 16);
    CHECK(file->Size() == 16);
    CHECK(file->Sync());
    CHECK(file->Sync());
    CHECK(file->HostSyncCount() == 0);

    // a write makes it dirty, and the sync that follows is the only one
    CHECK(file->WriteAt(0, buffer.data(), 4) == 4);
    CHECK(file->Dirty());
    CHECK(file->Sync());
    CHECK(file->HostSyncCount() == 1);
    CHECK(!file->Dirty());
    CHECK(file->Sync());
    CHECK(file->HostSyncCount() == 1);

    // a resize makes it dirty too; a resize the host refuses does not
    CHECK(file->Resize(32));
    CHECK(file->Dirty());
    CHECK(file->Sync());
    CHECK(file->HostSyncCount() == 2);
    CHECK(file->Sync());
    CHECK(file->HostSyncCount() == 2);

    // a write after a sync dirties it again (the flag is not stuck clean)
    CHECK(file->WriteAt(20, buffer.data(), 4) == 4);
    CHECK(file->WriteAt(24, buffer.data(), 4) == 4);
    CHECK(file->Dirty());
    CHECK(file->Sync());
    CHECK(file->HostSyncCount() == 3);

    // a dirty file that is closed is synced by Close, once; a clean one is not
    CHECK(file->WriteAt(0, buffer.data(), 1) == 1);
    CHECK(file->Close());
    CHECK(file->HostSyncCount() == 4);
    CHECK(!file->Dirty());
    CHECK(file->Close());
    CHECK(file->Sync());
    CHECK(file->HostSyncCount() == 4);
    CHECK(fs::WritableFile::TotalHostSyncs() - total_before == 4);

    // a write after Close does nothing and leaves the file clean
    CHECK(file->WriteAt(0, buffer.data(), 1) == 0);
    CHECK(!file->Dirty());
    CHECK(!file->Resize(1));
    CHECK(!file->Dirty());

    // opened and closed with nothing done: no sync at all
    const auto untouched = fs::WritableFile::Open(path, true, &error);
    REQUIRE(untouched != nullptr);
    CHECK(untouched->Close());
    CHECK(untouched->HostSyncCount() == 0);
    CHECK(fs::WritableFile::TotalHostSyncs() - total_before == 4);

    // read-only: nothing to sync, and a write that fails leaves it clean
    const auto read_only = fs::WritableFile::Open(path, false, &error);
    REQUIRE(read_only != nullptr);
    CHECK(read_only->WriteAt(0, buffer.data(), 1) == 0);
    CHECK(!read_only->Dirty());
    CHECK(read_only->Sync());
    CHECK(read_only->Close());
    CHECK(read_only->HostSyncCount() == 0);
    CHECK(fs::WritableFile::TotalHostSyncs() - total_before == 4);
}

TEST_CASE("D42: a file that was written is synced when it goes away without Close") {
    TempDir dir;
    const std::string path = (dir.Path() / "f.bin").string();
    REQUIRE(fs::host::WriteFileDurably(path, Pattern(16, 1).data(), 16));
    const u64 before = fs::WritableFile::TotalHostSyncs();
    {
        const auto clean = fs::WritableFile::Open(path, true, nullptr);
        REQUIRE(clean != nullptr);
    }
    CHECK(fs::WritableFile::TotalHostSyncs() == before);  // clean: the destructor does not sync
    {
        const auto dirty = fs::WritableFile::Open(path, true, nullptr);
        REQUIRE(dirty != nullptr);
        const std::vector<u8> bytes = Pattern(4, 2);
        CHECK(dirty->WriteAt(0, bytes.data(), 4) == 4);
    }
    CHECK(fs::WritableFile::TotalHostSyncs() == before + 1);  // dirty: the destructor syncs once
    const std::vector<u8> content = ReadBytesFrom(path);
    REQUIRE(content.size() >= 4);
    CHECK(std::vector<u8>(content.begin(), content.begin() + 4) == Pattern(4, 2));
}

TEST_CASE("D42: writers and syncers on one file never leave a change unsynced") {
    TempDir dir;
    const std::string path = (dir.Path() / "f.bin").string();
    REQUIRE(fs::host::WriteFileDurably(path, std::vector<u8>(256, 0).data(), 256));
    const auto file = fs::WritableFile::Open(path, true, nullptr);
    REQUIRE(file != nullptr);
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            const u8 value = static_cast<u8>(t + 1);
            for (int round = 0; round < 200; ++round) {
                if (file->WriteAt(static_cast<u64>(t) * 8, &value, 1) != 1 || !file->Sync()) {
                    ++failures;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK(failures == 0);
    // the last write of every thread was followed by that thread's Sync, so whatever is left is at most the
    // flag of a write that landed while another Sync ran; one more Sync settles it, and then it is clean
    CHECK(file->Sync());
    CHECK(!file->Dirty());
    const u64 count = file->HostSyncCount();
    CHECK(count >= 1);
    CHECK(count <= 4 * 200 + 1);
    CHECK(file->Sync());
    CHECK(file->HostSyncCount() == count);
}

TEST_CASE("D42: through fs:USER, a file that was only read costs no sync when it is closed") {
    Fixture f;
    TempDir dir;
    u64 open_read_close = 0, flush_write = 0, flush_after = 0, close_after_flush = 0, plain_write = 0,
        close_after_plain = 0, set_size = 0, close_after_size = 0, flush_clean = 0, ext_read_close = 0,
        ext_flush_write = 0, ext_close = 0, ext_plain_write = 0, ext_close_dirty = 0;
    Result close_result = 0;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none"}, [&] {
        Guest g(f, dir);
        const auto syncs = [] { return fs::WritableFile::TotalHostSyncs(); };
        g.Format();
        const u64 a = g.MustOpen(4, kEmpty, kEmptyPath);
        g.CreateFile(a, "/s.bin", 0x40);
        g.CreateFile(a, "/t.bin", 0x40);
        g.CreateFile(a, "/u.bin", 0x40);
        g.CreateFile(a, "/v.bin", 0x40);

        // a file opened for reading and writing, only read, closed
        u64 mark = syncs();
        Guest::Opened o = g.OpenFile(a, "/s.bin", 3);
        g.Read(o.session, 0, 0x10);
        g.GetSize(o.session);
        close_result = g.CloseFile(o.session);
        open_read_close = syncs() - mark;

        // a write with the flush flag syncs; the Flush and the Close after it find the file clean
        o = g.OpenFile(a, "/s.bin", 3);
        mark = syncs();
        g.Write(o.session, 0, Pattern(8, 1), 1);
        flush_write = syncs() - mark;
        mark = syncs();
        g.Flush(o.session);
        flush_after = syncs() - mark;
        mark = syncs();
        g.CloseFile(o.session);
        close_after_flush = syncs() - mark;

        // a write without the flush flag is synced by the Close, once
        o = g.OpenFile(a, "/t.bin", 3);
        mark = syncs();
        g.Write(o.session, 0, Pattern(8, 2), 0);
        plain_write = syncs() - mark;
        mark = syncs();
        g.CloseFile(o.session);
        close_after_plain = syncs() - mark;

        // SetSize syncs (it always did), and then the file is clean
        o = g.OpenFile(a, "/u.bin", 3);
        mark = syncs();
        g.SetSize(o.session, 0x80);
        set_size = syncs() - mark;
        mark = syncs();
        g.CloseFile(o.session);
        close_after_size = syncs() - mark;

        // a Flush of a file that was not written does nothing
        o = g.OpenFile(a, "/v.bin", 3);
        mark = syncs();
        g.Flush(o.session);
        flush_clean = syncs() - mark;
        g.CloseFile(o.session);

        // extdata files are always opened for reading and writing, even when the game asked to read
        g.CreateExtSaveData(1, 0, 0x55, Pattern(4, 1));
        const u64 e = g.MustOpen(6, kBinary, ExtPath(1, 0x55, 0));
        g.CreateFile(e, "/d.bin", 0x20);
        o = g.OpenFile(e, "/d.bin", 1);
        mark = syncs();
        g.Read(o.session, 0, 0x20);
        g.CloseFile(o.session);
        ext_read_close = syncs() - mark;
        o = g.OpenFile(e, "/d.bin", 1);
        mark = syncs();
        g.Write(o.session, 0, Pattern(8, 3), 1);
        ext_flush_write = syncs() - mark;
        mark = syncs();
        g.CloseFile(o.session);
        ext_close = syncs() - mark;
        o = g.OpenFile(e, "/d.bin", 1);
        mark = syncs();
        g.Write(o.session, 0, Pattern(8, 4), 0);
        ext_plain_write = syncs() - mark;
        mark = syncs();
        g.CloseFile(o.session);
        ext_close_dirty = syncs() - mark;
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(close_result == kResultSuccess);
    CHECK(open_read_close == 0);
    CHECK(flush_write == 1);
    CHECK(flush_after == 0);
    CHECK(close_after_flush == 0);
    CHECK(plain_write == 0);
    CHECK(close_after_plain == 1);
    CHECK(set_size == 1);
    CHECK(close_after_size == 0);
    CHECK(flush_clean == 0);
    CHECK(ext_read_close == 0);
    CHECK(ext_flush_write == 1);
    CHECK(ext_close == 0);
    CHECK(ext_plain_write == 0);
    CHECK(ext_close_dirty == 1);
    // none of it cost data: what was written is on the disk
    const std::vector<u8> save_content = ReadBytesFrom(dir.SaveFolder() / "s.bin");
    REQUIRE(save_content.size() >= 8);
    CHECK(std::vector<u8>(save_content.begin(), save_content.begin() + 8) == Pattern(8, 1));
    CHECK(ReadBytesFrom(dir.SaveFolder() / "u.bin").size() == 0x80);
    const std::vector<u8> ext_content = ReadBytesFrom(dir.ExtFolder(0, 0x55) / "user" / "d.bin");
    REQUIRE(ext_content.size() >= 8);
    CHECK(std::vector<u8>(ext_content.begin(), ext_content.begin() + 8) == Pattern(8, 4));
}

// ---- stale files of a Windows delete -----------------------------------------------------------------------------------

TEST_CASE("IsStaleDeletedName matches <name>.deleted.<digits>.<digits> and nothing else") {
    using fs::host::IsStaleDeletedName;
    CHECK(IsStaleDeletedName("save.bin.deleted.4242.0"));
    CHECK(IsStaleDeletedName("a.deleted.1.2"));
    CHECK(IsStaleDeletedName("x.deleted.0.0"));
    CHECK(IsStaleDeletedName("name with spaces.deleted.123456.789"));
    CHECK(IsStaleDeletedName("a.deleted.b.deleted.1.2"));                // the name may hold the word
    CHECK(IsStaleDeletedName("caf\xC3\xA9.deleted.7.8"));
    CHECK(!IsStaleDeletedName(""));
    CHECK(!IsStaleDeletedName("save.bin"));
    CHECK(!IsStaleDeletedName("save.bin.deleted"));
    CHECK(!IsStaleDeletedName("save.bin.deleted.12"));
    CHECK(!IsStaleDeletedName("save.bin.deleted.12."));
    CHECK(!IsStaleDeletedName("save.bin.deleted..12"));
    CHECK(!IsStaleDeletedName("save.bin.deleted.1.2x"));
    CHECK(!IsStaleDeletedName("save.bin.deleted.x1.2"));
    CHECK(!IsStaleDeletedName("save.bin.deleted.1.2.txt"));
    CHECK(!IsStaleDeletedName("save.bin.deleted.1.2.3"));                // three numbers: the second one is not ".deleted"
    CHECK(!IsStaleDeletedName("save.bin.Deleted.1.2"));
    CHECK(!IsStaleDeletedName("save.bin.undeleted.1.2"));                // ".undeleted" is not ".deleted"
    CHECK(!IsStaleDeletedName("deleted.1.2"));
    CHECK(!IsStaleDeletedName(".deleted.1.2"));                          // nothing in front of it
    CHECK(!IsStaleDeletedName("save.bin.deleted.-1.2"));
    CHECK(!IsStaleDeletedName("save.bin.deleted.1.+2"));
    CHECK(!IsStaleDeletedName("save.bin.deleted. 1.2"));
    CHECK(!IsStaleDeletedName("save.bin.deleted.\xD9\xA1.2"));           // an Arabic-Indic digit is not a digit here
}

TEST_CASE("RemoveStaleDeletedFiles removes the orphans of a folder and its subfolders, and nothing else") {
    TempDir dir;
    const std::string root = (dir.Path() / "t").string();
    REQUIRE(fs::host::CreateFullPath(root + "/save/sub/deeper/"));
    REQUIRE(fs::host::CreateFullPath(root + "/other/"));
    const auto put = [&](const std::string& path) { WriteBytesTo(path, {1, 2, 3}); };
    // orphans, at every depth
    put(root + "/save/a.bin.deleted.4242.0");
    put(root + "/save/sub/b.bin.deleted.1.2");
    put(root + "/save/sub/deeper/c.deleted.77.78");
    // not orphans
    put(root + "/save/a.bin");
    put(root + "/save/a.bin.deleted");
    put(root + "/save/a.bin.deleted.1");
    put(root + "/save/a.bin.deleted.x.2");
    put(root + "/save/a.bin.deleted.1.2.bak");
    put(root + "/save/.deleted.1.2");
    // a directory with the name of one, with a file in it (the file is kept; the directory is not a file)
    REQUIRE(fs::host::CreateFullPath(root + "/save/dir.deleted.3.4/"));
    put(root + "/save/dir.deleted.3.4/keep.bin");
    // outside the folder given
    put(root + "/other/z.deleted.1.2");
    put(root + "/top.deleted.1.2");

    CHECK(fs::host::RemoveStaleDeletedFiles(root + "/save") == 3);
    CHECK(!fs::host::Exists(root + "/save/a.bin.deleted.4242.0"));
    CHECK(!fs::host::Exists(root + "/save/sub/b.bin.deleted.1.2"));
    CHECK(!fs::host::Exists(root + "/save/sub/deeper/c.deleted.77.78"));
    for (const char* kept : {"/save/a.bin", "/save/a.bin.deleted", "/save/a.bin.deleted.1", "/save/a.bin.deleted.x.2",
                             "/save/a.bin.deleted.1.2.bak", "/save/.deleted.1.2", "/save/dir.deleted.3.4/keep.bin",
                             "/other/z.deleted.1.2", "/top.deleted.1.2"}) {
        CHECK_MESSAGE(fs::host::Exists(root + kept), kept);
    }
    CHECK(fs::host::IsDirectory(root + "/save/dir.deleted.3.4"));
    CHECK(fs::host::RemoveStaleDeletedFiles(root + "/save") == 0);  // again: nothing left
    CHECK(fs::host::RemoveStaleDeletedFiles(root + "/missing") == 0);
    CHECK(fs::host::RemoveStaleDeletedFiles(root + "/save/a.bin") == 0);  // not a folder
    CHECK(fs::host::RemoveStaleDeletedFiles("") == 0);
#if !defined(_WIN32)
    // a link is neither removed nor followed
    std::filesystem::create_directories(dir.Path() / "outside");
    WriteBytesTo(dir.Path() / "outside" / "o.deleted.1.2", {9});
    std::filesystem::create_directory_symlink(dir.Path() / "outside", root + "/save/link");
    std::filesystem::create_symlink(dir.Path() / "outside" / "o.deleted.1.2", root + "/save/l.deleted.5.6");
    CHECK(fs::host::RemoveStaleDeletedFiles(root + "/save") == 0);
    CHECK(std::filesystem::exists(dir.Path() / "outside" / "o.deleted.1.2"));
    CHECK(std::filesystem::is_symlink(root + "/save/l.deleted.5.6"));
    CHECK(std::filesystem::is_symlink(root + "/save/link"));
#endif
}

TEST_CASE("opening a save data or extdata archive removes the orphans of a deleted-while-open file, there only") {
    Fixture f;
    TempDir dir;
    const auto put = [](const std::filesystem::path& path) { WriteBytesTo(path, {1, 2, 3}); };
    Result open_save = 0, open_save_again = 0, open_ext = 0, open_boss = 0, open_other = 0;
    // what is on the disk after the extdata archive was opened, and after the boss archive was
    bool ext_user_orphan = true, ext_sub_orphan = true, ext_kept = false, ext_boss_orphan_after_user = false,
         ext_icon_orphan = false, ext_boss_orphan_after_boss = true;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none"}, [&] {
        Guest g(f, dir);
        g.Format();
        const std::filesystem::path save = dir.SaveFolder();
        std::filesystem::create_directories(save / "sub");
        std::filesystem::create_directories(dir.User() / "sdmc" / "Nintendo 3DS" / "00000000000000000000000000000000" /
                                            "00000000000000000000000000000000" / "title" / "00000000" / "00000002" / "data" /
                                            "00000001");
        const std::filesystem::path other_title = dir.User() / "sdmc" / "Nintendo 3DS" / "00000000000000000000000000000000" /
                                                  "00000000000000000000000000000000" / "title" / "00000000" / "00000002" /
                                                  "data" / "00000001";
        put(save / "keep.bin");
        put(save / "keep.bin.deleted.1");              // not the pattern
        put(save / "gone.bin.deleted.4242.0");
        put(save / "sub" / "gone2.bin.deleted.1.2");
        put(save.parent_path() / "00000001.metadata.deleted.1.2");  // next to the folder, not in it
        put(dir.User() / "root.bin.deleted.1.2");                    // the user folder
        put(dir.User() / "sdmc" / "sd.bin.deleted.1.2");             // the SD card folder
        put(other_title / "other.bin.deleted.1.2");                  // another title's save

        open_save = g.OpenArchive(4, kEmpty, kEmptyPath)[1];
        // every open looks again: a new one appears, and goes at the next open
        put(save / "second.bin.deleted.9.9");
        open_save_again = g.OpenArchive(4, kEmpty, kEmptyPath)[1];

        // extdata: the user/ folder of the archive that is opened
        g.CreateExtSaveData(1, 0, 0x1234, Pattern(4, 1));
        const std::filesystem::path ext = dir.ExtFolder(0, 0x1234);
        std::filesystem::create_directories(ext / "user" / "sub");
        put(ext / "user" / "e.bin");
        put(ext / "user" / "e.bin.deleted.10.20");
        put(ext / "user" / "sub" / "f.bin.deleted.30.40");
        put(ext / "boss" / "b.bin.deleted.50.60");
        put(ext / "icon.deleted.1.2");
        open_ext = g.OpenArchive(6, kBinary, ExtPath(1, 0x1234, 0))[1];
        ext_user_orphan = std::filesystem::exists(ext / "user" / "e.bin.deleted.10.20");
        ext_sub_orphan = std::filesystem::exists(ext / "user" / "sub" / "f.bin.deleted.30.40");
        ext_kept = std::filesystem::exists(ext / "user" / "e.bin");
        ext_boss_orphan_after_user = std::filesystem::exists(ext / "boss" / "b.bin.deleted.50.60");  // another archive's folder
        ext_icon_orphan = std::filesystem::exists(ext / "icon.deleted.1.2");                         // not inside an archive
        open_boss = g.OpenArchive(0x12345678, kBinary, ExtPath(1, 0x1234, 0))[1];
        ext_boss_orphan_after_boss = std::filesystem::exists(ext / "boss" / "b.bin.deleted.50.60");
        // an archive that does not exist opens nothing and removes nothing
        open_other = g.OpenArchive(6, kBinary, ExtPath(1, 0x9999, 0))[1];
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(open_save == kResultSuccess);
    CHECK(open_save_again == kResultSuccess);
    CHECK(open_ext == kResultSuccess);
    CHECK(open_boss == kResultSuccess);
    CHECK(open_other == kNotFoundInvalidState);
    CHECK(!ext_user_orphan);
    CHECK(!ext_sub_orphan);
    CHECK(ext_kept);
    CHECK(ext_boss_orphan_after_user);
    CHECK(ext_icon_orphan);
    CHECK(!ext_boss_orphan_after_boss);
    const std::filesystem::path save = dir.SaveFolder();
    CHECK(std::filesystem::exists(save / "keep.bin"));
    CHECK(std::filesystem::exists(save / "keep.bin.deleted.1"));
    CHECK(!std::filesystem::exists(save / "gone.bin.deleted.4242.0"));
    CHECK(!std::filesystem::exists(save / "sub" / "gone2.bin.deleted.1.2"));
    CHECK(!std::filesystem::exists(save / "second.bin.deleted.9.9"));
    CHECK(std::filesystem::exists(save.parent_path() / "00000001.metadata.deleted.1.2"));
    CHECK(std::filesystem::exists(dir.User() / "root.bin.deleted.1.2"));
    CHECK(std::filesystem::exists(dir.User() / "sdmc" / "sd.bin.deleted.1.2"));
    CHECK(std::filesystem::exists(dir.User() / "sdmc" / "Nintendo 3DS" / "00000000000000000000000000000000" /
                                  "00000000000000000000000000000000" / "title" / "00000000" / "00000002" / "data" /
                                  "00000001" / "other.bin.deleted.1.2"));
}

#if defined(_WIN32)
TEST_CASE("Windows: a file deleted while it is open leaves, at most, a name that RemoveStaleDeletedFiles knows") {
    TempDir dir;
    const std::string folder = (dir.Path() / "w").string();
    REQUIRE(fs::host::CreateFullPath(folder + "/"));
    const std::string path = folder + "/a.bin";
    REQUIRE(fs::host::WriteFileDurably(path, Pattern(8, 1).data(), 8));
    const auto open = fs::WritableFile::Open(path, true, nullptr);
    REQUIRE(open != nullptr);
    CHECK(fs::host::RemoveFile(path));
    CHECK(!fs::host::Exists(path));  // the name is free at once
    // while the handle is open the file may still be listed, renamed; whatever is there has the orphan's name
    std::size_t leftovers = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        ++leftovers;
        CHECK_MESSAGE(fs::host::IsStaleDeletedName(Utf8(entry.path().filename())), Utf8(entry.path().filename()));
    }
    CHECK(leftovers <= 1);
    CHECK(fs::host::WriteFileDurably(path, Pattern(8, 2).data(), 8));  // and the name can be made again
}
#endif

// ---- N1-12c: what the replay of a recorded save session showed -----------------------------------------------------------
//
// The replay of G1, G2 and G6 on the maintainer's PC differs in the descriptor kind of a directory session, in
// the archive handle words of OpenArchive, in the result codes of requests that carry a handle and in
// directory listings. Each test below pins one of the causes the code could have (or shows that the code
// does what Azahar does); docs/native/ipc-replay.md has the rest.

namespace {

// An archive with nothing in it, to count the handles of the archive manager without a runtime.
class NothingArchive final : public fs::ArchiveBackend {
public:
    std::string GetName() const override { return "nothing"; }
    Result OpenFile(const fs::Path&, u32, u32, std::shared_ptr<fs::FileBackend>*) override { return kFileNotFound; }
};

// Opens (a path whose first byte is 'o') or fails (any other): what a game's OpenArchive meets in one run,
// a save that is there and a save that is not formatted.
class SometimesFactory final : public fs::ArchiveFactory {
public:
    std::string GetName() const override { return "sometimes"; }
    Result Open(const fs::Path& path, u64, std::unique_ptr<fs::ArchiveBackend>* archive) override {
        if (path.data.empty() || path.data[0] != 'o') {
            return kNotFormatted;
        }
        *archive = std::make_unique<NothingArchive>();
        return kResultSuccess;
    }
};

fs::Path TextPath(const char* text) {
    fs::Path path;
    path.raw_type = kChar;
    path.data.assign(text, text + std::strlen(text));
    return path;
}

}  // namespace

TEST_CASE("N1-12c: archive handles are numbered as Azahar's ArchiveManager numbers them") {
    // Azahar (archive.cpp, OpenArchive): `next_handle` starts at 1; a number is taken only when the open
    // succeeded, is never given back when the archive is closed, and skips a number in use. The reference's
    // trace therefore shows 1, 2, 3, ... for the successful opens, in the order they were made, and a
    // replay whose opens succeed and fail in the same places sees the same numbers.
    fs::ArchiveManager manager;
    REQUIRE(manager.Register(0x77, std::make_unique<SometimesFactory>()));
    fs::ArchiveHandle handle = 0;

    CHECK(manager.Open(0x77, TextPath("open"), 1, &handle) == kResultSuccess);
    CHECK(handle == 1);
    CHECK(manager.Open(0x77, TextPath("fail"), 1, &handle) == kNotFormatted);  // takes no number
    CHECK(manager.Open(0x99, TextPath("open"), 1, &handle) == 0xC8804478);     // an id nobody registered: NotFound (120), none either
    CHECK(manager.Open(0x77, TextPath("open"), 1, &handle) == kResultSuccess);
    CHECK(handle == 2);
    CHECK(manager.Close(1) == kResultSuccess);
    CHECK(manager.Close(1) == kInvalidArchiveHandle);  // already closed
    CHECK(manager.Close(77) == kInvalidArchiveHandle);  // never opened; takes no number
    CHECK(manager.Get(1) == nullptr);
    CHECK(manager.Get(2) != nullptr);
    CHECK(manager.Open(0x77, TextPath("open"), 1, &handle) == kResultSuccess);
    CHECK(handle == 3);  // not 1 again
    CHECK(manager.Close(3) == kResultSuccess);
    CHECK(manager.Open(0x77, TextPath("open"), 1, &handle) == kResultSuccess);
    CHECK(handle == 4);
}

TEST_CASE("N1-12c: through fs:USER an OpenFileDirectly takes a number whether or not its file opens, a failed OpenArchive none") {
    // Azahar's OpenFileDirectly opens the archive (a number), opens the file, and closes the archive again.
    Fixture f;
    TempDir dir;
    std::vector<u64> handles;
    std::vector<Result> opened, others;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none"}, [&] {
        Guest g(f, dir);
        const auto open = [&](u32 id, u32 type, const std::vector<u8>& path) {
            const std::vector<u32> got = g.OpenArchive(id, type, path);
            opened.push_back(got[1]);
            handles.push_back(got[2] | (static_cast<u64>(got[3]) << 32));
        };
        open(0x1234, kEmpty, kEmptyPath);       // no such archive id: no number
        open(4, kEmpty, kEmptyPath);            // not formatted: none
        g.Format();
        open(4, kEmpty, kEmptyPath);            // 1
        open(6, kBinary, ExtPath(1, 0x77, 0));  // no such extdata: none
        open(4, kEmpty, kEmptyPath);            // 2
        others.push_back(g.CloseArchive(handles[2]));
        others.push_back(g.CloseArchive(handles[2]));  // closed twice: invalid handle
        // OpenFileDirectly: the file is not there, but the archive was opened (and closed): number 3
        others.push_back(g.OpenFileDirectly(4, kEmpty, kEmptyPath, "/missing.bin")[1]);
        // an archive that does not open: no number
        others.push_back(g.OpenFileDirectly(6, kBinary, ExtPath(1, 0x77, 0), "/x")[1]);
        open(4, kEmpty, kEmptyPath);            // 4
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(handles == std::vector<u64>{0, 0, 1, 0, 2, 4});
    CHECK(opened == std::vector<Result>{0xC8804478,  // OpenArchive of an unknown id: NotFound (120)
                                        kNotFormatted, kResultSuccess, kNotFoundInvalidState, kResultSuccess,
                                        kResultSuccess});
    CHECK(others == std::vector<Result>{kResultSuccess,         // CloseArchive(1)
                                        kInvalidArchiveHandle,  // CloseArchive(1) again
                                        kFileNotFound,          // OpenFileDirectly: the file
                                        kNotFoundInvalidState});  // OpenFileDirectly: the archive
}

TEST_CASE("N1-12c: an IPC trace names a directory session fs:USER/dir#k and a file session fs:USER/file#k") {
    // The reference runner names them `dir` and `file` (the reference runner's IpcKindOf); the replay compares
    // the kind in the name of every handle a reply hands out.
    TempDir dir;
    const std::string trace = (dir.Path() / "ipc.txt").string();
    {
        Fixture f;
        const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none", "--trace-ipc", trace}, [&] {
            Guest g(f, dir, false);
            g.Format();
            const u64 a = g.MustOpen(4, kEmpty, kEmptyPath);
            g.CreateFile(a, "/f.bin", 4);
            const Guest::Opened file = g.OpenFile(a, "/f.bin", 1);
            const Guest::Opened directory = g.OpenDirectory(a, "/");
            const Guest::Opened second = g.OpenDirectory(a, "/");
            g.ReadDirectory(directory.session, 2);
            g.CloseDirectory(directory.session);
            g.ReadDirectory(second.session, 2);
            g.CloseFile(file.session);
            ExitProcess(*f.runtime);
        });
        REQUIRE(status == 0);
    }
    const std::string text = [&] {
        const std::vector<u8> bytes = ReadBytesFrom(trace);
        return std::string(bytes.begin(), bytes.end());
    }();
    INFO(text);
    // OpenFile hands out file#0, the two OpenDirectory dir#0 and dir#1, and each later request names its session
    CHECK(text.find("outdesc=move:2:1:fs:USER/file#0") != std::string::npos);
    CHECK(text.find("outdesc=move:2:1:fs:USER/dir#0") != std::string::npos);
    CHECK(text.find("outdesc=move:2:1:fs:USER/dir#1") != std::string::npos);
    CHECK(text.find("session=fs:USER/dir#0 cmd=0x08010042") != std::string::npos);
    CHECK(text.find("session=fs:USER/dir#1 cmd=0x08010042") != std::string::npos);
    CHECK(text.find("session=fs:USER/file#0 cmd=0x08080000") != std::string::npos);
    CHECK(text.find("directory") == std::string::npos);
}

TEST_CASE("N1-12c: a listing is Azahar's, byte for byte: names, 8.3 names, attributes, sizes, zero padding") {
    // Worked out from DiskDirectory::Read and FileUtil::SplitFilename83: the UTF-16 name (at most 261 units
    // and the null), the short name from the UTF-8 bytes of the name (the letters not in ".\"/\\[]:;=, ",
    // upper-cased bytewise, the 9th giving "~1" in places 6 and 7) and the first three bytes after the last
    // dot as the extension, hidden for a leading dot, the archive bit for every file, the size of a file and
    // 0 for a directory, every other byte zero.
    struct Expected {
        std::string name;
        std::u16string utf16;
        std::string short_name;  // 8 characters
        std::string extension;   // 3 characters
        bool is_directory;
        bool is_hidden;
        u64 size;
    };
    // 100 bytes: long, but a path with it stays under Windows' 260 characters in a temporary folder
    const std::string long_name = std::string(96, 'L') + ".txt";
    std::vector<Expected> expected = {
        {"longfilename.txt", u"longfilename.txt", "LONGFI~1", "TXT", false, false, 7},
        {".hidden", u".hidden", "        ", "HID", false, true, 3},
        {"Folder", u"Folder", "FOLDER  ", "   ", true, false, 0},
        {"My File.tar.gz", u"My File.tar.gz", "MYFILE~1", "GZ ", false, false, 1},
        {"trailing.", u"trailing.", "TRAILING", "   ", false, false, 2},
        {"UPPER.C", u"UPPER.C", "UPPER   ", "C  ", false, false, 0},
        {"a.b.c.dd", u"a.b.c.dd", "ABC     ", "DD ", false, false, 4},
        {"dir.d", u"dir.d", "DIR     ", "D  ", true, false, 0},  // a directory's extension is taken the same way
        {"\xC3\xA9.bin", u"é.bin", "\xC3\xA9      ", "BIN", false, false, 5},
        {"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E.txt", u"日本語.txt",
         std::string("\xE6\x97\xA5\xE6\x9C\xAC") + "~1", "TXT", false, false, 6},  // 9 bytes: the tilde
        {"\xF0\x9F\x98\x80.txt", u"\xD83D\xDE00.txt", std::string("\xF0\x9F\x98\x80") + "    ", "TXT", false, false, 8},
        {long_name, std::u16string(96, u'L') + u".txt", "LLLLLL~1", "TXT", false, false, 9},
    };
#if defined(_WIN32)
    // Windows drops the dot at the end of a name when it makes the file
    expected.erase(std::remove_if(expected.begin(), expected.end(),
                                  [](const Expected& e) { return e.name == "trailing."; }),
                   expected.end());
#endif
    TempDir dir;
    const std::filesystem::path folder = dir.Path() / "listing";
    std::filesystem::create_directories(folder);
    for (const Expected& e : expected) {
        const std::filesystem::path path = folder / std::filesystem::path(std::u8string(e.name.begin(), e.name.end()));
        if (e.is_directory) {
            std::filesystem::create_directory(path);
            if (e.name == "Folder") {
                WriteBytesTo(path / "inside.bin", Pattern(1000, 1));  // a directory's size stays 0
            }
        } else {
            WriteBytesTo(path, Pattern(static_cast<std::size_t>(e.size), 3));
        }
    }

    const std::unique_ptr<fs::DirectoryBackend> directory = fs::OpenDiskDirectory(Utf8(folder));
    std::vector<fs::DirEntry> entries(expected.size() + 4);
    const u32 count = directory->Read(static_cast<u32>(entries.size()), entries.data());
    REQUIRE(count == expected.size());
    CHECK(directory->Read(4, entries.data()) == 0);  // nothing left
    for (u32 i = 0; i < count; ++i) {
        std::vector<u8> raw(sizeof(fs::DirEntry));
        std::memcpy(raw.data(), &entries[i], raw.size());
        const Entry decoded = Decode(raw);
        const auto found = std::find_if(expected.begin(), expected.end(), [&](const Expected& e) { return e.name == decoded.name; });
        REQUIRE_MESSAGE(found != expected.end(), "an entry that was not made: " << decoded.name.size() << " bytes");
        const Expected& e = *found;

        // Azahar's Entry as bytes: all zero but the fields it sets.
        std::vector<u8> want(0x228, 0);
        for (std::size_t j = 0; j < e.utf16.size(); ++j) {
            want[j * 2] = static_cast<u8>(e.utf16[j]);
            want[j * 2 + 1] = static_cast<u8>(e.utf16[j] >> 8);
        }
        for (std::size_t j = 0; j < 8; ++j) {
            want[0x20C + j] = static_cast<u8>(e.short_name[j]);  // [8] of the short name is the null
        }
        for (std::size_t j = 0; j < 3; ++j) {
            want[0x216 + j] = static_cast<u8>(e.extension[j]);  // [3] of the extension is the null
        }
        want[0x21C] = e.is_directory ? 1 : 0;
        want[0x21D] = e.is_hidden ? 1 : 0;
        want[0x21E] = e.is_directory ? 0 : 1;
        want[0x21F] = 0;
        for (int j = 0; j < 8; ++j) {
            want[0x220 + static_cast<std::size_t>(j)] = static_cast<u8>(e.size >> (8 * j));
        }
        CHECK_MESSAGE(raw == want, "entry " << i << " of " << e.name.size() << " name bytes, short name "
                                            << e.short_name.size());
    }
}

namespace {

// A folder whose names were made in an order that is not the sorted one: mixed case, digits, an underscore, a
// name beyond ASCII, a directory among the files and (where the host tells the case apart) two names that
// differ only by the case of a letter. Returns the names in the D54 order; `kinds` says which are directories.
struct OrderFolder {
    std::vector<std::string> sorted;
    std::map<std::string, bool> is_directory;
};

OrderFolder MakeOrderFolder(const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    OrderFolder made;
    const auto file = [&](const std::string& name) { WriteBytesTo(folder / std::filesystem::path(std::u8string(name.begin(), name.end())), Pattern(4, 1)); };
    for (const char* name : {"zeta", "Alpha", "_under", "mid", "Beta", "alpha2", "0first", "Zed", "tie", "Zz"}) {
        file(name);
    }
    file("\xC3\xA9.bin");  // U+00E9: beyond ASCII, so after '_' (0x5F)
    std::filesystem::create_directories(folder / "Mdir");
    made.is_directory["Mdir"] = true;
    // Two names that differ only by case: a case-insensitive host (NTFS, APFS by default) has made one
    file("Tie");
    std::size_t on_host = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        (void)entry;
        ++on_host;
    }
    const bool case_sensitive = on_host == 13;
    // folded: 0FIRST ALPHA ALPHA2 BETA MDIR MID TIE(Tie, tie: 'T' < 't') ZED ZETA ZZ _UNDER U+00E9
    made.sorted = {"0first", "Alpha", "alpha2", "Beta", "Mdir", "mid"};
    if (case_sensitive) {
        made.sorted.insert(made.sorted.end(), {"Tie", "tie"});
    } else {
        // the host kept one of the two names (which one is the host's)
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            const std::string name = Utf8(entry.path().filename());
            if (name == "Tie" || name == "tie") {
                made.sorted.push_back(name);
            }
        }
    }
    made.sorted.insert(made.sorted.end(), {"Zed", "zeta", "Zz", "_under", "\xC3\xA9.bin"});
    return made;
}

std::vector<std::string> NamesOf(const std::vector<std::vector<u8>>& raw_entries, std::vector<Entry>* decoded = nullptr) {
    std::vector<std::string> names;
    for (const std::vector<u8>& raw : raw_entries) {
        const Entry entry = Decode(raw);
        names.push_back(entry.name);
        if (decoded != nullptr) {
            decoded->push_back(entry);
        }
    }
    return names;
}

void CheckOrderedEntries(const OrderFolder& made, const std::vector<std::vector<u8>>& raw_entries) {
    std::vector<Entry> decoded;
    CHECK(NamesOf(raw_entries, &decoded) == made.sorted);
    REQUIRE(decoded.size() == made.sorted.size());
    for (std::size_t i = 0; i < decoded.size(); ++i) {
        const bool directory = made.is_directory.count(made.sorted[i]) != 0;
        CHECK_MESSAGE(decoded[i].is_directory == directory, "entry " << i);
        CHECK_MESSAGE(decoded[i].file_size == (directory ? 0u : 4u), "entry " << i);
        CHECK_MESSAGE(decoded[i].is_archive == !directory, "entry " << i);
        CHECK_MESSAGE(decoded[i].is_hidden == false, "entry " << i);
    }
}

}  // namespace

TEST_CASE("D54: the ordering helper compares UTF-16 code units after upper-casing the ASCII letters") {
    using fs::host::DirectoryNameLess;
    // '_' (0x5F) is after the letters, which compare as upper case (0x41..0x5A): NTFS's order for such names
    CHECK(DirectoryNameLess(u"Zed", u"_a"));
    CHECK(!DirectoryNameLess(u"_a", u"Zed"));
    CHECK(DirectoryNameLess(u"zeta", u"_a"));
    // 'a' and 'B' compare as 'A' and 'B'
    CHECK(DirectoryNameLess(u"a", u"B"));
    CHECK(!DirectoryNameLess(u"B", u"a"));
    CHECK(DirectoryNameLess(u"Ab", u"aC"));
    // digits before letters
    CHECK(DirectoryNameLess(u"9", u"a"));
    CHECK(DirectoryNameLess(u"0first", u"Alpha"));
    // a prefix first
    CHECK(DirectoryNameLess(u"Alpha", u"alpha2"));
    // names that differ only by the case of ASCII letters: bytewise, so upper case first
    CHECK(DirectoryNameLess(u"Tie", u"tie"));
    CHECK(!DirectoryNameLess(u"tie", u"Tie"));
    CHECK(!DirectoryNameLess(u"tie", u"tie"));
    // only ASCII letters are folded: U+00C9 and U+00E9 stay what they are, and both are after 'z'
    CHECK(DirectoryNameLess(u"É", u"é"));
    CHECK(DirectoryNameLess(u"z", u"É"));
    // UTF-16 code units, not code points: a surrogate (0xD800) is before U+FF21, though U+10000 is the larger
    // code point
    CHECK(DirectoryNameLess(u"\U00010000", u"Ａ"));
}

TEST_CASE("D54: a listing is sorted, not in the host's order") {
    TempDir dir;
    const std::filesystem::path folder = dir.Path() / "order";
    const OrderFolder made = MakeOrderFolder(folder);
    const std::unique_ptr<fs::DirectoryBackend> directory = fs::OpenDiskDirectory(Utf8(folder));
    std::vector<fs::DirEntry> entries(32);
    const u32 count = directory->Read(32, entries.data());
    REQUIRE(count == made.sorted.size());
    std::vector<std::vector<u8>> raw;
    for (u32 i = 0; i < count; ++i) {
        raw.emplace_back(sizeof(fs::DirEntry));
        std::memcpy(raw.back().data(), &entries[i], sizeof(fs::DirEntry));
    }
    CheckOrderedEntries(made, raw);
    // host::ListDirectory is in that order too
    const auto children = fs::host::ListDirectory(Utf8(folder));
    REQUIRE(children.has_value());
    std::vector<std::string> listed;
    for (const fs::host::Child& child : *children) {
        listed.push_back(child.name);
    }
    CHECK(listed == made.sorted);
}

TEST_CASE("D54: fs:USER lists a SaveData and an ExtSaveData directory in the sorted order") {
    Fixture f;
    TempDir dir;
    OrderFolder save_made, ext_made;
    std::vector<std::vector<u8>> save_entries, ext_entries;
    Result save_result = kUnknown, ext_result = kUnknown;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none"}, [&] {
        Guest g(f, dir);
        g.Format();
        REQUIRE(g.CreateExtSaveData(1, 0, 0x1234, Pattern(8, 1)) == kResultSuccess);
        save_made = MakeOrderFolder(dir.SaveFolder() / "d");
        ext_made = MakeOrderFolder(dir.ExtFolder(0, 0x1234) / "user" / "d");
        const u64 save = g.MustOpen(4, kEmpty, kEmptyPath);
        const Guest::Opened save_dir = g.OpenDirectory(save, "/d");
        REQUIRE(save_dir.result == kResultSuccess);
        save_entries = g.ReadDirectory(save_dir.session, 14, &save_result);
        const u64 ext = g.MustOpen(6, kBinary, ExtPath(1, 0x1234, 0));
        const Guest::Opened ext_dir = g.OpenDirectory(ext, "/d");
        REQUIRE(ext_dir.result == kResultSuccess);
        ext_entries = g.ReadDirectory(ext_dir.session, 14, &ext_result);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(save_result == kResultSuccess);
    CHECK(ext_result == kResultSuccess);
    CheckOrderedEntries(save_made, save_entries);
    CheckOrderedEntries(ext_made, ext_entries);
}

TEST_CASE("N1-12c: a directory read writes the entries it read and nothing after them; the bytes after stay the client's") {
    // Azahar's Directory::Read writes `read * 0x228` bytes into the client's mapped buffer. What the buffer
    // held after them is what the recording's CRC covers, so the native must not clear or fill it.
    Fixture f;
    TempDir dir;
    u32 read = 0, second = 0;
    std::vector<u8> buffer, after;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none"}, [&] {
        Guest g(f, dir);
        g.Format();
        const u64 a = g.MustOpen(4, kEmpty, kEmptyPath);
        g.CreateFile(a, "/one.bin", 1);
        g.CreateFile(a, "/two.bin", 2);
        const Guest::Opened opened = g.OpenDirectory(a, "/");
        std::tie(read, buffer) = g.ReadDirectoryBuffer(opened.session, 5, 5, 0xEE);
        std::tie(second, after) = g.ReadDirectoryBuffer(opened.session, 5, 5, 0x5A);  // nothing left
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(read == 2);
    REQUIRE(buffer.size() == 5 * 0x228);
    CHECK(std::all_of(buffer.begin() + 2 * 0x228, buffer.end(), [](u8 b) { return b == 0xEE; }));
    // inside an entry the bytes the service writes are all of it: after the name's null, zeros
    CHECK(buffer[0x228 * 0 + 0x20A] == 0);
    CHECK(buffer[0x228 * 1 + 0x20A] == 0);
    CHECK(std::none_of(buffer.begin(), buffer.begin() + 2 * 0x228, [](u8 b) { return b == 0xEE; }));
    CHECK(second == 0);
    CHECK(std::all_of(after.begin(), after.end(), [](u8 b) { return b == 0x5A; }));  // not touched at all
}

TEST_CASE("N1-12c: the same request in SaveData, ExtSaveData and SharedExtSaveData gets Azahar's result code") {
    // Decoded from Azahar's errors.h (0xC82044B4 is description 180, FileAlreadyExists, NothingHappened,
    // Status: the answer to a CreateFile of a file that is there; 0xC8804470 is description 112,
    // FileNotFound: the answer to an OpenFile of one that is not; 0xC8804465 is ArchiveNotMounted, a handle
    // that is not open).
    struct Kind {
        const char* name;
        bool zero_size_create;  // ExtSaveDataArchive refuses a file of size 0 and the create flag; SaveData allows both
    };
    const std::vector<Kind> kinds = {{"SaveData", true}, {"ExtSaveData", false}, {"SharedExtSaveData", false}};
    for (const Kind& kind : kinds) {
        CAPTURE(kind.name);
        Fixture f;
        TempDir dir;
        std::map<std::string, Result> r;
        const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none"}, [&] {
            Guest g(f, dir);
            u64 a = 0;
            if (std::string(kind.name) == "SaveData") {
                g.Format();
                a = g.MustOpen(4, kEmpty, kEmptyPath);
            } else if (std::string(kind.name) == "ExtSaveData") {
                REQUIRE(g.CreateExtSaveData(1, 0, 0x1234, Pattern(8, 1)) == kResultSuccess);
                a = g.MustOpen(6, kBinary, ExtPath(1, 0x1234, 0));
            } else {
                REQUIRE(g.CreateExtSaveData(0, 0x48000, 0xF000000E, Pattern(8, 1)) == kResultSuccess);
                a = g.MustOpen(7, kBinary, ExtPath(0, 0xF000000E, 0));
            }
            r["create"] = g.CreateFile(a, "/gamecoin.dat", 0x10);
            r["create again"] = g.CreateFile(a, "/gamecoin.dat", 0x10);
            g.CreateDirectory(a, "/dir");
            r["create over a directory"] = g.CreateFile(a, "/dir", 0x10);
            r["create under a file"] = g.CreateFile(a, "/gamecoin.dat/x", 0x10);
            r["create in a missing dir"] = g.CreateFile(a, "/nodir/x", 0x10);
            r["create zero size"] = g.CreateFile(a, "/zero.bin", 0);
            r["create invalid path"] = g.CreateFile(a, "x", 0x10);
            r["open"] = g.OpenFile(a, "/gamecoin.dat", 3).result;
            r["open missing"] = g.OpenFile(a, "/missing.bin", 1).result;
            r["open missing, read and write"] = g.OpenFile(a, "/missing.bin", 3).result;
            r["open missing in a missing dir"] = g.OpenFile(a, "/nodir/missing.bin", 1).result;
            r["open under a file"] = g.OpenFile(a, "/gamecoin.dat/x", 1).result;
            r["open a directory"] = g.OpenFile(a, "/dir", 1).result;
            r["open with no flags"] = g.OpenFile(a, "/gamecoin.dat", 0).result;
            r["open with create"] = g.OpenFile(a, "/new.bin", 7).result;
            r["delete missing"] = g.DeleteFile(a, "/missing.bin");
            r["delete in a missing dir"] = g.DeleteFile(a, "/nodir/missing.bin");
            r["open directory"] = g.OpenDirectory(a, "/dir").result;
            r["open directory missing"] = g.OpenDirectory(a, "/nodir").result;
            r["open directory on a file"] = g.OpenDirectory(a, "/gamecoin.dat").result;
            r["close"] = g.CloseArchive(a);
            // The handle is closed now: every request that carries it is "invalid archive handle"
            r["closed: create"] = g.CreateFile(a, "/gamecoin.dat", 0x10);
            r["closed: open"] = g.OpenFile(a, "/gamecoin.dat", 1).result;
            r["closed: open directory"] = g.OpenDirectory(a, "/").result;
            r["closed: delete"] = g.DeleteFile(a, "/gamecoin.dat");
            r["closed: create directory"] = g.CreateDirectory(a, "/x");
            r["closed: close again"] = g.CloseArchive(a);
            ExitProcess(*f.runtime);
        });
        CHECK(status == 0);
        CHECK(r["create"] == kResultSuccess);
        CHECK(r["create again"] == kFileAlreadyExists);          // 0xC82044B4
        CHECK(r["create over a directory"] == kFileAlreadyExists);
        CHECK(r["create under a file"] == kUnexpectedFileOrDirectory);
        CHECK(r["create in a missing dir"] == kPathNotFound);
        CHECK(r["create zero size"] == (kind.zero_size_create ? kResultSuccess : kUnsupportedOpenFlags));
        CHECK(r["create invalid path"] == kInvalidPath);
        CHECK(r["open"] == kResultSuccess);
        CHECK(r["open missing"] == kFileNotFound);               // 0xC8804470
        CHECK(r["open missing, read and write"] == kFileNotFound);
        CHECK(r["open missing in a missing dir"] == kPathNotFound);
        CHECK(r["open under a file"] == kUnexpectedFileOrDirectory);
        CHECK(r["open a directory"] == kUnexpectedFileOrDirectory);
        CHECK(r["open with no flags"] == kUnsupportedOpenFlags);
        // read, write and create: SaveData makes the file; the extdata archives never take the create flag
        CHECK(r["open with create"] == (kind.zero_size_create ? kResultSuccess : kUnsupportedOpenFlags));
        CHECK(r["delete missing"] == kFileNotFound);
        CHECK(r["delete in a missing dir"] == kPathNotFound);
        CHECK(r["open directory"] == kResultSuccess);
        CHECK(r["open directory missing"] == kPathNotFound);
        CHECK(r["open directory on a file"] == kUnexpectedFileOrDirectory);
        CHECK(r["close"] == kResultSuccess);
        CHECK(r["closed: create"] == kInvalidArchiveHandle);     // 0xC8804465
        CHECK(r["closed: open"] == kInvalidArchiveHandle);
        CHECK(r["closed: open directory"] == kInvalidArchiveHandle);
        CHECK(r["closed: delete"] == kInvalidArchiveHandle);
        CHECK(r["closed: create directory"] == kInvalidArchiveHandle);
        CHECK(r["closed: close again"] == kInvalidArchiveHandle);
    }
}


namespace {

// Records a session like the start of a game's: the shared extdata archive that Azahar's PTM makes at
// start-up (it is in the user folder before the run), a CreateFile in it that finds its file there, a save
// that is not formatted and is then, directories and files, an OpenFile of a missing file, a listing.
// Returns the trace's path; `before` is a copy of the user folder as it was before the run.
std::string RecordStartOfGame(const TempDir& dir, const std::filesystem::path& before) {
    const std::filesystem::path shared = dir.SharedExtFolder(0x48000, 0xF000000E);
    std::filesystem::create_directories(shared / "user");
    std::filesystem::create_directories(shared / "boss");
    WriteBytesTo(shared / "metadata", std::vector<u8>(16, 0));
    WriteBytesTo(shared / "user" / "gamecoin.dat", Pattern(0x10, 9));
    std::filesystem::copy(dir.User(), before, std::filesystem::copy_options::recursive);

    const std::string trace = (dir.Path() / "ipc.txt").string();
    Fixture f;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "none", "--trace-ipc", trace, "--trace-ipc-full"}, [&] {
        Guest g(f, dir, false);
        const u64 shared_archive = g.MustOpen(7, kBinary, ExtPath(0, 0xF000000E, 0));
        CHECK(g.CreateFile(shared_archive, "/gamecoin.dat", 0x10) == kFileAlreadyExists);
        CHECK(g.OpenArchive(4, kEmpty, kEmptyPath)[1] == kNotFormatted);
        CHECK(g.Format() == kResultSuccess);
        const u64 save = g.MustOpen(4, kEmpty, kEmptyPath);
        CHECK(g.CreateDirectory(save, "/d") == kResultSuccess);
        CHECK(g.CreateFile(save, "/d/b.bin", 3) == kResultSuccess);
        CHECK(g.CreateFile(save, "/d/a.bin", 2) == kResultSuccess);
        CHECK(g.OpenFile(save, "/d/missing.bin", 1).result == kFileNotFound);
        const Guest::Opened directory = g.OpenDirectory(save, "/d");
        REQUIRE(directory.result == kResultSuccess);
        CHECK(g.ReadDirectoryBuffer(directory.session, 4, 4, 0).first == 2);
        CHECK(g.CloseDirectory(directory.session) == kResultSuccess);
        CHECK(g.CloseArchive(shared_archive) == kResultSuccess);
        CHECK(g.CloseArchive(save) == kResultSuccess);
        g.MustOpen(4, kEmpty, kEmptyPath);
        ExitProcess(*f.runtime);
    });
    REQUIRE(status == 0);
    return trace;
}

struct ReplayRun {
    int status = -1;
    std::string out, err;
};

ReplayRun ReplaySave(const std::string& trace, const TempDir& dir, const std::filesystem::path& user) {
    std::ostringstream out, err;
    ReplayRun run;
    run.status = ipc_replay::RunIpcReplay({trace, "--service", "fs:USER", "--game", dir.Game(), "--user", Utf8(user),
                                           "--io-latency", "none", "--continue"},
                                          out, err);
    run.out = out.str();
    run.err = err.str();
    return run;
}

}  // namespace

TEST_CASE("N1-12c: the replay of a recorded save session matches on a copy of its user folder") {
    TempDir dir;
    const std::filesystem::path before = dir.Path() / "before";
    const std::string trace = RecordStartOfGame(dir, before);
    const std::filesystem::path copy = dir.Path() / "copy";
    std::filesystem::copy(before, copy, std::filesystem::copy_options::recursive);
    const ReplayRun run = ReplaySave(trace, dir, copy);
    INFO(run.out);
    INFO(run.err);
    CHECK(run.status == 0);
    CHECK(run.out.find(" 0 differ, 0 skipped") != std::string::npos);
    // the replay is not vacuous: it made the save (and its files) in the copy, and the directory session is
    // named as the reference runner names it, `dir`, which the replay compared with the native's kind
    CHECK(std::filesystem::exists(copy / "sdmc"));
    const std::vector<u8> bytes = ReadBytesFrom(trace);
    const std::string text(bytes.begin(), bytes.end());
    CHECK(text.find("outdesc=move:2:1:fs:USER/dir#0") != std::string::npos);
}

TEST_CASE("N1-12c: an archive that opens in the recording and not in the replay shifts every later handle, and the replay says so") {
    // What the maintainer's first workaround showed: the shared extdata archive that Azahar's PTM makes at
    // start-up was not there, so the native's OpenArchive failed where the reference's succeeded. The
    // reference's handles are 1, 2, 3 and the native's are 1, 2 for the same opens; the replay sends the reference's
    // values, so every request that carries one differs, and a CreateFile in the archive that exists in the
    // recording answers "invalid archive handle". The replay does not remap handle words (the numbering is
    // Azahar's, see the test above): the first difference is the one to remove.
    TempDir dir;
    const std::filesystem::path before = dir.Path() / "before";
    const std::string trace = RecordStartOfGame(dir, before);
    const std::filesystem::path copy = dir.Path() / "copy";
    std::filesystem::copy(before, copy, std::filesystem::copy_options::recursive);
    std::filesystem::remove_all(copy / "nand");
    const ReplayRun run = ReplaySave(trace, dir, copy);
    INFO(run.out);
    CHECK(run.status == 1);
    // 1. the archive that is not there: expected success, got NotFormatted (340)
    CHECK(run.out.find("DIFF fs:USER seq=3 session=fs:USER header=0x080c00c2\n"
                       "  result: expected 0x00000000, got 0xc8a04554 (expected: description 0 (Success), module 0 "
                       "(Common), summary 0 (Success), level 0 (Success); got: description 340 (NotFormatted), module "
                       "17 (FS), summary 5 (InvalidState), level 25 (Status))\n") != std::string::npos);
    // 2. the CreateFile in it: expected "file exists" (180), got "invalid archive handle" (101)
    CHECK(run.out.find("result: expected 0xc82044b4, got 0xc8804465 (expected: description 180 (FileAlreadyExists), "
                       "module 17 (FS), summary 1 (NothingHappened), level 25 (Status); got: description 101 "
                       "(ArchiveNotMounted), module 17 (FS), summary 4 (NotFound), level 25 (Status))") != std::string::npos);
    // 3. the save's OpenArchive, after the format: the reference's handle is 2, the native's 1; the word is named
    //    by its number, never by value
    CHECK(run.out.find("DIFF fs:USER seq=7 session=fs:USER header=0x080c00c2\n  normal word 2 differs\n") !=
          std::string::npos);
    // 4. everything that carries a reference handle fails (the native has none of that number)
    CHECK(run.out.find("DIFF fs:USER seq=11 session=fs:USER header=0x080201c2\n  result: expected 0xc8804470, got "
                       "0xc8804465") != std::string::npos);  // the OpenFile of a missing file
}


#endif  // __has_include("recomp.h")
