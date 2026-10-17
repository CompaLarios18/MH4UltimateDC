// Derived from Azahar src/core/hle/ipc.h, src/core/hle/ipc_helpers.h, src/core/hle/kernel/hle_ipc.cpp,
// src/core/hle/service/service.cpp, src/core/hle/service/sm/sm.cpp and src/core/hle/kernel/server_session.cpp
// at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"): the expected values of the descriptor encodings (the example headers 0x00050042 and
// 0x00010800 of the srv: and err:f commands), the translation rules the tests check (moved handles
// closed, static and mapped buffers, the calling PID), the answer to an unknown command (the request's
// command id, one normal parameter, the result 0), the limit on a port's sessions and the 39 microsecond
// wait after a reply to an HLE service.
// Copyright 2016 Citra Emulator Project (ipc.h)
// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project (ipc_helpers.h)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (hle_ipc.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (service.cpp)
// Copyright 2017 Citra Emulator Project (sm.cpp)
// Copyright 2016-2024 Citra Emulator Project / Azahar Emulator Project (server_session.cpp)
//
// mh4n_ipc (N1-10): descriptors, requests and replies, sessions and ports, deferred replies and the
// IPC trace. The programs are ELF files written by the test whose main thread runs a C++ function
// (Runtime::SetMainThreadEntry), so no code library is needed; time is a ManualTimeSource.
#include <doctest/doctest.h>

#if !__has_include("recomp.h")
TEST_CASE("ipc_test needs recomp.h (the repository around native/)" * doctest::skip()) {}
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
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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

std::string ReadFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        out.push_back(line);
    }
    return out;
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

// ---- the service the tests talk to ------------------------------------------------------------------------

// Shared with the factory's services: set by a test for the duration of its run.
struct Probe {
    std::mutex mutex;
    std::condition_variable cv;
    std::shared_ptr<Request> pending;  // a request that returned Deferred
    std::vector<u32> seen_words;       // what a handler read (see the commands)
    std::vector<std::string> events;   // the order things happened in
    std::thread::id replied_on;        // the thread an observer saw the reply on
    std::atomic<int> replied{0};
};
Probe* g_probe = nullptr;

class SubService final : public Service {
public:
    explicit SubService(ServiceContext& context) : Service(context, "tst:file") {
        Register(0x0001, "Answer", &SubService::Answer);
    }
    const char* SessionKind() const override { return "file"; }

private:
    void Answer(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(u32{0x1234});
    }
};

// Commands (all with the request words listed in the test that sends them):
//   0x01 Words        a u32, a u64, a u8, a bool and a float come back changed
//   0x02 Handles      objects of three descriptors; hands back the first and new ones
//   0x03 StaticIn     one static buffer: its id, address, size and the sum of its bytes come back
//   0x04 StaticOut    a static buffer of 10 bytes in the client's receive buffer 1
//   0x05 Mapped       two mapped buffers: one read-write (reversed in place), one read-only
//   0x06 Pid          the calling PID in, the calling PID out
//   0x07 Defer        keeps the request and waits for the test to complete it
//   0x08 DeferNow     completes the request before it returns Deferred
//   0x09 DeferCancel  marks the request cancelled, then waits for a Complete
//   0x0A Wrong        declares two normal words and pushes three
//   0x0B StaticBig    a static buffer of 32 bytes in the client's receive buffer 0
//   0x0C NoReply      returns without a reply
//   0x0D Objects      a session of another service and a semaphore
//   0x0E Overflow     pushes more words than a command buffer has
//   0x10 Typed        an event and a semaphore, an event, a null handle: which are of the type asked for
class TestService final : public Service {
public:
    explicit TestService(ServiceContext& context) : Service(context, "tst:ipc") {
        Register(0x0001, "Words", &TestService::Words);
        Register(0x0002, "Handles", &TestService::Handles);
        Register(0x0003, "StaticIn", &TestService::StaticIn);
        Register(0x0004, "StaticOut", &TestService::StaticOut);
        Register(0x0005, "Mapped", &TestService::Mapped);
        Register(0x0006, "Pid", &TestService::Pid);
        Register(0x0007, "Defer", &TestService::Defer);
        Register(0x0008, "DeferNow", &TestService::DeferNow);
        Register(0x0009, "DeferCancel", &TestService::DeferCancel);
        Register(0x000A, "Wrong", &TestService::Wrong);
        Register(0x000B, "StaticBig", &TestService::StaticBig);
        Register(0x000C, "NoReply", &TestService::NoReply);
        Register(0x000D, "Objects", &TestService::Objects);
        Register(0x000E, "Overflow", &TestService::Overflow);
        Register(0x000F, "Known");  // listed, not implemented
        Register(0x0010, "Typed", &TestService::Typed);
    }

private:
    void Words(Request& request) {
        const u32 a = request.Pop<u32>();
        const u64 b = request.Pop<u64>();
        const u8 c = request.Pop<u8>();
        const bool d = request.Pop<bool>();
        const float e = request.Pop<float>();
        Reply& reply = request.MakeReply(7, 0);
        reply.Push(kResultSuccess);
        reply.Push(a + 1);
        reply.Push(b * 2);
        reply.Push(static_cast<u8>(c + 1));
        reply.Push(!d);
        reply.Push(e * 2.0f);
    }

    void Handles(Request& request) {
        // 2 handles, then 2 more (moved), then a null one
        const auto first = request.PopGenericObjects<2>();
        const auto second = request.PopGenericObjects<2>();
        const auto third = request.PopGenericObjects<1>();
        auto type_of = [](const std::shared_ptr<Object>& o) {
            return o ? static_cast<u32>(o->GetHandleType()) : 0xFFFFu;
        };
        Probe* probe = g_probe;
        if (probe != nullptr) {
            probe->seen_words = {type_of(first[0]), type_of(first[1]), type_of(second[0]),
                                 type_of(second[1]), type_of(third[0])};
        }
        std::shared_ptr<Semaphore> semaphore;
        Context().GetKernel().NewSemaphore(&semaphore, 0, 3, "tst");
        Reply& reply = request.MakeReply(1, 5);
        reply.Push(kResultSuccess);
        reply.PushCopyObjects(first[0]);              // 2 words
        reply.PushMoveObjects(semaphore, second[1]);  // 3 words
    }

    void StaticIn(Request& request) {
        const StaticBuffer* buffer = request.PopStaticBuffer();
        REQUIRE(buffer != nullptr);
        u32 sum = 0;
        for (const u8 byte : buffer->data) {
            sum += byte;
        }
        Reply& reply = request.MakeReply(5, 0);
        reply.Push(kResultSuccess);
        reply.Push(buffer->id);
        reply.Push(buffer->address);
        reply.Push(buffer->size);
        reply.Push(sum);
    }

    void StaticOut(Request& request) {
        std::vector<u8> data(10);
        for (u8 i = 0; i < data.size(); ++i) {
            data[i] = static_cast<u8>(0xA0 + i);
        }
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushStaticBuffer(std::move(data), 1);
    }

    void StaticBig(Request& request) {
        std::vector<u8> data(32);
        for (u8 i = 0; i < data.size(); ++i) {
            data[i] = static_cast<u8>(0x40 + i);
        }
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushStaticBuffer(std::move(data), 0);
    }

    void Mapped(Request& request) {
        const MappedBuffer* rw = request.PopMappedBuffer();
        const MappedBuffer* ro = request.PopMappedBuffer();
        REQUIRE(rw != nullptr);
        REQUIRE(ro != nullptr);
        std::vector<u8> bytes(rw->Size());
        const bool read_ok = rw->Read(bytes.data(), 0, rw->Size());
        std::reverse(bytes.begin(), bytes.end());
        const bool write_ok = rw->Write(bytes.data(), 0, rw->Size());
        u8 one = 1;
        const bool ro_write = ro->Write(&one, 0, 1);          // refused: read-only
        const bool past_end = rw->Read(&one, rw->Size(), 1);  // refused: outside the buffer
        Reply& reply = request.MakeReply(5, 4);
        reply.Push(kResultSuccess);
        reply.Push(read_ok);
        reply.Push(write_ok);
        reply.Push(ro_write);
        reply.Push(past_end);
        reply.PushMappedBuffer(*rw);
        reply.PushMappedBuffer(*ro);
    }

    void Pid(Request& request) {
        const u32 pid = request.PopPid();
        Reply& reply = request.MakeReply(2, 2);
        reply.Push(kResultSuccess);
        reply.Push(pid);
        reply.PushPid();
    }

    Handled Defer(Request& request) {
        Probe* probe = g_probe;
        {
            std::lock_guard lock(probe->mutex);
            probe->pending = request.Share();
        }
        probe->cv.notify_all();
        return Handled::Deferred;
    }

    Handled DeferNow(Request& request) {
        Reply reply(request.CommandId(), 2, 0);
        reply.Push(kResultSuccess);
        reply.Push(u32{0xD0D0});
        Complete(request.Share(), std::move(reply));
        return Handled::Deferred;
    }

    Handled DeferCancel(Request& request) {
        Probe* probe = g_probe;
        request.Cancel();
        {
            std::lock_guard lock(probe->mutex);
            probe->pending = request.Share();
        }
        probe->cv.notify_all();
        return Handled::Deferred;
    }

    void Wrong(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(u32{1});
        reply.Push(u32{2});
    }

    void NoReply(Request&) {}

    void Objects(Request& request) {
        auto session = Context().CreateSession(std::make_unique<SubService>(Context()));
        std::shared_ptr<Semaphore> semaphore;
        Context().GetKernel().NewSemaphore(&semaphore, 0, 3, "tst");
        Reply& reply = request.MakeReply(1, 4);
        reply.Push(kResultSuccess);
        reply.PushMoveObjects(session);
        reply.PushCopyObjects(semaphore);
    }

    void Typed(Request& request) {
        const auto [event, semaphore] = request.PopObjects<Event, Semaphore>();
        const auto wrong = request.PopObject<Semaphore>();  // the next descriptor holds an event
        const auto missing = request.PopObject<Event>();    // and the last one a null handle
        Reply& reply = request.MakeReply(5, 0);
        reply.Push(kResultSuccess);
        reply.Push(event != nullptr);
        reply.Push(semaphore != nullptr);
        reply.Push(wrong != nullptr);
        reply.Push(missing != nullptr);
    }

    void Overflow(Request& request) {
        Reply& reply = request.MakeReply(1, 0);
        for (int i = 0; i < 80; ++i) {
            reply.Push(u32{7});
        }
    }
};

ServiceFactory MakeTestFactory(u32 max_sessions = 3) {
    return ServiceFactory::NamedPort(
        [](ServiceContext& c) { return std::make_unique<TestService>(c); }, max_sessions);
}
// A named port from the registry; the tests add "tst:port" themselves with their own limit.
MH4N_REGISTER(ServiceFactory, "tst:reg", MakeTestFactory(3));

// A pointer to the service manager of the running fixture.
ServiceManager& Manager(Fixture& f) {
    ServiceManager* manager = FindServiceManager(*f.runtime);
    REQUIRE(manager != nullptr);
    return *manager;
}

// Connects to "tst:reg".
Handle ConnectTest(Guest& g) {
    Handle h = 0;
    REQUIRE(g.ConnectToPort("tst:reg", &h) == kResultSuccess);
    REQUIRE(h != 0);
    return h;
}

}  // namespace

// ---- descriptors ---------------------------------------------------------------------------------------

TEST_CASE("headers and descriptors encode and decode as the 3DS (and Azahar) do") {
    using namespace ipc;
    // the examples of the srv: and err:f commands
    CHECK(MakeHeader(0x0005, 1, 2) == 0x00050042);
    CHECK(MakeHeader(0x0001, 0x20, 0) == 0x00010800);
    CHECK(MakeHeader(0x0001, 0, 2) == 0x00010002);
    const Header header{0x00050142};
    CHECK(header.CommandId() == 0x0005);
    CHECK(header.NormalParamsSize() == 5);
    CHECK(header.TranslateParamsSize() == 2);
    CHECK(MakeHeader(0xFFFF, 63, 63) == 0xFFFF0FFF);

    // handles: the count minus one is in the top six bits
    CHECK(CopyHandleDesc() == 0x00000000);
    CHECK(MoveHandleDesc() == 0x00000010);
    CHECK(CopyHandleDesc(3) == 0x08000000);
    CHECK(MoveHandleDesc(2) == 0x04000010);
    CHECK(HandleNumberFromDesc(0x00000000) == 1);
    CHECK(HandleNumberFromDesc(0x08000000) == 3);
    CHECK(HandleNumberFromDesc(MoveHandleDesc(64)) == 64);
    CHECK(CallingPidDesc() == 0x20);
    CHECK(IsHandleDescriptor(0x04000010));
    CHECK(IsHandleDescriptor(0x20));
    CHECK_FALSE(IsHandleDescriptor(0x00400802));

    // static buffers: id in bits 10-13, size from bit 14
    CHECK(StaticBufferDesc(0x100, 2) == 0x00400802);
    CHECK(StaticBufferDescId(0x00400802) == 2);
    CHECK(StaticBufferDescSize(0x00400802) == 0x100);
    CHECK(StaticBufferDesc(0x3FFFF, 15) == 0xFFFFFC02 + 0x0);
    CHECK(StaticBufferDescSize(StaticBufferDesc(0x3FFFF, 15)) == 0x3FFFF);

    // mapped buffers: the permissions in bits 1-2, size from bit 4
    CHECK(MappedBufferDesc(0x1000, RW) == 0x0001000E);
    CHECK(MappedBufferDesc(0x20, R) == 0x0000020A);
    CHECK(MappedBufferDesc(0x20, W) == 0x0000020C);
    CHECK(MappedBufferDescPerms(0x0001000E) == RW);
    CHECK(MappedBufferDescPerms(0x0000020A) == R);
    CHECK(MappedBufferDescSize(0x0001000E) == 0x1000);

    // PXI buffers: read-only adds 2, the id is in bits 4-7, the size from bit 8
    CHECK(PXIBufferDesc(0x40, 3, true) == 0x00004036);
    CHECK(PXIBufferDesc(0x40, 3, false) == 0x00004034);
    CHECK(PXIBufferDescSize(0x00004036) == 0x40);

    // the order of the checks in GetDescriptorType
    CHECK(GetDescriptorType(0x00000000) == CopyHandle);
    CHECK(GetDescriptorType(0x08000000) == CopyHandle);
    CHECK(GetDescriptorType(0x00000010) == MoveHandle);
    CHECK(GetDescriptorType(0x04000010) == MoveHandle);
    CHECK(GetDescriptorType(0x00000020) == CallingPid);
    CHECK(GetDescriptorType(0x00400802) == ipc::StaticBuffer);
    CHECK(GetDescriptorType(0x0001000E) == ipc::MappedBuffer);
    CHECK(GetDescriptorType(0x00004036) == PXIBuffer);
    CHECK(static_cast<u32>(GetDescriptorType(0x00000030)) == 0x30);  // no such type
    static_assert(kCommandBufferLength == 0x40 && kCommandAreaLength == 0x60);
    static_assert(StaticBufferDescriptorWord(0) == 0x40 && StaticBufferDescriptorWord(15) == 0x5E);
}

TEST_CASE("a Reply is built word by word, the sizes are checked, and what it holds is kept apart") {
    Fixture f;
    {
        Reply reply(0x1234, 9, 0);
        reply.Push(kResultSuccess);
        reply.Push(u32{0x11223344});
        reply.Push(u64{0x0102030405060708});
        reply.Push(u8{0xAB});
        reply.Push(u16{0xCDEF});
        reply.Push(true);
        reply.Push(1.5f);
        reply.Push(s32{-2});
        // 3 more with the two-argument form
        CHECK(reply.HeaderWord() == 0x12340240);
        CHECK(reply.CommandSize() == 10);
        const auto& w = reply.Words();
        CHECK(w[1] == 0);
        CHECK(w[2] == 0x11223344);
        CHECK(w[3] == 0x05060708);  // 64-bit values: low word first
        CHECK(w[4] == 0x01020304);
        CHECK(w[5] == 0x000000AB);  // the bytes above a small value are zero
        CHECK(w[6] == 0x0000CDEF);
        CHECK(w[7] == 1);
        CHECK(w[8] == 0x3FC00000);
        CHECK(w[9] == 0xFFFFFFFE);
        CHECK(reply.Validate());
        CHECK_FALSE(f.Logged("declares"));
    }
    {
        Reply reply(0x0001, 3, 0);
        reply.Push(kResultSuccess, u32{1});  // one word short
        CHECK_FALSE(reply.Validate());
        CHECK(f.Logged("declares 4 words and 3 were pushed"));
    }
    {
        struct Six {
            u8 bytes[6];
        };
        Reply reply(0x0001, 4, 0);
        reply.Push(kResultSuccess);
        reply.PushRaw(Six{{1, 2, 3, 4, 5, 6}});  // two words, the rest zero
        reply.Push(u32{9});
        const auto& w = reply.Words();
        CHECK(w[2] == 0x04030201);
        CHECK(w[3] == 0x00000605);
        CHECK(w[4] == 9);
        CHECK(reply.Validate());
    }
    {
        // handles: one descriptor for all the objects of a push, the words are indices
        Reply reply(0x0001, 1, 4);
        reply.Push(kResultSuccess);
        reply.PushMoveObjects(std::shared_ptr<Object>(), std::shared_ptr<Object>());
        reply.PushCopyObjects(std::shared_ptr<Object>());
        const auto& w = reply.Words();
        CHECK(w[2] == ipc::MoveHandleDesc(2));
        CHECK(w[3] == 0);
        CHECK(w[4] == 1);
        CHECK(w[5] == ipc::CopyHandleDesc(1));
        CHECK(w[6] == 2);
        CHECK(reply.Objects().size() == 3);
        CHECK_FALSE(reply.Validate());  // 7 words for a declared 6
    }
    {
        Reply reply(0x0001, 1, 2);
        reply.Push(kResultSuccess);
        reply.PushStaticBuffer({1, 2, 3}, 4);
        CHECK(reply.Words()[2] == ipc::StaticBufferDesc(3, 4));
        CHECK(reply.Words()[3] == 0xDEADC0DE);  // replaced when the reply is written
        REQUIRE(reply.StaticData(4).has_value());
        CHECK(reply.StaticData(4)->size() == 3);
        CHECK_FALSE(reply.StaticData(5).has_value());
        CHECK(reply.Validate());
    }
}

// ---- requests, replies and translation ---------------------------------------------------------------------------

TEST_CASE("normal parameters of every type come out of a request and back into a reply") {
    Fixture f;
    TempDir dir;
    std::vector<u32> reply;
    Result result = 0;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle session = ConnectTest(g);
        const u64 big = 0x0000000200000003ull;
        const float x = 2.25f;
        u32 xbits;
        std::memcpy(&xbits, &x, 4);
        // a u32, a u64 (low word first), a u8 in the low byte of a word, a bool (low byte 0), a float
        reply = g.Request(session,
                          {ipc::MakeHeader(0x0001, 6, 0), 41, static_cast<u32>(big),
                           static_cast<u32>(big >> 32), 0x1234FF, 0xFFFFFF00, xbits},
                          8, &result);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(result == kResultSuccess);
    // the scheduler counts the time in the service by its name
    const auto& services = f.runtime->Scheduler().Counters().services;
    REQUIRE(services.find("tst:ipc") != services.end());
    CHECK(services.find("tst:ipc")->second.calls == 1);
    REQUIRE(reply.size() == 8);
    CHECK(reply[0] == ipc::MakeHeader(0x0001, 7, 0));  // the reply keeps the request's command id
    CHECK(reply[1] == kResultSuccess);
    CHECK(reply[2] == 42);          // u32 + 1
    CHECK(reply[3] == 6);           // u64 * 2: low word,
    CHECK(reply[4] == 4);           // high word
    CHECK(reply[5] == 0x00000000);  // the u8 0xFF + 1 wraps to 0; the bytes above it are zero
    CHECK(reply[6] == 1);           // !bool: the low byte was 0, so false became true
    CHECK(reply[7] == 0x40900000);  // 2.25f * 2 = 4.5f
}

TEST_CASE("a handle descriptor with several handles: copies stay, every moved handle is closed") {
    Fixture f;
    TempDir dir;
    Probe probe;
    g_probe = &probe;
    std::vector<u32> reply;
    bool copied_open[2] = {false, false};
    bool moved_open[2] = {true, true};
    std::vector<u32> client_handles;
    Handle first_event = 0;
    Result invalid_result = 0;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle session = ConnectTest(g);
        const Handle e1 = g.NewEvent(), e2 = g.NewEvent(), e3 = g.NewEvent(), e4 = g.NewEvent();
        // the 3 descriptors: 2 copied, 2 moved, one null copied
        reply = g.Request(session,
                          {ipc::MakeHeader(0x0002, 0, 8), ipc::CopyHandleDesc(2), e1, e2,
                           ipc::MoveHandleDesc(2), e3, e4, ipc::CopyHandleDesc(1), 0},
                          7);
        copied_open[0] = g.Wait(e1, 0) == kTimeout;  // an open, unsignalled event times out
        copied_open[1] = g.Wait(e2, 0) == kTimeout;
        moved_open[0] = g.Wait(e3, 0) != 0xD8E007F7;
        moved_open[1] = g.Wait(e4, 0) != 0xD8E007F7;
        // a handle that is not in the table
        g.Request(session, {ipc::MakeHeader(0x0002, 0, 2), ipc::CopyHandleDesc(1), 0x12345678}, 1,
                  &invalid_result);
        client_handles = {e1, e2, e3, e4};
        first_event = e1;
        ExitProcess(*f.runtime);
    });
    g_probe = nullptr;
    CHECK(status == 0);
    // the service saw: copy, copy, move, move, null
    const u32 event_type = static_cast<u32>(HandleType::Event);
    REQUIRE(probe.seen_words.size() == 5);
    CHECK(probe.seen_words[0] == event_type);
    CHECK(probe.seen_words[1] == event_type);
    CHECK(probe.seen_words[2] == event_type);
    CHECK(probe.seen_words[3] == event_type);
    CHECK(probe.seen_words[4] == 0xFFFF);
    CHECK(copied_open[0]);
    CHECK(copied_open[1]);
    // both handles of the move descriptor are closed (Azahar closes only a descriptor with one: it
    // compares the whole word with MoveHandle)
    CHECK_FALSE(moved_open[0]);
    CHECK_FALSE(moved_open[1]);
    // the reply: header, result, copy desc + 1 handle, move desc + 2 handles = 7 words
    REQUIRE(reply.size() == 7);
    CHECK(reply[0] == ipc::MakeHeader(0x0002, 1, 5));
    CHECK(reply[2] == ipc::CopyHandleDesc(1));
    CHECK(reply[3] != 0);
    CHECK(reply[3] != first_event);  // the same object, under a new handle
    CHECK(reply[4] == ipc::MoveHandleDesc(2));
    CHECK(reply[5] != 0);
    CHECK(reply[6] != 0);
    // a handle the client does not have stops the request
    CHECK(invalid_result == 0xD8E007F7);
}

TEST_CASE("objects are cast to the type a service asks for, and null if it is another one") {
    Fixture f;
    TempDir dir;
    std::vector<u32> reply;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle session = ConnectTest(g);
        const Handle event = g.NewEvent();
        const Handle event2 = g.NewEvent();
        const Handle semaphore =
            CallSvc(*f.runtime, 0x15, {0, 0, 1}).r[1];  // svcCreateSemaphore: initial 0, maximum 1
        REQUIRE(semaphore != 0);
        reply = g.Request(session,
                          {ipc::MakeHeader(0x0010, 0, 7), ipc::CopyHandleDesc(2), event, semaphore,
                           ipc::CopyHandleDesc(1), event2, ipc::CopyHandleDesc(1), 0},
                          6);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    REQUIRE(reply.size() == 6);
    CHECK(reply[1] == kResultSuccess);
    CHECK(reply[2] == 1);  // an event where an event was asked for
    CHECK(reply[3] == 1);  // a semaphore where a semaphore was asked for
    CHECK(reply[4] == 0);  // an event where a semaphore was asked for
    CHECK(reply[5] == 0);  // no object at all
}

TEST_CASE("the sessions of a service share the state the context keeps for it") {
    Fixture f;
    TempDir dir;
    std::shared_ptr<int> first, again, other;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        ServiceContext& context = Manager(f).Context();
        first = context.State<int>("tst:state");
        *first = 41;
        again = context.State<int>("tst:state");
        other = context.State<int>("tst:other");
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(first == again);
    CHECK(*again == 41);
    CHECK(other != first);
    CHECK(*other == 0);
}

TEST_CASE(
    "a static buffer is copied out of the client and a static reply goes into its receive buffer") {
    Fixture f;
    TempDir dir;
    std::vector<u32> in_reply, out_reply, empty_reply;
    std::vector<u8> received;
    u32 receive = 0, in_address = 0;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle session = ConnectTest(g);
        in_address = g.Scratch(0x100);
        // StaticIn: 6 bytes at scratch+0x100, buffer id 2
        g.PutBytes(g.Scratch(0x100), {1, 2, 3, 4, 5, 6});
        in_reply = g.Request(
            session, {ipc::MakeHeader(0x0003, 0, 2), ipc::StaticBufferDesc(6, 2), g.Scratch(0x100)},
            6);
        // StaticOut: the client's receive buffer 1 (16 bytes) is described after the command buffer
        receive = g.Scratch(0x200);
        g.PutBytes(receive, std::vector<u8>(16, 0xEE));
        g.PutWords(g.Cmd() + ipc::StaticBufferDescriptorWord(1) * 4,
                   {ipc::StaticBufferDesc(16, 1), receive});
        out_reply = g.Request(session, {ipc::MakeHeader(0x0004, 0, 0)}, 4);
        received = g.GetBytes(receive, 16);
        // an empty static buffer at the address 0 is fine: nothing to read
        empty_reply =
            g.Request(session, {ipc::MakeHeader(0x0003, 0, 2), ipc::StaticBufferDesc(0, 3), 0}, 6);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    REQUIRE(in_reply.size() == 6);
    CHECK(in_reply[0] == ipc::MakeHeader(0x0003, 5, 0));
    CHECK(in_reply[1] == kResultSuccess);
    CHECK(in_reply[2] == 2);           // the buffer id
    CHECK(in_reply[4] == 6);           // the size
    CHECK(in_reply[5] == 21);          // 1 + 2 + 3 + 4 + 5 + 6
    CHECK(in_reply[3] == in_address);  // the address the client gave
    REQUIRE(out_reply.size() == 4);
    CHECK(out_reply[0] == ipc::MakeHeader(0x0004, 1, 2));
    CHECK(out_reply[1] == kResultSuccess);
    CHECK(out_reply[2] == ipc::StaticBufferDesc(10, 1));
    CHECK(out_reply[3] == receive);  // the address word is the receive buffer's
    for (u32 i = 0; i < 10; ++i) {
        CHECK(received[i] == 0xA0 + i);
    }
    for (u32 i = 10; i < 16; ++i) {
        CHECK(received[i] == 0xEE);  // only the bytes of the reply are written
    }
    REQUIRE(empty_reply.size() == 6);
    CHECK(empty_reply[2] == 3);
    CHECK(empty_reply[3] == 0);
    CHECK(empty_reply[4] == 0);
    CHECK(empty_reply[5] == 0);
    CHECK_FALSE(f.Logged("is not mapped"));
}

TEST_CASE("a static reply bigger than the client's receive buffer is written short, and logged") {
    Fixture f;
    TempDir dir;
    std::vector<u8> received;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle session = ConnectTest(g);
        const u32 receive = g.Scratch(0x300);
        g.PutBytes(receive, std::vector<u8>(40, 0xEE));
        g.PutWords(g.Cmd() + ipc::StaticBufferDescriptorWord(0) * 4,
                   {ipc::StaticBufferDesc(16, 0), receive});
        g.Request(session, {ipc::MakeHeader(0x000B, 0, 0)}, 4);
        received = g.GetBytes(receive, 40);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    for (u32 i = 0; i < 16; ++i) {
        CHECK(received[i] == 0x40 + i);
    }
    for (u32 i = 16; i < 40; ++i) {
        CHECK(received[i] == 0xEE);
    }
    CHECK(f.Logged("static buffer 0 has 32 bytes and the client's receive buffer 16"));
}

TEST_CASE("mapped buffers stay in the client's memory, behind their permissions and range") {
    Fixture f;
    TempDir dir;
    std::vector<u32> reply;
    std::vector<u8> rw_after, ro_after;
    u32 rw_address = 0, ro_address = 0;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle session = ConnectTest(g);
        rw_address = g.Scratch(0x400);
        ro_address = g.Scratch(0x500);
        g.PutBytes(rw_address, {1, 2, 3, 4, 5, 6, 7, 8});
        g.PutBytes(ro_address, {9, 9, 9, 9});
        reply = g.Request(session,
                          {ipc::MakeHeader(0x0005, 0, 4), ipc::MappedBufferDesc(8, ipc::RW),
                           rw_address, ipc::MappedBufferDesc(4, ipc::R), ro_address},
                          10);
        rw_after = g.GetBytes(rw_address, 8);
        ro_after = g.GetBytes(ro_address, 4);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    REQUIRE(reply.size() == 10);
    CHECK(reply[0] == ipc::MakeHeader(0x0005, 5, 4));
    CHECK(reply[1] == kResultSuccess);
    CHECK(reply[2] == 1);  // the read-write buffer was read
    CHECK(reply[3] == 1);  // and written
    CHECK(reply[4] == 0);  // a write to the read-only one is refused
    CHECK(reply[5] == 0);  // a read past the end is refused
    CHECK(reply[6] == ipc::MappedBufferDesc(8, ipc::RW));  // the descriptors come back
    CHECK(reply[7] == rw_address);                         // with the client's own addresses
    CHECK(reply[8] == ipc::MappedBufferDesc(4, ipc::R));
    CHECK(reply[9] == ro_address);
    CHECK(rw_after == std::vector<u8>{8, 7, 6, 5, 4, 3, 2, 1});
    CHECK(ro_after == std::vector<u8>{9, 9, 9, 9});
    CHECK(f.Logged("no write permission"));
    CHECK(f.Logged("is outside mapped buffer"));
}

TEST_CASE("the calling PID is filled in, in requests and in replies") {
    Fixture f;
    TempDir dir;
    std::vector<u32> reply;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle session = ConnectTest(g);
        reply = g.Request(session, {ipc::MakeHeader(0x0006, 0, 2), ipc::CallingPidDesc(), 0}, 5);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    REQUIRE(reply.size() == 5);
    CHECK(reply[0] == ipc::MakeHeader(0x0006, 2, 2));
    CHECK(reply[2] == kApplicationProcessId);  // what the service read
    CHECK(reply[3] == ipc::CallingPidDesc());
    CHECK(reply[4] == kApplicationProcessId);
}

TEST_CASE("an unknown command is logged once and answered with the command id, one word and the "
          "result 0") {
    Fixture f;
    TempDir dir;
    std::vector<u32> first, second, known, other_header;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle session = ConnectTest(g);
        first = g.Request(session, {0x00FF0040, 7}, 3);
        second = g.Request(session, {0x00FF0040, 7}, 3);
        known = g.Request(session, {0x000F0000}, 3);         // registered without a handler
        other_header = g.Request(session, {0x00FF0000}, 3);  // same command, another header
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    for (const auto* reply : {&first, &second}) {
        REQUIRE(reply->size() == 3);
        CHECK((*reply)[0] == 0x00FF0040);
        CHECK((*reply)[1] == 0);
    }
    CHECK(known[0] == 0x000F0040);
    CHECK(known[1] == 0);
    CHECK(other_header[0] == 0x00FF0040);
    CHECK(f.LoggedCount("unimplemented tst:ipc 0x00ff0040") == 1);  // once per header
    CHECK(f.LoggedCount("unimplemented tst:ipc 0x000f0000") == 1);
    CHECK(f.LoggedCount("unimplemented tst:ipc 0x00ff0000") == 1);
    CHECK(f.Logged("Known"));  // the name of a command that is listed
}

TEST_CASE("a malformed command makes svcSendSyncRequest fail and the thread keeps running") {
    Fixture f;
    TempDir dir;
    Result too_long = 0, bad_descriptor = 0, cut_off = 0, ok = 0;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle session = ConnectTest(g);
        g.Request(session, {ipc::MakeHeader(0x0001, 63, 63)}, 1, &too_long);
        g.Request(session, {ipc::MakeHeader(0x0001, 0, 2), 0x00000030, 0}, 1, &bad_descriptor);
        g.Request(session, {ipc::MakeHeader(0x0001, 0, 2), ipc::CopyHandleDesc(5), 0}, 1, &cut_off);
        g.Request(session, {ipc::MakeHeader(0x000F, 0, 0)}, 1, &ok);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(too_long == 0xD9001830);
    CHECK(bad_descriptor == 0xD9001830);
    CHECK(cut_off == 0xD9001830);
    CHECK(ok == kResultSuccess);
}

TEST_CASE(
    "a handler that makes no reply, a reply of the wrong size and one that overflows are logged") {
    Fixture f;
    TempDir dir;
    std::vector<u32> no_reply, wrong, overflow;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle session = ConnectTest(g);
        no_reply = g.Request(session, {ipc::MakeHeader(0x000C, 0, 0)}, 2);
        wrong = g.Request(session, {ipc::MakeHeader(0x000A, 0, 0)}, 3);
        overflow = g.Request(session, {ipc::MakeHeader(0x000E, 0, 0)}, 2);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(no_reply[0] == ipc::MakeHeader(0x000C, 1, 0));
    CHECK(no_reply[1] == 0);
    CHECK(f.Logged("the handler returned without a reply"));
    CHECK(wrong[0] == ipc::MakeHeader(0x000A, 2, 0));
    CHECK(wrong[1] == 0);
    CHECK(wrong[2] == 1);  // the words that fit
    CHECK(f.Logged("declares 3 words and 4 were pushed"));
    CHECK(overflow[0] == ipc::MakeHeader(0x000E, 1, 0));
    CHECK(f.Logged("more than 64 words pushed"));
}

// ---- ports and sessions ------------------------------------------------------------------------------------------

TEST_CASE(
    "a port refuses more sessions than it allows, and gives the slot back when one is closed") {
    Fixture f;
    TempDir dir;
    Result fourth = 0, after_close = 0, unknown = 0, too_long = 0, empty = 0;
    Handle sessions[3] = {0, 0, 0};
    Handle again = 0;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        for (Handle& s : sessions) {
            s = ConnectTest(g);
        }
        Handle h = 0;
        fourth = g.ConnectToPort("tst:reg", &h);
        g.Close(sessions[1]);
        after_close = g.ConnectToPort("tst:reg", &again);
        unknown = g.ConnectToPort("tst:none", &h);
        too_long = g.ConnectToPort("twelve:chars", &h);
        empty = g.ConnectToPort("", &h);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(sessions[0] != sessions[1]);
    CHECK(sessions[1] != sessions[2]);
    CHECK(fourth == 0xD0401834);
    CHECK(after_close == kResultSuccess);
    CHECK(again != 0);
    CHECK(unknown == 0xD88007FA);
    CHECK(too_long == 0xE0E0181E);  // the SVC layer's check, before the manager
    CHECK(empty == 0xD88007FA);
}

TEST_CASE("the manager registers ports and services by name, with Azahar's checks") {
    Fixture f;
    TempDir dir;
    std::vector<Result> results;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        ServiceManager& m = Manager(f);
        results.push_back(m.RegisterService("tst:svc", MakeTestFactory()));    // ok
        results.push_back(m.RegisterService("tst:svc", MakeTestFactory()));    // taken
        results.push_back(m.RegisterService("", MakeTestFactory()));           // empty
        results.push_back(m.RegisterService("ninechars", MakeTestFactory()));  // 9 characters
        results.push_back(m.RegisterService(std::string("ab\0cd", 5), MakeTestFactory()));  // a NUL
        results.push_back(m.RegisterService("tst:nof", ServiceFactory()));      // no factory
        results.push_back(m.AddNamedPort("tst:long:port", MakeTestFactory()));  // 13 characters
        results.push_back(m.AddNamedPort("tst:port", MakeTestFactory()));       // ok
        results.push_back(m.AddNamedPort("tst:port", MakeTestFactory()));       // taken
        // a service is not a port and a port is not a service
        std::shared_ptr<Object> port_session;
        std::shared_ptr<ClientSession> service_session;
        results.push_back(m.ConnectToPort("tst:svc", &port_session));
        results.push_back(m.ConnectToService("tst:port", &service_session));
        results.push_back(m.ConnectToService("tst:svc", &service_session));
        results.push_back(m.ConnectToService("ninechars", &service_session));
        results.push_back(m.ConnectToService(std::string("a\0b", 3), &service_session));
        CHECK(m.IsServiceRegistered("tst:svc"));
        CHECK_FALSE(m.IsServiceRegistered("tst:port"));
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    REQUIRE(results.size() == 14);
    CHECK(results[0] == kResultSuccess);
    CHECK(results[1] == kResultAlreadyRegistered);
    CHECK(results[2] == kResultSrvInvalidNameSize);
    CHECK(results[3] == kResultSrvInvalidNameSize);
    CHECK(results[4] == kResultSrvNameContainsNul);
    CHECK(results[5] == kResultSrvAccessDenied);
    CHECK(results[6] == kResultSrvInvalidNameSize);
    CHECK(results[7] == kResultSuccess);
    CHECK(results[8] == kResultAlreadyRegistered);
    CHECK(results[9] == kResultUnknownPort);
    CHECK(results[10] == kResultSrvServiceNotRegistered);
    CHECK(results[11] == kResultSuccess);
    CHECK(results[12] == kResultSrvInvalidNameSize);
    CHECK(results[13] == kResultSrvNameContainsNul);
}

TEST_CASE("every factory in the registry is installed: named ones as ports, the rest as services") {
    Fixture f;
    TempDir dir;
    bool srv = false, errf = false, reg = false, srv_as_service = false;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Handle h = 0;
        srv = g.ConnectToPort("srv:", &h) == kResultSuccess;
        errf = g.ConnectToPort("err:f", &h) == kResultSuccess;
        reg = g.ConnectToPort("tst:reg", &h) == kResultSuccess;
        srv_as_service = Manager(f).IsServiceRegistered("srv:");
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(srv);
    CHECK(errf);
    CHECK(reg);
    CHECK_FALSE(srv_as_service);
}

TEST_CASE("svcSendSyncRequest on something that is not a session answers the invalid handle") {
    Fixture f;
    TempDir dir;
    Result not_a_session = 0, closed = 0, zero = 0;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle event = g.NewEvent();
        not_a_session = g.Send(event);
        const Handle session = ConnectTest(g);
        g.Close(session);
        closed = g.Send(session);
        zero = g.Send(0);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(not_a_session == 0xD8E007F7);
    CHECK(closed == 0xD8E007F7);
    CHECK(zero == 0xD8E007F7);
}

TEST_CASE(
    "sessions a service hands out are sessions of that service, closed with their last handle") {
    Fixture f;
    TempDir dir;
    std::vector<u32> objects_reply, answer;
    Handle sub = 0;
    Result send_sub = 0, closed = 0;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle session = ConnectTest(g);
        objects_reply = g.Request(session, {ipc::MakeHeader(0x000D, 0, 0)}, 6);
        sub = objects_reply[3];
        answer = g.Request(sub, {ipc::MakeHeader(0x0001, 0, 0)}, 3, &send_sub);
        g.Close(sub);
        g.Request(sub, {ipc::MakeHeader(0x0001, 0, 0)}, 1, &closed);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    REQUIRE(objects_reply.size() == 6);
    CHECK(objects_reply[0] == ipc::MakeHeader(0x000D, 1, 4));
    CHECK(objects_reply[2] == ipc::MoveHandleDesc(1));
    CHECK(objects_reply[4] == ipc::CopyHandleDesc(1));
    CHECK(send_sub == kResultSuccess);
    CHECK(answer[1] == 0);
    CHECK(answer[2] == 0x1234);
    CHECK(closed == 0xD8E007F7);
}

TEST_CASE(
    "the client waits 39 microseconds of game time after a reply, unless the delay is turned off") {
    Fixture f;
    TempDir dir;
    u64 with_delay = 0, without = 0;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle session = ConnectTest(g);
        u64 before = f.runtime->Clock().Now();
        g.Request(session, {ipc::MakeHeader(0x000F, 0, 0)}, 2);
        with_delay = f.runtime->Clock().Now() - before;
        Manager(f).SetReplyDelayNs(0);
        before = f.runtime->Clock().Now();
        g.Request(session, {ipc::MakeHeader(0x000F, 0, 0)}, 2);
        without = f.runtime->Clock().Now() - before;
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(with_delay + 2 >= NsToTicks(kIpcReplyDelayNs));
    CHECK(with_delay <= NsToTicks(kIpcReplyDelayNs) + 2);
    CHECK(without == 0);
    CHECK(kIpcReplyDelayNs == 39000);
}

// ---- deferred replies ------------------------------------------------------------------------------------------------

TEST_CASE(
    "a deferred reply completed from another host thread is written on the scheduler thread") {
    Fixture f;
    TempDir dir;
    Probe probe;
    g_probe = &probe;
    const std::thread::id scheduler_thread = std::this_thread::get_id();
    std::vector<u32> reply;
    std::thread worker([&] {
        std::shared_ptr<Request> request;
        {
            std::unique_lock lock(probe.mutex);
            if (!probe.cv.wait_for(lock, std::chrono::seconds(30),
                                   [&] { return probe.pending != nullptr; })) {
                return;
            }
            request = probe.pending;
        }
        // Built on this thread, delivered through the kernel's queue.
        Reply answer(request->CommandId(), 3, 0);
        answer.Push(kResultSuccess);
        answer.Push(u32{0xCAFE});
        answer.Push(u32{0xBEEF});
        Complete(request, std::move(answer));
    });
    // No time limit: the scheduler idles until the worker posts the reply.
    const int status = BootWith(f, dir, {"--frames", "100000000"}, [&] {
        Guest g(*f.runtime);
        Manager(f).AddObserver(IpcObserver{nullptr, [&](const Request&, std::span<const u32>) {
                                               probe.replied_on = std::this_thread::get_id();
                                               ++probe.replied;
                                           }});
        const Handle session = ConnectTest(g);
        reply = g.Request(session, {ipc::MakeHeader(0x0007, 0, 0)}, 4);
        ExitProcess(*f.runtime);
    });
    worker.join();
    g_probe = nullptr;
    CHECK(status == 0);
    REQUIRE(reply.size() == 4);
    CHECK(reply[0] == ipc::MakeHeader(0x0007, 3, 0));
    CHECK(reply[1] == kResultSuccess);
    CHECK(reply[2] == 0xCAFE);
    CHECK(reply[3] == 0xBEEF);
    CHECK(probe.replied == 1);
    CHECK(probe.replied_on == scheduler_thread);
}

TEST_CASE("a handler that completes the request before it returns Deferred still gets its reply") {
    Fixture f;
    TempDir dir;
    std::vector<u32> reply;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle session = ConnectTest(g);
        reply = g.Request(session, {ipc::MakeHeader(0x0008, 0, 0)}, 3);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    REQUIRE(reply.size() == 3);
    CHECK(reply[0] == ipc::MakeHeader(0x0008, 2, 0));
    CHECK(reply[1] == kResultSuccess);
    CHECK(reply[2] == 0xD0D0);
}

TEST_CASE(
    "a reply to a request whose client stopped waiting is dropped, and a second one is refused") {
    Fixture f;
    TempDir dir;
    Probe probe;
    g_probe = &probe;
    std::atomic<int> replied{0};
    u32 command_address = 0;
    const int status = BootWith(f, dir, {"--ms", "50"}, [&] {
        Guest g(*f.runtime);
        Manager(f).AddObserver(
            IpcObserver{nullptr, [&](const Request&, std::span<const u32>) { ++replied; }});
        const Handle session = ConnectTest(g);
        command_address = g.Cmd();
        // The completion is scheduled before the request is sent; it runs while the client waits.
        Kernel& kernel = f.runtime->GetKernel();
        kernel.ScheduleAt(kernel.NowTicks() + NsToTicks(1000000), [&probe] {
            Reply answer(0x0009, 1, 0);
            answer.Push(kResultSuccess);
            Complete(probe.pending, std::move(answer));
        });
        // never returns: nobody wakes the client
        g.Request(session, {ipc::MakeHeader(0x0009, 1, 0), 0x77}, 2);
        ExitProcess(*f.runtime);
    });
    g_probe = nullptr;
    CHECK(status == 0);  // the time limit
    REQUIRE(probe.pending != nullptr);
    CHECK(probe.pending->Finished());
    CHECK(replied == 0);
    // the client's command buffer still holds its request: nothing was written
    std::array<u32, 2> words{};
    REQUIRE(f.runtime->Memory().ReadBlock(words.data(), command_address, 8));
    CHECK(words[0] == ipc::MakeHeader(0x0009, 1, 0));
    CHECK(words[1] == 0x77);
}

TEST_CASE("a client thread that ends while it waits for a deferred reply cancels its request") {
    Fixture f;
    TempDir dir;
    Probe probe;
    g_probe = &probe;
    std::atomic<int> replied{0};
    bool client_resumed = false;
    bool cancelled = false, finished = false;
    u32 client_command = 0;
    std::array<u32, 1> client_words{};
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        ServiceManager& manager = Manager(f);
        manager.AddObserver(
            IpcObserver{nullptr, [&](const Request&, std::span<const u32>) { ++replied; }});
        const Handle session = ConnectTest(g);
        // A second guest thread with a command buffer of its own (the thread manager makes the real
        // ones; a page of the test's will do).
        Kernel& kernel = f.runtime->GetKernel();
        GuestScheduler& scheduler = f.runtime->Scheduler();
        REQUIRE(IsSuccess(
            f.runtime->Memory().Map(0x08200000, kPageSize, MemPerm::ReadWrite, MemState::Private)));
        const std::shared_ptr<Thread> client =
            kernel.NewThread("client", 48, 0, kernel.CurrentProcess());
        client->tls_address = 0x08200000;
        client_command = CommandBufferAddress(*client);
        REQUIRE(f.runtime->Memory().Write<u32>(client_command, ipc::MakeHeader(0x0007, 0, 0)));
        scheduler.AttachFunction(client, [&] {
            const Result result = manager.SendSyncRequest(*client, session);
            REQUIRE(result == kResultSuccess);
            if (client->status != ThreadStatus::Running) {
                scheduler.Park(*client);  // what the SVC layer does
            }
            client_resumed = true;
        });
        scheduler.Start(client);
        scheduler.Sleep(1000000);  // the client sends its request and waits
        REQUIRE(probe.pending != nullptr);
        CHECK(client->status == ThreadStatus::WaitIPC);
        scheduler.Terminate(client);
        Reply answer(0x0007, 1, 0);
        answer.Push(kResultSuccess);
        Complete(probe.pending, std::move(answer));
        scheduler.Sleep(1000000);  // the completion runs
        cancelled = probe.pending->Cancelled();
        finished = probe.pending->Finished();
        REQUIRE(f.runtime->Memory().ReadBlock(client_words.data(), client_command, 4));
        ExitProcess(*f.runtime);
    });
    g_probe = nullptr;
    CHECK(status == 0);
    CHECK(cancelled);
    CHECK(finished);
    CHECK_FALSE(client_resumed);
    CHECK(replied == 0);
    CHECK(client_words[0] == ipc::MakeHeader(0x0007, 0, 0));  // nothing was written for it
}

// ---- --trace-ipc ------------------------------------------------------------------------------------------------------

namespace {

// One request line of an IPC trace: the thread and the key=value fields.
struct TraceLine {
    std::string thread;
    std::map<std::string, std::string> fields;
    std::vector<std::string> data;  // the data= fields, in order
};

std::vector<TraceLine> RequestLines(const std::vector<std::string>& lines) {
    std::vector<TraceLine> out;
    for (const std::string& line : lines) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        TraceLine t;
        std::istringstream in(line);
        in >> t.thread;
        for (std::string token; in >> token;) {
            const auto eq = token.find('=');
            REQUIRE(eq != std::string::npos);
            const std::string key = token.substr(0, eq), value = token.substr(eq + 1);
            if (key == "data") {
                t.data.push_back(value);
            } else {
                t.fields[key] = value;
            }
        }
        out.push_back(std::move(t));
    }
    return out;
}

std::string Crc(const std::vector<u8>& bytes) {
    return fmt::format("0x{:08x}", Crc32(bytes.data(), bytes.size()));
}

}  // namespace

TEST_CASE("--trace-ipc writes every request with its reply in the format of "
          "tools/compare_ipc_traces.py") {
    Fixture f;
    TempDir dir;
    const std::string path = dir.File("ipc.txt");
    Probe probe;
    g_probe = &probe;
    u32 rw_address = 0;
    const std::vector<u8> static_bytes = {1, 2, 3, 4, 5, 6};
    const int status =
        BootWith(f, dir, {"--ms", "50", "--trace-ipc", path, "--trace-ipc-full"}, [&] {
            Guest g(*f.runtime);
            const Handle session = ConnectTest(g);
            const Handle event = g.NewEvent();
            // 1. a plain request
            g.Request(session, {ipc::MakeHeader(0x000F, 1, 0), 5}, 2);
            // 2. a static buffer in
            g.PutBytes(g.Scratch(0x100), static_bytes);
            g.Request(
                session,
                {ipc::MakeHeader(0x0003, 0, 2), ipc::StaticBufferDesc(6, 2), g.Scratch(0x100)}, 6);
            // 3. a static buffer out
            g.PutWords(g.Cmd() + ipc::StaticBufferDescriptorWord(1) * 4,
                       {ipc::StaticBufferDesc(16, 1), g.Scratch(0x200)});
            g.Request(session, {ipc::MakeHeader(0x0004, 0, 0)}, 4);
            // 4. mapped buffers, one read-write and one read-only
            rw_address = g.Scratch(0x400);
            g.PutBytes(rw_address, {1, 2, 3, 4, 5, 6, 7, 8});
            g.PutBytes(g.Scratch(0x500), {9, 9, 9, 9});
            g.Request(session,
                      {ipc::MakeHeader(0x0005, 0, 4), ipc::MappedBufferDesc(8, ipc::RW), rw_address,
                       ipc::MappedBufferDesc(4, ipc::R), g.Scratch(0x500)},
                      10);
            // 5. the PID
            g.Request(session, {ipc::MakeHeader(0x0006, 0, 2), ipc::CallingPidDesc(), 0}, 5);
            // 6. handles: a copied event (the first descriptor of 0x0002 wants two: this one is refused), then
            //    a request that hands out a session and a semaphore
            const std::vector<u32> objects = g.Request(session, {ipc::MakeHeader(0x000D, 0, 0)}, 6);
            // 7. a request through the session that reply handed out
            g.Request(objects[3], {ipc::MakeHeader(0x0001, 0, 0)}, 3);
            // 8. a request that fails in translation (a handle the client does not have)
            g.Request(session, {ipc::MakeHeader(0x0002, 0, 2), ipc::CopyHandleDesc(1), 0x12345678},
                      1);
            (void)event;
            // 9. a request that never gets a reply: the run ends first
            g.Request(session, {ipc::MakeHeader(0x0007, 1, 0), 0x99}, 1);
            ExitProcess(*f.runtime);
        });
    g_probe = nullptr;
    CHECK(status == 0);  // the time limit
    const std::vector<std::string> lines = Lines(ReadFile(path));
    REQUIRE(lines.size() >= 3);
    CHECK(lines[0].rfind("# ipctrace v1 ", 0) == 0);
    CHECK(lines[0].find(" game=0x") != std::string::npos);
    CHECK(lines[0].find(" cpu=native new3ds=off") != std::string::npos);
    // the main thread's line comes before the first request
    CHECK(lines[1].rfind("# thread 1 parent=0 entry=00100000 ", 0) == 0);

    const std::vector<TraceLine> requests = RequestLines(lines);
    REQUIRE(requests.size() == 9);
    for (std::size_t i = 0; i < requests.size(); ++i) {
        CHECK(requests[i].thread == "1");
        CHECK(requests[i].fields.at("seq") == std::to_string(i + 1));
        CHECK(requests[i].fields.at("session") == (i == 6 ? "tst:reg/file#0" : "tst:reg"));
    }
    // the game clock only moves forward (and the 39 microsecond waits after the replies move it)
    for (std::size_t i = 1; i < requests.size(); ++i) {
        CHECK(std::stoull(requests[i].fields.at("t")) >=
              std::stoull(requests[i - 1].fields.at("t")));
    }
    CHECK(std::stoull(requests[7].fields.at("t")) > std::stoull(requests[0].fields.at("t")));
    // 1. words in and out; nothing else
    CHECK(requests[0].fields.at("cmd") == "0x000f0040");
    CHECK(requests[0].fields.at("in") == "0x000f0040,0x00000005");
    CHECK(requests[0].fields.at("desc") == "-");
    CHECK(requests[0].fields.at("out") == "0x000f0040,0x00000000");
    CHECK(requests[0].fields.at("outdesc") == "-");
    CHECK(requests[0].data.empty());
    // 2. a static buffer: its size, the CRC-32 of its bytes, and the bytes with --trace-ipc-full
    CHECK(requests[1].fields.at("desc") == "static:1:6:" + Crc(static_bytes));
    REQUIRE(requests[1].data.size() == 1);
    CHECK(requests[1].data[0] == "1:010203040506");
    CHECK(requests[1].fields.at("outdesc") == "-");
    // 3. a static buffer out: the CRC-32 of what the reply wrote into the client's receive buffer
    std::vector<u8> written(10);
    for (u8 i = 0; i < written.size(); ++i) {
        written[i] = static_cast<u8>(0xA0 + i);
    }
    CHECK(requests[2].fields.at("desc") == "-");
    CHECK(requests[2].fields.at("outdesc") == "static:2:10:" + Crc(written));
    // 4. mapped buffers: the readable ones are summarised with a CRC-32 at the request; the writable one
    //    once more after the reply (its bytes are reversed by then)
    const std::vector<u8> before = {1, 2, 3, 4, 5, 6, 7, 8}, after = {8, 7, 6, 5, 4, 3, 2, 1};
    CHECK(requests[3].fields.at("desc") ==
          "mapped-rw:1:8:" + Crc(before) + ",mapped-r:3:4:" + Crc({9, 9, 9, 9}));
    CHECK(requests[3].fields.at("outdesc") == "mapped-rw:1:8:" + Crc(after));
    REQUIRE(requests[3].data.size() == 2);
    CHECK(requests[3].data[0] == "1:0102030405060708");
    CHECK(requests[3].data[1] == "3:09090909");
    // 5. the calling PID
    CHECK(requests[4].fields.at("desc") == "pid:1:1:-");
    CHECK(requests[4].fields.at("outdesc") == "pid:3:1:-");
    // 6. the reply hands out a session of the port and a semaphore, named after the port, the kind and
    //    their order; the index is the descriptor's word in the reply
    CHECK(requests[5].fields.at("outdesc") ==
          "move:2:1:tst:reg/file#0,copy:4:1:tst:reg/semaphore#0");
    // 7. a request through that session
    CHECK(requests[6].fields.at("cmd") == "0x00010000");
    // 8. a request that failed in translation has no contents
    CHECK(requests[7].fields.at("cmd") == "-");
    CHECK(requests[7].fields.at("in") == "-");
    CHECK(requests[7].fields.at("desc") == "-");
    CHECK(requests[7].fields.at("out") == "-");
    // 9. a request that never got a reply
    CHECK(requests[8].fields.at("cmd") == "0x00070040");
    CHECK(requests[8].fields.at("in") == "0x00070040,0x00000099");
    CHECK(requests[8].fields.at("out") == "-");
    CHECK(requests[8].fields.at("outdesc") == "-");
}

TEST_CASE("--trace-ipc without --trace-ipc-full leaves the buffers' bytes out, and the flag alone "
          "is refused") {
    {
        Fixture f;
        TempDir dir;
        const std::string path = dir.File("ipc.txt");
        const int status = BootWith(f, dir, {"--ms", "50", "--trace-ipc", path}, [&] {
            Guest g(*f.runtime);
            const Handle session = ConnectTest(g);
            g.PutBytes(g.Scratch(0x100), {1, 2, 3});
            g.Request(
                session,
                {ipc::MakeHeader(0x0003, 0, 2), ipc::StaticBufferDesc(3, 0), g.Scratch(0x100)}, 6);
            ExitProcess(*f.runtime);
        });
        CHECK(status == 0);
        const std::vector<TraceLine> requests = RequestLines(Lines(ReadFile(path)));
        REQUIRE(requests.size() == 1);
        CHECK(requests[0].data.empty());
        CHECK(requests[0].fields.at("desc") == "static:1:3:" + Crc({1, 2, 3}));
    }
    {
        Fixture f;
        TempDir dir;
        const int status = BootWith(f, dir, {"--trace-ipc-full"}, [&] { ExitProcess(*f.runtime); });
        CHECK(status == 2);
        CHECK(f.Logged("--trace-ipc-full needs --trace-ipc FILE"));
    }
    {
        Fixture f;
        TempDir dir;
        const int status = BootWith(f, dir, {"--trace-ipc", dir.File("no-such-dir/ipc.txt")},
                                    [&] { ExitProcess(*f.runtime); });
        CHECK(status == 2);
        CHECK(f.Logged("cannot write"));
    }
}

#endif
