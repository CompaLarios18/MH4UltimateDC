// Derived from Azahar src/core/hle/service/am/am.cpp, src/core/hle/service/am/am_app.cpp,
// src/core/hle/service/nim/nim_aoc.cpp, src/core/hle/service/act/act.cpp,
// src/core/hle/service/act/act_u.cpp, src/core/hle/service/act/act_errors.cpp,
// src/core/hle/service/act/act_errors.h, src/core/hle/service/frd/frd.cpp,
// src/core/hle/service/frd/frd_u.cpp, src/core/hle/service/cecd/cecd.cpp,
// src/core/hle/service/cecd/cecd.h, src/core/hle/service/boss/boss.cpp,
// src/core/hle/service/boss/online_service.cpp, src/core/hle/service/boss/online_service.h,
// src/core/hle/service/soc/soc_u.cpp, src/core/hle/service/http/http_c.cpp,
// src/core/hle/service/cam/cam.cpp, src/core/hle/service/cam/y2r_u.cpp,
// src/core/file_sys/savedata_archive.cpp and src/core/file_sys/errors.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: am.cpp says "GPLv3 or any later version";
// the others say "GPLv2 or any later version"): the expected replies, result codes and structure
// layouts of the services the tests send requests to.
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (am.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (soc_u.cpp, http_c.cpp)
// Copyright 2014-2025 Citra Emulator Project / Azahar Emulator Project (the others)
//
// The services of N1-19 (am:app, nim:aoc, act:u, frd:u, cecd:u, boss, soc:U, http:C, cam:u, y2r:u),
// through the SVC layer: the guest connects to srv: and sends requests as a program would. The programs
// are ELF files written by the test whose main thread runs a C++ function (Runtime::SetMainThreadEntry);
// time is a ManualTimeSource.
#include <doctest/doctest.h>

#if !__has_include("recomp.h")
TEST_CASE("other_services_test needs recomp.h (the repository around native/)" * doctest::skip()) {}
#else

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "mh4n/clock.h"
#include "mh4n/ipc.h"
#include "mh4n/log.h"
#include "mh4n/memory.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"
#include "mh4n/service.h"
#include "mh4n/threads.h"
#include "../src/services/cecd/sha256.h"
#include "../src/services/y2r/y2r_convert.h"
#include "../src/cpu/recomp_c.h"  // last: it defines macros

using namespace mh4n;

namespace {

constexpr u32 kText = 0x00100000;

// ---- harness: an ELF whose main thread runs a C++ body -------------------------------------------

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

class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("mh4n_ipc_test_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                 std::to_string(counter++));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    std::string File(const std::string& name) const { return (path_ / name).string(); }
    std::string WriteElf() const {
        const std::string path = File("program.elf");
        const std::vector<u8> bytes = MakeElf();
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        return path;
    }

private:
    std::filesystem::path path_;
};

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
    int LoggedCount(const std::string& needle) const {
        int n = 0;
        for (const std::string& line : logged) {
            n += line.find(needle) != std::string::npos ? 1 : 0;
        }
        return n;
    }
    bool Logged(const std::string& needle) const { return LoggedCount(needle) > 0; }
};

Options Parse(const std::vector<std::string>& args) {
    const Options options = ParseOptions("runner", args);
    REQUIRE_MESSAGE(options.ok(), options.error());
    return options;
}

// Boots the ELF whose main thread runs `body`, which must end the process.
int BootWith(Fixture& f, const TempDir& dir, std::vector<std::string> extra,
             std::function<void()> body) {
    std::vector<std::string> args = {dir.WriteElf()};
    args.insert(args.end(), extra.begin(), extra.end());
    f.runtime->SetMainThreadEntry([body = std::move(body)](GuestThread&) { body(); });
    return f.runtime->Boot(Parse(args));
}

// ---- what the main thread does ----------------------------------------------------------------------

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

// The guest side of one run: the main thread's command buffer and a scratch area on its stack.
struct Guest {
    Runtime& rt;
    explicit Guest(Runtime& runtime) : rt(runtime) {}

    u32 Cmd() const { return CommandBufferAddress(*rt.MainThread()); }
    // Scratch memory below the stack top (the stack is 0x4000 bytes).
    u32 Scratch(u32 offset = 0) const { return rt.MainThread()->stack_top - 0x3000 + offset; }

    void PutWords(u32 address, const std::vector<u32>& words) {
        REQUIRE(rt.Memory().WriteBlock(address, words.data(), static_cast<u32>(words.size() * 4)));
    }
    void Put(const std::vector<u32>& words, u32 first_word = 0) {
        PutWords(Cmd() + first_word * 4, words);
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

    // svcConnectToPort: the result, and the handle in *handle.
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
    Result Wait(Handle handle, s64 ns) {
        return CallSvc(rt, 0x24,
                       {handle, static_cast<u32>(ns), static_cast<u32>(static_cast<u64>(ns) >> 32)})
            .r[0];
    }
    // Sends `words` (as the command) through `session` and returns the reply's first `reply_words`.
    std::vector<u32> Request(Handle session, const std::vector<u32>& words, std::size_t reply_words,
                             Result* result = nullptr) {
        Put(words);
        const Result r = Send(session);
        if (result != nullptr) {
            *result = r;
        }
        return Get(reply_words);
    }
    // svcCreateEvent (a OneShot one): the handle.
    Handle NewEvent() { return CallSvc(rt, 0x17, {0}).r[1]; }
};

// Packs a service name into the two words of GetServiceHandle (8 bytes, zero padded).
std::array<u32, 2> NameWords(const std::string& name) {
    std::array<u32, 2> words{};
    std::memcpy(words.data(), name.data(), std::min<std::size_t>(name.size(), 8));
    return words;
}

// srv: GetServiceHandle: the reply's words, and the handle (0 if none).
struct Got {
    std::vector<u32> reply;
    Handle handle = 0;
};
Got GetService(Guest& g, Handle srv, const std::string& name, u32 length, u32 flags) {
    const auto words = NameWords(name);
    Got got;
    got.reply =
        g.Request(srv, {ipc::MakeHeader(0x0005, 4, 0), words[0], words[1], length, flags}, 4);
    got.handle = got.reply[0] == ipc::MakeHeader(0x0005, 1, 2) ? got.reply[3] : 0;
    return got;
}

Handle ConnectSrv(Guest& g) {
    Handle h = 0;
    REQUIRE(g.ConnectToPort("srv:", &h) == kResultSuccess);
    return h;
}


// ---- helpers for these services --------------------------------------------------------------------

Handle OpenService(Guest& g, Handle srv, const std::string& name) {
    const Got got = GetService(g, srv, name, static_cast<u32>(name.size()), 0);
    REQUIRE_MESSAGE(got.handle != 0, name);
    return got.handle;
}

// The two words of a mapped buffer descriptor and its address.
std::vector<u32> Mapped(u32 size, ipc::MappedBufferPermissions perms, u32 address) {
    return {ipc::MappedBufferDesc(size, perms), address};
}

std::vector<u32> Cat(std::initializer_list<std::vector<u32>> parts) {
    std::vector<u32> out;
    for (const auto& part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

// A static receive buffer description in the command area: id, size, address.
void SetReceive(Guest& g, u32 id, u32 size, u32 address) {
    g.Put({ipc::StaticBufferDesc(size, static_cast<u8>(id)), address},
          static_cast<u32>(ipc::StaticBufferDescriptorWord(id)));
}

}  // namespace

// ---- am:app -----------------------------------------------------------------------------------------

TEST_CASE("am:app: the DLC and patch queries find nothing, with Azahar's id checks and replies") {
    Fixture f;
    TempDir dir;
    std::vector<u32> count_dlc, count_other, titles_dlc, titles_other, titles_empty, patch_dlc,
        patch_update, list_dlc, list_other, find_dlc, del, tickets, unknown;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle srv = ConnectSrv(g);
        const Handle am = OpenService(g, srv, "am:app");
        const u32 ids = g.Scratch(0), infos = g.Scratch(0x100), out = g.Scratch(0x200);
        g.PutWords(ids, {0x00000102, 0x0004008C, 0x00000103, 0x0004000E});

        count_dlc = g.Request(am, {ipc::MakeHeader(0x1001, 3, 0), 1, 0x00000102, 0x0004008C}, 3);
        count_other = g.Request(am, {ipc::MakeHeader(0x1001, 3, 0), 1, 0x00000102, 0x00040000}, 3);

        // GetDLCTitleInfos(media, count, ids (read), infos (write)): the first id is a DLC id and no
        // title is installed
        titles_dlc = g.Request(am,
                               Cat({{ipc::MakeHeader(0x1005, 2, 4), 1, 1},
                                    Mapped(8, ipc::R, ids),
                                    Mapped(0x18, ipc::W, infos)}),
                               6);
        // the second id is an update id: not a DLC id
        titles_other = g.Request(am,
                                 Cat({{ipc::MakeHeader(0x1005, 2, 4), 1, 2},
                                      Mapped(16, ipc::R, ids),
                                      Mapped(0x30, ipc::W, infos)}),
                                 6);
        titles_empty = g.Request(am,
                                 Cat({{ipc::MakeHeader(0x1005, 2, 4), 1, 0},
                                      Mapped(8, ipc::R, ids),
                                      Mapped(0x18, ipc::W, infos)}),
                                 6);
        // GetPatchTitleInfos wants update ids
        patch_dlc = g.Request(am,
                              Cat({{ipc::MakeHeader(0x100D, 2, 4), 1, 1},
                                   Mapped(8, ipc::R, ids),
                                   Mapped(0x18, ipc::W, infos)}),
                              6);
        g.PutWords(ids, {0x00000103, 0x0004000E});
        patch_update = g.Request(am,
                                 Cat({{ipc::MakeHeader(0x100D, 2, 4), 1, 1},
                                      Mapped(8, ipc::R, ids),
                                      Mapped(0x18, ipc::W, infos)}),
                                 6);

        // ListDLCContentInfos(count, media, title id, start, out)
        list_dlc = g.Request(am,
                             Cat({{ipc::MakeHeader(0x1003, 5, 2), 4, 1, 0x00000102, 0x0004008C, 0},
                                  Mapped(0x40, ipc::W, out)}),
                             3);
        list_other = g.Request(am,
                               Cat({{ipc::MakeHeader(0x1003, 5, 2), 4, 1, 0x00000102, 0x00040000, 0},
                                    Mapped(0x40, ipc::W, out)}),
                               3);
        // FindDLCContentInfos(media, title id, count, indices (read), infos (write))
        find_dlc = g.Request(am,
                             Cat({{ipc::MakeHeader(0x1002, 4, 4), 1, 0x00000102, 0x0004008C, 1},
                                  Mapped(2, ipc::R, ids),
                                  Mapped(0x10, ipc::W, out)}),
                             3);
        // DeleteContents is stubbed: success, the buffer given back
        del = g.Request(am,
                        Cat({{ipc::MakeHeader(0x1004, 4, 2), 1, 0x00000102, 0x0004008C, 1},
                             Mapped(2, ipc::R, ids)}),
                        4);
        tickets = g.Request(am,
                            Cat({{ipc::MakeHeader(0x1007, 4, 2), 5, 0x00000102, 0x0004008C, 0},
                                 Mapped(0x40, ipc::W, out)}),
                            3);
        unknown = g.Request(am, {ipc::MakeHeader(0x100A, 0, 0)}, 2);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    // GetDLCContentInfoCount: "number of content infos plus one" for a missing TMD
    CHECK(count_dlc[0] == 0x10010080);
    CHECK(count_dlc[1] == kResultSuccess);
    CHECK(count_dlc[2] == 1);
    CHECK(count_other[1] == MakeResult(31, 32, 7, 28));
    CHECK(count_other[2] == 0);
    // GetDLCTitleInfos: reply (1 normal, 4 translate words)
    CHECK(titles_dlc[0] == ipc::MakeHeader(0x1005, 1, 4));
    CHECK(titles_dlc[1] == MakeResult(1018, 32, 5, 27));
    CHECK(titles_dlc[2] == ipc::MappedBufferDesc(8, ipc::R));
    CHECK(titles_dlc[4] == ipc::MappedBufferDesc(0x18, ipc::W));
    CHECK(titles_other[1] == MakeResult(60, 32, 7, 28));
    CHECK(titles_empty[1] == kResultSuccess);
    CHECK(patch_dlc[1] == MakeResult(60, 32, 7, 28));
    CHECK(patch_update[1] == MakeResult(1018, 32, 5, 27));
    CHECK(list_dlc[0] == ipc::MakeHeader(0x1003, 2, 0));
    CHECK(list_dlc[1] == kResultSuccess);
    CHECK(list_dlc[2] == 0);
    CHECK(list_other[1] == MakeResult(60, 32, 7, 28));
    CHECK(find_dlc[0] == ipc::MakeHeader(0x1002, 2, 0));
    CHECK(find_dlc[1] == kResultSuccess);
    CHECK(del[0] == ipc::MakeHeader(0x1004, 1, 2));
    CHECK(del[1] == kResultSuccess);
    CHECK(del[2] == ipc::MappedBufferDesc(2, ipc::R));
    CHECK(tickets[0] == ipc::MakeHeader(0x1007, 2, 0));
    CHECK(tickets[1] == kResultSuccess);
    CHECK(tickets[2] == 0);
    // IsExternalTitleDatabaseInitialized is a nullptr entry in Azahar: the unknown-command answer
    CHECK(unknown[0] == ipc::MakeHeader(0x100A, 1, 0));
    CHECK(unknown[1] == kResultSuccess);
    CHECK(f.Logged("unimplemented am:app"));
}

// ---- nim:aoc -------------------------------------------------------------------------------------------

TEST_CASE("nim:aoc: two sessions at most, and every command is answered as an unknown one") {
    Fixture f;
    TempDir dir;
    std::vector<u32> init, set_tin, other;
    Got third;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle srv = ConnectSrv(g);
        const Handle first = OpenService(g, srv, "nim:aoc");
        const Handle second = OpenService(g, srv, "nim:aoc");
        init = g.Request(first, {ipc::MakeHeader(0x0021, 0, 0)}, 2);
        set_tin = g.Request(second, {ipc::MakeHeader(0x0004, 1, 2), 5, ipc::CallingPidDesc(), 0}, 2);
        other = g.Request(first, {ipc::MakeHeader(0x0026, 0, 0)}, 2);  // not in Azahar's table at all
        third = GetService(g, srv, "nim:aoc", 7, 0);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(init[0] == ipc::MakeHeader(0x0021, 1, 0));
    CHECK(init[1] == kResultSuccess);
    CHECK(set_tin[0] == ipc::MakeHeader(0x0004, 1, 0));
    CHECK(other[0] == ipc::MakeHeader(0x0026, 1, 0));
    CHECK(third.reply[1] == 0xD0401834);  // the third session is refused
    CHECK(f.LoggedCount("unimplemented nim:aoc") == 3);  // 0x00210000, 0x00040142, 0x00260000
}

// ---- act:u -------------------------------------------------------------------------------------------------

TEST_CASE("act:u: Initialize and GetAccountInfo succeed, GetErrorCode maps ACT results") {
    Fixture f;
    TempDir dir;
    std::vector<u32> init, ok_code, lib_code, other_module, unknown_desc, info, banned;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle srv = ConnectSrv(g);
        const Handle act = OpenService(g, srv, "act:u");
        init = g.Request(
            act, {ipc::MakeHeader(0x0001, 2, 4), 0x00040000, 0x1000, ipc::CallingPidDesc(), 0,
                  ipc::CopyHandleDesc(1), 0},
            2);
        // description 0 is MySuccess, 101 NotInitialized (022-0501), 104 is "022-5104"
        ok_code = g.Request(act, {ipc::MakeHeader(0x0002, 1, 0), MakeResult(0, 83, 0, 0)}, 3);
        lib_code = g.Request(act, {ipc::MakeHeader(0x0002, 1, 0), MakeResult(101, 83, 5, 25)}, 3);
        banned = g.Request(act, {ipc::MakeHeader(0x0002, 1, 0), MakeResult(999, 83, 5, 25)}, 3);
        other_module = g.Request(act, {ipc::MakeHeader(0x0002, 1, 0), MakeResult(101, 82, 5, 25)}, 3);
        unknown_desc = g.Request(act, {ipc::MakeHeader(0x0002, 1, 0), MakeResult(5, 83, 5, 25)}, 3);
        info = g.Request(act,
                         Cat({{ipc::MakeHeader(0x0006, 3, 2), 1, 0x10, 0x20},
                              Mapped(0x10, ipc::W, g.Scratch(0))}),
                         2);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(init[0] == ipc::MakeHeader(0x0001, 1, 0));
    CHECK(init[1] == kResultSuccess);
    CHECK(ok_code[0] == ipc::MakeHeader(0x0002, 2, 0));
    CHECK(ok_code[2] == 220000);
    CHECK(lib_code[2] == 220501);
    CHECK(banned[2] == 222999);  // AuthenticationFailureUnknown
    CHECK(other_module[2] == 229999);
    CHECK(unknown_desc[2] == 229999);
    CHECK(info[0] == ipc::MakeHeader(0x0006, 1, 0));
    CHECK(info[1] == kResultSuccess);
}

// ---- frd:u ---------------------------------------------------------------------------------------------------

TEST_CASE("frd:u: nobody is a friend, and Login completes 500 ms later by the game clock") {
    Fixture f;
    TempDir dir;
    std::vector<u32> before, login, key, pref, profile, game, comment, online_before, online_after,
        after, unknown, sdk, last;
    Result early_wait = 0, late_wait = 0;
    std::vector<u8> presence, key_list, flags, codes;
    std::vector<u32> key_list_reply, presence_reply, flags_reply, codes_reply;
    const int status = BootWith(f, dir, {"--ms", "2000"}, [&] {
        Guest g(*f.runtime);
        const Handle srv = ConnectSrv(g);
        const Handle frd = OpenService(g, srv, "frd:u");
        before = g.Request(frd, {ipc::MakeHeader(0x0001, 0, 0)}, 3);
        online_before = g.Request(frd, {ipc::MakeHeader(0x0002, 0, 0)}, 3);
        const Handle event = g.NewEvent();
        login = g.Request(frd, {ipc::MakeHeader(0x0003, 0, 2), ipc::CopyHandleDesc(1), event}, 2);
        f.runtime->Scheduler().Sleep(499000000);
        early_wait = g.Wait(event, 0);
        online_after = g.Request(frd, {ipc::MakeHeader(0x0001, 0, 0)}, 3);
        f.runtime->Scheduler().Sleep(2000000);
        late_wait = g.Wait(event, 0);
        after = g.Request(frd, {ipc::MakeHeader(0x0001, 0, 0)}, 3);
        key = g.Request(frd, {ipc::MakeHeader(0x0005, 0, 0)}, 6);
        pref = g.Request(frd, {ipc::MakeHeader(0x0006, 0, 0)}, 5);
        profile = g.Request(frd, {ipc::MakeHeader(0x0007, 0, 0)}, 4);
        game = g.Request(frd, {ipc::MakeHeader(0x000C, 0, 0)}, 6);
        comment = g.Request(frd, {ipc::MakeHeader(0x000F, 0, 0)}, 11);

        // static receive buffers: pre-filled with 0xFF so the zeros are seen being written
        const u32 out = g.Scratch(0x400);
        g.PutBytes(out, std::vector<u8>(0x400, 0xFF));
        SetReceive(g, 0, 0x12C, out);
        presence_reply = g.Request(frd, {ipc::MakeHeader(0x0008, 0, 0)}, 4);
        presence = g.GetBytes(out, 0x130);
        g.PutBytes(out, std::vector<u8>(0x400, 0xFF));
        SetReceive(g, 0, 0x30, out);
        key_list_reply = g.Request(frd, {ipc::MakeHeader(0x0011, 2, 0), 0, 3}, 5);
        key_list = g.GetBytes(out, 0x34);
        // GetFriendAttributeFlags(count, keys as a static buffer)
        g.PutBytes(out, std::vector<u8>(0x400, 0xFF));
        SetReceive(g, 0, 2, out);
        const u32 keys = g.Scratch(0x100);
        g.PutBytes(keys, std::vector<u8>(32, 0));
        flags_reply = g.Request(
            frd, {ipc::MakeHeader(0x0017, 1, 2), 2, ipc::StaticBufferDesc(32, 0), keys}, 4);
        flags = g.GetBytes(out, 4);
        g.PutBytes(out, std::vector<u8>(0x400, 0xFF));
        SetReceive(g, 0, 16, out);
        const u32 scrambled = g.Scratch(0x100);
        g.PutBytes(scrambled, std::vector<u8>(24, 0x55));
        codes_reply = g.Request(
            frd, {ipc::MakeHeader(0x001C, 1, 2), 2, ipc::StaticBufferDesc(24, 0), scrambled}, 4);
        codes = g.GetBytes(out, 20);
        sdk = g.Request(frd, {ipc::MakeHeader(0x0032, 1, 2), 0x70000000, ipc::CallingPidDesc(), 0}, 2);
        last = g.Request(frd, {ipc::MakeHeader(0x0023, 0, 0)}, 2);
        unknown = g.Request(frd, {ipc::MakeHeader(0x0004, 0, 0)}, 2);  // Logout: nullptr in Azahar
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(before[0] == ipc::MakeHeader(0x0001, 2, 0));
    CHECK(before[1] == kResultSuccess);
    CHECK(before[2] == 0);
    CHECK(online_before[2] == 0);
    CHECK(login[0] == ipc::MakeHeader(0x0003, 1, 0));
    CHECK(login[0] == 0x00030040);
    CHECK(early_wait == kTimeout);  // 499 ms: not yet
    CHECK(online_after[2] == 0);
    CHECK(late_wait == kResultSuccess);  // 501 ms: the event is signalled
    CHECK(after[2] == 1);
    CHECK(key[0] == ipc::MakeHeader(0x0005, 5, 0));
    CHECK(key == std::vector<u32>{ipc::MakeHeader(0x0005, 5, 0), 0, 0, 0, 0, 0});
    CHECK(pref == std::vector<u32>{ipc::MakeHeader(0x0006, 4, 0), 0, 1, 1, 0});
    CHECK(profile == std::vector<u32>{ipc::MakeHeader(0x0007, 3, 0), 0, 0x01010101, 0x00000001});
    CHECK(game == std::vector<u32>{ipc::MakeHeader(0x000C, 5, 0), 0, 0x00030700, 0x0004000E, 1, 0});
    REQUIRE(comment.size() == 11);
    CHECK(comment[2] == ('H' | ('e' << 16)));
    CHECK(comment[3] == ('y' | ('!' << 16)));
    CHECK(comment[10] == 0);
    CHECK(presence_reply[0] == ipc::MakeHeader(0x0008, 1, 2));
    CHECK(presence_reply[2] == ipc::StaticBufferDesc(0x12C, 0));
    CHECK(std::all_of(presence.begin(), presence.begin() + 0x12C, [](u8 b) { return b == 0; }));
    CHECK(presence[0x12C] == 0xFF);  // nothing past the structure
    CHECK(key_list_reply[0] == ipc::MakeHeader(0x0011, 2, 2));
    CHECK(key_list_reply[2] == 0);  // 0 friends
    CHECK(std::all_of(key_list.begin(), key_list.begin() + 0x30, [](u8 b) { return b == 0; }));
    CHECK(key_list[0x30] == 0xFF);
    CHECK(flags_reply[0] == ipc::MakeHeader(0x0017, 1, 2));
    CHECK(flags_reply[1] == kResultSuccess);
    CHECK(flags[0] == 0);
    CHECK(flags[1] == 0);
    CHECK(flags[2] == 0xFF);
    CHECK(codes_reply[1] == kResultSuccess);
    CHECK(std::all_of(codes.begin(), codes.begin() + 16, [](u8 b) { return b == 0; }));
    CHECK(codes[16] == 0xFF);
    CHECK(sdk == std::vector<u32>{ipc::MakeHeader(0x0032, 1, 0), 0});
    CHECK(last == std::vector<u32>{ipc::MakeHeader(0x0023, 1, 0), 0});
    CHECK(unknown[0] == ipc::MakeHeader(0x0004, 1, 0));
    CHECK(f.Logged("unimplemented frd:u"));
}

// ---- cecd:u ---------------------------------------------------------------------------------------------------

TEST_CASE("cecd:u: the archive starts as Azahar creates it, files and boxes follow its rules") {
    Fixture f;
    TempDir dir;
    constexpr u32 kProgram = 0x00020000;
    constexpr Result kNoData = MakeResult(1007, 66, 4, 25);
    constexpr Result kNotAuthorized = MakeResult(1002, 66, 4, 25);
    std::vector<u32> root, list, read_list, make_dir_no, make_dir_yes, make_in, make_out, write_info,
        read_info, write_list, read_list2, dir_read, state, start, event_reply, unknown,
        write_msg, read_msg, read_msg_after, hmac_write, hmac_read, del_msg, del_dir, open_after, shared;
    std::vector<u8> list_bytes, info_bytes, list2_bytes, msg_bytes, hmac_bytes, eula_version, eula,
        parent, empty_dest;
    std::vector<u32> eula_reply;
    Result event_wait = 0;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle srv = ConnectSrv(g);
        const Handle cecd = OpenService(g, srv, "cecd:u");
        const u32 a = g.Scratch(0), b = g.Scratch(0x400), id = g.Scratch(0x800), key = g.Scratch(0x900);
        const auto open = [&](Handle h, u32 program, u32 type, u32 mode) {
            return g.Request(h,
                             {ipc::MakeHeader(0x0001, 3, 2), program, type, mode,
                              ipc::CallingPidDesc(), 0},
                             3);
        };
        // /CEC holds MBoxList____ only (eventlog.dat is in the archive's root)
        root = open(cecd, 0, 10, 0);
        // MBoxList____: 0x18C bytes starting 'hh', version 1
        list = open(cecd, 0, 1, 0x2 | 0x4 | 0x8);
        g.PutBytes(a, std::vector<u8>(0x200, 0xEE));
        read_list = g.Request(cecd, Cat({{ipc::MakeHeader(0x0002, 1, 2), 0x18C}, Mapped(0x18C, ipc::W, a)}), 5);
        list_bytes = g.GetBytes(a, 0x190);

        make_dir_no = open(cecd, kProgram, 11, 0);
        make_dir_yes = open(cecd, kProgram, 11, 0x8);
        make_in = open(cecd, kProgram, 12, 0x8);
        make_out = open(cecd, kProgram, 13, 0x8);

        // OpenAndWrite(size, program, type, mode, PID, data): MBoxInfo with the check flag
        g.PutBytes(a, std::vector<u8>(0x60, 0));
        write_info = g.Request(cecd,
                               Cat({{ipc::MakeHeader(0x0011, 4, 4), 0x60, kProgram, 2, 0x4 | 0x8 | 0x10,
                                     ipc::CallingPidDesc(), 0},
                                    Mapped(0x60, ipc::R, a)}),
                               4);
        g.PutBytes(b, std::vector<u8>(0x60, 0xEE));
        read_info = g.Request(cecd,
                              Cat({{ipc::MakeHeader(0x0012, 4, 4), 0x60, kProgram, 2, 0x2,
                                    ipc::CallingPidDesc(), 0},
                                   Mapped(0x60, ipc::W, b)}),
                              5);
        info_bytes = g.GetBytes(b, 0x60);

        // MBoxList____ rewritten through the check: the title is added once
        g.PutBytes(a, std::vector<u8>(0x18C, 0));
        write_list = g.Request(cecd,
                               Cat({{ipc::MakeHeader(0x0011, 4, 4), 0x18C, kProgram, 1, 0x4 | 0x10,
                                     ipc::CallingPidDesc(), 0},
                                    Mapped(0x18C, ipc::R, a)}),
                               4);
        g.PutBytes(a, std::vector<u8>(0x18C, 0));
        write_list = g.Request(cecd,
                               Cat({{ipc::MakeHeader(0x0011, 4, 4), 0x18C, kProgram, 1, 0x4 | 0x10,
                                     ipc::CallingPidDesc(), 0},
                                    Mapped(0x18C, ipc::R, a)}),
                               4);
        g.PutBytes(b, std::vector<u8>(0x18C, 0xEE));
        read_list2 = g.Request(cecd,
                               Cat({{ipc::MakeHeader(0x0012, 4, 4), 0x18C, 0, 1, 0x2,
                                     ipc::CallingPidDesc(), 0},
                                    Mapped(0x18C, ipc::W, b)}),
                               5);
        list2_bytes = g.GetBytes(b, 0x18C);

        // a directory cannot be read through a session that opened it
        const Handle second = OpenService(g, srv, "cecd:u");
        open(second, 0, 10, 0);
        shared = open(second, kProgram, 11, 0);  // the box the first session created
        dir_read = g.Request(second, Cat({{ipc::MakeHeader(0x0002, 1, 2), 8}, Mapped(8, ipc::W, a)}), 5);

        // messages: header 0x70, body 4, then 0x20 bytes of HMAC
        std::vector<u8> message(0x94, 0);
        message[8] = 0x70;  // header_size
        message[12] = 4;    // body_size
        message[0x70] = 0xDE, message[0x71] = 0xAD, message[0x72] = 0xBE, message[0x73] = 0xEF;
        g.PutBytes(a, message);
        g.PutBytes(id, {1, 2, 3, 4, 5, 6, 7, 8});
        // WriteMessage(program, is_outbox, id size, size; data, id)
        write_msg = g.Request(cecd,
                              Cat({{ipc::MakeHeader(0x0006, 4, 4), kProgram, 0, 8, 0x94},
                                   Mapped(0x94, ipc::R, a), Mapped(8, ipc::R, id)}),
                              6);
        g.PutBytes(b, std::vector<u8>(0x94, 0xEE));
        read_msg = g.Request(cecd,
                             Cat({{ipc::MakeHeader(0x0003, 4, 4), kProgram, 0, 8, 0x94},
                                  Mapped(8, ipc::R, id), Mapped(0x94, ipc::W, b)}),
                             6);
        msg_bytes = g.GetBytes(b, 0x94);
        // the same with the HMAC (key of 0x20 bytes) in the outbox
        g.PutBytes(key, std::vector<u8>(0x20, 0x0B));
        hmac_write = g.Request(cecd,
                               Cat({{ipc::MakeHeader(0x0007, 4, 6), kProgram, 1, 8, 0x94},
                                    Mapped(0x94, ipc::R, a), Mapped(0x20, ipc::R, key),
                                    Mapped(8, ipc::R, id)}),
                               8);
        g.PutBytes(b, std::vector<u8>(0x94, 0xEE));
        hmac_read = g.Request(cecd,
                              Cat({{ipc::MakeHeader(0x0004, 4, 6), kProgram, 1, 8, 0x94},
                                   Mapped(8, ipc::R, id), Mapped(0x20, ipc::R, key),
                                   Mapped(0x94, ipc::W, b)}),
                              8);
        hmac_bytes = g.GetBytes(b, 0x94);
        // Delete(program, type 6, is_outbox 0, id size 8, id): the inbox message
        del_msg = g.Request(cecd,
                            Cat({{ipc::MakeHeader(0x0008, 4, 2), kProgram, 6, 0, 8},
                                 Mapped(8, ipc::R, id)}),
                            4);
        read_msg_after = g.Request(cecd,
                                   Cat({{ipc::MakeHeader(0x0003, 4, 4), kProgram, 0, 8, 0x94},
                                        Mapped(8, ipc::R, id), Mapped(0x94, ipc::W, b)}),
                                   6);
        // Delete the whole box (type 11), then it cannot be opened without the create flag
        del_dir = g.Request(cecd,
                            Cat({{ipc::MakeHeader(0x0008, 4, 2), kProgram, 11, 0, 0},
                                 Mapped(8, ipc::R, id)}),
                            4);
        open_after = open(cecd, kProgram, 11, 0);

        // ReadData(dest size, info type, param size; param, dest)
        const auto read_data = [&](u32 type, u32 size) {
            g.PutBytes(b, std::vector<u8>(8, 0xEE));
            eula_reply = g.Request(cecd,
                                   Cat({{ipc::MakeHeader(0x000A, 3, 4), size, type, 0},
                                        Mapped(4, ipc::R, a), Mapped(size, ipc::W, b)}),
                                   6);
            return g.GetBytes(b, 4);
        };
        eula_version = read_data(1, 2);
        eula = read_data(2, 1);
        parent = read_data(3, 1);
        empty_dest = read_data(9, 2);

        state = g.Request(cecd, {ipc::MakeHeader(0x000E, 0, 0)}, 3);
        start = g.Request(cecd, {ipc::MakeHeader(0x000B, 1, 0), 1}, 2);
        event_reply = g.Request(cecd, {ipc::MakeHeader(0x000F, 0, 0)}, 4);
        event_wait = g.Wait(event_reply[3], 0);
        unknown = g.Request(cecd, {ipc::MakeHeader(0x001E, 0, 0)}, 2);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(root == std::vector<u32>{ipc::MakeHeader(0x0001, 2, 0), kResultSuccess, 1});
    CHECK(list == std::vector<u32>{ipc::MakeHeader(0x0001, 2, 0), kResultSuccess, 0x18C});
    CHECK(read_list[0] == ipc::MakeHeader(0x0002, 2, 2));
    CHECK(read_list[1] == kResultSuccess);
    CHECK(read_list[2] == 0x18C);
    CHECK(list_bytes[0] == 0x68);
    CHECK(list_bytes[1] == 0x68);
    CHECK(list_bytes[4] == 1);
    CHECK(list_bytes[0x18C] == 0xEE);  // nothing past the requested size
    // a box that does not exist, without and with the create flag
    CHECK(make_dir_no == std::vector<u32>{ipc::MakeHeader(0x0001, 2, 0), kNoData, 0});
    CHECK(make_dir_yes == std::vector<u32>{ipc::MakeHeader(0x0001, 2, 0), kResultSuccess, 0});
    CHECK(make_in[1] == kResultSuccess);
    CHECK(make_out[1] == kResultSuccess);
    CHECK(write_info[0] == ipc::MakeHeader(0x0011, 1, 2));
    CHECK(write_info[1] == kResultSuccess);
    CHECK(read_info[0] == ipc::MakeHeader(0x0012, 2, 2));
    CHECK(read_info[2] == 0x60);
    CHECK(info_bytes[0] == 0x63);  // 'cc' set by the check
    CHECK(info_bytes[1] == 0x63);
    CHECK(read_list2[2] == 0x18C);
    CHECK(list2_bytes[0] == 0x68);
    CHECK(list2_bytes[4] == 1);
    CHECK(list2_bytes[8] == 1);  // num_boxes: added once, not twice
    CHECK(std::string(list2_bytes.begin() + 12, list2_bytes.begin() + 20) == "00020000");
    CHECK(shared == std::vector<u32>{ipc::MakeHeader(0x0001, 2, 0), kResultSuccess, 3});  // InBox___, MBoxInfo____, OutBox__
    CHECK(dir_read[0] == ipc::MakeHeader(0x0002, 2, 2));
    CHECK(dir_read[1] == kNotAuthorized);
    CHECK(dir_read[2] == 0);
    CHECK(write_msg[0] == ipc::MakeHeader(0x0006, 1, 4));
    CHECK(write_msg[1] == kResultSuccess);
    CHECK(read_msg[0] == ipc::MakeHeader(0x0003, 2, 4));
    CHECK(read_msg[1] == kResultSuccess);
    CHECK(read_msg[2] == 0x94);
    CHECK(msg_bytes[0x70] == 0xDE);
    CHECK(msg_bytes[0x73] == 0xEF);
    CHECK(hmac_write[0] == ipc::MakeHeader(0x0007, 1, 6));
    CHECK(hmac_write[1] == kResultSuccess);
    CHECK(hmac_read[0] == ipc::MakeHeader(0x0004, 2, 6));
    CHECK(hmac_read[1] == kResultSuccess);
    {
        // the digest stored after the body is HMAC-SHA256(key, body)
        const std::vector<u8> key_bytes(0x20, 0x0B);
        const std::vector<u8> body = {0xDE, 0xAD, 0xBE, 0xEF};
        const auto expected = cecd::HmacSha256(key_bytes, body);
        REQUIRE(hmac_bytes.size() == 0x94);
        CHECK(std::equal(expected.begin(), expected.end(), hmac_bytes.begin() + 0x74));
    }
    CHECK(del_msg == std::vector<u32>{ipc::MakeHeader(0x0008, 1, 2), kResultSuccess,
                                      ipc::MappedBufferDesc(8, ipc::R), del_msg[3]});
    CHECK(read_msg_after[1] == kNoData);
    CHECK(read_msg_after[2] == 0);
    CHECK(del_dir[1] == kResultSuccess);
    CHECK(open_after[1] == kNoData);
    CHECK(eula_reply[0] == ipc::MakeHeader(0x000A, 1, 4));
    CHECK(eula_version[0] == 0x7F);
    CHECK(eula_version[1] == 0x7F);
    CHECK(eula_version[2] == 0xEE);  // two bytes were asked for
    CHECK(eula[0] == 1);
    CHECK(parent[0] == 0);
    CHECK(empty_dest[0] == 0xEE);  // an unknown type writes nothing
    CHECK(state == std::vector<u32>{ipc::MakeHeader(0x000E, 2, 0), kResultSuccess, 1});
    CHECK(start == std::vector<u32>{ipc::MakeHeader(0x000B, 1, 0), kResultSuccess});
    CHECK(event_reply[0] == ipc::MakeHeader(0x000F, 1, 2));
    CHECK(event_reply[2] == ipc::CopyHandleDesc(1));
    CHECK(event_wait == kTimeout);  // created, never signalled
    CHECK(unknown[0] == ipc::MakeHeader(0x001E, 1, 0));
    CHECK(f.Logged("unimplemented cecd:u"));
}

TEST_CASE("cecd: SHA-256 and HMAC-SHA256 match their test vectors") {
    // FIPS 180-4: "abc"
    const std::string abc = "abc";
    const auto digest = cecd::Sha256({reinterpret_cast<const u8*>(abc.data()), abc.size()});
    const u8 expected_abc[32] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
                                 0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
                                 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    CHECK(std::equal(digest.begin(), digest.end(), expected_abc));
    // the empty message
    const auto empty = cecd::Sha256({});
    CHECK(empty[0] == 0xe3);
    CHECK(empty[1] == 0xb0);
    CHECK(empty[31] == 0x55);
    // RFC 4231 test case 2: key "Jefe", data "what do ya want for nothing?"
    const std::string key = "Jefe";
    const std::string data = "what do ya want for nothing?";
    const auto mac = cecd::HmacSha256({reinterpret_cast<const u8*>(key.data()), key.size()},
                                      {reinterpret_cast<const u8*>(data.data()), data.size()});
    const u8 expected_mac[32] = {0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e, 0x6a, 0x04, 0x24,
                                 0x26, 0x08, 0x95, 0x75, 0xc7, 0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27,
                                 0x39, 0x83, 0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43};
    CHECK(std::equal(mac.begin(), mac.end(), expected_mac));
    // a message of more than one block, and a key longer than a block (RFC 4231 test case 6)
    const std::vector<u8> long_key(131, 0xaa);
    const std::string long_data = "Test Using Larger Than Block-Size Key - Hash Key First";
    const auto mac6 = cecd::HmacSha256(long_key, {reinterpret_cast<const u8*>(long_data.data()), long_data.size()});
    CHECK(mac6[0] == 0x60);
    CHECK(mac6[1] == 0xe4);
    CHECK(mac6[31] == 0x54);
}

// ---- boss ---------------------------------------------------------------------------------------------------

TEST_CASE("boss: sessions keep their own tasks and properties; there is no SpotPass data") {
    Fixture f;
    TempDir dir;
    constexpr Result kUnknown = 0xFFFFFFFF;
    std::vector<u32> uninit, init, reg, list, total, send_unknown, recv_unknown, unreg_missing,
        unreg_long, unreg, total_after, other_session, ns_ids, ns_header, ns_read, last_update,
        optout, arrival, finish, state, storage, unknown, privileged_header, privileged_read;
    std::vector<u8> ids_bytes, url_bytes, headers_bytes;
    std::vector<u32> url_reply, headers_reply, ids_reply, send_reply;
    Result finish_wait = 0;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle srv = ConnectSrv(g);
        const Handle boss = OpenService(g, srv, "boss:U");
        const Handle other = OpenService(g, srv, "boss:U");
        const u32 a = g.Scratch(0), b = g.Scratch(0x400);
        // a command before InitializeSession
        uninit = g.Request(boss, {ipc::MakeHeader(0x000E, 0, 0)}, 2);
        init = g.Request(boss, {ipc::MakeHeader(0x0001, 2, 2), 0, 0, ipc::CallingPidDesc(), 0}, 2);
        g.PutBytes(a, {'T', 'A', 'S', 'K', '0', '0', '0', '1'});
        // RegisterTask(size, 2 bytes, task id)
        reg = g.Request(boss,
                        Cat({{ipc::MakeHeader(0x000B, 3, 2), 8, 0, 0}, Mapped(8, ipc::R, a)}), 4);
        list = g.Request(boss, {ipc::MakeHeader(0x000E, 0, 0)}, 2);
        // ReceiveProperty(property 0x35 TotalTasks, size 2) and the id list (0x36, 0x400 bytes)
        g.PutBytes(b, std::vector<u8>(0x400, 0xEE));
        total = g.Request(boss,
                          Cat({{ipc::MakeHeader(0x0016, 2, 2), 0x35, 2}, Mapped(2, ipc::W, b)}), 6);
        total_after.push_back(g.GetBytes(b, 4)[0]);
        ids_reply = g.Request(boss,
                              Cat({{ipc::MakeHeader(0x0016, 2, 2), 0x36, 0x400},
                                   Mapped(0x400, ipc::W, b)}),
                              6);
        ids_bytes = g.GetBytes(b, 12);
        // property 0x7 is the URL (0x200 bytes): send, then receive back
        std::vector<u8> url(0x200, 0);
        std::copy_n("http://x.test/a", 15, url.begin());
        g.PutBytes(a, url);
        send_reply = g.Request(boss,
                               Cat({{ipc::MakeHeader(0x0014, 2, 2), 0x07, 0x200}, Mapped(0x200, ipc::R, a)}), 4);
        g.PutBytes(b, std::vector<u8>(0x200, 0xEE));
        url_reply = g.Request(boss,
                              Cat({{ipc::MakeHeader(0x0016, 2, 2), 0x07, 0x200},
                                   Mapped(0x200, ipc::W, b)}),
                              6);
        url_bytes = g.GetBytes(b, 16);
        // unknown property ids
        send_unknown = g.Request(boss,
                                 Cat({{ipc::MakeHeader(0x0014, 2, 2), 0x99, 4}, Mapped(4, ipc::R, a)}), 4);
        recv_unknown = g.Request(boss,
                                 Cat({{ipc::MakeHeader(0x0016, 2, 2), 0x99, 4}, Mapped(4, ipc::W, b)}), 6);
        // a second session starts without tasks, until it is initialised itself
        other_session = g.Request(other, {ipc::MakeHeader(0x000E, 0, 0)}, 2);
        // UnregisterTask(size, byte, id): a task that is not there, an id longer than 8, the real one
        g.PutBytes(a, {'N', 'O', 'P', 'E', 'N', 'O', 'P', 'E', 'X', 'X'});
        unreg_missing = g.Request(boss,
                                  Cat({{ipc::MakeHeader(0x000C, 2, 2), 8, 0}, Mapped(8, ipc::R, a)}), 4);
        unreg_long = g.Request(boss,
                               Cat({{ipc::MakeHeader(0x000C, 2, 2), 10, 0}, Mapped(10, ipc::R, a)}), 4);
        g.PutBytes(a, {'T', 'A', 'S', 'K', '0', '0', '0', '1'});
        unreg = g.Request(boss,
                          Cat({{ipc::MakeHeader(0x000C, 2, 2), 8, 0}, Mapped(8, ipc::R, a)}), 4);
        g.Request(boss, {ipc::MakeHeader(0x000E, 0, 0)}, 2);
        g.PutBytes(b, std::vector<u8>(0x400, 0xEE));
        g.Request(boss, Cat({{ipc::MakeHeader(0x0016, 2, 2), 0x35, 2}, Mapped(2, ipc::W, b)}), 6);
        total_after.push_back(g.GetBytes(b, 4)[0]);

        // no NsData
        ns_ids = g.Request(boss,
                           Cat({{ipc::MakeHeader(0x0010, 4, 2), 0xFFFFFFFF, 8, 0, 0},
                                Mapped(0x20, ipc::W, b)}),
                           6);
        ns_header = g.Request(boss,
                              Cat({{ipc::MakeHeader(0x0027, 3, 2), 0x12345678, 6, 0x20},
                                   Mapped(0x20, ipc::W, b)}),
                              4);
        ns_read = g.Request(boss,
                            Cat({{ipc::MakeHeader(0x0028, 4, 2), 0x12345678, 0, 0, 0x10},
                                 Mapped(0x10, ipc::W, b)}),
                            2);
        last_update = g.Request(boss, {ipc::MakeHeader(0x002D, 1, 0), 0x12345678}, 2);
        // flags kept by the module, shared by the sessions
        g.Request(boss, {ipc::MakeHeader(0x0009, 1, 0), 1}, 2);
        optout = g.Request(other, {ipc::MakeHeader(0x000A, 0, 0)}, 3);
        arrival = g.Request(boss, {ipc::MakeHeader(0x0007, 0, 0)}, 3);
        finish = g.Request(boss, {ipc::MakeHeader(0x001F, 0, 0)}, 4);
        finish_wait = g.Wait(finish[3], 0);
        state = g.Request(boss,
                          Cat({{ipc::MakeHeader(0x0020, 2, 2), 8, 1}, Mapped(8, ipc::R, a)}), 8);
        storage = g.Request(boss, {ipc::MakeHeader(0x0004, 0, 0)}, 3);
        unknown = g.Request(boss, {ipc::MakeHeader(0x0038, 0, 0)}, 2);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(uninit == std::vector<u32>{ipc::MakeHeader(0x000E, 1, 0), kUnknown});
    CHECK(init == std::vector<u32>{ipc::MakeHeader(0x0001, 1, 0), kResultSuccess});
    CHECK(reg[0] == ipc::MakeHeader(0x000B, 1, 2));
    CHECK(reg[1] == kResultSuccess);
    CHECK(list == std::vector<u32>{ipc::MakeHeader(0x000E, 1, 0), kResultSuccess});
    CHECK(total[0] == ipc::MakeHeader(0x0016, 2, 2));
    CHECK(total[1] == kResultSuccess);
    CHECK(total[2] == 2);  // the reply echoes the size
    REQUIRE(total_after.size() == 2);
    CHECK(total_after[0] == 1);  // one task
    CHECK(total_after[1] == 0);  // none after UnregisterTask
    CHECK(ids_reply[1] == kResultSuccess);
    CHECK(std::string(ids_bytes.begin(), ids_bytes.begin() + 8) == "TASK0001");
    CHECK(ids_bytes[8] == 0);  // the list is zero padded
    CHECK(send_reply[0] == ipc::MakeHeader(0x0014, 1, 2));
    CHECK(send_reply[1] == kResultSuccess);
    CHECK(url_reply[1] == kResultSuccess);
    CHECK(std::string(url_bytes.begin(), url_bytes.begin() + 15) == "http://x.test/a");
    CHECK(send_unknown[1] == MakeResult(77, 62, 11, 25));
    CHECK(recv_unknown[1] == MakeResult(77, 62, 11, 25));
    CHECK(recv_unknown[2] == 4);
    CHECK(other_session == std::vector<u32>{ipc::MakeHeader(0x000E, 1, 0), kUnknown});
    CHECK(unreg_missing[1] == MakeResult(51, 62, 5, 25));
    CHECK(unreg_long[1] == kUnknown);
    CHECK(unreg[1] == kResultSuccess);
    CHECK(ns_ids[0] == ipc::MakeHeader(0x0010, 3, 2));
    CHECK(ns_ids[1] == kResultSuccess);
    CHECK(ns_ids[2] == 0);
    CHECK(ns_ids[3] == 0);
    CHECK(ns_header[1] == MakeResult(64, 62, 5, 25));
    CHECK(ns_read == std::vector<u32>{ipc::MakeHeader(0x0028, 1, 0), MakeResult(64, 62, 5, 25)});
    CHECK(last_update == std::vector<u32>{ipc::MakeHeader(0x002D, 1, 0), kUnknown});
    CHECK(optout == std::vector<u32>{ipc::MakeHeader(0x000A, 2, 0), kResultSuccess, 1});
    CHECK(arrival == std::vector<u32>{ipc::MakeHeader(0x0007, 2, 0), kResultSuccess, 0});
    CHECK(finish[0] == ipc::MakeHeader(0x001F, 1, 2));
    CHECK(finish_wait == kTimeout);
    CHECK(state[0] == ipc::MakeHeader(0x0020, 4, 2));
    CHECK(state[1] == kResultSuccess);
    CHECK(storage == std::vector<u32>{ipc::MakeHeader(0x0004, 2, 0), kResultSuccess, 0});
    CHECK(unknown[0] == ipc::MakeHeader(0x0038, 1, 0));
}

TEST_CASE("boss:P: the privileged commands are Azahar's stubs") {
    Fixture f;
    TempDir dir;
    std::vector<u32> init, flag, read, header, unknown_in_u;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle srv = ConnectSrv(g);
        const Handle boss_p = OpenService(g, srv, "boss:P");
        const Handle boss_u = OpenService(g, srv, "boss:U");
        const u32 a = g.Scratch(0);
        init = g.Request(boss_p, {ipc::MakeHeader(0x0401, 2, 2), 0, 0, ipc::CallingPidDesc(), 0}, 2);
        flag = g.Request(boss_p, {ipc::MakeHeader(0x0404, 2, 0), 0, 0}, 3);
        header = g.Request(boss_p,
                           Cat({{ipc::MakeHeader(0x0416, 4, 2), 0, 0, 5, 6, 8},
                                Mapped(8, ipc::W, a)}),
                           4);
        read = g.Request(boss_p,
                         Cat({{ipc::MakeHeader(0x0417, 6, 2), 0, 0, 0x1234, 0, 0, 0x40},
                              Mapped(0x40, ipc::W, a)}),
                         5);
        unknown_in_u = g.Request(boss_u, {ipc::MakeHeader(0x0404, 2, 0), 0, 0}, 2);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(init == std::vector<u32>{ipc::MakeHeader(0x0401, 1, 0), kResultSuccess});
    CHECK(flag == std::vector<u32>{ipc::MakeHeader(0x0404, 2, 0), kResultSuccess, 0});
    CHECK(header[0] == ipc::MakeHeader(0x0416, 1, 2));
    CHECK(header[1] == kResultSuccess);
    CHECK(read[0] == ipc::MakeHeader(0x0417, 3, 2));
    CHECK(read[2] == 0x40);  // Azahar echoes the size as the size read
    CHECK(unknown_in_u[0] == ipc::MakeHeader(0x0404, 1, 0));  // boss:U has no 0x04xx commands
}


// ---- soc:U -----------------------------------------------------------------------------------------------------

namespace {

constexpr Result kSocNotInitialized = MakeResult(6, 28, 7, 27);
constexpr Result kSocInvalidDescriptor = MakeResult(7, 28, 7, 27);
constexpr Result kSocAlreadyInitialized = MakeResult(11, 28, 5, 25);
constexpr Result kSocUnimplemented = MakeResult(1012, 28, 6, 27);
constexpr s32 kEAGAIN = -6, kENOTCONN = -56, kENETUNREACH = -40, kEADDRINUSE = -3,
              kEADDRNOTAVAIL = -4, kEPIPE = -66, kEDESTADDRREQ = -17, kEBADF = -8, kEINVAL = -28,
              kENOPROTOOPT = -51;

// The 3DS sockaddr_in: length 8, family 2, port (network order), address.
std::vector<u8> InetAddr(u16 host_port, std::array<u8, 4> ip) {
    return {8, 2, static_cast<u8>(host_port >> 8), static_cast<u8>(host_port), ip[0], ip[1], ip[2], ip[3]};
}

struct SocGuest {
    Guest& g;
    Handle soc;
    u32 data, aux, out, out2;  // scratch areas
    SocGuest(Guest& guest, Handle service)
        : g(guest), soc(service), data(guest.Scratch(0)), aux(guest.Scratch(0x200)),
          out(guest.Scratch(0x400)), out2(guest.Scratch(0x800)) {}
    std::vector<u32> Call(const std::vector<u32>& words, std::size_t reply) {
        return g.Request(soc, words, reply);
    }
    Result Init() { return Call({ipc::MakeHeader(1, 1, 4), 0x10000, ipc::CallingPidDesc(), 0, ipc::CopyHandleDesc(1), 0}, 2)[1]; }
    // Socket(domain, type, protocol): the reply's words
    std::vector<u32> Socket(u32 domain, u32 type, u32 protocol = 0) {
        return Call({ipc::MakeHeader(2, 3, 2), domain, type, protocol, ipc::CallingPidDesc(), 0}, 3);
    }
    s32 Ret(const std::vector<u32>& reply) { return static_cast<s32>(reply[2]); }
    s32 OpenSocket(u32 type) {
        const auto reply = Socket(2, type);
        REQUIRE(reply[1] == kResultSuccess);
        return Ret(reply);
    }
    std::vector<u32> Close(s32 fd) {
        return Call({ipc::MakeHeader(0xB, 1, 2), static_cast<u32>(fd), ipc::CallingPidDesc(), 0}, 3);
    }
    std::vector<u32> Bind(s32 fd, const std::vector<u8>& address) {
        g.PutBytes(data, address);
        return Call({ipc::MakeHeader(5, 2, 4), static_cast<u32>(fd), static_cast<u32>(address.size()),
                     ipc::CallingPidDesc(), 0, ipc::StaticBufferDesc(address.size(), 0), data},
                    3);
    }
    // The sockaddr GetSockName returns, as bytes.
    std::vector<u8> SockName(s32 fd, u32 max = 0x1C) {
        g.PutBytes(out, std::vector<u8>(0x40, 0xEE));
        SetReceive(g, 0, 0x1C, out);
        const auto reply = Call({ipc::MakeHeader(0x17, 2, 2), static_cast<u32>(fd), max, ipc::CallingPidDesc(), 0}, 5);
        REQUIRE(reply[1] == kResultSuccess);
        return g.GetBytes(out, 0x20);
    }
    s32 Fcntl(s32 fd, u32 cmd, u32 arg) {
        return Ret(Call({ipc::MakeHeader(0x13, 3, 2), static_cast<u32>(fd), cmd, arg, ipc::CallingPidDesc(), 0}, 3));
    }
    s32 SetOpt(s32 fd, u32 level, u32 opt, const std::vector<u8>& value) {
        g.PutBytes(aux, value);
        return Ret(Call({ipc::MakeHeader(0x12, 4, 4), static_cast<u32>(fd), level, opt, static_cast<u32>(value.size()),
                         ipc::CallingPidDesc(), 0, ipc::StaticBufferDesc(value.size(), 0), aux},
                        3));
    }
    // GetSockOpt: the error word and the value
    std::pair<s32, std::vector<u8>> GetOpt(s32 fd, u32 level, u32 opt, u32 len) {
        g.PutBytes(out, std::vector<u8>(0x20, 0xEE));
        SetReceive(g, 0, 0x20, out);
        const auto reply = Call({ipc::MakeHeader(0x11, 4, 2), static_cast<u32>(fd), level, opt, len,
                                 ipc::CallingPidDesc(), 0},
                                5);
        REQUIRE(reply[0] == ipc::MakeHeader(0x11, 3, 2));
        return {static_cast<s32>(reply[2]), g.GetBytes(out, reply[3])};
    }
};

// Another guest thread, with a command buffer and receive buffers of its own, whose body sends one
// request through `session` and notes when it resumes.
struct OtherThread {
    std::shared_ptr<Thread> thread;
    u32 command = 0;
    bool resumed = false;
    u64 start = 0, end = 0;
    Result result = 0;
};

OtherThread* SpawnRequest(Fixture& f, std::deque<OtherThread>& list, Handle session,
                          const std::vector<u32>& words,
                          const std::vector<std::array<u32, 3>>& receives /* id, size, address */) {
    Kernel& kernel = f.runtime->GetKernel();
    GuestScheduler& scheduler = f.runtime->Scheduler();
    ServiceManager* manager = FindServiceManager(*f.runtime);
    REQUIRE(manager != nullptr);
    const u32 tls = 0x08200000 + static_cast<u32>(list.size()) * kPageSize;
    REQUIRE(IsSuccess(f.runtime->Memory().Map(tls, kPageSize, MemPerm::ReadWrite, MemState::Private)));
    OtherThread& other = list.emplace_back();
    other.thread = kernel.NewThread("other", 48, 0, kernel.CurrentProcess());
    other.thread->tls_address = tls;
    other.command = CommandBufferAddress(*other.thread);
    REQUIRE(f.runtime->Memory().WriteBlock(other.command, words.data(), static_cast<u32>(words.size() * 4)));
    for (const auto& receive : receives) {
        const u32 pair[2] = {ipc::StaticBufferDesc(receive[1], static_cast<u8>(receive[0])), receive[2]};
        REQUIRE(f.runtime->Memory().WriteBlock(
            other.command + static_cast<u32>(ipc::StaticBufferDescriptorWord(receive[0])) * 4, pair, 8));
    }
    OtherThread* self = &other;
    scheduler.AttachFunction(other.thread, [&kernel, &scheduler, manager, session, self] {
        self->start = kernel.NowTicks();
        self->result = manager->SendSyncRequest(*self->thread, session);
        if (self->thread->status != ThreadStatus::Running) {
            scheduler.Park(*self->thread);  // what the SVC layer does
        }
        self->end = kernel.NowTicks();
        self->resumed = true;
    });
    scheduler.Start(other.thread);
    return &other;
}

}  // namespace

TEST_CASE("soc:U: process and descriptor checks, socket creation and closing") {
    Fixture f;
    TempDir dir;
    std::vector<u32> before_init, bad_protocol, bad_domain, bad_type, zero_type, close_ok, close_again, bind_after, again;
    s32 first = 0, second = 0, third = 0, later = 0;
    Result init = 0, init_again = 0, shutdown_before = 0, shutdown = 0, shutdown_again = 0;
    std::vector<u32> unknown;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle srv = ConnectSrv(g);
        SocGuest s(g, OpenService(g, srv, "soc:U"));
        shutdown_before = s.Call({ipc::MakeHeader(0x19, 0, 0)}, 2)[1];
        // Socket does not look at the process; the commands that take a socket do
        const std::vector<u32> created = s.Socket(2, 2);
        before_init = s.Bind(static_cast<s32>(created[2]), InetAddr(0, {0, 0, 0, 0}));
        init = s.Init();
        init_again = s.Init();
        bind_after = s.Bind(static_cast<s32>(created[2]), InetAddr(0, {0, 0, 0, 0}));
        bad_protocol = s.Socket(2, 1, 6);
        bad_domain = s.Socket(23, 1);
        bad_type = s.Socket(2, 3);
        zero_type = s.Socket(2, 0);
        first = s.Ret(created);
        second = s.OpenSocket(1);
        third = s.OpenSocket(2);
        close_ok = s.Close(second);
        close_again = s.Close(second);
        later = s.OpenSocket(1);
        unknown = s.Call({ipc::MakeHeader(0x1B, 0, 0)}, 2);  // ICMPSocket: a nullptr entry in Azahar
        shutdown = s.Call({ipc::MakeHeader(0x19, 0, 0)}, 2)[1];
        again = s.Call({ipc::MakeHeader(0x0B, 1, 2), static_cast<u32>(third), ipc::CallingPidDesc(), 0}, 2);
        shutdown_again = s.Call({ipc::MakeHeader(0x19, 0, 0)}, 2)[1];
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(shutdown_before == kSocNotInitialized);
    CHECK(before_init == std::vector<u32>{ipc::MakeHeader(5, 1, 0), kSocNotInitialized, before_init[2]});
    CHECK(init == kResultSuccess);
    CHECK(init_again == kSocAlreadyInitialized);
    CHECK(bind_after[1] == kResultSuccess);
    CHECK(bind_after[2] == 0);
    // protocol, family and type outside what Azahar supports: "unimplemented", and the word it skips
    CHECK(bad_protocol[0] == ipc::MakeHeader(2, 2, 0));
    CHECK(bad_protocol[1] == kSocUnimplemented);
    CHECK(bad_protocol[2] == 1);  // the request's second parameter, untouched
    CHECK(bad_domain[1] == kSocUnimplemented);
    CHECK(bad_type[1] == kSocUnimplemented);
    CHECK(zero_type[1] == kSocUnimplemented);
    CHECK(first == 3);
    CHECK(second == 4);
    CHECK(third == 5);
    CHECK(close_ok == std::vector<u32>{ipc::MakeHeader(0xB, 2, 0), kResultSuccess, 0});
    CHECK(close_again[1] == kSocInvalidDescriptor);
    CHECK(later == 6);  // descriptors are not reused
    CHECK(unknown[0] == ipc::MakeHeader(0x1B, 1, 0));
    CHECK(shutdown == kResultSuccess);
    CHECK(again[1] == kSocNotInitialized);  // ShutdownSockets forgot the process
    CHECK(shutdown_again == kSocNotInitialized);
}

TEST_CASE("soc:U: bind, names, options and flags follow the offline host") {
    Fixture f;
    TempDir dir;
    s32 bind_any = 0, bind_again = 0, bind_other = 0, bind_twice = 0, bind_v6 = 0, bind_ephemeral = 0,
        bind_broadcast = 0, bind_reuse_a = 0, bind_reuse_b = 0, listen_udp = 0, listen_tcp = 0;
    std::vector<u8> name_unbound, name_bound, name_ephemeral;
    s32 get_flags = 0, set_flags = 0, get_flags_after = 0, bad_cmd = 0;
    std::pair<s32, std::vector<u8>> type_opt, reuse_before, reuse_after, ttl_default, ttl_after, rcvbuf_default,
        sndbuf_set, nodelay_udp, error_opt, linger, unknown_opt, one_byte;
    s32 set_reuse = 0, set_ttl = 0, set_ttl_zero = 0, set_type = 0, set_nodelay_udp = 0, set_unknown = 0,
        set_bad_name = 0, set_sndbuf = 0, set_linger = 0;
    std::vector<u32> host_id, mac, ip_info, bad_level, bad_opt, bad_opt_reply;
    std::vector<u8> mac_bytes, ip_bytes;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle srv = ConnectSrv(g);
        SocGuest s(g, OpenService(g, srv, "soc:U"));
        s.Init();
        const s32 a = s.OpenSocket(2), b = s.OpenSocket(2), c = s.OpenSocket(2), t = s.OpenSocket(1);
        name_unbound = s.SockName(a);
        bind_any = static_cast<s32>(s.Bind(a, InetAddr(0x1234, {0, 0, 0, 0}))[2]);
        name_bound = s.SockName(a);
        bind_again = static_cast<s32>(s.Bind(b, InetAddr(0x1234, {0, 0, 0, 0}))[2]);
        bind_other = static_cast<s32>(s.Bind(b, InetAddr(0x1235, {192, 168, 1, 5}))[2]);
        bind_twice = static_cast<s32>(s.Bind(a, InetAddr(0x2000, {0, 0, 0, 0}))[2]);
        {
            std::vector<u8> v6(28, 0);
            v6[0] = 28, v6[1] = 23;
            bind_v6 = static_cast<s32>(s.Bind(b, v6)[2]);
        }
        bind_broadcast = static_cast<s32>(s.Bind(b, InetAddr(0x1236, {255, 255, 255, 255}))[2]);
        bind_ephemeral = static_cast<s32>(s.Bind(c, InetAddr(0, {0, 0, 0, 0}))[2]);
        name_ephemeral = s.SockName(c);
        // SO_REUSEADDR on both sockets allows a shared port
        const s32 d = s.OpenSocket(2), e = s.OpenSocket(2);
        set_reuse = s.SetOpt(d, 0xFFFF, 4, {1, 0, 0, 0});
        s.SetOpt(e, 0xFFFF, 4, {1, 0, 0, 0});
        bind_reuse_a = static_cast<s32>(s.Bind(d, InetAddr(0x3000, {0, 0, 0, 0}))[2]);
        bind_reuse_b = static_cast<s32>(s.Bind(e, InetAddr(0x3000, {0, 0, 0, 0}))[2]);
        listen_udp = s.Ret(s.Call({ipc::MakeHeader(3, 2, 2), static_cast<u32>(a), 5, ipc::CallingPidDesc(), 0}, 3));
        listen_tcp = s.Ret(s.Call({ipc::MakeHeader(3, 2, 2), static_cast<u32>(t), 5, ipc::CallingPidDesc(), 0}, 3));

        get_flags = s.Fcntl(a, 3, 0);
        set_flags = s.Fcntl(a, 4, 4);
        get_flags_after = s.Fcntl(a, 3, 0);
        bad_cmd = s.Fcntl(a, 9, 0);
        s.Fcntl(a, 4, 0);

        type_opt = s.GetOpt(b, 0xFFFF, 0x1008, 4);
        reuse_before = s.GetOpt(b, 0xFFFF, 4, 4);
        reuse_after = s.GetOpt(d, 0xFFFF, 4, 4);
        ttl_default = s.GetOpt(b, 0, 8, 4);
        set_ttl = s.SetOpt(b, 0, 8, {32, 0, 0, 0});
        ttl_after = s.GetOpt(b, 0, 8, 4);
        set_ttl_zero = s.SetOpt(b, 0, 8, {0, 0, 0, 0});
        rcvbuf_default = s.GetOpt(b, 0xFFFF, 0x1002, 4);
        set_sndbuf = s.SetOpt(b, 0xFFFF, 0x1001, {0x00, 0x40, 0x00, 0x00});
        sndbuf_set = s.GetOpt(b, 0xFFFF, 0x1001, 4);
        set_type = s.SetOpt(b, 0xFFFF, 0x1008, {1, 0, 0, 0});
        nodelay_udp = s.GetOpt(b, 6, 0x2001, 4);
        set_nodelay_udp = s.SetOpt(b, 6, 0x2001, {1, 0, 0, 0});
        error_opt = s.GetOpt(b, 0xFFFF, 0x1009, 4);
        set_linger = s.SetOpt(b, 0xFFFF, 0x80, {1, 0, 0, 0, 5, 0, 0, 0});
        linger = s.GetOpt(b, 0xFFFF, 0x80, 8);
        one_byte = s.GetOpt(d, 0xFFFF, 4, 1);
        unknown_opt = s.GetOpt(b, 0xFFFF, 0x7777, 4);
        set_unknown = s.SetOpt(b, 0xFFFF, 0x7777, {1, 0, 0, 0});
        set_bad_name = s.SetOpt(b, 0xFFFF, static_cast<u32>(-5), {1, 0, 0, 0});

        host_id = s.Call({ipc::MakeHeader(0x16, 0, 0)}, 3);
        g.PutBytes(s.out, std::vector<u8>(0x20, 0xEE));
        SetReceive(g, 0, 0x20, s.out);
        mac = s.Call({ipc::MakeHeader(0x1A, 3, 0), 0xFFFE, 0x1004, 6}, 5);
        mac_bytes = g.GetBytes(s.out, 8);
        g.PutBytes(s.out, std::vector<u8>(0x20, 0xEE));
        ip_info = s.Call({ipc::MakeHeader(0x1A, 3, 0), 0xFFFE, 0x4003, 12}, 5);
        ip_bytes = g.GetBytes(s.out, 14);
        bad_level = s.Call({ipc::MakeHeader(0x1A, 3, 0), 0x1234, 0x4003, 12}, 5);
        bad_opt = s.Call({ipc::MakeHeader(0x1A, 3, 0), 0xFFFE, 0x4004, 12}, 5);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    // the name of a new socket: AF_INET, port 0, address 0
    CHECK(name_unbound[0] == 8);
    CHECK(name_unbound[1] == 2);
    CHECK(name_unbound[2] == 0);
    CHECK(name_unbound[4] == 0);
    CHECK(name_unbound[8] == 0);  // past the fields Azahar fills: zero here
    CHECK(bind_any == 0);
    CHECK(name_bound[2] == 0x12);
    CHECK(name_bound[3] == 0x34);
    CHECK(bind_again == kEADDRINUSE);
    CHECK(bind_other == kEADDRNOTAVAIL);
    CHECK(bind_twice == kEINVAL);
    CHECK(bind_v6 == -5);  // EAFNOSUPPORT
    CHECK(bind_broadcast == 0);
    CHECK(bind_ephemeral == 0);
    CHECK(((name_ephemeral[2] << 8) | name_ephemeral[3]) >= 49152);
    CHECK(set_reuse == 0);
    CHECK(bind_reuse_a == 0);
    CHECK(bind_reuse_b == 0);
    CHECK(listen_udp == -63);  // EOPNOTSUPP
    CHECK(listen_tcp == 0);
    CHECK(get_flags == 0);
    CHECK(set_flags == 0);
    CHECK(get_flags_after == 4);  // O_NONBLOCK
    CHECK(bad_cmd == kEINVAL);
    CHECK(type_opt.first == 0);
    CHECK(type_opt.second == std::vector<u8>{2, 0, 0, 0});
    CHECK(reuse_before.second == std::vector<u8>{0, 0, 0, 0});
    CHECK(reuse_after.second == std::vector<u8>{1, 0, 0, 0});
    CHECK(ttl_default.second == std::vector<u8>{64, 0, 0, 0});
    CHECK(set_ttl == 0);
    CHECK(ttl_after.second == std::vector<u8>{32, 0, 0, 0});
    CHECK(set_ttl_zero == 0);  // a TTL of 0 means the default
    CHECK(rcvbuf_default.second == std::vector<u8>{0x00, 0x40, 0x03, 0x00});  // 212992
    CHECK(set_sndbuf == 0);
    CHECK(sndbuf_set.second == std::vector<u8>{0x00, 0x80, 0x00, 0x00});  // doubled by the kernel
    CHECK(set_type == kENOPROTOOPT);
    CHECK(nodelay_udp.first == kENOPROTOOPT);
    CHECK(set_nodelay_udp == kENOPROTOOPT);
    CHECK(error_opt.second == std::vector<u8>{0, 0, 0, 0});
    CHECK(set_linger == 0);
    CHECK(linger.second == std::vector<u8>{1, 0, 0, 0, 5, 0, 0, 0});
    CHECK(one_byte.second == std::vector<u8>{1});  // an int into one byte
    CHECK(unknown_opt.first == kENOPROTOOPT);
    CHECK(set_unknown == kENOPROTOOPT);
    CHECK(set_bad_name == 22);  // EINVAL, not translated (Azahar returns the host's number)
    // the default interface is not found: zeros
    CHECK(host_id == std::vector<u32>{ipc::MakeHeader(0x16, 2, 0), kResultSuccess, 0});
    CHECK(mac[0] == ipc::MakeHeader(0x1A, 3, 2));
    CHECK(mac[2] == 0);
    CHECK(mac[3] == 6);
    CHECK(std::all_of(mac_bytes.begin(), mac_bytes.begin() + 6, [](u8 b) { return b == 0; }));
    CHECK(ip_info[2] == 0);
    CHECK(ip_info[3] == 12);
    CHECK(std::all_of(ip_bytes.begin(), ip_bytes.begin() + 12, [](u8 b) { return b == 0; }));
    CHECK(ip_bytes[12] == 0xEE);
    CHECK(bad_level[2] == 0xFFFF8025);
    CHECK(bad_level[3] == 0);
    CHECK(bad_opt[2] == 0xFFFF8025);
}

TEST_CASE("soc:U: nothing is reachable, nothing resolves, nothing is received") {
    Fixture f;
    TempDir dir;
    s32 connect_tcp = 0, connect_udp = 0, connect_v6 = 0, send_udp = 0, send_no_addr = 0, send_tcp = 0,
        send_big = 0, send_multi = 0, recv_stream = 0, recv_nonblock = 0, recv_dontwait = 0, recv_other_nonblock = 0,
        accept_nonblock = 0, accept_stream_unbound = 0, peer = 0, shutdown_udp = 0, shutdown_tcp = 0,
        recv_after_shutdown = 0, bad_how = 0, atmark = 0, accept_udp = 0;
    std::vector<u32> poll_udp, poll_tcp, poll_listen, poll_bad_fd, poll_bug, name_localhost, name_remote,
        addr_info, addr_numeric, name_info;
    std::vector<u8> poll_udp_bytes, poll_tcp_bytes, poll_listen_bytes, poll_bug_bytes, host_bytes,
        info_bytes, name_bytes, serv_bytes;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle srv = ConnectSrv(g);
        SocGuest s(g, OpenService(g, srv, "soc:U"));
        s.Init();
        const s32 udp = s.OpenSocket(2), tcp = s.OpenSocket(1), listener = s.OpenSocket(1);
        const std::vector<u8> remote = InetAddr(0x5000, {10, 0, 0, 1});
        const auto connect = [&](s32 fd, const std::vector<u8>& addr) {
            g.PutBytes(s.data, addr);
            return s.Ret(s.Call({ipc::MakeHeader(6, 2, 4), static_cast<u32>(fd), static_cast<u32>(addr.size()),
                                 ipc::CallingPidDesc(), 0, ipc::StaticBufferDesc(addr.size(), 0), s.data},
                                3));
        };
        connect_tcp = connect(tcp, remote);
        connect_udp = connect(udp, remote);
        std::vector<u8> v6(28, 0);
        v6[0] = 28, v6[1] = 23;
        connect_v6 = connect(udp, v6);

        const auto send = [&](s32 fd, u32 len, const std::vector<u8>& addr) {
            g.PutBytes(s.data, addr);
            g.PutBytes(s.aux, std::vector<u8>(16, 0x41));
            return s.Ret(s.Call({ipc::MakeHeader(0xA, 4, 6), static_cast<u32>(fd), len, 0, static_cast<u32>(addr.size()),
                                 ipc::CallingPidDesc(), 0, ipc::StaticBufferDesc(16, 0), s.aux,
                                 ipc::StaticBufferDesc(addr.size(), 1), s.data},
                                3));
        };
        send_udp = send(udp, 16, remote);
        send_no_addr = send(udp, 16, {});
        send_tcp = send(tcp, 16, remote);
        send_big = send(udp, 70000, remote);
        // SendToMultiple (0x20): stops at the first failure
        g.PutBytes(s.data, remote);
        g.PutBytes(s.aux, std::vector<u8>(16, 0x41));
        send_multi = s.Ret(s.Call({ipc::MakeHeader(0x20, 5, 6), static_cast<u32>(udp), 16, 0, 8, 16,
                                   ipc::CallingPidDesc(), 0, ipc::StaticBufferDesc(16, 0), s.aux,
                                   ipc::StaticBufferDesc(16, 1), s.data},
                                  3));

        // RecvFrom(handle, len, flags, addr_len): result, ret, received, then the buffers
        const auto recv = [&](s32 fd, u32 flags) {
            g.PutBytes(s.out, std::vector<u8>(0x400, 0xEE));
            SetReceive(g, 0, 0x100, s.out);
            SetReceive(g, 1, 0x20, s.out2);
            return s.Call({ipc::MakeHeader(8, 4, 2), static_cast<u32>(fd), 0x100, flags, 0x10,
                           ipc::CallingPidDesc(), 0},
                          6);
        };
        recv_stream = s.Ret(recv(tcp, 0));
        s.Fcntl(udp, 4, 4);
        const auto nonblock = recv(udp, 0);
        recv_nonblock = s.Ret(nonblock);
        recv_dontwait = s.Ret(recv(udp, 4));
        // RecvFromOther (7)
        g.PutBytes(s.out, std::vector<u8>(0x40, 0xEE));
        SetReceive(g, 0, 0x20, s.out2);
        recv_other_nonblock = s.Ret(s.Call({ipc::MakeHeader(7, 4, 4), static_cast<u32>(udp), 0x40, 0, 0x10,
                                            ipc::CallingPidDesc(), 0,
                                            ipc::MappedBufferDesc(0x40, ipc::W), s.out},
                                           3));
        s.Fcntl(udp, 4, 0);

        // listen, accept
        s.Call({ipc::MakeHeader(3, 2, 2), static_cast<u32>(listener), 5, ipc::CallingPidDesc(), 0}, 3);
        s.Fcntl(listener, 4, 4);
        SetReceive(g, 0, 0x1C, s.out);
        accept_nonblock = s.Ret(s.Call({ipc::MakeHeader(4, 2, 2), static_cast<u32>(listener), 0x1C,
                                        ipc::CallingPidDesc(), 0},
                                       3));
        accept_udp = s.Ret(s.Call({ipc::MakeHeader(4, 2, 2), static_cast<u32>(udp), 0x1C, ipc::CallingPidDesc(), 0}, 3));
        accept_stream_unbound = s.Ret(s.Call({ipc::MakeHeader(4, 2, 2), static_cast<u32>(tcp), 0x1C, ipc::CallingPidDesc(), 0}, 3));
        peer = s.Ret(s.Call({ipc::MakeHeader(0x18, 2, 2), static_cast<u32>(tcp), 0x1C, ipc::CallingPidDesc(), 0}, 3));
        atmark = s.Ret(s.Call({ipc::MakeHeader(0x15, 1, 2), static_cast<u32>(tcp), ipc::CallingPidDesc(), 0}, 3));

        // poll: three descriptors, with the events a guest asks for (3DS bits: 1 read, 8 write)
        const auto poll = [&](const std::vector<std::array<u32, 3>>& fds, s32 timeout) {
            std::vector<u8> bytes(fds.size() * 12);
            for (std::size_t i = 0; i < fds.size(); ++i) std::memcpy(bytes.data() + i * 12, fds[i].data(), 12);
            g.PutBytes(s.data, bytes);
            g.PutBytes(s.out, std::vector<u8>(0x100, 0xEE));
            SetReceive(g, 0, static_cast<u32>(bytes.size()), s.out);
            return s.Call({ipc::MakeHeader(0x14, 2, 4), static_cast<u32>(fds.size()), static_cast<u32>(timeout),
                           ipc::CallingPidDesc(), 0, ipc::StaticBufferDesc(bytes.size(), 0), s.data},
                          6);
        };
        poll_udp = poll({{static_cast<u32>(udp), 1 | 8, 0}}, 0);
        poll_udp_bytes = g.GetBytes(s.out, 12);
        poll_tcp = poll({{static_cast<u32>(tcp), 1 | 8, 0}}, 0);
        poll_tcp_bytes = g.GetBytes(s.out, 12);
        poll_listen = poll({{static_cast<u32>(listener), 1 | 8, 0}}, 0);
        poll_listen_bytes = g.GetBytes(s.out, 12);
        poll_bad_fd = poll({{99, 1, 0}}, 0);
        // the libctru bug: POLLOUT asked as bit 4 comes back as bit 4
        poll_bug = poll({{static_cast<u32>(udp), 16, 0}}, 0);
        poll_bug_bytes = g.GetBytes(s.out, 12);

        // shutdown: ENOTCONN, but the direction is closed
        shutdown_udp = s.Ret(s.Call({ipc::MakeHeader(0xC, 2, 2), static_cast<u32>(udp), 0, ipc::CallingPidDesc(), 0}, 3));
        recv_after_shutdown = s.Ret(recv(udp, 0));
        shutdown_tcp = s.Ret(s.Call({ipc::MakeHeader(0xC, 2, 2), static_cast<u32>(tcp), 2, ipc::CallingPidDesc(), 0}, 3));
        bad_how = s.Ret(s.Call({ipc::MakeHeader(0xC, 2, 2), static_cast<u32>(tcp), 5, ipc::CallingPidDesc(), 0}, 3));

        // names
        const auto by_name = [&](const std::string& name) {
            std::vector<u8> text(name.begin(), name.end());
            text.push_back(0);
            g.PutBytes(s.data, text);
            g.PutBytes(s.out, std::vector<u8>(0x1A90, 0xEE));
            SetReceive(g, 0, 0x1A88, s.out);
            return s.Call({ipc::MakeHeader(0xD, 2, 2), static_cast<u32>(text.size()), 0x1A88,
                           ipc::StaticBufferDesc(text.size(), 0), s.data},
                          5);
        };
        name_localhost = by_name("localhost");
        host_bytes = g.GetBytes(s.out, 0x1A88);
        name_remote = by_name("example.com");
        // GetAddrInfo(node length, service length, hints size, out size; node, service, hints)
        const auto info = [&](const std::string& node, const std::string& service, s32 flags, s32 family, s32 socktype) {
            std::vector<u8> n(node.begin(), node.end()), v(service.begin(), service.end());
            n.push_back(0), v.push_back(0);
            g.PutBytes(s.data, n);
            g.PutBytes(s.aux, v);
            std::array<s32, 5> hints = {flags, family, socktype, 0, 0};
            g.PutBytes(s.out2, std::vector<u8>(reinterpret_cast<u8*>(hints.data()), reinterpret_cast<u8*>(hints.data()) + 20));
            g.PutBytes(s.out, std::vector<u8>(0x400, 0xEE));
            SetReceive(g, 0, 0x400, s.out);
            return s.Call({ipc::MakeHeader(0xF, 4, 6), static_cast<u32>(n.size()), static_cast<u32>(v.size()), 20, 0x400,
                           ipc::StaticBufferDesc(n.size(), 0), s.data, ipc::StaticBufferDesc(v.size(), 1), s.aux,
                           ipc::StaticBufferDesc(20, 2), s.out2},
                          5);
        };
        addr_info = info("example.com", "80", 0, 0, 1);
        addr_numeric = info("10.1.2.3", "8080", 0, 0, 2);
        info_bytes = g.GetBytes(s.out, 0x130);
        // GetNameInfo(address length, host length, service length, flags; address)
        g.PutBytes(s.data, InetAddr(0x5000, {10, 1, 2, 3}));
        g.PutBytes(s.out, std::vector<u8>(0x40, 0xEE));
        g.PutBytes(s.out2, std::vector<u8>(0x40, 0xEE));
        SetReceive(g, 0, 0x20, s.out);
        SetReceive(g, 1, 0x10, s.out2);
        name_info = s.Call({ipc::MakeHeader(0x10, 4, 2), 8, 0x20, 0x10, 0, ipc::StaticBufferDesc(8, 0), s.data}, 6);
        name_bytes = g.GetBytes(s.out, 0x10);
        serv_bytes = g.GetBytes(s.out2, 8);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(connect_tcp == kENETUNREACH);
    CHECK(connect_udp == kENETUNREACH);
    CHECK(connect_v6 == -5);
    CHECK(send_udp == kENETUNREACH);
    CHECK(send_no_addr == kEDESTADDRREQ);
    CHECK(send_tcp == kEPIPE);
    CHECK(send_big == -35);  // EMSGSIZE
    CHECK(send_multi == kENETUNREACH);
    CHECK(recv_stream == kENOTCONN);
    CHECK(recv_nonblock == kEAGAIN);
    CHECK(recv_dontwait == kEAGAIN);
    CHECK(recv_other_nonblock == kEAGAIN);
    CHECK(accept_nonblock == kEAGAIN);
    CHECK(accept_udp == -63);
    CHECK(accept_stream_unbound == kEINVAL);
    CHECK(peer == kENOTCONN);
    CHECK(atmark == 0);
    // a datagram socket is writable, an unconnected stream socket is writable and hung up, a
    // listener has nothing
    CHECK(poll_udp[0] == ipc::MakeHeader(0x14, 2, 2));
    CHECK(poll_udp[2] == 1);
    CHECK(poll_udp_bytes[8] == 8);  // revents: write only
    CHECK(poll_tcp[2] == 1);
    CHECK(poll_tcp_bytes[8] == (8 | 64));  // write and hang-up
    CHECK(poll_listen[2] == 0);
    CHECK(poll_listen_bytes[8] == 0);
    CHECK(poll_bad_fd[1] == kSocInvalidDescriptor);
    CHECK(poll_bug_bytes[4] == 16);  // events come back as the guest wrote them
    CHECK(poll_bug_bytes[8] == 16);
    CHECK(shutdown_udp == kENOTCONN);
    CHECK(recv_after_shutdown == 0);  // the closed direction reads as the end of the stream
    CHECK(shutdown_tcp == kENOTCONN);
    CHECK(bad_how == kEINVAL);
    CHECK(name_localhost[2] == 0);
    CHECK(host_bytes[0] == 2);   // AF_INET
    CHECK(host_bytes[2] == 4);   // address length
    CHECK(host_bytes[4] == 1);   // one address
    CHECK(std::string(host_bytes.begin() + 8, host_bytes.begin() + 17) == "localhost");
    CHECK(host_bytes[8 + 256 + 24 * 256] == 127);
    CHECK(host_bytes[8 + 256 + 24 * 256 + 3] == 1);
    CHECK(name_remote[2] == kENETUNREACH);
    CHECK(addr_info[0] == ipc::MakeHeader(0xF, 3, 2));
    CHECK(static_cast<s32>(addr_info[2]) == -302);  // EAI_AGAIN
    CHECK(addr_info[3] == 0);
    CHECK(addr_numeric[2] == 0);
    CHECK(addr_numeric[3] == 1);  // one entry for a datagram hint
    CHECK(info_bytes[4] == 2);    // ai_family
    CHECK(info_bytes[8] == 2);    // ai_socktype
    CHECK(info_bytes[12] == 17);  // ai_protocol: UDP, derived from the type as soc does
    CHECK(info_bytes[16] == 8);   // ai_addrlen
    CHECK(info_bytes[20 + 256 + 1] == 2);
    CHECK(info_bytes[20 + 256 + 2] == 0x1F);  // port 8080, network order
    CHECK(info_bytes[20 + 256 + 3] == 0x90);
    CHECK(info_bytes[20 + 256 + 4] == 10);
    CHECK(name_info[2] == 0);
    CHECK(std::string(name_bytes.begin(), name_bytes.begin() + 8) == "10.1.2.3");
    CHECK(std::string(serv_bytes.begin(), serv_bytes.begin() + 5) == "20480");
}


TEST_CASE("soc:U: waiting calls wait in game time and end when their socket does") {
    Fixture f;
    TempDir dir;
    std::deque<OtherThread> threads;
    std::vector<u32> shutdown_reply, close_listener, close_d, recv_words, accept_words, poll_timeout_words, poll_closed_words;
    std::vector<u8> poll_timeout_bytes, poll_closed_bytes;
    bool resumed_early = true;
    u64 poll_elapsed = 0;
    const int status = BootWith(f, dir, {"--ms", "500"}, [&] {
        Guest g(*f.runtime);
        const Handle srv = ConnectSrv(g);
        SocGuest s(g, OpenService(g, srv, "soc:U"));
        s.Init();
        const s32 a = s.OpenSocket(2), c = s.OpenSocket(2), d = s.OpenSocket(2), listener = s.OpenSocket(1);
        s.Call({ipc::MakeHeader(3, 2, 2), static_cast<u32>(listener), 5, ipc::CallingPidDesc(), 0}, 3);
        GuestScheduler& scheduler = f.runtime->Scheduler();

        const u32 scratch = 0x08100000;  // the other threads' buffers
        REQUIRE(IsSuccess(f.runtime->Memory().Map(scratch, kPageSize, MemPerm::ReadWrite, MemState::Private)));
        const auto poll_words = [&](s32 fd, s32 timeout, u32 at) {
            const std::array<u32, 3> entry = {static_cast<u32>(fd), 1 /* read */, 0};
            REQUIRE(f.runtime->Memory().WriteBlock(at, entry.data(), 12));
            return std::vector<u32>{ipc::MakeHeader(0x14, 2, 4), 1, static_cast<u32>(timeout),
                                    ipc::CallingPidDesc(), 0, ipc::StaticBufferDesc(12, 0), at};
        };
        OtherThread* recv = SpawnRequest(
            f, threads, s.soc,
            {ipc::MakeHeader(8, 4, 2), static_cast<u32>(a), 0x40, 0, 0x10, ipc::CallingPidDesc(), 0},
            {{0, 0x40, scratch}, {1, 0x20, scratch + 0x100}});
        OtherThread* accept = SpawnRequest(
            f, threads, s.soc,
            {ipc::MakeHeader(4, 2, 2), static_cast<u32>(listener), 0x1C, ipc::CallingPidDesc(), 0},
            {{0, 0x1C, scratch + 0x200}});
        OtherThread* poll_timeout = SpawnRequest(f, threads, s.soc, poll_words(c, 50, scratch + 0x300),
                                                 {{0, 12, scratch + 0x320}});
        OtherThread* poll_closed = SpawnRequest(f, threads, s.soc, poll_words(d, -1, scratch + 0x400),
                                                {{0, 12, scratch + 0x420}});
        scheduler.Sleep(1000000);  // 1 ms: all four send their request and wait
        resumed_early = recv->resumed || accept->resumed || poll_timeout->resumed || poll_closed->resumed;

        // shutting the datagram socket down for reading answers ENOTCONN and wakes its receiver with 0
        shutdown_reply = s.Call({ipc::MakeHeader(0xC, 2, 2), static_cast<u32>(a), 0, ipc::CallingPidDesc(), 0}, 3);
        scheduler.Sleep(1000000);
        recv_words = g.GetWords(recv->command, 5);
        // closing the listener wakes its accept with EBADF
        close_listener = s.Close(listener);
        scheduler.Sleep(1000000);
        accept_words = g.GetWords(accept->command, 4);
        // closing the socket a poll without timeout watches ends the poll: that descriptor is invalid
        close_d = s.Close(d);
        scheduler.Sleep(1000000);
        poll_closed_words = g.GetWords(poll_closed->command, 4);
        poll_closed_bytes = g.GetBytes(scratch + 0x420, 12);
        // the poll with a timeout of 50 ms ends 50 ms after it began, on the game clock
        scheduler.Sleep(60000000);
        poll_timeout_words = g.GetWords(poll_timeout->command, 4);
        poll_timeout_bytes = g.GetBytes(scratch + 0x320, 12);
        poll_elapsed = poll_timeout->end - poll_timeout->start;
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK_FALSE(resumed_early);
    CHECK(shutdown_reply[2] == static_cast<u32>(kENOTCONN));
    REQUIRE(threads.size() == 4);
    CHECK(threads[0].resumed);
    CHECK(recv_words[0] == ipc::MakeHeader(8, 3, 4));
    CHECK(recv_words[1] == kResultSuccess);
    CHECK(recv_words[2] == 0);
    CHECK(recv_words[3] == 0);
    CHECK(close_listener[2] == 0);
    CHECK(threads[1].resumed);
    CHECK(accept_words[0] == ipc::MakeHeader(4, 2, 2));
    CHECK(accept_words[2] == static_cast<u32>(kEBADF));
    CHECK(close_d[2] == 0);
    CHECK(threads[3].resumed);
    CHECK(poll_closed_words[0] == ipc::MakeHeader(0x14, 2, 2));
    CHECK(poll_closed_words[2] == 1);
    CHECK(poll_closed_bytes[8] == 128);  // POLLNVAL
    CHECK(threads[2].resumed);
    CHECK(poll_timeout_words[1] == kResultSuccess);
    CHECK(poll_timeout_words[2] == 0);
    CHECK(poll_timeout_bytes[8] == 0);
    CHECK(poll_elapsed >= NsToTicks(50000000));
    CHECK(poll_elapsed <= NsToTicks(50000000) + NsToTicks(100000));  // exact to the timer queue's tick
}


// ---- http:C ---------------------------------------------------------------------------------------------------

namespace {

constexpr Result kHttpStateError = MakeResult(102, 40, 5, 27);          // 0xD8A0A066
constexpr Result kHttpNotImplemented = MakeResult(1012, 40, 11, 27);    // 0xD960A3F4
constexpr Result kHttpContextNotFound = MakeResult(100, 40, 5, 27);
constexpr Result kHttpTimeout = MakeResult(105, 40, 1, 27);             // 0xD820A069
constexpr Result kHttpHeaderNotFound = MakeResult(40, 40, 5, 27);       // 0xD8A0A028
constexpr Result kHttpInvalidMethod = MakeResult(32, 40, 5, 27);
constexpr Result kHttpTooManyContexts = MakeResult(26, 40, 5, 27);
constexpr Result kHttpInvalidState = MakeResult(22, 40, 5, 27);
constexpr Result kHttpIncompatibleAdd = MakeResult(50, 40, 5, 27);
constexpr Result kHttpIncompatibleSend = MakeResult(54, 40, 5, 27);
constexpr Result kHttpInvalidEncoding = MakeResult(53, 40, 5, 27);
constexpr Result kHttpWrongCertId = MakeResult(57, 46, 7, 27);
constexpr Result kHttpWrongCertHandle = MakeResult(201, 40, 5, 27);
constexpr Result kHttpTooManyClientCerts = MakeResult(203, 40, 5, 27);

struct HttpGuest {
    Guest& g;
    Handle h;
    u32 data, aux, out;
    HttpGuest(Guest& guest, Handle service)
        : g(guest), h(service), data(guest.Scratch(0)), aux(guest.Scratch(0x200)), out(guest.Scratch(0x400)) {}
    std::vector<u32> Call(const std::vector<u32>& words, std::size_t reply) { return g.Request(h, words, reply); }
    Result Initialize() {
        return Call({ipc::MakeHeader(1, 1, 4), 0x1000, ipc::CallingPidDesc(), 0, ipc::CopyHandleDesc(1), 0}, 2)[1];
    }
    // CreateContext: the reply (result, the context)
    std::vector<u32> Create(const std::string& url, u32 method) {
        std::vector<u8> text(url.begin(), url.end());
        text.push_back(0);
        g.PutBytes(data, text);
        return Call(Cat({{ipc::MakeHeader(2, 2, 2), static_cast<u32>(text.size()), method},
                         Mapped(static_cast<u32>(text.size()), ipc::R, data)}),
                    5);
    }
    Result Simple(u32 command, std::vector<u32> params) {
        std::vector<u32> words = {ipc::MakeHeader(static_cast<u16>(command), static_cast<u32>(params.size()), 0)};
        words.insert(words.end(), params.begin(), params.end());
        return Call(words, 2)[1];
    }
    Result Bind(u32 context) {
        return Call({ipc::MakeHeader(8, 1, 2), context, ipc::CallingPidDesc(), 0}, 2)[1];
    }
};

}  // namespace

TEST_CASE("http:C: sessions and contexts follow Azahar's rules; a request fails at once") {
    Fixture f;
    TempDir dir;
    std::vector<u32> not_init, ok, created, bad_zero, bad_nine, no_bound, state_before, state_after, status, status_timeout,
        header, data_reply, receive, download, closed, close_missing, unknown, ssl_error, begin_again, state_on_other;
    Result init_again = 0, bind_missing = 0, bind_twice = 0, bound_mismatch = 0, begin = 0, add_header = 0,
           add_header_after = 0;
    std::vector<u32> contexts;
    Result too_many = 0;
    std::vector<u8> data_bytes;
    const int status_code = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle srv = ConnectSrv(g);
        HttpGuest unbound(g, OpenService(g, srv, "http:C"));
        not_init = unbound.Create("http://x.test/", 1);
        REQUIRE(unbound.Initialize() == kResultSuccess);
        init_again = unbound.Initialize();
        created = unbound.Create("http://x.test/a", 1);
        bad_zero = unbound.Create("http://x.test/", 0);
        bad_nine = unbound.Create("http://x.test/", 9);
        for (int i = 0; i < 7; ++i) contexts.push_back(unbound.Create("http://x.test/", 1)[2]);
        too_many = unbound.Create("http://x.test/", 1)[1];
        // commands that need a bound context
        no_bound = unbound.Call({ipc::MakeHeader(0xA, 1, 0), created[2]}, 2);
        // a session bound to the context
        HttpGuest bound(g, OpenService(g, srv, "http:C"));
        bind_missing = bound.Bind(99);
        REQUIRE(bound.Bind(created[2]) == kResultSuccess);
        bind_twice = bound.Bind(created[2]);
        bound_mismatch = bound.Call({ipc::MakeHeader(0xA, 1, 0), contexts[0]}, 2)[1];
        state_before = bound.Call({ipc::MakeHeader(5, 1, 0), created[2]}, 3);
        add_header = bound.Call(Cat({{ipc::MakeHeader(0x11, 3, 4), created[2], 6, 4, ipc::StaticBufferDesc(6, 0), bound.aux},
                                     Mapped(4, ipc::R, bound.data)}),
                                4)[1];
        begin = bound.Call({ipc::MakeHeader(0xA, 1, 0), created[2]}, 2)[1];
        begin_again = bound.Call({ipc::MakeHeader(0xA, 1, 0), created[2]}, 2);
        state_after = bound.Call({ipc::MakeHeader(5, 1, 0), created[2]}, 3);
        add_header_after = bound.Call(Cat({{ipc::MakeHeader(0x11, 3, 4), created[2], 6, 4, ipc::StaticBufferDesc(6, 0), bound.aux},
                                           Mapped(4, ipc::R, bound.data)}),
                                      4)[1];
        // the response of a request that failed
        status = bound.Call({ipc::MakeHeader(0x22, 1, 0), created[2]}, 3);
        status_timeout = bound.Call({ipc::MakeHeader(0x23, 3, 0), created[2], 1000000, 0}, 3);
        g.PutBytes(bound.aux, {'X', '-', 'A', 0});
        g.PutBytes(bound.out, std::vector<u8>(0x40, 0xEE));
        header = bound.Call(Cat({{ipc::MakeHeader(0x1E, 3, 4), created[2], 4, 0x40, ipc::StaticBufferDesc(4, 0), bound.aux},
                                 Mapped(0x40, ipc::W, bound.out)}),
                            6);
        g.PutBytes(bound.out, std::vector<u8>(0x40, 0xEE));
        data_reply = bound.Call(Cat({{ipc::MakeHeader(0x20, 2, 2), created[2], 0x40}, Mapped(0x40, ipc::W, bound.out)}), 3);
        data_bytes = g.GetBytes(bound.out, 12);
        receive = bound.Call(Cat({{ipc::MakeHeader(0xB, 2, 2), created[2], 0x40}, Mapped(0x40, ipc::W, bound.out)}), 2);
        download = bound.Call({ipc::MakeHeader(6, 1, 0), created[2]}, 4);
        ssl_error = bound.Call({ipc::MakeHeader(0x2A, 1, 0), created[2]}, 3);
        // closing: a context that does not exist succeeds
        closed = unbound.Call({ipc::MakeHeader(3, 1, 0), created[2]}, 2);
        close_missing = unbound.Call({ipc::MakeHeader(3, 1, 0), 777}, 2);
        state_on_other = unbound.Call({ipc::MakeHeader(5, 1, 0), created[2]}, 2);
        unknown = unbound.Call({ipc::MakeHeader(0xC00, 0, 0)}, 2);
        ExitProcess(*f.runtime);
    });
    CHECK(status_code == 0);
    CHECK(not_init[0] == ipc::MakeHeader(2, 1, 0));
    CHECK(not_init[1] == kHttpStateError);
    CHECK(init_again == kHttpStateError);
    CHECK(created[0] == ipc::MakeHeader(2, 2, 2));
    CHECK(created[1] == kResultSuccess);
    CHECK(created[2] == 1);
    CHECK(created[3] == ipc::MappedBufferDesc(16, ipc::R));  // the URL and its NUL
    CHECK(bad_zero[0] == ipc::MakeHeader(2, 1, 2));
    CHECK(bad_zero[1] == kHttpInvalidMethod);
    CHECK(bad_nine[1] == kHttpInvalidMethod);
    REQUIRE(contexts.size() == 7);
    CHECK(contexts[6] == 8);   // the eighth context of the session
    CHECK(too_many == kHttpTooManyContexts);
    CHECK(no_bound[1] == kHttpNotImplemented);
    CHECK(bind_missing == kHttpContextNotFound);
    CHECK(bind_twice == kHttpStateError);
    CHECK(bound_mismatch == kHttpStateError);
    CHECK(state_before[2] == 1);   // NotStarted
    CHECK(add_header == kResultSuccess);
    CHECK(begin == kResultSuccess);
    CHECK(begin_again[1] == kResultSuccess);
    CHECK(state_after[2] == 0xA);  // Completed: the request failed
    CHECK(add_header_after == kHttpInvalidState);
    CHECK(status[0] == ipc::MakeHeader(0x22, 2, 0));
    CHECK(status[1] == kResultSuccess);
    CHECK(status[2] == 0xFFFFFFFF);
    CHECK(status_timeout[1] == kResultSuccess);
    CHECK(status_timeout[2] == 0xFFFFFFFF);
    CHECK(header[0] == ipc::MakeHeader(0x1E, 2, 2));
    CHECK(header[1] == kHttpHeaderNotFound);
    CHECK(header[2] == 0);
    CHECK(data_reply == std::vector<u32>{ipc::MakeHeader(0x20, 2, 0), kResultSuccess, 8});
    CHECK(std::string(data_bytes.begin(), data_bytes.begin() + 8) == " -1 \r\n\r\n");
    CHECK(data_bytes[8] == 0xEE);
    CHECK(receive == std::vector<u32>{ipc::MakeHeader(0xB, 1, 0), kResultSuccess});
    CHECK(download == std::vector<u32>{ipc::MakeHeader(6, 3, 0), kResultSuccess, 0, 0});
    CHECK(ssl_error == std::vector<u32>{ipc::MakeHeader(0x2A, 2, 0), kResultSuccess, 0});
    CHECK(closed[1] == kResultSuccess);
    CHECK(close_missing[1] == kResultSuccess);
    CHECK(state_on_other[1] == kHttpContextNotFound);  // the context is gone (Azahar asserts)
    CHECK(unknown[0] == ipc::MakeHeader(0xC00, 1, 0));
    CHECK(f.Logged("unimplemented http:C"));
}

TEST_CASE("http:C: post data checks, the pending POST request and certificates") {
    Fixture f;
    TempDir dir;
    std::vector<u32> post;
    Result raw_after_ascii = 0, type_after_data = 0, encoding_bad = 0, encoding_ascii = 0, encoding_auto = 0,
           notify_early = 0, begin = 0, notify = 0, add_ascii = 0, state_pending_result = 0, chunk_after_ascii = 0,
           send_ascii_not_chunked = 0, set_chunk = 0, send_raw = 0, add_after_chunk = 0,
           cert_id_bad = 0, cert_default = 0, begin_with_cert = 0, set_handle_bad = 0, close_cert = 0,
           open_third = 0, set_after_close = 0, set_ssl = 0, keepalive = 0, finalize = 0;
    u32 state_pending = 0, state_after_notify = 0, cert_a = 0, cert_b = 0, cert_c = 0;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle srv = ConnectSrv(g);
        HttpGuest main(g, OpenService(g, srv, "http:C"));
        REQUIRE(main.Initialize() == kResultSuccess);
        const u32 post_ctx = main.Create("http://x.test/post", 2)[2];  // POST
        const u32 other_ctx = main.Create("http://x.test/other", 2)[2];
        HttpGuest s(g, OpenService(g, srv, "http:C"));
        REQUIRE(s.Bind(post_ctx) == kResultSuccess);
        g.PutBytes(s.aux, {'f', 'o', 'o', 0});
        g.PutBytes(s.data, {'b', 'a', 'r', 0});
        const auto add_ascii_call = [&](u16 command) {
            return s.Call(Cat({{ipc::MakeHeader(command, 3, 4), post_ctx, 4, 4, ipc::StaticBufferDesc(4, 0), s.aux},
                               Mapped(4, ipc::R, s.data)}),
                          4)[1];
        };
        // a POST with no data waits for it: the state reads "sending"
        begin = s.Call({ipc::MakeHeader(0xA, 1, 0), post_ctx}, 2)[1];
        state_pending = s.Call({ipc::MakeHeader(5, 1, 0), post_ctx}, 3)[2];
        notify_early = s.Call({ipc::MakeHeader(0x1D, 1, 0), post_ctx}, 2)[1];
        add_ascii = add_ascii_call(0x12);
        raw_after_ascii = s.Call(Cat({{ipc::MakeHeader(0x14, 2, 2), post_ctx, 4}, Mapped(4, ipc::R, s.data)}), 4)[1];
        type_after_data = s.Call({ipc::MakeHeader(0x15, 2, 0), post_ctx, 2}, 2)[1];
        encoding_bad = s.Call({ipc::MakeHeader(0x1C, 2, 0), post_ctx, 5}, 2)[1];
        encoding_ascii = s.Call({ipc::MakeHeader(0x1C, 2, 0), post_ctx, 1}, 2)[1];
        encoding_auto = s.Call({ipc::MakeHeader(0x1C, 2, 0), post_ctx, 0}, 2)[1];
        send_ascii_not_chunked = s.Call(Cat({{ipc::MakeHeader(0x16, 3, 4), post_ctx, 4, 4, ipc::StaticBufferDesc(4, 0), s.aux},
                                             Mapped(4, ipc::R, s.data)}),
                                        4)[1];
        notify = s.Call({ipc::MakeHeader(0x1D, 1, 0), post_ctx}, 2)[1];
        state_after_notify = s.Call({ipc::MakeHeader(5, 1, 0), post_ctx}, 3)[2];

        // chunked mode: raw data only after SetPostDataType, never together with added data
        HttpGuest c(g, OpenService(g, srv, "http:C"));
        REQUIRE(c.Bind(other_ctx) == kResultSuccess);
        set_chunk = c.Call({ipc::MakeHeader(0x15, 2, 0), other_ctx, 2}, 2)[1];
        add_after_chunk = c.Call(Cat({{ipc::MakeHeader(0x14, 2, 2), other_ctx, 4}, Mapped(4, ipc::R, c.data)}), 4)[1];
        send_raw = c.Call(Cat({{ipc::MakeHeader(0x1A, 2, 2), other_ctx, 4}, Mapped(4, ipc::R, c.data)}), 4)[1];
        chunk_after_ascii = c.Call({ipc::MakeHeader(0x15, 2, 0), other_ctx, 0}, 2)[1];

        // certificates
        cert_id_bad = c.Call({ipc::MakeHeader(0x28, 2, 0), other_ctx, 0x41}, 2)[1];
        set_handle_bad = c.Call({ipc::MakeHeader(0x29, 2, 0), other_ctx, 55}, 2)[1];
        cert_default = c.Call({ipc::MakeHeader(0x28, 2, 0), other_ctx, 0x40}, 2)[1];
        begin_with_cert = c.Call({ipc::MakeHeader(0xA, 1, 0), other_ctx}, 2)[1];
        const auto open_default = [&](HttpGuest& h) { return h.Call({ipc::MakeHeader(0x33, 1, 0), 0x40}, 3); };
        HttpGuest certs(g, OpenService(g, srv, "http:C"));
        REQUIRE(certs.Initialize() == kResultSuccess);
        cert_a = open_default(certs)[2];
        cert_b = open_default(certs)[2];
        open_third = open_default(certs)[1];
        cert_c = certs.Call({ipc::MakeHeader(0x34, 1, 0), cert_a}, 2)[1];
        close_cert = certs.Call({ipc::MakeHeader(0x34, 1, 0), 9999}, 2)[1];
        set_after_close = open_default(certs)[1];
        set_ssl = c.Call({ipc::MakeHeader(0x2B, 2, 0), other_ctx, 3}, 2)[1];
        keepalive = c.Call({ipc::MakeHeader(0x37, 2, 0), other_ctx, 1}, 2)[1];
        finalize = c.Call({ipc::MakeHeader(0x39, 0, 0)}, 2)[1];
        post = s.Call({ipc::MakeHeader(0x22, 1, 0), post_ctx}, 3);
        (void)state_pending_result;
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(begin == kResultSuccess);
    CHECK(state_pending == 6);  // SendingRequest: waiting for the post data
    CHECK(notify_early == kHttpInvalidState);
    CHECK(add_ascii == kResultSuccess);
    CHECK(raw_after_ascii == kHttpIncompatibleAdd);
    CHECK(type_after_data == kHttpIncompatibleSend);
    CHECK(encoding_bad == kHttpInvalidEncoding);
    CHECK(encoding_ascii == kResultSuccess);
    CHECK(encoding_auto == kResultSuccess);
    CHECK(send_ascii_not_chunked == kHttpInvalidState);
    CHECK(notify == kResultSuccess);   // the request starts, and fails
    CHECK(state_after_notify == 0xA);
    CHECK(post[2] == 0xFFFFFFFF);
    CHECK(set_chunk == kResultSuccess);
    CHECK(add_after_chunk == kHttpIncompatibleAdd);
    CHECK(send_raw == kResultSuccess);  // chunked mode, type Raw
    CHECK(chunk_after_ascii == kHttpIncompatibleSend);  // the type cannot change once data was sent
    CHECK(cert_id_bad == kHttpWrongCertId);
    CHECK(set_handle_bad == kHttpWrongCertHandle);
    CHECK(cert_default == kResultSuccess);
    CHECK(begin_with_cert == kHttpStateError);  // the default client certificate is not there
    CHECK(cert_a == 1);
    CHECK(cert_b == 2);   // Azahar stores cert id 0 on the context it makes, so the same call makes another one
    CHECK(open_third == kHttpTooManyClientCerts);
    CHECK(cert_c == kResultSuccess);
    CHECK(close_cert == kResultSuccess);
    CHECK(set_after_close == kResultSuccess);
    CHECK(set_ssl == kResultSuccess);
    CHECK(keepalive == kResultSuccess);
    CHECK(finalize == kResultSuccess);
    (void)send_ascii_not_chunked;
    (void)state_pending_result;
    (void)set_handle_bad;
}

TEST_CASE("http:C: a call waiting for a response waits in game time") {
    Fixture f;
    TempDir dir;
    std::deque<OtherThread> threads;
    std::vector<u32> timeout_words, woken_words, closed_words, cancel_words;
    u64 timeout_elapsed = 0;
    bool early = true;
    const int status = BootWith(f, dir, {"--ms", "500"}, [&] {
        Guest g(*f.runtime);
        const Handle srv = ConnectSrv(g);
        HttpGuest main(g, OpenService(g, srv, "http:C"));
        REQUIRE(main.Initialize() == kResultSuccess);
        const u32 a = main.Create("http://x.test/a", 1)[2];
        const u32 b = main.Create("http://x.test/b", 1)[2];
        const u32 c = main.Create("http://x.test/c", 1)[2];
        const u32 d = main.Create("http://x.test/d", 1)[2];
        HttpGuest sa(g, OpenService(g, srv, "http:C")), sb(g, OpenService(g, srv, "http:C")),
            sc(g, OpenService(g, srv, "http:C")), sd(g, OpenService(g, srv, "http:C"));
        REQUIRE(sa.Bind(a) == kResultSuccess);
        REQUIRE(sb.Bind(b) == kResultSuccess);
        REQUIRE(sc.Bind(c) == kResultSuccess);
        REQUIRE(sd.Bind(d) == kResultSuccess);
        GuestScheduler& scheduler = f.runtime->Scheduler();
        // a: GetResponseStatusCodeTimeout with 20 ms; b: GetResponseStatusCode without a timeout, woken by
        // the request starting; c: the same, woken by CloseContext; d: ReceiveData woken by CancelConnection
        OtherThread* ta = SpawnRequest(f, threads, sa.h, {ipc::MakeHeader(0x23, 3, 0), a, 20000000, 0}, {});
        OtherThread* tb = SpawnRequest(f, threads, sb.h, {ipc::MakeHeader(0x22, 1, 0), b}, {});
        OtherThread* tc = SpawnRequest(f, threads, sc.h, {ipc::MakeHeader(0x22, 1, 0), c}, {});
        const u32 scratch = 0x08100000;
        REQUIRE(IsSuccess(f.runtime->Memory().Map(scratch, kPageSize, MemPerm::ReadWrite, MemState::Private)));
        OtherThread* td = SpawnRequest(f, threads, sd.h,
                                       {ipc::MakeHeader(0xB, 2, 2), d, 0x10, ipc::MappedBufferDesc(0x10, ipc::W), scratch}, {});
        scheduler.Sleep(1000000);
        early = ta->resumed || tb->resumed || tc->resumed || td->resumed;
        REQUIRE(sb.Call({ipc::MakeHeader(0xA, 1, 0), b}, 2)[1] == kResultSuccess);  // the request starts (and fails)
        scheduler.Sleep(1000000);
        woken_words = g.GetWords(tb->command, 3);
        REQUIRE(main.Call({ipc::MakeHeader(3, 1, 0), c}, 2)[1] == kResultSuccess);  // CloseContext
        scheduler.Sleep(1000000);
        closed_words = g.GetWords(tc->command, 3);
        REQUIRE(sd.Call({ipc::MakeHeader(4, 1, 0), d}, 2)[1] == kResultSuccess);    // CancelConnection
        scheduler.Sleep(1000000);
        cancel_words = g.GetWords(td->command, 2);
        scheduler.Sleep(30000000);
        timeout_words = g.GetWords(ta->command, 2);
        timeout_elapsed = ta->end - ta->start;
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK_FALSE(early);
    CHECK(threads[1].resumed);
    CHECK(woken_words == std::vector<u32>{ipc::MakeHeader(0x22, 2, 0), kResultSuccess, 0xFFFFFFFF});
    CHECK(threads[2].resumed);
    CHECK(closed_words == std::vector<u32>{ipc::MakeHeader(0x22, 2, 0), kResultSuccess, 0xFFFFFFFF});
    CHECK(threads[3].resumed);
    CHECK(cancel_words == std::vector<u32>{ipc::MakeHeader(0xB, 1, 0), kResultSuccess});
    CHECK(threads[0].resumed);
    CHECK(timeout_words == std::vector<u32>{ipc::MakeHeader(0x23, 1, 0), kHttpTimeout});
    CHECK(timeout_elapsed >= NsToTicks(20000000));
    CHECK(timeout_elapsed <= NsToTicks(20000000) + NsToTicks(100000));
}


// ---- y2r:u ---------------------------------------------------------------------------------------------------

namespace {

constexpr Result kY2rOutOfRange = MakeResult(1021, 20, 7, 28);      // 0xE0E053FD
constexpr Result kY2rInvalidEnum = MakeResult(1005, 20, 7, 28);     // 0xE0E053ED

// The pixel the hardware formula gives (the ITU Rec. 601 set), written out in the test.
std::array<u8, 3> ExpectedRgb(int Y, int U, int V) {
    const int c[8] = {0x100, 0x166, 0xB6, 0x58, 0x1C5, -0x166F, 0x10EE, -0x1C5B};
    int r = c[0] * Y + c[1] * V;
    int g = c[0] * Y - c[2] * V - c[3] * U;
    int b = c[0] * Y + c[4] * U;
    r = ((r >> 3) + c[5] + 0x18) >> 5;
    g = ((g >> 3) + c[6] + 0x18) >> 5;
    b = ((b >> 3) + c[7] + 0x18) >> 5;
    const auto clamp = [](int v) { return static_cast<u8>(v < 0 ? 0 : v > 255 ? 255 : v); };
    return {clamp(r), clamp(g), clamp(b)};
}

// An 8x8 YUV 4:2:2 test image: Y varies with the position, U and V per pair of pixels.
struct TestImage {
    std::vector<u8> y, u, v;
    TestImage() : y(64), u(32), v(32) {
        for (int py = 0; py < 8; ++py)
            for (int px = 0; px < 8; ++px) y[static_cast<std::size_t>(py * 8 + px)] = static_cast<u8>(16 * px + py * 3 + 20);
        for (int i = 0; i < 32; ++i) {
            u[static_cast<std::size_t>(i)] = static_cast<u8>(100 + 3 * i);
            v[static_cast<std::size_t>(i)] = static_cast<u8>(190 - 4 * i);
        }
    }
    std::array<u8, 3> Pixel(int x, int py) const {
        const int i = py * 8 + x;
        return ExpectedRgb(y[static_cast<std::size_t>(i)], u[static_cast<std::size_t>(i / 2)], v[static_cast<std::size_t>(i / 2)]);
    }
};

}  // namespace

TEST_CASE("y2r: the conversion matches the hardware formula for every output format, rotation and alignment") {
    Fixture f;
    TempDir dir;
    const TestImage image;
    struct Run {
        y2r::OutputFormat format;
        y2r::Rotation rotation;
        y2r::BlockAlignment alignment;
        std::vector<u8> out;
    };
    std::vector<Run> runs = {
        {y2r::OutputFormat::RGBA8, y2r::Rotation::None, y2r::BlockAlignment::Linear, {}},
        {y2r::OutputFormat::RGB8, y2r::Rotation::None, y2r::BlockAlignment::Linear, {}},
        {y2r::OutputFormat::RGB565, y2r::Rotation::None, y2r::BlockAlignment::Linear, {}},
        {y2r::OutputFormat::RGB5A1, y2r::Rotation::None, y2r::BlockAlignment::Linear, {}},
        {y2r::OutputFormat::RGBA8, y2r::Rotation::Clockwise_90, y2r::BlockAlignment::Linear, {}},
        {y2r::OutputFormat::RGBA8, y2r::Rotation::Clockwise_180, y2r::BlockAlignment::Linear, {}},
        {y2r::OutputFormat::RGBA8, y2r::Rotation::Clockwise_270, y2r::BlockAlignment::Linear, {}},
        {y2r::OutputFormat::RGBA8, y2r::Rotation::None, y2r::BlockAlignment::Block8x8, {}},
    };
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        for (Run& run : runs) {
            const u32 y_at = g.Scratch(0), u_at = g.Scratch(0x100), v_at = g.Scratch(0x200), out_at = g.Scratch(0x400);
            g.PutBytes(y_at, image.y);
            g.PutBytes(u_at, image.u);
            g.PutBytes(v_at, image.v);
            g.PutBytes(out_at, std::vector<u8>(0x400, 0xEE));
            y2r::ConversionConfiguration c;
            c.input_format = y2r::InputFormat::YUV422_Indiv8;
            c.output_format = run.format;
            c.rotation = run.rotation;
            c.block_alignment = run.alignment;
            c.input_line_width = 8;
            c.input_lines = 8;
            c.coefficients = {0x100, 0x166, 0xB6, 0x58, 0x1C5, -0x166F, 0x10EE, -0x1C5B};
            c.alpha = 0xFF;
            c.src_Y = {y_at, 64, 8, 0};
            c.src_U = {u_at, 32, 4, 0};
            c.src_V = {v_at, 32, 4, 0};
            const u16 bytes_per_pixel = run.format == y2r::OutputFormat::RGBA8 ? 4 : run.format == y2r::OutputFormat::RGB8 ? 3 : 2;
            c.dst = {out_at, 64u * bytes_per_pixel, static_cast<u16>(8 * bytes_per_pixel), 0};
            REQUIRE(y2r::PerformConversion(f.runtime->Memory(), c));
            run.out = g.GetBytes(out_at, 0x100);
        }
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    // pixel (x, y) of the output image for a run, as the bytes of its format
    const auto bytes_of = [](const Run& run, std::array<u8, 3> rgb) {
        const u8 r = rgb[0], g = rgb[1], b = rgb[2], a = 0xFF;
        switch (run.format) {
        case y2r::OutputFormat::RGBA8: return std::vector<u8>{a, b, g, r};
        case y2r::OutputFormat::RGB8: return std::vector<u8>{b, g, r};
        case y2r::OutputFormat::RGB565: {
            const u16 d = static_cast<u16>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            return std::vector<u8>{static_cast<u8>(d), static_cast<u8>(d >> 8)};
        }
        default: {
            const u16 d = static_cast<u16>(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >> 7));
            return std::vector<u8>{static_cast<u8>(d), static_cast<u8>(d >> 8)};
        }
        }
    };
    const auto check_at = [&](const Run& run, std::size_t out_index, std::array<u8, 3> rgb, const char* what) {
        const std::vector<u8> expected = bytes_of(run, rgb);
        const std::size_t bpp = expected.size();
        INFO(what << " at output pixel " << out_index);
        CHECK(std::equal(expected.begin(), expected.end(), run.out.begin() + static_cast<std::ptrdiff_t>(out_index * bpp)));
    };
    for (std::size_t k = 0; k < 4; ++k) {  // linear, no rotation: the pixels in order
        for (int py = 0; py < 8; ++py)
            for (int px = 0; px < 8; ++px) check_at(runs[k], static_cast<std::size_t>(py * 8 + px), image.Pixel(px, py), "linear");
    }
    for (int py = 0; py < 8; ++py)
        for (int px = 0; px < 8; ++px) {
            // 90 degrees clockwise: the pixel (x, y) goes to (7 - y, x)
            check_at(runs[4], static_cast<std::size_t>(px * 8 + (7 - py)), image.Pixel(px, py), "90");
            // 180: reversed
            check_at(runs[5], static_cast<std::size_t>(63 - (py * 8 + px)), image.Pixel(px, py), "180");
            // 270 clockwise: (x, y) goes to (y, 7 - x)
            check_at(runs[6], static_cast<std::size_t>((7 - px) * 8 + py), image.Pixel(px, py), "270");
        }
    // 8x8 blocks: the Morton order of the PICA's tiles
    static constexpr u8 kMorton[64] = {0,  1,  4,  5,  16, 17, 20, 21, 2,  3,  6,  7,  18, 19, 22, 23,
                                       8,  9,  12, 13, 24, 25, 28, 29, 10, 11, 14, 15, 26, 27, 30, 31,
                                       32, 33, 36, 37, 48, 49, 52, 53, 34, 35, 38, 39, 50, 51, 54, 55,
                                       40, 41, 44, 45, 56, 57, 60, 61, 42, 43, 46, 47, 58, 59, 62, 63};
    for (int py = 0; py < 8; ++py)
        for (int px = 0; px < 8; ++px) check_at(runs[7], kMorton[py * 8 + px], image.Pixel(px, py), "block");
}

TEST_CASE("y2r: a transfer unit that does not divide the data ends the conversion without touching memory") {
    Fixture f;
    TempDir dir;
    bool ok = true;
    std::vector<u8> out;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const u32 at = g.Scratch(0);
        g.PutBytes(at, std::vector<u8>(0x200, 0xEE));
        y2r::ConversionConfiguration c;
        c.input_line_width = 8;
        c.input_lines = 8;
        c.coefficients = {0x100, 0x166, 0xB6, 0x58, 0x1C5, -0x166F, 0x10EE, -0x1C5B};
        c.src_Y = {at, 64, 0, 0};  // a unit of 0
        c.dst = {at + 0x100, 256, 32, 0};
        ok = y2r::PerformConversion(f.runtime->Memory(), c);
        out = g.GetBytes(at + 0x100, 16);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK_FALSE(ok);
    CHECK(out[0] == 0xEE);
}

TEST_CASE("y2r:u: the service converts, signals 50000 ticks later and keeps its configuration") {
    Fixture f;
    TempDir dir;
    const TestImage image;
    std::vector<u32> init, line_bad, line_zero, line_ok, lines_ok, lines_big, std_bad, std_get, std_get_bad, get_width,
        get_lines, coeff, coeff_set, package_bad, package_ok, get_package, start, busy_after_start, busy_later, stop_start,
        busy_after_stop, finished, ping, alpha, dither, tei, formats, event_reply;
    std::vector<u8> out;
    Result event_before = 0, event_after = 0, event_after_stop = 0;
    u64 elapsed = 0;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle srv = ConnectSrv(g);
        const Handle y2r = OpenService(g, srv, "y2r:u");
        const auto call = [&](std::vector<u32> words, std::size_t reply) { return g.Request(y2r, words, reply); };
        const u32 y_at = g.Scratch(0), u_at = g.Scratch(0x100), v_at = g.Scratch(0x200), out_at = g.Scratch(0x400);
        g.PutBytes(y_at, image.y);
        g.PutBytes(u_at, image.u);
        g.PutBytes(v_at, image.v);
        g.PutBytes(out_at, std::vector<u8>(0x400, 0xEE));
        init = call({ipc::MakeHeader(0x2B, 0, 0)}, 2);
        get_width = call({ipc::MakeHeader(0x1B, 0, 0)}, 3);
        get_lines = call({ipc::MakeHeader(0x1D, 0, 0)}, 3);
        line_bad = call({ipc::MakeHeader(0x1A, 1, 0), 7}, 2);
        line_zero = call({ipc::MakeHeader(0x1A, 1, 0), 0}, 2);
        line_ok = call({ipc::MakeHeader(0x1A, 1, 0), 8}, 2);
        lines_big = call({ipc::MakeHeader(0x1C, 1, 0), 1025}, 2);
        lines_ok = call({ipc::MakeHeader(0x1C, 1, 0), 8}, 2);
        std_bad = call({ipc::MakeHeader(0x20, 1, 0), 4}, 2);
        call({ipc::MakeHeader(0x20, 1, 0), 0}, 2);
        std_get = call({ipc::MakeHeader(0x21, 1, 0), 3}, 6);
        std_get_bad = call({ipc::MakeHeader(0x21, 1, 0), 9}, 2);
        coeff_set = call({ipc::MakeHeader(0x1E, 4, 0), 0x01000100, 0x00020003, 0x00040005, 0x00060007}, 2);
        coeff = call({ipc::MakeHeader(0x1F, 0, 0)}, 6);
        call({ipc::MakeHeader(0x20, 1, 0), 0}, 2);
        alpha = call({ipc::MakeHeader(0x22, 1, 0), 0xFF}, 2);
        call({ipc::MakeHeader(1, 1, 0), 0}, 2);   // input 4:2:2 8-bit
        call({ipc::MakeHeader(3, 1, 0), 0}, 2);   // output RGBA8
        call({ipc::MakeHeader(5, 1, 0), 0}, 2);   // no rotation
        call({ipc::MakeHeader(7, 1, 0), 0}, 2);   // linear
        formats = call({ipc::MakeHeader(2, 0, 0)}, 3);
        // SetSendingY/U/V and SetReceiving(address, size, unit, gap; the process)
        const auto set_buffer = [&](u16 command, u32 at, u32 size, u32 unit) {
            return call({ipc::MakeHeader(command, 4, 2), at, size, unit, 0, ipc::CopyHandleDesc(1), 0xFFFF8001}, 2)[1];
        };
        REQUIRE(set_buffer(0x10, y_at, 64, 8) == kResultSuccess);
        REQUIRE(set_buffer(0x11, u_at, 32, 4) == kResultSuccess);
        REQUIRE(set_buffer(0x12, v_at, 32, 4) == kResultSuccess);
        REQUIRE(set_buffer(0x18, out_at, 256, 32) == kResultSuccess);
        finished = call({ipc::MakeHeader(0x19, 0, 0)}, 3);
        event_reply = call({ipc::MakeHeader(0x0F, 0, 0)}, 4);
        event_before = g.Wait(event_reply[3], 0);
        const u64 before = f.runtime->Clock().Now();
        start = call({ipc::MakeHeader(0x26, 0, 0)}, 2);
        busy_after_start = call({ipc::MakeHeader(0x28, 0, 0)}, 3);
        out = g.GetBytes(out_at, 64);
        f.runtime->Scheduler().Sleep(60000);  // 60 us: still busy (the conversion takes 50000 ticks, 186 us)
        busy_later = call({ipc::MakeHeader(0x28, 0, 0)}, 3);
        f.runtime->Scheduler().Sleep(150000);  // past 50000 ticks
        elapsed = f.runtime->Clock().Now() - before;
        event_after = g.Wait(event_reply[3], 0);
        // a second conversion that is stopped before it ends does not signal
        call({ipc::MakeHeader(0x26, 0, 0)}, 2);
        stop_start = call({ipc::MakeHeader(0x27, 0, 0)}, 2);
        busy_after_stop = call({ipc::MakeHeader(0x28, 0, 0)}, 3);
        f.runtime->Scheduler().Sleep(300000);
        event_after_stop = g.Wait(event_reply[3], 0);
        ping = call({ipc::MakeHeader(0x2A, 0, 0)}, 3);
        dither = call({ipc::MakeHeader(0x24, 8, 0), 1, 2, 3, 4, 5, 6, 7, 8}, 2);
        tei = call({ipc::MakeHeader(0x0D, 1, 0), 1}, 2);
        // SetPackageParameter(12 bytes) and GetPackageParameter
        package_bad = call({ipc::MakeHeader(0x29, 3, 0), 0x00010100, 0x00080007, 0x00FF0000}, 2);
        package_ok = call({ipc::MakeHeader(0x29, 3, 0), 0x00020101, 0x00100010, 0x01230001}, 2);
        get_package = call({ipc::MakeHeader(0x2D, 0, 0)}, 5);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(init == std::vector<u32>{ipc::MakeHeader(0x2B, 1, 0), kResultSuccess});
    CHECK(get_width == std::vector<u32>{ipc::MakeHeader(0x1B, 2, 0), kResultSuccess, 1024});
    CHECK(get_lines[2] == 0);  // 1024 lines are not stored (Azahar mirrors the hardware bug of the camera module)
    CHECK(line_bad[1] == kY2rOutOfRange);
    CHECK(line_zero[1] == kY2rOutOfRange);
    CHECK(line_ok[1] == kResultSuccess);
    CHECK(lines_big[1] == kY2rOutOfRange);
    CHECK(lines_ok[1] == kResultSuccess);
    CHECK(std_bad[1] == kY2rInvalidEnum);
    CHECK(std_get == std::vector<u32>{ipc::MakeHeader(0x21, 5, 0), kResultSuccess, 0x01CA012A, 0x00360088, 0xE0FC021C, 0xDBDF099C});
    CHECK(std_get_bad == std::vector<u32>{ipc::MakeHeader(0x21, 1, 0), kY2rInvalidEnum});
    CHECK(coeff_set[1] == kResultSuccess);
    CHECK(coeff[0] == ipc::MakeHeader(0x1F, 5, 0));
    CHECK(coeff[2] == 0x01000100);
    CHECK(coeff[5] == 0x00060007);
    CHECK(alpha[1] == kResultSuccess);
    CHECK(formats == std::vector<u32>{ipc::MakeHeader(2, 2, 0), kResultSuccess, 0});
    CHECK(finished == std::vector<u32>{ipc::MakeHeader(0x19, 2, 0), kResultSuccess, 1});
    CHECK(event_reply[0] == ipc::MakeHeader(0x0F, 1, 2));
    CHECK(event_before == kTimeout);
    CHECK(start == std::vector<u32>{ipc::MakeHeader(0x26, 1, 0), kResultSuccess});
    CHECK(busy_after_start == std::vector<u32>{ipc::MakeHeader(0x28, 2, 0), kResultSuccess, 1});
    {
        // the first row of the converted image (Rec. 601 coefficients, alpha 0xFF)
        REQUIRE(out.size() == 64);
        for (int px = 0; px < 8; ++px) {
            const auto rgb = image.Pixel(px, 0);
            INFO("pixel " << px);
            CHECK(out[static_cast<std::size_t>(px * 4)] == 0xFF);
            CHECK(out[static_cast<std::size_t>(px * 4 + 1)] == rgb[2]);
            CHECK(out[static_cast<std::size_t>(px * 4 + 2)] == rgb[1]);
            CHECK(out[static_cast<std::size_t>(px * 4 + 3)] == rgb[0]);
        }
    }
    CHECK(busy_later[2] == 1);
    CHECK(elapsed >= 50000);
    CHECK(event_after == kResultSuccess);
    CHECK(stop_start[1] == kResultSuccess);
    CHECK(busy_after_stop[2] == 0);
    CHECK(event_after_stop == kTimeout);
    CHECK(ping == std::vector<u32>{ipc::MakeHeader(0x2A, 2, 0), kResultSuccess, 0});
    CHECK(dither[1] == kResultSuccess);
    CHECK(tei[1] == kResultSuccess);
    CHECK(package_bad[1] == kY2rOutOfRange);
    CHECK(package_ok[1] == kResultSuccess);
    CHECK(get_package[0] == ipc::MakeHeader(0x2D, 4, 0));
    CHECK(get_package[2] == 0x00020101);  // input 1, output 1, rotation 2, linear
    CHECK(get_package[3] == 0x00100010);  // 16 by 16
    CHECK(get_package[4] == 0x01930100);  // the first two coefficients of the Rec. 709 set
}

#endif  // recomp.h
