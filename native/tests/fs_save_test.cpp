// Derived from Azahar src/core/file_sys/savedata_archive.cpp, src/core/file_sys/archive_extsavedata.cpp,
// src/core/file_sys/archive_source_sd_savedata.cpp, src/core/file_sys/archive_other_savedata.cpp,
// src/core/file_sys/disk_archive.cpp, src/core/file_sys/errors.h, src/core/hle/service/fs/fs_user.cpp,
// src/core/hle/service/fs/directory.cpp, src/core/hle/service/fs/archive.cpp and
// src/core/hle/service/fs/archive.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: archive.h
// says "GPLv3 or any later version", the source of the layout of ExtSaveDataInfo; the others say "GPLv2 or any
// later version"): the result codes written out as numbers, the paths of the folders under the user folder,
// the commands' parameters and replies, and what each operation answers for a missing or an existing path.
// Copyright 2016-2024 Citra Emulator Project / Azahar Emulator Project (savedata_archive.cpp,
//   archive_other_savedata.cpp)
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (archive_source_sd_savedata.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (disk_archive.cpp, fs_user.cpp,
//   archive.cpp, archive.h)
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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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

void WriteBytesTo(const std::filesystem::path& path, const std::vector<u8>& bytes) {
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<u8> ReadBytesFrom(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A temporary folder with a game folder and a user folder; the user folder is the one the tests look at.
class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("mh4n_save_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                 std::to_string(counter++));
        std::filesystem::create_directories(path_ / "game");
        std::filesystem::create_directories(path_ / "user");
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    std::filesystem::path Path() const { return path_; }
    std::filesystem::path User() const { return path_ / "user"; }
    std::string Game() const { return (path_ / "game").string(); }
    std::string UserText() const { return User().string(); }
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

    Guest(Fixture& f, const TempDir& dir) : rt(*f.runtime) {
        Runtime& runtime = *f.runtime;
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
        const std::shared_ptr<fs::FsState> state = manager->Context().State<fs::FsState>("fs:USER");
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
        r["missing"] = g.OpenArchive(7, kBinary, ExtPath(0, 0xF000000B, 0x12345))[1];
        r["create"] = g.CreateExtSaveData(0, 0x00048000, 0xF000000B, Pattern(8, 1));
        // the id the game asks with has any high word: Azahar overwrites it
        const u64 a = g.MustOpen(7, kBinary, ExtPath(0, 0xF000000B, 0xDEAD));
        r["create file"] = g.CreateFile(a, "/f.bin", 4);
        r["unknown"] = g.OpenArchive(7, kBinary, ExtPath(0, 0xF0000099, 0))[1];
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(r["missing"] == kNotFormatted);
    CHECK(r["create"] == kResultSuccess);
    CHECK(r["create file"] == kResultSuccess);
    CHECK(r["unknown"] == kNotFormatted);
    const std::filesystem::path folder = dir.SharedExtFolder(0x48000, 0xF000000B);
    CHECK(std::filesystem::is_directory(folder / "user"));
    CHECK(std::filesystem::is_directory(folder / "boss"));
    CHECK(std::filesystem::exists(folder / "metadata"));
    CHECK(std::filesystem::file_size(folder / "user" / "f.bin") == 4);
    CHECK(std::filesystem::exists(folder / "icon"));  // the game's high word was the shared one
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

#endif  // __has_include("recomp.h")
