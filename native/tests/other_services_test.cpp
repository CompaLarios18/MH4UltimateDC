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
#include "mh4n/options.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"
#include "mh4n/service.h"
#include "mh4n/threads.h"
#include "../src/services/cecd/sha256.h"
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

#endif  // recomp.h
