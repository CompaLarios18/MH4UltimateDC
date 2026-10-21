// Derived from Azahar src/core/hle/service/nwm/nwm_uds.cpp, src/core/hle/service/nwm/nwm_uds.h,
// src/core/hle/service/nwm/uds_beacon.h and src/core/hle/kernel/hle_ipc.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version"): the expected command ids, replies, result codes and structure layouts of nwm::UDS without a
// room, and the 300 ms and 5 s waits of RecvBeaconBroadcastData and ConnectToNetwork (nwm_uds.cpp, with
// the sleep of hle_ipc.cpp). The test harness is the one of the other service tests.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (nwm_uds.cpp, nwm_uds.h)
// Copyright 2017 Citra Emulator Project (uds_beacon.h)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (hle_ipc.cpp)
//
// nwm::UDS without a room (N1-29), through the SVC layer: the guest gets a session by srv:
// GetServiceHandle and sends requests as a program would. The programs are ELF files written by the
// test whose main thread runs a C++ function (Runtime::SetMainThreadEntry); time is a
// ManualTimeSource, so the waits are measured in game ticks.
#include <doctest/doctest.h>

#if !__has_include("recomp.h")
TEST_CASE("nwm_test needs recomp.h (the repository around native/)" * doctest::skip()) {}
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
#include <string>
#include <vector>

#include "mh4n/clock.h"
#include "mh4n/ipc.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
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

// ---- the service ------------------------------------------------------------------------------------------

constexpr Result kWrongStatus = 0xC8A111EA;     // 490, UDS, InvalidState, Status
constexpr Result kNotInitialized = 0xC94113F8;  // NotInitialized, UDS, StatusChanged, Status
constexpr Result kNodeNotFound = 0xC90113FA;    // NotFound, UDS, WrongArgument, Status
constexpr Result kBindArgsZero = 0xE10113EA;    // NotAuthorized, UDS, WrongArgument, Usage
constexpr Result kBindMaxBinds = 0xC86113F3;    // OutOfMemory, UDS, OutOfResource, Status
constexpr Result kTooLargeUsage = 0xE10113E9;   // TooLarge, UDS, WrongArgument, Usage
constexpr Result kNotConnected = 0xC8A113EA;    // NotAuthorized, UDS, InvalidState, Status
constexpr Result kEjectNotHost = 0xE0A113EA;    // NotAuthorized, UDS, InvalidState, Usage
constexpr Result kConnectTimeout = 0xC92113FE;  // Timeout, UDS, Canceled, Status

enum Status : u32 { NotConnected = 3, ConnectedAsHost = 6, Connecting = 7 };

struct Session {
    Guest& g;
    Handle handle;

    std::vector<u32> Call(const std::vector<u32>& words, std::size_t reply_words) {
        return g.Request(handle, words, reply_words);
    }
    Result CallResult(const std::vector<u32>& words) { return Call(words, 2)[1]; }
};

Handle ConnectSrv(Guest& g) {
    Handle h = 0;
    REQUIRE(g.ConnectToPort("srv:", &h) == kResultSuccess);
    return h;
}

// srv: GetServiceHandle "nwm::UDS": a new session of the service.
Handle OpenUds(Guest& g, Handle srv) {
    std::array<u32, 2> name{};
    std::memcpy(name.data(), "nwm::UDS", 8);
    const std::vector<u32> reply =
        g.Request(srv, {ipc::MakeHeader(0x0005, 4, 0), name[0], name[1], 8, 0}, 4);
    REQUIRE(reply[0] == ipc::MakeHeader(0x0005, 1, 2));
    REQUIRE(reply[1] == kResultSuccess);
    return reply[3];
}

// A shared memory block for Initialize: the handle.
Handle MakeBlock(Guest& g, u32 size = 0x1000) {
    const Regs out = CallSvc(g.rt, 0x1E, {0x10000000, 0, size, 3});
    REQUIRE(out.r[0] == kResultSuccess);
    return out.r[1];
}

// InitializeWithVersion with a node whose network node id is `node_id`: the reply.
std::vector<u32> Initialize(Session& s, Handle block, u32 size = 0x1000, u16 node_id = 0x77,
                            u16 version = 0x400) {
    std::vector<u32> words = {ipc::MakeHeader(0x001B, 12, 2), size};
    std::array<u32, 10> node{};
    node[0] = 0x11223344;  // friend code seed, low
    node[1] = 0x55667788;  // high
    node[2] = 0x00610041;  // "Aa"
    node[8] = node_id;     // NodeInfo.network_node_id (offset 32)
    words.insert(words.end(), node.begin(), node.end());
    words.push_back(version);
    words.push_back(ipc::CopyHandleDesc(1));
    words.push_back(block);
    return s.Call(words, 4);
}

struct ConnectionStatusWords {
    std::vector<u32> w;  // the 12 words of the structure
    u32 status() const { return w[0]; }
    u32 reason() const { return w[1]; }
    u16 node_id() const { return static_cast<u16>(w[2]); }
    u16 changed() const { return static_cast<u16>(w[2] >> 16); }
    u16 node(std::size_t i) const { return static_cast<u16>(w[3 + i / 2] >> (16 * (i % 2))); }
    u8 total() const { return static_cast<u8>(w[11]); }
    u8 max() const { return static_cast<u8>(w[11] >> 8); }
    u16 bitmask() const { return static_cast<u16>(w[11] >> 16); }
};

ConnectionStatusWords GetStatus(Session& s) {
    const std::vector<u32> reply = s.Call({ipc::MakeHeader(0x000B, 0, 0)}, 14);
    REQUIRE(reply[0] == ipc::MakeHeader(0x000B, 13, 0));
    REQUIRE(reply[1] == kResultSuccess);
    return {std::vector<u32>(reply.begin() + 2, reply.end())};
}

// BeginHostingNetwork with `max_nodes`, the channel `channel` (0: the game has none) and application
// data. The network structure is the 0x108 bytes of Azahar's NetworkInfo.
Result BeginHosting(Guest& g, Session& s, u8 max_nodes, u8 channel = 0,
                    const std::vector<u8>& app_data = {}) {
    std::vector<u8> info(0x108, 0);
    info[0x6] = channel;
    info[0x1D] = max_nodes;
    info[0x3F] = static_cast<u8>(app_data.size());
    std::copy(app_data.begin(), app_data.end(), info.begin() + 0x40);
    g.PutBytes(g.Scratch(0x100), info);
    g.PutBytes(g.Scratch(0x300), {'p', 'a', 's', 's'});
    return s.CallResult({ipc::MakeHeader(0x001D, 1, 4), 4, ipc::StaticBufferDesc(0x108, 0),
                         g.Scratch(0x100), ipc::StaticBufferDesc(4, 1), g.Scratch(0x300)});
}

// Bind: the reply (header, result, descriptor, event handle).
std::vector<u32> Bind(Session& s, u32 bind_node, u32 recv_size, u8 channel, u16 node = 0) {
    return s.Call({ipc::MakeHeader(0x0012, 4, 0), bind_node, recv_size, channel, node}, 4);
}

bool Signaled(Guest& g, Handle event) {
    return g.Wait(event, 0) == kResultSuccess;
}

u64 Now(Guest& g) {
    return g.rt.Clock().Now();
}

}  // namespace

// ---- Initialize and the connection status -------------------------------------------------------------------

TEST_CASE("nwm::UDS Initialize answers with the connection status event, and the status starts NotConnected") {
    Fixture f;
    TempDir dir;
    std::vector<u32> init;
    ConnectionStatusWords st;
    bool signaled_after_init = true;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Session s{g, OpenUds(g, ConnectSrv(g))};
        const Handle block = MakeBlock(g);
        init = Initialize(s, block);
        signaled_after_init = Signaled(g, init[3]);
        st = GetStatus(s);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    REQUIRE(init.size() == 4);
    CHECK(init[0] == 0x001B0042);
    CHECK(init[1] == kResultSuccess);
    CHECK(init[2] == ipc::CopyHandleDesc(1));
    CHECK(init[3] != 0);
    CHECK_FALSE(signaled_after_init);  // Azahar does not signal the event in Initialize
    CHECK(st.status() == NotConnected);
    CHECK(st.reason() == 0);
    CHECK(st.node_id() == 0);
    CHECK(st.changed() == 0);
    CHECK(st.total() == 0);
    CHECK(st.max() == 0);
    CHECK(st.bitmask() == 0);
    CHECK_FALSE(f.Logged("shared memory"));
}

TEST_CASE("nwm::UDS GetNodeInformation needs Initialize, and finds the node the game gave") {
    Fixture f;
    TempDir dir;
    std::vector<u32> before, ours, other;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Session s{g, OpenUds(g, ConnectSrv(g))};
        before = s.Call({ipc::MakeHeader(0x000D, 1, 0), 0x77}, 12);
        Initialize(s, MakeBlock(g), 0x1000, 0x77);
        ours = s.Call({ipc::MakeHeader(0x000D, 1, 0), 0x77}, 12);
        other = s.Call({ipc::MakeHeader(0x000D, 1, 0), 0x78}, 2);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(before[0] == ipc::MakeHeader(0x000D, 1, 0));
    CHECK(before[1] == kNotInitialized);
    REQUIRE(ours.size() == 12);
    CHECK(ours[0] == ipc::MakeHeader(0x000D, 11, 0));
    CHECK(ours[1] == kResultSuccess);
    CHECK(ours[2] == 0x11223344);
    CHECK(ours[3] == 0x55667788);
    CHECK(ours[4] == 0x00610041);
    CHECK(ours[10] == 0x77);  // network_node_id at offset 32 of the structure
    CHECK(other[1] == kNodeNotFound);
}

TEST_CASE("nwm::UDS Initialize with a wrong shared memory size or none is logged and goes on") {
    Fixture f;
    TempDir dir;
    std::vector<u32> wrong_size, none;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Session s{g, OpenUds(g, ConnectSrv(g))};
        wrong_size = Initialize(s, MakeBlock(g), 0x2000);
        none = Initialize(s, 0);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(wrong_size[1] == kResultSuccess);
    CHECK(none[1] == kResultSuccess);
    CHECK(f.Logged("Invalid shared memory size"));
    CHECK(f.Logged("without a shared memory block"));
}

TEST_CASE("nwm::UDS the sessions share one service") {
    Fixture f;
    TempDir dir;
    ConnectionStatusWords st;
    std::vector<u32> init;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        const Handle srv = ConnectSrv(g);
        Session a{g, OpenUds(g, srv)};
        Session b{g, OpenUds(g, srv)};
        init = Initialize(a, MakeBlock(g));
        BeginHosting(g, a, 4);
        st = GetStatus(b);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(st.status() == ConnectedAsHost);
}

// ---- scanning -----------------------------------------------------------------------------------------------------

TEST_CASE("nwm::UDS a scan finds nothing, and the reply comes 300 ms of game time later") {
    Fixture f;
    TempDir dir;
    std::vector<u32> reply;
    std::vector<u8> buffer;
    u64 elapsed = 0;
    u32 buffer_address = 0;
    const int status = BootWith(f, dir, {"--ms", "2000"}, [&] {
        Guest g(*f.runtime);
        Session s{g, OpenUds(g, ConnectSrv(g))};
        Initialize(s, MakeBlock(g));
        buffer_address = g.Scratch(0x400);
        g.PutBytes(buffer_address, std::vector<u8>(0x20, 0xEE));
        const Handle event = g.NewEvent();
        std::vector<u32> request = {ipc::MakeHeader(0x000F, 16, 4), 0x20, 0, 0, 0xFFFFFFFF, 0xFFFF};
        request.resize(15, 0);
        request.push_back(0x7A7A7A7A);  // wlan comm id
        request.push_back(3);           // id
        request.push_back(ipc::CopyHandleDesc(1));
        request.push_back(event);
        request.push_back(ipc::MappedBufferDesc(0x20, ipc::W));
        request.push_back(buffer_address);
        const u64 before = Now(g);
        reply = s.Call(request, 4);
        elapsed = Now(g) - before;
        buffer = g.GetBytes(buffer_address, 0x20);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    REQUIRE(reply.size() == 4);
    CHECK(reply[0] == ipc::MakeHeader(0x000F, 1, 2));
    CHECK(reply[1] == kResultSuccess);
    CHECK(reply[2] == ipc::MappedBufferDesc(0x20, ipc::W));
    CHECK(reply[3] == buffer_address);
    // BeaconDataReplyHeader: max_output_size, total_size, total_entries; nothing else is written.
    u32 words[3];
    std::memcpy(words, buffer.data(), sizeof(words));
    CHECK(words[0] == 0x20);
    CHECK(words[1] == 12);
    CHECK(words[2] == 0);
    for (std::size_t i = 12; i < buffer.size(); ++i) {
        CHECK(buffer[i] == 0xEE);
    }
    // exactly 300 ms (Azahar's nsToCycles), with no 39 microsecond reply delay on top
    CHECK(elapsed >= NsToTicks(300000000));
    CHECK(elapsed == NsToTicks(300000000));
}

TEST_CASE("nwm::UDS a scan loop with nothing to find keeps its pace: every scan is 300 ms") {
    Fixture f;
    TempDir dir;
    std::vector<u64> elapsed;
    const int status = BootWith(f, dir, {"--ms", "3000"}, [&] {
        Guest g(*f.runtime);
        Session s{g, OpenUds(g, ConnectSrv(g))};
        Initialize(s, MakeBlock(g));
        const Handle event = g.NewEvent();
        for (int i = 0; i < 4; ++i) {
            std::vector<u32> request = {ipc::MakeHeader(0x000F, 16, 4), 0x20};
            request.resize(17, 0);
            request.push_back(ipc::CopyHandleDesc(1));
            request.push_back(event);
            request.push_back(ipc::MappedBufferDesc(0x20, ipc::W));
            request.push_back(g.Scratch(0x400));
            const u64 before = Now(g);
            s.Call(request, 2);
            elapsed.push_back(Now(g) - before);
        }
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    REQUIRE(elapsed.size() == 4);
    for (const u64 e : elapsed) {
        CHECK(e >= NsToTicks(300000000));
        CHECK(e == NsToTicks(300000000));
    }
}

// ---- hosting -----------------------------------------------------------------------------------------------------

TEST_CASE("nwm::UDS hosting a network and destroying it") {
    Fixture f;
    TempDir dir;
    Result begin = 0, destroy = 0, destroy_again = 0;
    ConnectionStatusWords hosting, hosting_again, destroyed;
    std::vector<u32> node, node_missing, channel_hosting, channel_after;
    bool event_after_begin = false, event_after_status = true, event_after_destroy = false;
    bool event_cleared = true;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Session s{g, OpenUds(g, ConnectSrv(g))};
        const std::vector<u32> init = Initialize(s, MakeBlock(g));
        const Handle event = init[3];
        begin = BeginHosting(g, s, 8, 0);
        event_after_begin = Signaled(g, event);
        event_cleared = !Signaled(g, event);  // a OneShot event clears when a wait takes it
        hosting = GetStatus(s);
        hosting_again = GetStatus(s);
        node = s.Call({ipc::MakeHeader(0x000D, 1, 0), 1}, 12);
        node_missing = s.Call({ipc::MakeHeader(0x000D, 1, 0), 9}, 12);
        channel_hosting = s.Call({ipc::MakeHeader(0x001A, 0, 0)}, 3);
        destroy = s.CallResult({ipc::MakeHeader(0x0008, 0, 0)});
        event_after_destroy = Signaled(g, event);
        destroyed = GetStatus(s);
        channel_after = s.Call({ipc::MakeHeader(0x001A, 0, 0)}, 3);
        destroy_again = s.CallResult({ipc::MakeHeader(0x0008, 0, 0)});
        event_after_status = Signaled(g, event);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(begin == kResultSuccess);
    CHECK(event_after_begin);
    CHECK(event_cleared);
    CHECK(hosting.status() == ConnectedAsHost);
    CHECK(hosting.reason() == 1);  // ConnectionEstablished
    CHECK(hosting.node_id() == 1);
    CHECK(hosting.changed() == 1);  // node 1 was set
    CHECK(hosting.node(0) == 1);
    CHECK(hosting.total() == 1);
    CHECK(hosting.max() == 8);
    CHECK(hosting.bitmask() == 1);
    // GetConnectionStatus clears the changed nodes
    CHECK(hosting_again.status() == ConnectedAsHost);
    CHECK(hosting_again.changed() == 0);
    CHECK(hosting_again.total() == 1);
    // the host is node 1, with the data the game gave; an unfilled slot has node id 0 and is not node 9
    REQUIRE(node.size() == 12);
    CHECK(node[1] == kResultSuccess);
    CHECK(node[2] == 0x11223344);
    CHECK(node[10] == 1);
    CHECK(node_missing[1] == kNodeNotFound);
    // the channel: the default of 11 while hosting, 0 once not connected
    REQUIRE(channel_hosting.size() == 3);
    CHECK(channel_hosting[0] == ipc::MakeHeader(0x001A, 2, 0));
    CHECK(channel_hosting[2] == 11);
    CHECK(destroy == kResultSuccess);
    CHECK(event_after_destroy);
    CHECK(destroyed.status() == NotConnected);
    CHECK(destroyed.reason() == 0);
    CHECK(destroyed.node_id() == 1);  // kept
    CHECK(destroyed.total() == 0);
    CHECK(destroyed.max() == 0);
    CHECK(destroyed.bitmask() == 0);
    CHECK(channel_after[2] == 0);
    CHECK(destroy_again == kWrongStatus);
    CHECK_FALSE(event_after_status);  // a failed DestroyNetwork signals nothing
}

TEST_CASE("nwm::UDS hosting on the game's channel, and with application data") {
    Fixture f;
    TempDir dir;
    std::vector<u32> channel, app_before, app_after, app_small;
    Result too_large = 0, set_ok = 0;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Session s{g, OpenUds(g, ConnectSrv(g))};
        Initialize(s, MakeBlock(g));
        const std::vector<u8> data = {9, 8, 7, 6, 5};
        BeginHosting(g, s, 4, 6, data);
        channel = s.Call({ipc::MakeHeader(0x001A, 0, 0)}, 3);
        // GetApplicationData: the receive buffer 0 (0x20 bytes) is described after the command buffer
        const u32 receive = g.Scratch(0x500);
        g.PutBytes(receive, std::vector<u8>(0x20, 0xEE));
        g.PutWords(g.Cmd() + ipc::StaticBufferDescriptorWord(0) * 4,
                   {ipc::StaticBufferDesc(0x20, 0), receive});
        app_before = s.Call({ipc::MakeHeader(0x0011, 1, 0), 0xC8}, 5);
        // a buffer smaller than the data: 0 bytes
        app_small = s.Call({ipc::MakeHeader(0x0011, 1, 0), 4}, 3);
        // SetApplicationData: 3 bytes
        g.PutBytes(g.Scratch(0x600), {1, 2, 3});
        set_ok = s.CallResult({ipc::MakeHeader(0x0010, 1, 2), 3, ipc::StaticBufferDesc(3, 0),
                               g.Scratch(0x600)});
        app_after = s.Call({ipc::MakeHeader(0x0011, 1, 0), 0xC8}, 5);
        g.PutBytes(g.Scratch(0x700), std::vector<u8>(0xC9, 1));
        too_large = s.CallResult({ipc::MakeHeader(0x0010, 1, 2), 0xC9, ipc::StaticBufferDesc(0xC9, 0),
                                  g.Scratch(0x700)});
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(channel[2] == 6);
    REQUIRE(app_before.size() == 5);
    CHECK(app_before[0] == ipc::MakeHeader(0x0011, 2, 2));
    CHECK(app_before[2] == 5);
    CHECK(app_before[3] == ipc::StaticBufferDesc(5, 0));
    CHECK(app_small[1] == kResultSuccess);
    CHECK(app_small[2] == 0);
    CHECK(set_ok == kResultSuccess);
    CHECK(app_after[2] == 3);
    CHECK(too_large == kTooLargeUsage);
}

TEST_CASE("nwm::UDS hosting a network of fewer than two nodes is fatal, as in the real module") {
    Fixture f;
    TempDir dir;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Session s{g, OpenUds(g, ConnectSrv(g))};
        Initialize(s, MakeBlock(g));
        BeginHosting(g, s, 1);
        // the run ends at the next safe point; a request after it is not needed
        g.Wait(g.NewEvent(), 1000000000);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 3);
    CHECK(f.Logged("trying to host a network of only one member"));
}

TEST_CASE("nwm::UDS hosting with application data over 0xC8 bytes is fatal, as in the real module") {
    Fixture f;
    TempDir dir;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Session s{g, OpenUds(g, ConnectSrv(g))};
        Initialize(s, MakeBlock(g));
        std::vector<u8> info(0x108, 0);
        info[0x1D] = 4;
        info[0x3F] = 0xC9;
        g.PutBytes(g.Scratch(0x100), info);
        g.PutBytes(g.Scratch(0x300), {'p', 'a', 's', 's'});
        s.CallResult({ipc::MakeHeader(0x001D, 1, 4), 4, ipc::StaticBufferDesc(0x108, 0),
                      g.Scratch(0x100), ipc::StaticBufferDesc(4, 1), g.Scratch(0x300)});
        g.Wait(g.NewEvent(), 1000000000);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 3);
    CHECK(f.Logged("application data is too big"));
}

// ---- connecting, disconnecting -------------------------------------------------------------------------------

TEST_CASE("nwm::UDS ConnectNetwork stays Connecting and answers the timeout error 5 s later") {
    Fixture f;
    TempDir dir;
    Result result = 0;
    std::vector<u32> reply;
    u64 elapsed = 0;
    ConnectionStatusWords st;
    const int status = BootWith(f, dir, {"--ms", "10000"}, [&] {
        Guest g(*f.runtime);
        Session s{g, OpenUds(g, ConnectSrv(g))};
        Initialize(s, MakeBlock(g));
        g.PutBytes(g.Scratch(0x100), std::vector<u8>(0x108, 0));
        g.PutBytes(g.Scratch(0x300), {'p', 'a', 's', 's'});
        const u64 before = Now(g);
        reply = s.Call({ipc::MakeHeader(0x001E, 2, 4), 1, 4, ipc::StaticBufferDesc(0x108, 0),
                        g.Scratch(0x100), ipc::StaticBufferDesc(4, 2), g.Scratch(0x300)},
                       2);
        elapsed = Now(g) - before;
        result = reply[1];
        st = GetStatus(s);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(reply[0] == ipc::MakeHeader(0x001E, 1, 0));
    CHECK(result == kConnectTimeout);
    CHECK(elapsed >= NsToTicks(5000000000ull));
    CHECK(elapsed == NsToTicks(5000000000ull));
    CHECK(st.status() == Connecting);  // Azahar never resets it after the timeout
}

TEST_CASE("nwm::UDS ConnectToNetwork (deprecated, 0x09) stays Connecting and answers the timeout 5 s later") {
    Fixture f;
    TempDir dir;
    std::vector<u32> reply;
    u64 elapsed = 0;
    ConnectionStatusWords st;
    const int status = BootWith(f, dir, {"--ms", "10000"}, [&] {
        Guest g(*f.runtime);
        Session s{g, OpenUds(g, ConnectSrv(g))};
        Initialize(s, MakeBlock(g));
        g.PutBytes(g.Scratch(0x300), {'p', 'a', 's', 's'});
        std::vector<u32> words = {ipc::MakeHeader(0x0009, 17, 2)};
        words.resize(1 + 15, 0);  // the first 0x3C bytes of the network structure
        words.push_back(1);       // connection type
        words.push_back(4);       // passphrase size
        words.push_back(ipc::StaticBufferDesc(4, 2));
        words.push_back(g.Scratch(0x300));
        const u64 before = Now(g);
        reply = s.Call(words, 2);
        elapsed = Now(g) - before;
        st = GetStatus(s);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(reply[0] == ipc::MakeHeader(0x0009, 1, 0));
    CHECK(reply[1] == kConnectTimeout);
    CHECK(elapsed == NsToTicks(5000000000ull));
    CHECK(st.status() == Connecting);
}

TEST_CASE("nwm::UDS DisconnectNetwork: as the host it fails and resets the status, else it succeeds and signals") {
    Fixture f;
    TempDir dir;
    Result as_host = 0, idle = 0;
    ConnectionStatusWords after_host, after_idle;
    bool event_host = true, event_idle = false;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Session s{g, OpenUds(g, ConnectSrv(g))};
        const std::vector<u32> init = Initialize(s, MakeBlock(g));
        BeginHosting(g, s, 4);
        Signaled(g, init[3]);
        as_host = s.CallResult({ipc::MakeHeader(0x000A, 0, 0)});
        event_host = Signaled(g, init[3]);
        after_host = GetStatus(s);
        s.CallResult({ipc::MakeHeader(0x0008, 0, 0)});  // destroy
        Signaled(g, init[3]);
        idle = s.CallResult({ipc::MakeHeader(0x000A, 0, 0)});
        event_idle = Signaled(g, init[3]);
        after_idle = GetStatus(s);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(as_host == kWrongStatus);
    CHECK_FALSE(event_host);
    // "a real 3ds makes strange things here": still a host, with the rest cleared
    CHECK(after_host.status() == ConnectedAsHost);
    CHECK(after_host.node_id() == 1);
    CHECK(after_host.total() == 0);
    CHECK(after_host.max() == 0);
    CHECK(after_host.bitmask() == 0);
    CHECK(idle == kResultSuccess);
    CHECK(event_idle);
    CHECK(after_idle.status() == NotConnected);
}

TEST_CASE("nwm::UDS EjectClient, EjectSpectators and UpdateNetworkAttribute") {
    Fixture f;
    TempDir dir;
    Result not_host = 0, host_node = 0, other = 0, broadcast = 0, spectators = 0, attribute = 0;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Session s{g, OpenUds(g, ConnectSrv(g))};
        Initialize(s, MakeBlock(g));
        not_host = s.CallResult({ipc::MakeHeader(0x0005, 1, 0), 2});
        BeginHosting(g, s, 4);
        host_node = s.CallResult({ipc::MakeHeader(0x0005, 1, 0), 1});
        other = s.CallResult({ipc::MakeHeader(0x0005, 1, 0), 3});
        broadcast = s.CallResult({ipc::MakeHeader(0x0005, 1, 0), 0xFFFF});
        spectators = s.CallResult({ipc::MakeHeader(0x0006, 0, 0)});
        attribute = s.CallResult({ipc::MakeHeader(0x0007, 2, 0), 0xFFFF, 1});
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(not_host == kEjectNotHost);
    CHECK(host_node == kBindArgsZero);
    CHECK(other == kResultSuccess);
    CHECK(broadcast == kResultSuccess);
    CHECK(spectators == kResultSuccess);
    CHECK(attribute == kResultSuccess);
}

// ---- data channels ------------------------------------------------------------------------------------------------

TEST_CASE("nwm::UDS Bind and Unbind: arguments, events and the limits") {
    Fixture f;
    TempDir dir;
    std::vector<u32> zero_channel, zero_node, small_buffer, good, unbind, unbind_unknown;
    Result unbind_zero = 0;
    bool event_after_unbind = false;
    std::vector<std::vector<u32>> many;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Session s{g, OpenUds(g, ConnectSrv(g))};
        Initialize(s, MakeBlock(g));
        zero_channel = Bind(s, 1, 0x5F4, 0);
        zero_node = Bind(s, 0, 0x5F4, 3);
        small_buffer = Bind(s, 1, 0x5F3, 3);
        good = Bind(s, 5, 0x5F4, 3);
        unbind = s.Call({ipc::MakeHeader(0x0013, 1, 0), 5}, 6);
        event_after_unbind = Signaled(g, good[3]);
        unbind_unknown = s.Call({ipc::MakeHeader(0x0013, 1, 0), 99}, 6);
        unbind_zero = s.CallResult({ipc::MakeHeader(0x0013, 1, 0), 0});
        for (u32 i = 0; i < 17; ++i) {
            many.push_back(Bind(s, 100 + i, 0x5F4, static_cast<u8>(10 + i)));
        }
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(zero_channel[1] == kBindArgsZero);
    CHECK(zero_node[1] == kBindArgsZero);
    CHECK(small_buffer[1] == kTooLargeUsage);
    REQUIRE(good.size() == 4);
    CHECK(good[0] == ipc::MakeHeader(0x0012, 1, 2));
    CHECK(good[1] == kResultSuccess);
    CHECK(good[2] == ipc::CopyHandleDesc(1));
    CHECK(good[3] != 0);
    REQUIRE(unbind.size() == 6);
    CHECK(unbind[0] == ipc::MakeHeader(0x0013, 5, 0));
    CHECK(unbind[1] == kResultSuccess);
    CHECK(unbind[2] == 5);
    CHECK(unbind[3] == 0);
    CHECK(unbind[4] == 0);
    CHECK(unbind[5] == 0);
    CHECK(event_after_unbind);  // Unbind signals the channel's event
    CHECK(unbind_unknown[1] == kResultSuccess);
    CHECK(unbind_unknown[2] == 99);
    CHECK(unbind_zero == kBindArgsZero);
    REQUIRE(many.size() == 17);
    for (std::size_t i = 0; i < 16; ++i) {
        CHECK(many[i][1] == kResultSuccess);
    }
    CHECK(many[16][1] == kBindMaxBinds);
}

TEST_CASE("nwm::UDS Destroy, Disconnect and Shutdown signal the bound channels' events and drop the bindings") {
    Fixture f;
    TempDir dir;
    bool destroy_signals = false, disconnect_signals = false, shutdown_signals = false;
    std::vector<u32> pull_after;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Session s{g, OpenUds(g, ConnectSrv(g))};
        Initialize(s, MakeBlock(g));
        BeginHosting(g, s, 4);
        std::vector<u32> bound = Bind(s, 7, 0x5F4, 4);
        s.CallResult({ipc::MakeHeader(0x0008, 0, 0)});
        destroy_signals = Signaled(g, bound[3]);

        BeginHosting(g, s, 4);
        bound = Bind(s, 7, 0x5F4, 4);
        s.CallResult({ipc::MakeHeader(0x0008, 0, 0)});
        bound = Bind(s, 8, 0x5F4, 4);  // the channel is free again
        s.CallResult({ipc::MakeHeader(0x000A, 0, 0)});
        disconnect_signals = Signaled(g, bound[3]);

        bound = Bind(s, 9, 0x5F4, 4);
        s.CallResult({ipc::MakeHeader(0x0003, 0, 0)});
        shutdown_signals = Signaled(g, bound[3]);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(destroy_signals);
    CHECK(disconnect_signals);
    CHECK(shutdown_signals);
}

TEST_CASE("nwm::UDS PullPacket: nothing is ever received") {
    Fixture f;
    TempDir dir;
    std::vector<u32> not_connected, unbound, hosted, big;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Session s{g, OpenUds(g, ConnectSrv(g))};
        Initialize(s, MakeBlock(g));
        const std::vector<u32> bound = Bind(s, 5, 0x5F4, 3);
        const u32 receive = g.Scratch(0x800);
        auto describe = [&](u32 size) {
            g.PutBytes(receive, std::vector<u8>(0x600, 0xEE));
            g.PutWords(g.Cmd() + ipc::StaticBufferDescriptorWord(0) * 4,
                       {ipc::StaticBufferDesc(size, 0), receive});
        };
        describe(0x600);
        not_connected = s.Call({ipc::MakeHeader(0x0014, 3, 0), 5, 0x80, 0x200}, 6);
        BeginHosting(g, s, 4);
        describe(0x600);
        unbound = s.Call({ipc::MakeHeader(0x0014, 3, 0), 6, 0x80, 0x200}, 6);
        describe(0x600);
        hosted = s.Call({ipc::MakeHeader(0x0014, 3, 0), 5, 0x80, 0x200}, 6);
        describe(0x600);
        big = s.Call({ipc::MakeHeader(0x0014, 3, 0), 5, 0x1000, 0x4000}, 6);
        (void)bound;
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(not_connected[1] == kNotConnected);
    CHECK(unbound[1] == kNotConnected);
    REQUIRE(hosted.size() == 6);
    CHECK(hosted[0] == ipc::MakeHeader(0x0014, 3, 2));
    CHECK(hosted[1] == kResultSuccess);
    CHECK(hosted[2] == 0);                              // no data
    CHECK((hosted[3] & 0xFFFF) == 0);                   // source node
    CHECK(hosted[4] == ipc::StaticBufferDesc(0x80 << 2, 0));  // min(aligned size, 0x172) * 4 bytes
    CHECK(big[4] == ipc::StaticBufferDesc(0x172 << 2, 0));
}

TEST_CASE("nwm::UDS SendTo: the checks of Azahar, and nothing is sent") {
    Fixture f;
    TempDir dir;
    Result not_connected = 0, node_zero = 0, itself = 0, unknown = 0, host_dest = 0, broadcast_flag = 0,
           broadcast_node = 0, too_large = 0, client_node = 0;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Session s{g, OpenUds(g, ConnectSrv(g))};
        Initialize(s, MakeBlock(g));
        g.PutBytes(g.Scratch(0x100), std::vector<u8>(0x800, 0x42));
        auto send = [&](u16 dest, u8 channel, u32 size, u8 flags) {
            return s.CallResult({ipc::MakeHeader(0x0017, 6, 2), 0, dest, channel, (size + 3) / 4, size, flags,
                                 ipc::StaticBufferDesc(size, 0), g.Scratch(0x100)});
        };
        not_connected = send(2, 1, 16, 0);
        BeginHosting(g, s, 4);  // the host is node 1
        node_zero = send(0, 1, 16, 0);
        itself = send(1, 1, 16, 0);
        unknown = send(2, 1, 16, 0);
        client_node = send(4, 1, 16, 0);
        host_dest = send(1, 1, 16, 0);  // (itself again: we are the host)
        broadcast_flag = send(2, 1, 16, 2);
        broadcast_node = send(0xFFFF, 1, 16, 0);
        too_large = send(0xFFFF, 1, 0x5C7, 0);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(not_connected == kNotConnected);
    CHECK(node_zero == kNodeNotFound);
    CHECK(itself == kNodeNotFound);
    CHECK(unknown == kNodeNotFound);  // no node 2 in the (empty) node map
    CHECK(client_node == kNodeNotFound);
    CHECK(host_dest == kNodeNotFound);
    CHECK(broadcast_flag == kResultSuccess);
    CHECK(broadcast_node == kResultSuccess);
    CHECK(too_large == kTooLargeUsage);
}

// ---- the rest -----------------------------------------------------------------------------------------------------

TEST_CASE("nwm::UDS commands Azahar does not implement are answered as unknown commands, once logged") {
    Fixture f;
    TempDir dir;
    std::vector<u32> probe_response, flush, scan, decrypt, unlisted;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Session s{g, OpenUds(g, ConnectSrv(g))};
        probe_response = s.Call({ipc::MakeHeader(0x0021, 2, 0), 0, 0}, 2);
        flush = s.Call({ipc::MakeHeader(0x0020, 0, 0)}, 2);
        flush = s.Call({ipc::MakeHeader(0x0020, 0, 0)}, 2);
        scan = s.Call({ipc::MakeHeader(0x0022, 16, 0)}, 2);
        decrypt = s.Call({ipc::MakeHeader(0x001F, 0, 6)}, 2);
        unlisted = s.Call({ipc::MakeHeader(0x0030, 0, 0)}, 2);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(probe_response[0] == ipc::MakeHeader(0x0021, 1, 0));
    CHECK(probe_response[1] == kResultSuccess);
    CHECK(flush[0] == ipc::MakeHeader(0x0020, 1, 0));
    CHECK(scan[0] == ipc::MakeHeader(0x0022, 1, 0));
    CHECK(unlisted[0] == ipc::MakeHeader(0x0030, 1, 0));
    CHECK(f.LoggedCount("unimplemented nwm::UDS 0x00200000") == 1);
}

TEST_CASE("nwm::UDS the deprecated Initialize and BeginHostingNetwork work as the new ones") {
    Fixture f;
    TempDir dir;
    std::vector<u32> init;
    Result begin = 0;
    ConnectionStatusWords st;
    const int status = BootWith(f, dir, {"--ms", "100"}, [&] {
        Guest g(*f.runtime);
        Session s{g, OpenUds(g, ConnectSrv(g))};
        std::vector<u32> words = {ipc::MakeHeader(0x0001, 11, 2), 0x1000};
        words.resize(12, 0);
        words.push_back(ipc::CopyHandleDesc(1));
        words.push_back(MakeBlock(g));
        init = s.Call(words, 4);
        // BeginHostingNetwork (deprecated): 15 words of the structure (max_nodes at byte 0x1D),
        // passphrase size, the passphrase as a static buffer
        std::vector<u32> hosting = {ipc::MakeHeader(0x0004, 16, 2)};
        std::vector<u32> info(15, 0);
        info[7] = 6u << 8;  // byte 0x1D = word 7, byte 1
        hosting.insert(hosting.end(), info.begin(), info.end());
        hosting.push_back(4);
        g.PutBytes(g.Scratch(0x300), {'p', 'a', 's', 's'});
        hosting.push_back(ipc::StaticBufferDesc(4, 1));
        hosting.push_back(g.Scratch(0x300));
        begin = s.CallResult(hosting);
        st = GetStatus(s);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(init[1] == kResultSuccess);
    CHECK(init[2] == ipc::CopyHandleDesc(1));
    CHECK(begin == kResultSuccess);
    CHECK(st.status() == ConnectedAsHost);
    CHECK(st.max() == 6);
}

#endif  // recomp.h
