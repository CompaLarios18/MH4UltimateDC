// Derived from Azahar src/core/hle/service/fs/fs_user.cpp, src/core/hle/service/fs/file.cpp,
// src/core/hle/service/fs/archive.h, src/core/file_sys/archive_selfncch.cpp,
// src/core/file_sys/archive_ncch.cpp, src/core/file_sys/errors.h, src/core/file_sys/ivfc_archive.h and
// externals/open_source_archives/Readme.md at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later:
// file.cpp and archive.h say "GPLv3 or any later version"; the others say "GPLv2 or any later version";
// Readme.md names no licence): the command ids and replies of fs:USER and of a file session, the archive
// ids, the layouts of the archive and file paths, the result codes of a missing RomFS, title and archive
// handle, the title ids of the system archives, and the delays of a RomFS open and read (94 ns a byte plus
// 582778 ns, at least 663124 ns; 9438006 ns to open).
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (fs_user.cpp, archive.h,
//   archive_ncch.cpp)
// Copyright 2018-2026 Citra Emulator Project / Azahar Emulator Project (file.cpp)
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (archive_selfncch.cpp)
// Copyright 2016 Citra Emulator Project (errors.h)
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (ivfc_archive.h)
//
// fs:USER and the RomFS (N1-12), through the SVC layer: the guest opens the service by name and sends
// requests as the game does. The game folder is made by the test (a synthetic RomFS with one directory
// and two files, in a section with a 0x1000-byte header); time is a manual clock (it waits for the I/O
// threads, then skips idle time), so a reply's delay is a number of ticks and not a measurement.
#include <doctest/doctest.h>

#if !__has_include("recomp.h")
TEST_CASE("fs_romfs_test needs recomp.h (the repository around native/)" * doctest::skip()) {}
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
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../src/ipc_replay/tool.h"
#include "../src/services/fs/fs_state.h"
#include "../src/services/fs/positioned_file.h"
#include "../src/services/fs/system_archives.h"
#include "mh4n/clock.h"
#include "mh4n/crc32.h"
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
    Put32(b, 52 + 16, 0x40), Put32(b, 52 + 20, 0x40), Put32(b, 52 + 24, 5),
        Put32(b, 52 + 28, 0x1000);
    for (u32 i = 0; i < 0x40; ++i) {
        b.push_back(static_cast<u8>(i));
    }
    return b;
}

void WriteFile(const std::filesystem::path& path, const std::vector<u8>& bytes) {
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("mh4n_fs_test_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                 std::to_string(counter++));
        std::filesystem::create_directories(path_ / "game");
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    std::filesystem::path Path() const { return path_; }
    std::string Game() const { return (path_ / "game").string(); }
    std::string WriteElf() const {
        const std::string path = (path_ / "program.elf").string();
        WriteFile(path, MakeElf());
        return path;
    }

private:
    std::filesystem::path path_;
};

// ---- a synthetic RomFS: the root directory with some files --------------------------------------------------

struct RomFsFile {
    std::u16string name;
    std::vector<u8> data;
};

void Append32(std::vector<u8>& b, u32 v) {
    for (int i = 0; i < 4; ++i) {
        b.push_back(static_cast<u8>(v >> (8 * i)));
    }
}
void Append64(std::vector<u8>& b, u64 v) {
    Append32(b, static_cast<u32>(v));
    Append32(b, static_cast<u32>(v >> 32));
}

// The level 3 RomFS of 3dbrew: the header, the directory hash table (one bucket) and metadata (the root),
// the file hash table (one bucket) and metadata, then the data.
std::vector<u8> MakeRomFs(const std::vector<RomFsFile>& files) {
    std::vector<u8> dir_meta;
    Append32(dir_meta, 0);           // parent
    Append32(dir_meta, 0xFFFFFFFF);  // sibling
    Append32(dir_meta, 0xFFFFFFFF);  // first subdirectory
    Append32(dir_meta, 0);           // first file
    Append32(dir_meta, 0xFFFFFFFF);  // next in the hash bucket
    Append32(dir_meta, 0);           // name length

    std::vector<u8> file_meta;
    u64 data_offset = 0;
    std::vector<u8> data;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const std::size_t name_bytes = files[i].name.size() * 2;
        const std::size_t entry_size = 0x20 + ((name_bytes + 3) & ~std::size_t{3});
        const u32 next = i + 1 < files.size() ? static_cast<u32>(file_meta.size() + entry_size) : 0xFFFFFFFFu;
        Append32(file_meta, 0);     // parent directory
        Append32(file_meta, next);  // next sibling
        Append64(file_meta, data_offset);
        Append64(file_meta, files[i].data.size());
        Append32(file_meta, next);  // next in the hash bucket
        Append32(file_meta, static_cast<u32>(name_bytes));
        for (const char16_t c : files[i].name) {
            file_meta.push_back(static_cast<u8>(c));
            file_meta.push_back(static_cast<u8>(c >> 8));
        }
        while (file_meta.size() % 4 != 0) {
            file_meta.push_back(0);
        }
        data.insert(data.end(), files[i].data.begin(), files[i].data.end());
        while (data.size() % 16 != 0) {
            data.push_back(0);
        }
        data_offset = data.size();
    }

    const u32 dir_hash = 0x28, dir_meta_at = dir_hash + 4;
    const u32 file_hash = dir_meta_at + static_cast<u32>(dir_meta.size());
    const u32 file_meta_at = file_hash + 4;
    u32 data_at = file_meta_at + static_cast<u32>(file_meta.size());
    data_at = (data_at + 15) & ~15u;

    std::vector<u8> image;
    Append32(image, 0x28);
    Append32(image, dir_hash), Append32(image, 4);
    Append32(image, dir_meta_at), Append32(image, static_cast<u32>(dir_meta.size()));
    Append32(image, file_hash), Append32(image, 4);
    Append32(image, file_meta_at), Append32(image, static_cast<u32>(file_meta.size()));
    Append32(image, data_at);
    Append32(image, 0);  // dir hash bucket -> the root
    image.insert(image.end(), dir_meta.begin(), dir_meta.end());
    Append32(image, files.empty() ? 0xFFFFFFFFu : 0);  // file hash bucket -> the first file
    image.insert(image.end(), file_meta.begin(), file_meta.end());
    image.resize(data_at, 0);
    image.insert(image.end(), data.begin(), data.end());
    return image;
}

// The NCCH RomFS section: 0x1000 bytes of IVFC header (the SDK never sees them), then the image.
std::vector<u8> MakeSection(const std::vector<u8>& romfs) {
    std::vector<u8> section(0x1000, 0);
    std::memcpy(section.data(), "IVFC", 4);
    section.insert(section.end(), romfs.begin(), romfs.end());
    return section;
}

std::vector<u8> Pattern(std::size_t size, u8 seed) {
    std::vector<u8> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<u8>(seed + i * 7 + (i >> 8));
    }
    return bytes;
}

constexpr u64 kProgramId = 0x0004000000ABCD00;
constexpr u64 kUpdateTitleId = 0x0004000E00ABCD00;

// An exheader the loader accepts: text 3 pages, ro 1, data 2, stack 0x4000; the program id is
// kProgramId. (code.bin is the six pages.)
std::vector<u8> MakeExheader() {
    std::vector<u8> b(0x800, 0);
    std::memcpy(b.data(), "SYNTH", 5);
    b[0x0D] = 1;
    auto put32 = [&](std::size_t at, u32 v) {
        for (int i = 0; i < 4; ++i) {
            b[at + static_cast<std::size_t>(i)] = static_cast<u8>(v >> (8 * i));
        }
    };
    put32(0x10, 0x00100000), put32(0x14, 3), put32(0x18, 0x2F00);
    put32(0x1C, 0x4000);
    put32(0x20, 0x00200000), put32(0x24, 1), put32(0x28, 0x900);
    put32(0x30, 0x00300000), put32(0x34, 2), put32(0x38, 0x1800);
    put32(0x3C, 0x1234);
    put32(0x200, static_cast<u32>(kProgramId)), put32(0x204, static_cast<u32>(kProgramId >> 32));
    put32(0x208, 2);
    b[0x20C] = 3;
    b[0x20D] = 1;
    b[0x20E] = static_cast<u8>(1 | (2 << 2) | (2 << 4));  // ideal processor 1, system mode 2
    b[0x20F] = 0x30;
    b[0x36F] = 0;
    std::vector<u32> caps = {0xFC000000u | 0x23A, 0xFE000000u | 0x200};
    caps.resize(28, 0xFFFFFFFFu);
    for (std::size_t i = 0; i < 28; ++i) {
        put32(0x370 + 4 * i, caps[i]);
    }
    return b;
}

struct GameFolder {
    std::vector<RomFsFile> base_files{{u"intro.bin", Pattern(300, 1)}, {u"data.bin", Pattern(5000, 2)}};
    std::vector<RomFsFile> update_files{{u"intro.bin", Pattern(300, 9)}, {u"data.bin", Pattern(7000, 4)},
                                        {u"new.bin", Pattern(40, 5)}};
    std::vector<u8> base_romfs = MakeRomFs(base_files);
    std::vector<u8> update_romfs = MakeRomFs(update_files);
    std::vector<u8> code = Pattern(6 * 0x1000, 0x33);

    void Write(const TempDir& dir) const {
        const std::filesystem::path game = dir.Path() / "game";
        WriteFile(game / "romfs.bin", MakeSection(base_romfs));
        WriteFile(game / "update_romfs.bin", MakeSection(update_romfs));
        WriteFile(game / "code.bin", code);
        WriteFile(game / "exheader.bin", MakeExheader());
    }
};

// ---- a test archive: a file whose read waits for the test -------------------------------------------------------

constexpr u32 kTestArchiveId = 0x7E570001;

struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool released = true;
    bool entered = false;
    std::thread::id reader;
    bool reader_set = false;

    void Reset(bool hold) {
        std::lock_guard lock(mutex);
        released = !hold;
        entered = false;
        reader_set = false;
    }
    void Release() {
        {
            std::lock_guard lock(mutex);
            released = true;
        }
        cv.notify_all();
    }
};
Gate& TheGate() {
    static Gate gate;
    return gate;
}

class GateFile final : public fs::FileBackend {
public:
    Result Read(u64 offset, std::size_t length, u8* buffer, std::size_t* read) override {
        Gate& gate = TheGate();
        std::unique_lock lock(gate.mutex);
        gate.reader = std::this_thread::get_id();
        gate.reader_set = true;
        gate.entered = true;
        gate.cv.notify_all();
        gate.cv.wait_for(lock, std::chrono::seconds(20), [&] { return gate.released; });
        lock.unlock();
        for (std::size_t i = 0; i < length; ++i) {
            buffer[i] = static_cast<u8>(0xA0 + offset + i);
        }
        *read = length;
        return kResultSuccess;
    }
    u64 GetSize() override { return 0x100; }
};

class GateArchive final : public fs::ArchiveBackend {
public:
    std::string GetName() const override { return "GateArchive"; }
    Result OpenFile(const fs::Path&, u32, u32, std::shared_ptr<fs::FileBackend>* file) override {
        *file = std::make_shared<GateFile>();
        return kResultSuccess;
    }
};

class GateFactory final : public fs::ArchiveFactory {
public:
    std::string GetName() const override { return "Gate"; }
    Result Open(const fs::Path&, u64, std::unique_ptr<fs::ArchiveBackend>* archive) override {
        *archive = std::make_unique<GateArchive>();
        return kResultSuccess;
    }
};

MH4N_REGISTER(fs::ArchiveProvider, "7e57.gate",
              fs::ArchiveProvider{kTestArchiveId, [](fs::FsState&) { return std::make_unique<GateFactory>(); }});

// ---- the clock --------------------------------------------------------------------------------------------------

// A manual clock like ManualTimeSource that does not skip idle time while the file system's I/O threads are
// still reading: it waits for them (in real time) and only then moves to the next deadline. Without that,
// a read that has not finished by the time the scheduler goes idle would lose to the next timer.
class IoAwareTime final : public TimeSource {
public:
    std::function<int()> in_flight = [] { return 0; };
    // False for a test whose I/O thread waits for something the game clock has to produce.
    std::atomic<bool> wait_for_io{true};

    s64 NowNs() override { return now_.load(std::memory_order_acquire); }
    bool WaitUntil(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                   std::optional<s64> deadline_ns, const std::function<bool()>& pred) override {
        while (!pred()) {
            if (!deadline_ns) {
                cv.wait(lock, pred);
                return true;
            }
            if (!wait_for_io || in_flight() == 0) {
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

// ---- the runtime -------------------------------------------------------------------------------------------------

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
        return std::any_of(logged.begin(), logged.end(), [&](const std::string& line) {
            return line.find(needle) != std::string::npos;
        });
    }
};

Options Parse(const std::vector<std::string>& args) {
    const Options options = ParseOptions("runner", args);
    REQUIRE_MESSAGE(options.ok(), options.error());
    return options;
}

int BootWith(Fixture& f, const TempDir& dir, std::vector<std::string> extra, std::function<void()> body) {
    std::vector<std::string> args = {dir.WriteElf()};
    args.insert(args.end(), extra.begin(), extra.end());
    f.runtime->SetMainThreadEntry([body = std::move(body)](GuestThread&) { body(); });
    return f.runtime->Boot(Parse(args));
}

// Boots the game folder (dir/game) with `body` as its main thread.
int BootFolder(Fixture& f, const TempDir& dir, std::vector<std::string> extra, std::function<void()> body) {
    std::vector<std::string> args = {dir.Game()};
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

// The guest side of a run.
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
    Result Close(Handle handle) { return CallSvc(rt, 0x23, {handle}).r[0]; }
    std::vector<u32> Request(Handle session, const std::vector<u32>& words, std::size_t reply_words) {
        PutWords(Cmd(), words);
        REQUIRE(Send(session) == kResultSuccess);
        return GetWords(Cmd(), reply_words);
    }
};

// ---- fs:USER from the guest's side ------------------------------------------------------------------------------

constexpr Result kNotFound = 0xC8804478;
constexpr Result kRomfsNotFound = 0xC8804464;
constexpr Result kInvalidArchiveHandle = 0xC8804465;
constexpr Result kInvalidPath = 0xE0E046BE;
constexpr Result kCommandNotAllowed = 0xD9004676;
constexpr Result kExefsSectionNotFound = 0xC8804637;

enum LowPath : u32 { kEmpty = 1, kBinary = 2 };

// An empty low path is carried as one zero byte.
const std::vector<u8> kEmptyPath = {0};

std::vector<u8> SelfNcchPath(u32 type, const std::string& exefs_name = {}) {
    std::vector<u8> path(12, 0);
    std::memcpy(path.data(), &type, 4);
    std::memcpy(path.data() + 4, exefs_name.data(), std::min<std::size_t>(exefs_name.size(), 8));
    return path;
}

std::vector<u8> NcchArchivePath(u64 title_id, u32 media) {
    std::vector<u8> path(0x10, 0);
    std::memcpy(path.data(), &title_id, 8);
    std::memcpy(path.data() + 8, &media, 4);
    return path;
}

std::vector<u8> NcchFilePath(u32 content_index, u32 file_type, const std::string& exefs_name = {}) {
    std::vector<u8> path(0x14, 0);
    std::memcpy(path.data() + 4, &content_index, 4);
    std::memcpy(path.data() + 8, &file_type, 4);
    std::memcpy(path.data() + 12, exefs_name.data(), std::min<std::size_t>(exefs_name.size(), 8));
    return path;
}

struct Opened {
    std::vector<u32> reply;
    Result result = 0;
    Handle file = 0;
};

// A guest with a second service manager that has the game folder (Prepare gives the services none).
struct FsGuest : Guest {
    Handle fs = 0;
    u32 next_path = 0;

    // `own_manager`: make a second manager that has the game folder (an ELF program has none). Without it
    // the runtime's own is used (the program is the game folder).
    FsGuest(Fixture& f, const TempDir& dir, bool own_manager = true) : Guest(*f.runtime) {
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
        env.request_exit = [&runtime](int status, const std::string& reason) {
            runtime.RequestExit(status, reason);
        };
        auto manager = std::make_shared<ServiceManager>(std::move(env));
        manager->InstallRegistered();
        manager->Install(runtime.Svc());
        runtime.Own(manager);
        manager_ptr = manager.get();
        }
        const std::shared_ptr<fs::FsState> state = manager_ptr->Context().State<fs::FsState>("fs:USER");
        f.time->in_flight = [state] { return state->io_in_flight.load(); };
        Handle srv = 0;
        REQUIRE(ConnectToPort("srv:", &srv) == kResultSuccess);
        std::array<u32, 2> name{};
        std::memcpy(name.data(), "fs:USER", 7);
        const std::vector<u32> got = Request(srv, {ipc::MakeHeader(0x0005, 4, 0), name[0], name[1], 7, 0}, 4);
        REQUIRE(got[1] == kResultSuccess);
        fs = got[3];
    }

    // A path in guest memory, one after the other.
    u32 Place(const std::vector<u8>& bytes) {
        const u32 at = Scratch(0x100 + next_path);
        next_path += (static_cast<u32>(bytes.size()) + 15) & ~15u;
        PutBytes(at, bytes);
        return at;
    }

    // 0x0861 InitializeWithSdkVersion (the game sends it first), or 0x0801 Initialize.
    std::vector<u32> Initialize() {
        return Request(fs, {ipc::MakeHeader(0x0861, 1, 2), 0x090B00C8, ipc::CallingPidDesc(), 0}, 2);
    }

    Opened OpenFileDirectly(u32 archive_id, u32 archive_path_type, const std::vector<u8>& archive_path,
                            const std::vector<u8>& file_path, u32 mode = 1) {
        next_path = 0;
        const u32 archive_at = Place(archive_path), file_at = Place(file_path);
        const u32 asize = static_cast<u32>(archive_path.size()), fsize = static_cast<u32>(file_path.size());
        Opened out;
        out.reply = Request(fs,
                            {ipc::MakeHeader(0x0803, 8, 4), 0, archive_id, archive_path_type, asize, kBinary, fsize,
                             mode, 0, ipc::StaticBufferDesc(asize, 0), archive_at, ipc::StaticBufferDesc(fsize, 2),
                             file_at},
                            4);
        out.result = out.reply[1];
        out.file = out.reply[3];
        return out;
    }

    std::vector<u32> OpenArchive(u32 archive_id, u32 path_type, const std::vector<u8>& path) {
        next_path = 0;
        const u32 at = Place(path);
        const u32 size = static_cast<u32>(path.size());
        return Request(fs,
                       {ipc::MakeHeader(0x080C, 3, 2), archive_id, path_type, size, ipc::StaticBufferDesc(size, 0), at},
                       4);
    }

    Opened OpenFile(u64 archive_handle, const std::vector<u8>& file_path) {
        next_path = 0;
        const u32 at = Place(file_path);
        const u32 size = static_cast<u32>(file_path.size());
        Opened out;
        out.reply = Request(fs,
                            {ipc::MakeHeader(0x0802, 7, 2), 0, static_cast<u32>(archive_handle),
                             static_cast<u32>(archive_handle >> 32), kBinary, size, 1, 0,
                             ipc::StaticBufferDesc(size, 0), at},
                            4);
        out.result = out.reply[1];
        out.file = out.reply[3];
        return out;
    }

    Result CloseArchive(u64 handle) {
        return Request(fs, {ipc::MakeHeader(0x080E, 2, 0), static_cast<u32>(handle), static_cast<u32>(handle >> 32)}, 2)[1];
    }

    struct ReadResult {
        Result result = 0;
        u32 count = 0;
        std::vector<u8> bytes;
    };
    // File Read into `buffer_size` bytes of the scratch area asking for `length`.
    ReadResult Read(Handle file, u64 offset, u32 length, u32 buffer_size = 0) {
        if (buffer_size == 0) {
            buffer_size = length;
        }
        const u32 target = Scratch(0x1000);
        PutBytes(target, std::vector<u8>(std::min<u32>(buffer_size, 0x1800), 0xEE));
        const std::vector<u32> reply =
            Request(file,
                    {ipc::MakeHeader(0x0802, 3, 2), static_cast<u32>(offset), static_cast<u32>(offset >> 32), length,
                     ipc::MappedBufferDesc(buffer_size, ipc::W), target},
                    5);
        ReadResult out;
        REQUIRE(reply[0] == ipc::MakeHeader(0x0802, 2, 2));
        out.result = reply[1];
        out.count = reply[2];
        REQUIRE(reply[3] == ipc::MappedBufferDesc(buffer_size, ipc::W));
        REQUIRE(reply[4] == target);
        out.bytes = GetBytes(target, std::min<u32>(std::max(out.count, 1u), 0x1800));
        out.bytes.resize(out.count);
        return out;
    }

    u64 GetSize(Handle file) {
        const std::vector<u32> reply = Request(file, {ipc::MakeHeader(0x0804, 0, 0)}, 4);
        REQUIRE(reply[0] == ipc::MakeHeader(0x0804, 3, 0));
        REQUIRE(reply[1] == kResultSuccess);
        return reply[2] | (static_cast<u64>(reply[3]) << 32);
    }

    u64 Now() { return rt.Clock().Now(); }
};

std::vector<u8> Slice(const std::vector<u8>& bytes, std::size_t from, std::size_t count) {
    return std::vector<u8>(bytes.begin() + static_cast<std::ptrdiff_t>(from),
                           bytes.begin() + static_cast<std::ptrdiff_t>(from + count));
}

}  // namespace

// ---- SelfNCCH ----------------------------------------------------------------------------------------------------

TEST_CASE("SelfNCCH: the RomFS is read from romfs.bin, 0x1000 bytes in, by OpenFileDirectly") {
    Fixture f;
    TempDir dir;
    GameFolder game;
    game.Write(dir);
    Opened opened;
    u64 size = 0;
    FsGuest::ReadResult header, middle, tail, past_end;
    const int status = BootWith(f, dir, {"--ms", "100", "--io-latency", "none"}, [&] {
        FsGuest g(f, dir);
        g.Initialize();
        opened = g.OpenFileDirectly(3, kEmpty, kEmptyPath, SelfNcchPath(0));
        size = g.GetSize(opened.file);
        header = g.Read(opened.file, 0, 0x28);
        middle = g.Read(opened.file, 0x40, 0x80);  // somewhere inside
        tail = g.Read(opened.file, size - 10, 100);  // cut at the end of the section
        past_end = g.Read(opened.file, size + 5, 16);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(opened.reply[0] == ipc::MakeHeader(0x0803, 1, 2));
    CHECK(opened.result == kResultSuccess);
    CHECK(opened.reply[2] == ipc::MoveHandleDesc(1));
    CHECK(opened.file != 0);
    // the file is the section without its first 0x1000 bytes: the image the SDK parses
    CHECK(size == game.base_romfs.size());
    CHECK(header.result == kResultSuccess);
    CHECK(header.bytes == Slice(game.base_romfs, 0, 0x28));
    CHECK(middle.bytes == Slice(game.base_romfs, 0x40, 0x80));
    CHECK(tail.count == 10);
    CHECK(tail.bytes == Slice(game.base_romfs, game.base_romfs.size() - 10, 10));
    CHECK(past_end.result == kResultSuccess);
    CHECK(past_end.count == 0);
}

TEST_CASE("SelfNCCH: path type 5 is the update's RomFS, from update_romfs.bin") {
    Fixture f;
    TempDir dir;
    GameFolder game;
    game.Write(dir);
    Opened base, update;
    u64 base_size = 0, update_size = 0;
    FsGuest::ReadResult update_bytes, base_bytes;
    const int status = BootWith(f, dir, {"--ms", "100", "--io-latency", "none"}, [&] {
        FsGuest g(f, dir);
        g.Initialize();
        base = g.OpenFileDirectly(3, kEmpty, kEmptyPath, SelfNcchPath(0));
        update = g.OpenFileDirectly(3, kEmpty, kEmptyPath, SelfNcchPath(5));
        base_size = g.GetSize(base.file);
        update_size = g.GetSize(update.file);
        base_bytes = g.Read(base.file, 0x100, 64);
        update_bytes = g.Read(update.file, 0x100, 64);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(base.result == kResultSuccess);
    CHECK(update.result == kResultSuccess);
    CHECK(base_size == game.base_romfs.size());
    CHECK(update_size == game.update_romfs.size());
    CHECK(base_bytes.bytes == Slice(game.base_romfs, 0x100, 64));
    CHECK(update_bytes.bytes == Slice(game.update_romfs, 0x100, 64));
    CHECK(base_bytes.bytes != update_bytes.bytes);
}

TEST_CASE("SelfNCCH: OpenArchive, OpenFile and CloseArchive, with Azahar's handles and results") {
    Fixture f;
    TempDir dir;
    GameFolder game;
    game.Write(dir);
    std::vector<u32> first, second, third;
    Opened via_handle, bad_handle;
    Result closed = 0, closed_again = 0;
    FsGuest::ReadResult bytes;
    u32 priority_before = 0, priority_after = 0;
    const int status = BootWith(f, dir, {"--ms", "100", "--io-latency", "none"}, [&] {
        FsGuest g(f, dir);
        g.Initialize();
        first = g.OpenArchive(3, kEmpty, kEmptyPath);
        via_handle = g.OpenFile(first[2] | (static_cast<u64>(first[3]) << 32), SelfNcchPath(0));
        bytes = g.Read(via_handle.file, 0, 0x28);
        bad_handle = g.OpenFile(0x1234, SelfNcchPath(0));
        closed = g.CloseArchive(first[2]);
        closed_again = g.CloseArchive(first[2]);
        // a direct open takes handle number 2 and gives it back: the next archive is number 3
        g.OpenFileDirectly(3, kEmpty, kEmptyPath, SelfNcchPath(0));
        second = g.OpenArchive(3, kEmpty, kEmptyPath);
        third = g.OpenArchive(3, kEmpty, kEmptyPath);
        priority_before = g.Request(g.fs, {ipc::MakeHeader(0x0863, 0, 0)}, 3)[2];
        g.Request(g.fs, {ipc::MakeHeader(0x0862, 1, 0), 7}, 2);
        priority_after = g.Request(g.fs, {ipc::MakeHeader(0x0863, 0, 0)}, 3)[2];
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(first[0] == ipc::MakeHeader(0x080C, 3, 0));
    CHECK(first[1] == kResultSuccess);
    CHECK(first[2] == 1);
    CHECK(first[3] == 0);
    CHECK(via_handle.result == kResultSuccess);
    CHECK(bytes.bytes == Slice(game.base_romfs, 0, 0x28));
    CHECK(bad_handle.result == kInvalidArchiveHandle);
    CHECK(bad_handle.reply[0] == ipc::MakeHeader(0x0802, 1, 2));
    CHECK(bad_handle.file == 0);
    CHECK(closed == kResultSuccess);
    CHECK(closed_again == kInvalidArchiveHandle);
    CHECK(second[2] == 3);
    CHECK(third[2] == 4);
    CHECK(priority_before == 0xFFFFFFFF);  // Azahar: -1 until it is set
    CHECK(priority_after == 7);
}

TEST_CASE("SelfNCCH: the other path types, and a session that did not initialise") {
    Fixture f;
    TempDir dir;
    GameFolder game;
    game.Write(dir);
    Opened uninitialised, code, icon, odd_section, wrong_type, wrong_size;
    const int status = BootWith(f, dir, {"--ms", "100", "--io-latency", "none"}, [&] {
        FsGuest g(f, dir);
        // no Initialize yet: the client has no program, so there is no RomFS for it (Azahar's ncch_data[0])
        uninitialised = g.OpenFileDirectly(3, kEmpty, kEmptyPath, SelfNcchPath(0));
        g.Initialize();
        code = g.OpenFileDirectly(3, kEmpty, kEmptyPath, SelfNcchPath(1));
        icon = g.OpenFileDirectly(3, kEmpty, kEmptyPath, SelfNcchPath(2, "icon"));
        odd_section = g.OpenFileDirectly(3, kEmpty, kEmptyPath, SelfNcchPath(2, "other"));
        wrong_type = g.OpenFileDirectly(3, kEmpty, kEmptyPath, SelfNcchPath(3));
        wrong_size = g.OpenFileDirectly(3, kEmpty, kEmptyPath, std::vector<u8>(8, 0));
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(uninitialised.result == kRomfsNotFound);
    CHECK(uninitialised.reply[2] == ipc::MoveHandleDesc(1));
    CHECK(uninitialised.file == 0);
    CHECK(code.result == kCommandNotAllowed);
    CHECK(icon.result == kExefsSectionNotFound);
    CHECK(odd_section.result == kInvalidPath);
    CHECK(wrong_type.result == kInvalidPath);
    CHECK(wrong_size.result == kInvalidPath);
}

TEST_CASE("SelfNCCH: a game folder without RomFS files has no RomFS") {
    Fixture f;
    TempDir dir;
    Opened base, update;
    const int status = BootWith(f, dir, {"--ms", "100", "--io-latency", "none"}, [&] {
        FsGuest g(f, dir);
        g.Initialize();
        base = g.OpenFileDirectly(3, kEmpty, kEmptyPath, SelfNcchPath(0));
        update = g.OpenFileDirectly(3, kEmpty, kEmptyPath, SelfNcchPath(5));
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(base.result == kRomfsNotFound);
    CHECK(update.result == kRomfsNotFound);
}

// ---- NCCH --------------------------------------------------------------------------------------------------------------

namespace {

struct Manifest {
    const char* name;
    const unsigned char* data;
    unsigned long long size;
    u32 crc;
    u64 title_id;
};
// The sizes and CRC-32 of native/third_party/open_source_archives/MANIFEST.txt: the lengths in Azahar's
// headers (BAD_WORD_LIST_DATA_len, COUNTRY_LIST_DATA_len, MII_DATA_len, SHARED_FONT_DATA_len).
const Manifest kSystemArchives[] = {
    {"bad word list", fs::embedded::kBadWordList, 1508, 0x4fe860bd, 0x000400DB00010302},
    {"country list", fs::embedded::kCountryList, 219040, 0x00e4e89d, 0x0004009B00010402},
    {"mii", fs::embedded::kMii, 73600, 0xdf40a54e, 0x0004009B00010202},
    {"shared font", fs::embedded::kSharedFont, 1110016, 0x3b26cc42, 0x0004009B00014002},
};

}  // namespace

TEST_CASE("the embedded system archives are the bytes of Azahar's open source archives") {
    for (const Manifest& archive : kSystemArchives) {
        INFO(archive.name);
        CHECK(archive.size > 0);
        CHECK(Crc32(archive.data, static_cast<std::size_t>(archive.size)) == archive.crc);
    }
    // sizes are the lengths of the C arrays in Azahar's headers
    CHECK(fs::embedded::kBadWordList_size == 1508);
    CHECK(fs::embedded::kCountryList_size == 219040);
    CHECK(fs::embedded::kMii_size == 73600);
    CHECK(fs::embedded::kSharedFont_size == 1110016);
    // each is a level 3 RomFS image: the header length 0x28 and the file data after the tables
    for (const Manifest& archive : kSystemArchives) {
        INFO(archive.name);
        u32 header_length = 0;
        std::memcpy(&header_length, archive.data, 4);
        CHECK(header_length == 0x28);
    }
}

TEST_CASE("NCCH: the system archives are served from the open source replacements") {
    Fixture f;
    TempDir dir;
    GameFolder game;
    game.Write(dir);
    struct Got {
        Opened opened;
        u64 size = 0;
        u32 crc = 0;
    };
    std::vector<Got> got;
    Opened font_chn;
    const int status = BootWith(f, dir, {"--ms", "100", "--io-latency", "none"}, [&] {
        FsGuest g(f, dir);
        g.Initialize();
        for (const Manifest& archive : kSystemArchives) {
            Got one;
            // the usual media type of the shared archives is NAND
            one.opened = g.OpenFileDirectly(0x2345678A, kBinary, NcchArchivePath(archive.title_id, 0), NcchFilePath(0, 0));
            if (one.opened.result == kResultSuccess) {
                one.size = g.GetSize(one.opened.file);
                u32 crc = 0;
                for (u64 at = 0; at < one.size; at += 0x1000) {
                    const FsGuest::ReadResult chunk = g.Read(one.opened.file, at, 0x1000);
                    crc = Crc32(chunk.bytes.data(), chunk.bytes.size(), crc);
                }
                one.crc = crc;
            }
            got.push_back(std::move(one));
        }
        font_chn = g.OpenFileDirectly(0x2345678A, kBinary, NcchArchivePath(0x0004009B00014102, 0), NcchFilePath(0, 0));
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    REQUIRE(got.size() == 4);
    for (std::size_t i = 0; i < got.size(); ++i) {
        INFO(kSystemArchives[i].name);
        CHECK(got[i].opened.result == kResultSuccess);
        CHECK(got[i].size == kSystemArchives[i].size);
        CHECK(got[i].crc == kSystemArchives[i].crc);
    }
    CHECK(font_chn.result == kResultSuccess);  // CHN/KOR/TWN fonts use the same replacement
    CHECK(f.Logged("Bad Word List file missing"));
}

TEST_CASE("NCCH: the update title's RomFS, the cartridge's, and titles that are not there") {
    Fixture f;
    TempDir dir;
    GameFolder game;
    game.Write(dir);
    Opened update, update_nand, update_content1, base_card, base_sdmc, other, card_other, update_code, update_icon;
    u64 update_size = 0, base_size = 0, code_size = 0;
    FsGuest::ReadResult code_bytes;
    const int status = BootWith(f, dir, {"--ms", "100", "--io-latency", "none"}, [&] {
        FsGuest g(f, dir);
        g.Initialize();
        update = g.OpenFileDirectly(0x2345678A, kBinary, NcchArchivePath(kUpdateTitleId, 1), NcchFilePath(0, 0));
        update_nand = g.OpenFileDirectly(0x2345678A, kBinary, NcchArchivePath(kUpdateTitleId, 0), NcchFilePath(0, 0));
        update_content1 = g.OpenFileDirectly(0x2345678A, kBinary, NcchArchivePath(kUpdateTitleId, 1), NcchFilePath(1, 0));
        base_card = g.OpenFileDirectly(0x2345678A, kBinary, NcchArchivePath(kProgramId, 2), NcchFilePath(0, 0));
        base_sdmc = g.OpenFileDirectly(0x2345678A, kBinary, NcchArchivePath(kProgramId, 1), NcchFilePath(0, 0));
        other = g.OpenFileDirectly(0x2345678A, kBinary, NcchArchivePath(0x0004000000FFFF00, 1), NcchFilePath(0, 0));
        card_other = g.OpenFileDirectly(0x2345678A, kBinary, NcchArchivePath(0x0004000000FFFF00, 2), NcchFilePath(0, 0));
        update_code = g.OpenFileDirectly(0x2345678A, kBinary, NcchArchivePath(kUpdateTitleId, 1), NcchFilePath(0, 1, ".code"));
        update_icon = g.OpenFileDirectly(0x2345678A, kBinary, NcchArchivePath(kUpdateTitleId, 1), NcchFilePath(0, 2, "icon"));
        if (update.result == kResultSuccess) {
            update_size = g.GetSize(update.file);
        }
        if (base_card.result == kResultSuccess) {
            base_size = g.GetSize(base_card.file);
        }
        if (update_code.result == kResultSuccess) {
            code_size = g.GetSize(update_code.file);
            code_bytes = g.Read(update_code.file, 0x10, 0x40);
        }
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(update.result == kResultSuccess);
    CHECK(update_size == game.update_romfs.size());
    CHECK(update_nand.result == kResultSuccess);
    CHECK(update_content1.result == kNotFound);  // the game folder holds content index 0 only
    CHECK(base_card.result == kResultSuccess);
    CHECK(base_size == game.base_romfs.size());
    CHECK(base_sdmc.result == kNotFound);  // the base game is not installed
    CHECK(other.result == kNotFound);
    CHECK(card_other.result == kNotFound);
    CHECK(update_code.result == kResultSuccess);
    CHECK(code_size == game.code.size());
    CHECK(code_bytes.bytes == Slice(game.code, 0x10, 0x40));
    CHECK(update_icon.result == kNotFound);
    CHECK(update.reply[2] == ipc::MoveHandleDesc(1));
    CHECK(other.file == 0);
}

TEST_CASE("NCCH: bad archive and file paths") {
    Fixture f;
    TempDir dir;
    GameFolder game;
    game.Write(dir);
    std::vector<u32> short_archive_path, char_archive_path;
    Opened short_file_path, wrong_file_type;
    u64 handle = 0;
    const int status = BootWith(f, dir, {"--ms", "100", "--io-latency", "none"}, [&] {
        FsGuest g(f, dir);
        g.Initialize();
        short_archive_path = g.OpenArchive(0x2345678A, kBinary, std::vector<u8>(8, 0));
        char_archive_path = g.OpenArchive(0x2345678A, 3, NcchArchivePath(kUpdateTitleId, 1));
        const std::vector<u32> opened = g.OpenArchive(0x2345678A, kBinary, NcchArchivePath(kUpdateTitleId, 1));
        handle = opened[2] | (static_cast<u64>(opened[3]) << 32);
        short_file_path = g.OpenFile(handle, std::vector<u8>(8, 0));
        wrong_file_type = g.OpenFile(handle, NcchFilePath(0, 7));
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(short_archive_path[1] == kInvalidPath);
    CHECK(short_archive_path[2] == 0);
    CHECK(short_archive_path[3] == 0);
    CHECK(char_archive_path[1] == kInvalidPath);
    CHECK(handle != 0);
    CHECK(short_file_path.result == kInvalidPath);
    CHECK(wrong_file_type.result == kNotFound);
}

TEST_CASE("an archive nobody registered is not found") {
    Fixture f;
    TempDir dir;
    std::vector<u32> opened;
    Opened direct;
    const int status = BootWith(f, dir, {"--ms", "100", "--io-latency", "none"}, [&] {
        FsGuest g(f, dir);
        g.Initialize();
        opened = g.OpenArchive(0x1234ABCD, kEmpty, kEmptyPath);
        direct = g.OpenFileDirectly(0x1234ABCD, kEmpty, kEmptyPath, SelfNcchPath(0));
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(opened[1] == kNotFound);
    CHECK(direct.result == kNotFound);
    CHECK(direct.file == 0);
}

// ---- file sessions --------------------------------------------------------------------------------------------------------

TEST_CASE("a file session: Write, SetSize, Flush, Close, priority and OpenLinkFile") {
    Fixture f;
    TempDir dir;
    GameFolder game;
    game.Write(dir);
    std::vector<u32> write_reply, set_size, flush, close, set_priority, get_priority;
    u64 size_before = 0, size_after_set = 0, link_size = 0;
    Handle link = 0;
    std::vector<u32> link_reply;
    FsGuest::ReadResult via_link;
    const int status = BootWith(f, dir, {"--ms", "100", "--io-latency", "none"}, [&] {
        FsGuest g(f, dir);
        g.Initialize();
        const Opened opened = g.OpenFileDirectly(3, kEmpty, kEmptyPath, SelfNcchPath(0));
        size_before = g.GetSize(opened.file);
        // Write to a RomFS file: Azahar's IVFCFile answers success with 0 bytes written
        g.PutBytes(g.Scratch(0x1000), Pattern(16, 0));
        write_reply = g.Request(opened.file,
                                {ipc::MakeHeader(0x0803, 4, 2), 0, 0, 16, 0, ipc::MappedBufferDesc(16, ipc::R),
                                 g.Scratch(0x1000)},
                                5);
        // SetSize records the size in the session although the file cannot change
        set_size = g.Request(opened.file, {ipc::MakeHeader(0x0805, 2, 0), 77, 0}, 2);
        size_after_set = g.GetSize(opened.file);
        flush = g.Request(opened.file, {ipc::MakeHeader(0x0809, 0, 0)}, 2);
        set_priority = g.Request(opened.file, {ipc::MakeHeader(0x080A, 1, 0), 5}, 2);
        get_priority = g.Request(opened.file, {ipc::MakeHeader(0x080B, 0, 0)}, 3);
        link_reply = g.Request(opened.file, {ipc::MakeHeader(0x080C, 0, 0)}, 4);
        link = link_reply[3];
        if (link != 0) {
            link_size = g.GetSize(link);  // a link starts from the backend's size, not the session's
            via_link = g.Read(link, 0, 0x28);
        }
        close = g.Request(opened.file, {ipc::MakeHeader(0x0808, 0, 0)}, 2);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(write_reply[0] == ipc::MakeHeader(0x0803, 2, 2));
    CHECK(write_reply[1] == kResultSuccess);
    CHECK(write_reply[2] == 0);
    CHECK(set_size == std::vector<u32>{ipc::MakeHeader(0x0805, 1, 0), 0});
    CHECK(size_after_set == 77);
    CHECK(size_before == game.base_romfs.size());
    CHECK(flush == std::vector<u32>{ipc::MakeHeader(0x0809, 1, 0), 0});
    CHECK(set_priority == std::vector<u32>{ipc::MakeHeader(0x080A, 1, 0), 0});
    CHECK(get_priority == std::vector<u32>{ipc::MakeHeader(0x080B, 2, 0), 0, 5});
    CHECK(link_reply[0] == ipc::MakeHeader(0x080C, 1, 2));
    CHECK(link_reply[1] == kResultSuccess);
    CHECK(link != 0);
    CHECK(link_size == game.base_romfs.size());
    CHECK(via_link.bytes == Slice(game.base_romfs, 0, 0x28));
    CHECK(close == std::vector<u32>{ipc::MakeHeader(0x0808, 1, 0), 0});
}

TEST_CASE("IsSdmcDetected says there is an SD card, as Azahar's default") {
    Fixture f;
    TempDir dir;
    std::vector<u32> reply;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        FsGuest g(f, dir);
        reply = g.Request(g.fs, {ipc::MakeHeader(0x0817, 0, 0)}, 3);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(reply == std::vector<u32>{ipc::MakeHeader(0x0817, 2, 0), 0, 1});
}

TEST_CASE("a read that asks for more than the client's buffer holds is cut to the buffer") {
    Fixture f;
    TempDir dir;
    GameFolder game;
    game.Write(dir);
    FsGuest::ReadResult cut;
    const int status = BootWith(f, dir, {"--ms", "100", "--io-latency", "none"}, [&] {
        FsGuest g(f, dir);
        g.Initialize();
        const Opened opened = g.OpenFileDirectly(3, kEmpty, kEmptyPath, SelfNcchPath(0));
        cut = g.Read(opened.file, 0, 0x100, 0x20);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(cut.count == 0x20);
    CHECK(cut.bytes == Slice(game.base_romfs, 0, 0x20));
    CHECK(f.Logged("Read of 0x100 bytes into a buffer of 0x20"));
}

// ---- when replies arrive (D26) -----------------------------------------------------------------------------------------

namespace {

constexpr u64 kOpenDelayNs = 9438006;
constexpr u64 ReadDelayNs(u64 length) {
    return std::max<u64>(length * 94 + 582778, 663124);
}

struct Timing {
    u64 open_ticks = 0, small_read_ticks = 0, big_read_ticks = 0, tiny_read_ticks = 0, link_ticks = 0;
    u64 small_length = 0x40, big_length = 0x1000;
};

Timing MeasureTimes(const std::vector<std::string>& options, int* status_out) {
    Fixture f;
    TempDir dir;
    GameFolder game;
    game.Write(dir);
    Timing t;
    std::vector<std::string> extra = {"--ms", "1000"};
    extra.insert(extra.end(), options.begin(), options.end());
    *status_out = BootWith(f, dir, extra, [&] {
        FsGuest g(f, dir);
        g.Initialize();
        u64 before = g.Now();
        const Opened opened = g.OpenFileDirectly(3, kEmpty, kEmptyPath, SelfNcchPath(0));
        t.open_ticks = g.Now() - before;
        before = g.Now();
        g.Read(opened.file, 0, static_cast<u32>(t.small_length));
        t.small_read_ticks = g.Now() - before;
        before = g.Now();
        g.Read(opened.file, 0, static_cast<u32>(t.big_length));
        t.big_read_ticks = g.Now() - before;
        before = g.Now();
        g.Read(opened.file, 0, 0);
        t.tiny_read_ticks = g.Now() - before;
        ExitProcess(*f.runtime);
    });
    return t;
}

}  // namespace

TEST_CASE("--io-latency azahar: opens and reads are answered after Azahar's delays, on the game clock") {
    int status = 0;
    const Timing t = MeasureTimes({"--io-latency", "azahar"}, &status);
    CHECK(status == 0);
    // the delays are exact ticks of the game clock: NsToTicks of Azahar's nanoseconds
    CHECK(t.open_ticks == NsToTicks(kOpenDelayNs));
    CHECK(t.small_read_ticks == NsToTicks(ReadDelayNs(t.small_length)));  // the minimum, 663124 ns
    CHECK(t.big_read_ticks == NsToTicks(ReadDelayNs(t.big_length)));      // 94 ns a byte + 582778 ns
    CHECK(t.tiny_read_ticks == NsToTicks(663124));
    CHECK(ReadDelayNs(t.small_length) == 663124);
    CHECK(ReadDelayNs(t.big_length) == 0x1000 * 94 + 582778);
}

TEST_CASE("--io-latency azahar: a Write reply costs the IPC reply delay and nothing else") {
    Fixture f;
    TempDir dir;
    GameFolder game;
    game.Write(dir);
    u64 write_ticks = 0;
    std::vector<u32> reply;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "azahar"}, [&] {
        FsGuest g(f, dir);
        g.Initialize();
        const Opened opened = g.OpenFileDirectly(3, kEmpty, kEmptyPath, SelfNcchPath(0));
        g.PutBytes(g.Scratch(0x1000), Pattern(16, 0));
        const u64 before = g.Now();
        reply = g.Request(opened.file,
                          {ipc::MakeHeader(0x0803, 4, 2), 0, 0, 16, 0, ipc::MappedBufferDesc(16, ipc::R),
                           g.Scratch(0x1000)},
                          5);
        write_ticks = g.Now() - before;
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(reply[0] == ipc::MakeHeader(0x0803, 2, 2));
    CHECK(reply[1] == kResultSuccess);
    CHECK(write_ticks == NsToTicks(kIpcReplyDelayNs));
}

TEST_CASE("--io-latency is azahar by default") {
    int status = 0;
    const Timing t = MeasureTimes({}, &status);
    CHECK(status == 0);
    CHECK(t.open_ticks == NsToTicks(kOpenDelayNs));
    CHECK(t.big_read_ticks == NsToTicks(ReadDelayNs(t.big_length)));
}

TEST_CASE("--io-latency none: replies come as soon as the data is read") {
    int status = 0;
    const Timing t = MeasureTimes({"--io-latency", "none"}, &status);
    CHECK(status == 0);
    // an open is answered at once, with the 39 microseconds every immediate reply of an HLE service takes
    CHECK(t.open_ticks == NsToTicks(kIpcReplyDelayNs));
    CHECK(t.small_read_ticks == 0);
    CHECK(t.big_read_ticks == 0);
    CHECK(t.tiny_read_ticks == 0);
}

TEST_CASE("--io-latency takes azahar or none") {
    Fixture f;
    TempDir dir;
    bool ran = false;
    const int status = BootWith(f, dir, {"--ms", "100", "--io-latency", "fast"}, [&] {
        ran = true;
        ExitProcess(*f.runtime);
    });
    CHECK(status == 2);
    CHECK_FALSE(ran);
    CHECK(f.Logged("--io-latency takes azahar or none"));
}

// ---- reads on the I/O thread --------------------------------------------------------------------------------------------

TEST_CASE("a read runs on an I/O thread and its reply is posted back; the buffer is written at the reply") {
    for (const char* latency : {"none", "azahar"}) {
        INFO(latency);
        Fixture f;
        TempDir dir;
        TheGate().Reset(true);
        FsGuest::ReadResult read;
        std::thread::id guest_thread, io_thread;
        bool io_entered_before_release = false;
        u64 ticks = 0;
        u64 released_at = 0;
        const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", latency}, [&] {
            FsGuest g(f, dir);
            g.Initialize();
            guest_thread = std::this_thread::get_id();
            const Opened opened = g.OpenFileDirectly(kTestArchiveId, kEmpty, kEmptyPath, {1, 2, 3});
            REQUIRE(opened.result == kResultSuccess);
            // 1 ms of game time later the test lets the reader go (a timer on the scheduler thread). Until
            // then the reader waits for the game clock, so the clock must not wait for it.
            f.time->wait_for_io = false;
            Kernel& kernel = f.runtime->GetKernel();
            kernel.ScheduleAt(kernel.NowTicks() + NsToTicks(1000000), [&] {
                {
                    // the host may not have started the read yet: give it time (this is the test's
                    // timer, on the scheduler thread, and the game clock is a manual one)
                    std::unique_lock lock(TheGate().mutex);
                    TheGate().cv.wait_for(lock, std::chrono::seconds(10), [] { return TheGate().entered; });
                    io_entered_before_release = TheGate().entered;
                }
                released_at = f.runtime->Clock().Now();
                f.time->wait_for_io = true;
                TheGate().Release();
            });
            const u64 before = g.Now();
            read = g.Read(opened.file, 4, 8);
            ticks = g.Now() - before;
            ExitProcess(*f.runtime);
        });
        CHECK(status == 0);
        CHECK(read.result == kResultSuccess);
        CHECK(read.count == 8);
        CHECK(read.bytes == std::vector<u8>{0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB});
        {
            std::lock_guard lock(TheGate().mutex);
            CHECK(TheGate().reader_set);
            io_thread = TheGate().reader;
        }
        CHECK(io_thread != guest_thread);  // the backend ran on an I/O thread
        CHECK(io_entered_before_release);   // and was waiting there while the game clock ran on
        // the reply is not earlier than the release (the read could not finish before) nor, with
        // azahar's latency, than the modelled delay; the clock did not run while the host waited
        CHECK(ticks >= NsToTicks(1000000));
        if (std::string(latency) == "azahar") {
            CHECK(ticks >= NsToTicks(ReadDelayNs(8)));
        }
        CHECK(released_at != 0);
        TheGate().Release();
    }
}

TEST_CASE("with azahar's latency the reply waits for the delay even when the data is read at once") {
    Fixture f;
    TempDir dir;
    TheGate().Reset(false);
    u64 ticks = 0;
    FsGuest::ReadResult read;
    const int status = BootWith(f, dir, {"--ms", "1000", "--io-latency", "azahar"}, [&] {
        FsGuest g(f, dir);
        g.Initialize();
        const Opened opened = g.OpenFileDirectly(kTestArchiveId, kEmpty, kEmptyPath, {1});
        REQUIRE(opened.result == kResultSuccess);
        const u64 before = g.Now();
        read = g.Read(opened.file, 0, 0x20);
        ticks = g.Now() - before;
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(read.count == 0x20);
    CHECK(ticks == NsToTicks(ReadDelayNs(0x20)));
}

TEST_CASE("positioned reads: several threads read one file at the same time") {
    TempDir dir;
    const std::vector<u8> bytes = Pattern(3 * 1024 * 1024 + 123, 17);
    const std::filesystem::path path = dir.Path() / "big.dat";
    WriteFile(path, bytes);
    std::string error;
    const std::shared_ptr<fs::PositionedFile> file = fs::PositionedFile::Open(path.string(), &error);
    REQUIRE_MESSAGE(file != nullptr, error);
    CHECK(file->Size() == bytes.size());

    std::atomic<int> mismatches{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            std::vector<u8> chunk;
            u32 state = 12345u + static_cast<u32>(t);
            for (int i = 0; i < 400; ++i) {
                state = state * 1664525u + 1013904223u;
                const u64 offset = state % bytes.size();
                const std::size_t length = 1 + (state >> 8) % 70000;
                chunk.assign(length, 0);
                bool failed = false;
                const std::size_t got = file->ReadAt(offset, chunk.data(), length, &failed);
                const std::size_t expected = static_cast<std::size_t>(std::min<u64>(length, bytes.size() - offset));
                if (failed || got != expected ||
                    std::memcmp(chunk.data(), bytes.data() + offset, expected) != 0) {
                    ++mismatches;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK(mismatches == 0);
    // at and past the end
    u8 one = 0;
    CHECK(file->ReadAt(bytes.size() - 1, &one, 1) == 1);
    CHECK(one == bytes.back());
    CHECK(file->ReadAt(bytes.size(), &one, 1) == 0);
    CHECK(file->ReadAt(bytes.size() + 100, &one, 1) == 0);
    // a file that is not there
    CHECK(fs::PositionedFile::Open((dir.Path() / "absent").string(), &error) == nullptr);
    CHECK_FALSE(error.empty());
}

// ---- the replay tool on a recorded session ---------------------------------------------------------------------------

TEST_CASE("mh4n_ipc_replay finds no difference in a recorded fs:USER session (RomFS and system archive reads)") {
    TempDir dir;
    GameFolder game;
    game.Write(dir);
    const std::string trace = (dir.Path() / "ipc.txt").string();
    {
        Fixture f;
        const int status = BootFolder(f, dir, {"--ms", "100", "--io-latency", "none", "--trace-ipc", trace, "--trace-ipc-full"}, [&] {
            FsGuest g(f, dir, false);
            g.Initialize();
            const Opened romfs = g.OpenFileDirectly(3, kEmpty, kEmptyPath, SelfNcchPath(0));
            g.GetSize(romfs.file);
            g.Read(romfs.file, 0, 0x28);
            g.Read(romfs.file, 0x100, 0x200);
            const Opened update = g.OpenFileDirectly(3, kEmpty, kEmptyPath, SelfNcchPath(5));
            g.Read(update.file, 0x20, 0x40);
            const Opened words = g.OpenFileDirectly(0x2345678A, kBinary, NcchArchivePath(0x000400DB00010302, 0), NcchFilePath(0, 0));
            g.GetSize(words.file);
            g.Read(words.file, 0, 0x400);
            const std::vector<u32> archive = g.OpenArchive(3, kEmpty, kEmptyPath);
            g.OpenFile(archive[2] | (static_cast<u64>(archive[3]) << 32), SelfNcchPath(0));
            g.CloseArchive(archive[2]);
            g.OpenFileDirectly(3, kEmpty, kEmptyPath, SelfNcchPath(7));  // an error
            ExitProcess(*f.runtime);
        });
        REQUIRE(status == 0);
    }
    std::ostringstream out, err;
    const int status = ipc_replay::RunIpcReplay({trace, "--service", "fs:USER", "--game", dir.Game(), "--io-latency", "none"}, out, err);
    INFO(out.str());
    INFO(err.str());
    CHECK(status == 0);
    CHECK(out.str().find("14 matched, 0 differ, 0 skipped") != std::string::npos);

    // The replay is not vacuous: with one byte of the RomFS changed, the output CRC of the first read
    // differs.
    {
        std::vector<u8> section = MakeSection(game.base_romfs);
        section[0x1000 + 0x10] ^= 0x5A;
        WriteFile(dir.Path() / "game" / "romfs.bin", section);
    }
    std::ostringstream out2, err2;
    const int changed = ipc_replay::RunIpcReplay(
        {trace, "--service", "fs:USER", "--game", dir.Game(), "--io-latency", "none"}, out2, err2);
    INFO(out2.str());
    CHECK(changed == 1);
    CHECK(out2.str().find("differ") != std::string::npos);
    CHECK(out2.str().find("0 differ") == std::string::npos);
}

TEST_CASE("the delays are Azahar's numbers") {
    CHECK(fs::RomFsReadDelayNs(0) == 663124);
    CHECK(fs::RomFsReadDelayNs(0x40) == 663124);
    CHECK(fs::RomFsReadDelayNs(852) == 663124);   // 852 * 94 + 582778 = 662866: still the minimum
    CHECK(fs::RomFsReadDelayNs(856) == 856 * 94 + 582778);
    CHECK(fs::RomFsReadDelayNs(0x100000) == 0x100000ull * 94 + 582778);
    CHECK(fs::kRomFsOpenDelayNs == 9438006);
}

#endif  // recomp.h
