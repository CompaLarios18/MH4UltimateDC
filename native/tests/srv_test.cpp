// Derived from Azahar src/core/hle/service/sm/srv.cpp, src/core/hle/service/sm/sm.h,
// src/core/hle/service/sm/sm.cpp, src/core/hle/service/err/err_f.cpp, src/core/hle/service/service.cpp,
// src/core/hle/kernel/client_port.cpp, src/core/hle/kernel/errors.h and src/core/hle/kernel/svc.cpp
// at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: svc.cpp says "GPLv3 or any later version";
// the others say "GPLv2 or any later version"): the commands of srv: and err:f with their headers and result codes (RegisterClient,
// EnableNotification, GetServiceHandle with its flags and name checks, Subscribe, Unsubscribe,
// PublishToSubscriber, ThrowFatalError), the SRV result codes, the answer to an unknown command, the
// limits of the two named ports (srv: 64 sessions, err:f 1), and the layout of the error information. The
// expected values for the name checks are the ones port/tests/expected/conf_srv.txt records from
// Azahar.
// Copyright 2016-2025 Citra Emulator Project / Azahar Emulator Project (srv.cpp)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (sm.h)
// Copyright 2017 Citra Emulator Project (sm.cpp, errors.h)
// Copyright 2014 Citra Emulator Project (err_f.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (service.cpp, svc.cpp)
// Copyright 2016-2024 Citra Emulator Project / Azahar Emulator Project (client_port.cpp)
//
// The srv: and err:f ports (N1-10), through the SVC layer: the guest connects to the ports by name and
// sends requests as a program would. The programs are ELF files written by the test whose main thread
// runs a C++ function (Runtime::SetMainThreadEntry); time is a ManualTimeSource.
#include <doctest/doctest.h>

#if !__has_include("recomp.h")
TEST_CASE("srv_test needs recomp.h (the repository around native/)" * doctest::skip()) {}
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

// ---- a service to get handles of -------------------------------------------------------------------------

class Echo final : public Service {
public:
    explicit Echo(ServiceContext& context) : Service(context, "tst:srv") {
        Register(0x0001, "Echo", &Echo::Answer);
    }

private:
    void Answer(Request& request) {
        const u32 value = request.Pop<u32>();
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(value + 0x5000);
    }
};

// A registered service (two sessions at most), reached through srv:.
MH4N_REGISTER(ServiceFactory, "tst:srv",
              ServiceFactory([](ServiceContext& c) { return std::make_unique<Echo>(c); }, 2));

ServiceManager& Manager(Fixture& f) {
    ServiceManager* manager = FindServiceManager(*f.runtime);
    REQUIRE(manager != nullptr);
    return *manager;
}

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

}  // namespace

// ---- srv: -----------------------------------------------------------------------------------------------------

TEST_CASE("srv: RegisterClient answers with the PID checked, and does not need to come first") {
    Fixture f;
    TempDir dir;
    std::vector<u32> ok, wrong, early;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle srv = ConnectSrv(g);
        early =
            GetService(g, srv, "tst:srv", 7, 0).reply;  // before RegisterClient: fine, as in Azahar
        ok = g.Request(srv, {ipc::MakeHeader(0x0001, 0, 2), ipc::CallingPidDesc(), 0}, 2);
        wrong = g.Request(srv, {ipc::MakeHeader(0x0001, 0, 2), ipc::CopyHandleDesc(1), 0}, 2);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(early[0] == 0x00050042);
    CHECK(early[1] == kResultSuccess);
    REQUIRE(ok.size() == 2);
    CHECK(ok[0] == 0x00010040);
    CHECK(ok[1] == kResultSuccess);
    CHECK(wrong[0] == 0x00010040);
    CHECK(wrong[1] == 0xD9001830);  // the descriptor is not the calling PID's
}

TEST_CASE("srv: GetServiceHandle gives a new session of a registered service every time") {
    Fixture f;
    TempDir dir;
    Got first, second;
    std::vector<u32> through_first, through_second;
    Got third;
    Result after_close = 0;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle srv = ConnectSrv(g);
        first = GetService(g, srv, "tst:srv", 7, 0);
        second = GetService(g, srv, "tst:srv", 7, 0);
        through_first = g.Request(first.handle, {ipc::MakeHeader(0x0001, 1, 0), 1}, 3);
        through_second = g.Request(second.handle, {ipc::MakeHeader(0x0001, 1, 0), 2}, 3);
        // the service allows two sessions: the third is refused, until one is closed
        third = GetService(g, srv, "tst:srv", 7, 0);
        g.Close(first.handle);
        Got fourth = GetService(g, srv, "tst:srv", 7, 0);
        after_close = fourth.reply[1];
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    REQUIRE(first.reply.size() == 4);
    CHECK(first.reply[0] == 0x00050042);
    CHECK(first.reply[1] == kResultSuccess);
    CHECK(first.reply[2] == ipc::MoveHandleDesc(1));
    CHECK(first.handle != 0);
    CHECK(second.handle != 0);
    CHECK(first.handle != second.handle);
    CHECK(through_first[1] == kResultSuccess);
    CHECK(through_first[2] == 0x5001);
    CHECK(through_second[2] == 0x5002);
    CHECK(third.reply[0] == 0x00050040);
    CHECK(third.reply[1] == 0xD0401834);
    CHECK(after_close == kResultSuccess);
}

TEST_CASE("srv: GetServiceHandle checks the name as Azahar does (conf_srv's cases)") {
    Fixture f;
    TempDir dir;
    std::vector<Got> got;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle srv = ConnectSrv(g);
        got.push_back(GetService(g, srv, "nope:u", 6, 1));   // 0: unknown, don't wait
        got.push_back(GetService(g, srv, "tst:srv", 3, 1));  // 1: a shorter name than the service's
        got.push_back(GetService(g, srv, "tst:srv", 9, 0));  // 2: longer than 8
        got.push_back(GetService(g, srv, "tst:srv", 0, 1));  // 3: no name
        got.push_back(GetService(g, srv, "tst:srv", 8, 0));  // 4: 8 bytes, with a NUL inside
        got.push_back(GetService(g, srv, "tst:srv", 7, 1));  // 5: flags 1 on a service that exists
        // 6: flags 2: bit 0 clear, it exists, no wait
        got.push_back(GetService(g, srv, "tst:srv", 7, 2));
        got.push_back(GetService(g, srv, "err:f", 5, 1));  // 7: a named port is not a service
        got.push_back(GetService(g, srv, "srv:", 4, 1));   // 8: neither is srv:
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    REQUIRE(got.size() == 9);
    auto expect_error = [&](std::size_t i, Result result) {
        INFO("case " << i);
        CHECK(got[i].reply[0] == 0x00050040);
        CHECK(got[i].reply[1] == result);
        CHECK(got[i].handle == 0);
    };
    expect_error(0, 0xD0406401);
    expect_error(1, 0xD0406401);
    expect_error(2, 0xD9006405);
    expect_error(3, 0xD9006405);
    expect_error(4, 0xD9006407);
    CHECK(got[5].reply[0] == 0x00050042);
    CHECK(got[5].handle != 0);
    CHECK(got[6].reply[0] == 0x00050042);
    CHECK(got[6].handle != 0);
    expect_error(7, 0xD0406401);
    expect_error(8, 0xD0406401);
}

TEST_CASE("srv: GetServiceHandle with the wait flag clear waits until the service is registered") {
    Fixture f;
    TempDir dir;
    Got got;
    std::vector<u32> through;
    u64 waited = 0;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle srv = ConnectSrv(g);
        Kernel& kernel = f.runtime->GetKernel();
        ServiceManager& manager = Manager(f);
        // another service registers first and does not wake the client; the one it wants comes later
        kernel.ScheduleAt(kernel.NowTicks() + NsToTicks(1000000), [&manager] {
            manager.RegisterService("tst:other", ServiceFactory([](ServiceContext& c) {
                                        return std::make_unique<Echo>(c);
                                    }));
        });
        kernel.ScheduleAt(kernel.NowTicks() + NsToTicks(3000000), [&manager] {
            manager.RegisterService("tst:late", ServiceFactory([](ServiceContext& c) {
                                        return std::make_unique<Echo>(c);
                                    }));
        });
        const u64 before = f.runtime->Clock().Now();
        got = GetService(g, srv, "tst:late", 8, 0);
        waited = f.runtime->Clock().Now() - before;
        through = g.Request(got.handle, {ipc::MakeHeader(0x0001, 1, 0), 7}, 3);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(waited >=
          NsToTicks(3000000) - 2);  // it came back when the service was registered, not before
    CHECK(got.reply[0] == 0x00050042);
    CHECK(got.reply[1] == kResultSuccess);
    CHECK(got.handle != 0);
    CHECK(through[2] == 0x5007);
    CHECK(f.Logged("GetServiceHandle service=tst:late delayed"));
}

TEST_CASE("srv: a client that waits for a service forever is the thread's problem, not the run's") {
    Fixture f;
    TempDir dir;
    bool returned = false;
    const int status = BootWith(f, dir, {"--ms", "20"}, [&] {
        Guest g(*f.runtime);
        const Handle srv = ConnectSrv(g);
        // Azahar waits for ever too (conf_srv avoids it with flags = 1)
        GetService(g, srv, "never:u", 7, 0);
        returned = true;
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);  // the time limit
    CHECK_FALSE(returned);
}

TEST_CASE("srv: EnableNotification gives a semaphore with nothing in it") {
    Fixture f;
    TempDir dir;
    std::vector<u32> reply;
    Result wait = 0, close = 0;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle srv = ConnectSrv(g);
        reply = g.Request(srv, {0x00020000}, 4);
        wait = g.Wait(reply[3], 0);
        close = g.Close(reply[3]);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(reply[0] == 0x00020042);
    CHECK(reply[1] == kResultSuccess);
    CHECK(reply[2] == ipc::CopyHandleDesc(1));
    CHECK(reply[3] != 0);
    CHECK(wait == kTimeout);
    CHECK(close == kResultSuccess);
}

TEST_CASE(
    "srv: Subscribe, Unsubscribe, ReceiveNotification and PublishToSubscriber answer a success") {
    Fixture f;
    TempDir dir;
    std::vector<std::vector<u32>> replies;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle srv = ConnectSrv(g);
        replies.push_back(g.Request(srv, {0x00090040, 0x1000}, 2));     // Subscribe
        replies.push_back(g.Request(srv, {0x000A0040, 0x1000}, 2));     // Unsubscribe
        replies.push_back(g.Request(srv, {0x000B0000}, 2));             // ReceiveNotification
        replies.push_back(g.Request(srv, {0x000C0080, 0x1000, 1}, 2));  // PublishToSubscriber
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    REQUIRE(replies.size() == 4);
    CHECK(replies[0] == std::vector<u32>{0x00090040, 0});
    CHECK(replies[1] == std::vector<u32>{0x000A0040, 0});
    CHECK(replies[2] == std::vector<u32>{0x000B0040, 0});
    CHECK(replies[3] == std::vector<u32>{0x000C0040, 0});
    CHECK_FALSE(f.runtime->ExitStatus().has_value());
    CHECK_FALSE(f.Logged("unimplemented srv:"));
}

TEST_CASE("srv: PublishToSubscriber of the power-off notification ends the run, as Azahar's does") {
    Fixture f;
    TempDir dir;
    bool ran_on = false;
    const int status = BootWith(f, dir, {"--frames", "100000000"}, [&] {
        Guest g(*f.runtime);
        const Handle srv = ConnectSrv(g);
        g.Request(srv, {0x000C0080, 0x203, 0}, 2);
        for (int i = 0; i < 10000; ++i) {  // the run has to stop by itself
            f.runtime->Scheduler().Sleep(1000000);
        }
        ran_on = true;
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK_FALSE(ran_on);
    REQUIRE(f.runtime->ExitStatus().has_value());
    CHECK(*f.runtime->ExitStatus() == 0);
    // the reply was written before the run stopped
    std::array<u32, 2> words{};
    REQUIRE(f.runtime->Memory().ReadBlock(words.data(),
                                          CommandBufferAddress(*f.runtime->MainThread()), 8));
    CHECK(words[0] == 0x000C0040);
    CHECK(words[1] == 0);
}

TEST_CASE("srv: the commands for servers, and any other, are answered as unknown commands") {
    Fixture f;
    TempDir dir;
    std::vector<u32> unknown, register_service, second_unknown;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle srv = ConnectSrv(g);
        unknown = g.Request(srv, {0x00FF0000}, 2);
        register_service = g.Request(srv, {0x00030100, 0x6d6f6f66, 0, 4, 10}, 2);
        second_unknown = g.Request(srv, {0x00FF0000}, 2);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(unknown == std::vector<u32>{0x00FF0040, 0});
    CHECK(second_unknown == unknown);
    CHECK(register_service == std::vector<u32>{0x00030040, 0});
    CHECK(f.LoggedCount("unimplemented srv: 0x00ff0000") == 1);
    CHECK(f.LoggedCount("unimplemented srv: 0x00030100") == 1);
}

TEST_CASE("srv: sessions are separate, a closed one cannot be used, and 64 can be open at once") {
    Fixture f;
    TempDir dir;
    Handle a = 0, b = 0;
    Result closed = 0, port_full = 0, again = 0;
    std::vector<Handle> many;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        a = ConnectSrv(g);
        b = ConnectSrv(g);
        g.Close(a);
        g.Request(a, {0x00020000}, 1, &closed);
        g.Close(b);
        for (int i = 0; i < 64; ++i) {
            many.push_back(ConnectSrv(g));
        }
        Handle extra = 0;
        port_full = g.ConnectToPort("srv:", &extra);
        g.Close(many.back());
        again = g.ConnectToPort("srv:", &extra);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(a != b);
    CHECK(closed == 0xD8E007F7);
    CHECK(many.size() == 64);
    CHECK(port_full == 0xD0401834);
    CHECK(again == kResultSuccess);
}

// ---- err:f ----------------------------------------------------------------------------------------------------------

namespace {

// The 32 words of ThrowFatalError: the common part (0x20 bytes) and 0x60 bytes by type.
std::vector<u32> ErrWords(u8 type, u32 result, u32 pc, const std::vector<u32>& body = {}) {
    std::vector<u32> words(1 + 32, 0);
    words[0] = ipc::MakeHeader(0x0001, 32, 0);
    words[1] = type | (0x04u << 8) | (0x1234u << 16);  // specifier, rev_high, rev_low
    words[2] = result;
    words[3] = pc;
    words[4] = kApplicationProcessId;
    words[5] = 0x00001234;  // title id low
    words[6] = 0x00040000;  // title id high
    words[7] = 0x0000ABCD;  // application title id low
    words[8] = 0x00040000;  // application title id high
    for (std::size_t i = 0; i < body.size(); ++i) {
        words[9 + i] = body[i];
    }
    return words;
}

}  // namespace

TEST_CASE("err:f ThrowFatalError logs the information and ends the run with status 3") {
    Fixture f;
    TempDir dir;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Handle errf = 0;
        REQUIRE(g.ConnectToPort("err:f", &errf) == kResultSuccess);
        g.Request(errf, ErrWords(0, 0xC8804464, 0x00123456), 2);
        ExitProcess(*f.runtime);  // the run was ended by err:f first: its status counts
    });
    CHECK(status == 3);
    // the reply was written before the run stopped: a success
    std::array<u32, 2> reply{};
    REQUIRE(f.runtime->Memory().ReadBlock(reply.data(),
                                          CommandBufferAddress(*f.runtime->MainThread()), 8));
    CHECK(reply[0] == 0x00010040);
    CHECK(reply[1] == 0);
    REQUIRE(f.runtime->ExitStatus().has_value());
    CHECK(*f.runtime->ExitStatus() == 3);
    CHECK(f.Logged("Fatal error type: Generic"));
    CHECK(f.Logged("PID: 0x0000000B"));
    CHECK(f.Logged("REV: 0x00000004_0x00001234"));
    CHECK(f.Logged("TID: 0x00040000_0x00001234"));
    CHECK(f.Logged("AID: 0x00040000_0x0000ABCD"));
    CHECK(f.Logged("ADR: 0x00123456"));
    CHECK(f.Logged("RSL: 0xC8804464"));
    CHECK(f.Logged("  Level: 25"));
    CHECK(f.Logged("  Summary: 4"));
    CHECK(f.Logged("  Module: 17"));
    CHECK(f.Logged("  Desc: 100"));
    CHECK(f.Logged("Datetime: "));
    CHECK(
        f.Logged("fatal error through err:f: type Generic, result 0xC8804464, address 0x00123456"));
}

TEST_CASE("err:f logs what each type of error carries") {
    struct Case {
        u8 type;
        const char* name;
        std::vector<u32> body;
        std::vector<std::string> logged;
        std::vector<std::string> not_logged;
    };
    // an exception: ExceptionInfo (type, sr, ar, fpexc, fpinst, fpinst2), then r0-r15 and cpsr
    std::vector<u32> exception(6 + 17 + 1, 0);
    exception[0] = 1;                // a data abort
    exception[1] = 0x00000805;       // DFSR
    exception[2] = 0xCAFEF00D;       // DFAR
    exception[6 + 13] = 0x0FFFE000;  // sp
    exception[6 + 14] = 0x00100ABC;  // lr
    exception[6 + 15] = 0x00100DEF;  // pc
    exception[6 + 16] = 0x60000010;  // cpsr
    std::vector<u32> vector_fp = exception;
    vector_fp[0] = 3;
    vector_fp[3] = 0x40000000;  // fpexc
    vector_fp[4] = 0x11111111;  // fpinst
    vector_fp[5] = 0x22222222;  // fpinst2
    std::vector<u32> prefetch = exception;
    prefetch[0] = 0;
    prefetch[1] = 0x00000007;
    prefetch[2] = 0x00100FFC;
    std::vector<u32> undefined = exception;
    undefined[0] = 2;
    // a failure message of 0x60 bytes that does not end with a NUL
    std::vector<u32> message(24, 0x41414141);
    const std::vector<Case> cases = {
        {3,
         "Exception",
         exception,
         {"Fatal error type: Exception", "ARM Registers:", "SP=0x0FFFE000", "LR=0x00100ABC",
          "PC=0x00100DEF", "CPSR=0x60000010", "EXCEPTION TYPE: Data Abort", "DFSR: 0x00000805",
          "DFAR: 0xCAFEF00D", "Datetime: ", "Data Abort at PC=0x00100DEF LR=0x00100ABC"},
         {"IFSR", "FPEXC"}},
        {3,
         "Exception",
         prefetch,
         {"EXCEPTION TYPE: Prefetch Abort", "IFSR: 0x00000007", "r15: 0x00100FFC"},
         {"DFSR"}},
        {3,
         "Exception",
         vector_fp,
         {"EXCEPTION TYPE: Vector Floating Point Exception", "FPEXC: 0x40000000",
          "FINST: 0x11111111", "FINST2: 0x22222222"},
         {"DFSR", "IFSR"}},
        {3,
         "Exception",
         undefined,
         {"EXCEPTION TYPE: Undefined Exception"},
         {"DFSR", "IFSR", "FPEXC"}},
        {4,
         "ResultFailure",
         message,
         {"Fatal error type: ResultFailure", "Failure Message: " + std::string(0x60, 'A'),
          "Datetime: "},
         {"ARM Registers"}},
        {1,
         "Corrupted",
         {},
         {"Fatal error type: Corrupted", "Datetime: "},
         {"ARM Registers", "Failure Message"}},
        {2, "CardRemoved", {}, {"Fatal error type: CardRemoved", "Datetime: "}, {}},
        {5, "Logged", {}, {"Fatal error type: Logged", "Datetime: "}, {}},
        {9,
         "Unknown",
         {},
         {"Fatal error type: Unknown Error Type", "PID: 0x0000000B"},
         {"Datetime"}},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        Fixture f;
        TempDir dir;
        const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
            Guest g(*f.runtime);
            Handle errf = 0;
            REQUIRE(g.ConnectToPort("err:f", &errf) == kResultSuccess);
            g.Request(errf, ErrWords(c.type, 0xC8804464, 0x00123456, c.body), 2);
            ExitProcess(*f.runtime);
        });
        CHECK(status == 3);
        for (const std::string& line : c.logged) {
            INFO("logged: " << line);
            CHECK(f.Logged(line));
        }
        for (const std::string& line : c.not_logged) {
            INFO("not logged: " << line);
            CHECK_FALSE(f.Logged(line));
        }
    }
}

TEST_CASE("err:f has one session at a time and SetUserString is not implemented") {
    Fixture f;
    TempDir dir;
    Result second = 0, third = 0;
    std::vector<u32> set_user_string;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Handle first = 0, h = 0;
        REQUIRE(g.ConnectToPort("err:f", &first) == kResultSuccess);
        second = g.ConnectToPort("err:f", &h);
        set_user_string = g.Request(first, {0x00020042, 1, 2, 3}, 2);
        g.Close(first);
        third = g.ConnectToPort("err:f", &h);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(second == 0xD0401834);
    CHECK(third == kResultSuccess);
    CHECK(set_user_string[0] == 0x00020040);
    CHECK(f.Logged("unimplemented err:f 0x00020042"));
    CHECK_FALSE(f.runtime->ExitStatus().has_value());
}

TEST_CASE("a fatal error through err:f stops the scheduler by itself") {
    // The run stops at the next safe point of the scheduler; the status is the one err:f asked for.
    Fixture f;
    TempDir dir;
    bool ran_on = false;
    const int status = BootWith(f, dir, {"--frames", "100000000"}, [&] {
        Guest g(*f.runtime);
        Handle errf = 0;
        REQUIRE(g.ConnectToPort("err:f", &errf) == kResultSuccess);
        g.Request(errf, ErrWords(0, 0x1, 0x2), 2);
        // nothing here ends the process: the run has to stop by itself (on manual time a sleep costs nothing)
        for (int i = 0; i < 10000; ++i) {
            f.runtime->Scheduler().Sleep(1000000);
        }
        ran_on = true;
        ExitProcess(*f.runtime);
    });
    CHECK(status == 3);
    CHECK_FALSE(ran_on);
}
#endif
