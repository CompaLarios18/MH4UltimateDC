// Derived from Azahar src/core/hle/service/apt/apt.cpp, src/core/hle/service/apt/applet_manager.cpp,
// src/core/hle/service/apt/applet_manager.h, src/core/hle/service/apt/errors.h,
// src/core/hle/applets/applet.cpp, src/core/hle/applets/swkbd.cpp, src/core/hle/applets/swkbd.h,
// src/core/frontend/applets/swkbd.cpp, src/core/hle/service/pm/pm_app.cpp and src/core/core_timing.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later: each of them says "GPLv2 or any later
// version" or "GPLv2+"): the expected answers (the command ids and reply layouts, the result codes of the
// applet module, the signals, the return codes of the keyboard, the validation rules, the update period)
// are the ones those sources give.
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (apt.cpp)
// Copyright 2018-2026 Citra Emulator Project / Azahar Emulator Project (applet_manager.cpp,
//   applet_manager.h)
// Copyright 2018-2025 Citra Emulator Project / Azahar Emulator Project (errors.h)
// Copyright 2015 Citra Emulator Project (applet.cpp, hle/applets/swkbd.cpp, hle/applets/swkbd.h)
// Copyright 2018 Citra Emulator Project (frontend/applets/swkbd.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (pm_app.cpp)
// Copyright 2008 Dolphin Emulator Project / 2017 Citra Emulator Project (core_timing.h)
//
// Tests of N1-14: APT:U, APT:A and APT:S over the applet manager, and the software keyboard applet, driven
// through the SVC layer as the game does: the registration of the application, the parameters, the
// library applet flow (PrepareToStartLibraryApplet, the Request message and its Response, StartLibraryApplet,
// the update tick, the WakeupByExit message), each button configuration, the text cut to the maximum
// length, the callback filter, the CPU time limit commands. The configuration of the keyboard is built
// byte by byte at the offsets 3dbrew lists, not with the structure under test. Everything is synthetic.
#include <doctest/doctest.h>

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

#include "applets/applet.h"
#include "applets/swkbd.h"
#include "mh4n/clock.h"
#include "mh4n/ipc.h"
#include "mh4n/log.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"
#include "mh4n/service.h"
#include "mh4n/threads.h"

using namespace mh4n;
using namespace mh4n::apt;

// ---- the text conversions ------------------------------------------------------------------------------------------

TEST_CASE("swkbd: UTF-8 and UTF-16 conversions") {
    CHECK(Utf8ToUtf16("AZAHAR") == u"AZAHAR");
    CHECK(Utf8ToUtf16("caf\xC3\xA9") == u"café");
    CHECK(Utf8ToUtf16("\xE3\x83\x8F\xE3\x83\xB3") == u"ハン");
    // a character outside the BMP is a surrogate pair
    CHECK(Utf8ToUtf16("a\xF0\x9F\x98\x80") == std::u16string(u"a\xD83D\xDE00"));
    CHECK(Utf16ToUtf8(u"a\xD83D\xDE00") == "a\xF0\x9F\x98\x80");
    CHECK(Utf16ToUtf8(u"café") == "caf\xC3\xA9");
    // bad input is not lost silently: it becomes U+FFFD, a byte at a time
    CHECK(Utf8ToUtf16("a\xFFz") == u"a�z");
    CHECK(Utf8ToUtf16("\xC3") == u"�");
    CHECK(Utf8ToUtf16("\xC0\x80") == u"��");  // overlong
    CHECK(Utf8ToUtf16("\xED\xA0\x80") == u"���");  // a surrogate in UTF-8
    CHECK(Utf16ToUtf8(std::u16string(u"x\xD83D")) == "x\xEF\xBF\xBD");  // an unpaired surrogate
    CHECK(Utf8ToUtf16("").empty());
}

TEST_CASE("applets: the structures have Azahar's sizes, and the update period is exact in ticks") {
    CHECK(sizeof(SoftwareKeyboardConfig) == 0x400);
    CHECK(sizeof(CaptureBufferInfo) == 0x20);
    // usToCycles(16666) at 268,111,856 Hz
    CHECK(kAppletUpdateIntervalTicks == 4468352);
    CHECK(kResultParameterPresent == 0xC8A0CC02);
    CHECK(kResultInvalidAppletSlot == 0xC8A0CC04);
}

TEST_CASE("--swkbd-text is an option of the runner and of the replay tool, and may be given several times") {
    for (const char* scope : {"runner", "ipc-replay"}) {
        const Options options = ParseOptions(scope, {"--swkbd-text", "first", "--swkbd-text=second"});
        INFO(scope);
        REQUIRE_MESSAGE(options.ok(), options.error());
        CHECK(options.All("--swkbd-text") == std::vector<std::string>{"first", "second"});
        CHECK(Usage(scope).find("--swkbd-text TEXT") != std::string::npos);
    }
}

#if !__has_include("recomp.h")
TEST_CASE("apt_test's service cases need recomp.h (the repository around native/)" * doctest::skip()) {}
#else

#include "../src/cpu/recomp_c.h"  // last: it defines macros

namespace {

namespace fs = std::filesystem;

constexpr u32 kText = 0x00100000;

// ---- harness: an ELF whose main thread runs a C++ body (as cfg_test) -----------------------------------------

void Put16(std::vector<u8>& b, std::size_t at, u32 v) {
    b[at] = static_cast<u8>(v), b[at + 1] = static_cast<u8>(v >> 8);
}
void Put32(std::vector<u8>& b, std::size_t at, u32 v) {
    Put16(b, at, v & 0xFFFF), Put16(b, at + 2, v >> 16);
}
u32 Get16(const std::vector<u8>& b, std::size_t at) {
    return b[at] | b[at + 1] << 8;
}
u32 Get32(const std::vector<u8>& b, std::size_t at) {
    return Get16(b, at) | Get16(b, at + 2) << 16;
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

class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = fs::temp_directory_path() /
                ("mh4n_apt_test_" +
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
    std::string WriteElf() const {
        const std::string path = File("program.elf");
        const std::vector<u8> bytes = MakeElf();
        std::ofstream(path, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        return path;
    }

private:
    fs::path path_;
};

void WriteAll(const std::string& path, const std::vector<u8>& bytes) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
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
    std::size_t Count(const std::string& needle) const {
        return static_cast<std::size_t>(
            std::count_if(logged.begin(), logged.end(), [&](const std::string& l) {
                return l.find(needle) != std::string::npos;
            }));
    }
};

Options Parse(const std::vector<std::string>& args) {
    const Options options = ParseOptions("runner", args);
    REQUIRE_MESSAGE(options.ok(), options.error());
    return options;
}

int BootWith(Fixture& f, const TempDir& dir, std::vector<std::string> extra, std::function<void()> body) {
    std::vector<std::string> args = {dir.WriteElf(), "--user", dir.User()};
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

// svcSleepThread: the game clock advances by `ns` (the timers due before it run, in order).
void SleepNs(Runtime& rt, u64 ns) {
    CallSvc(rt, 0x0A, {static_cast<u32>(ns), static_cast<u32>(ns >> 32)});
}
// The game clock's reading in ticks (with a manual time source nothing moves it but sleeping).
u64 Now(Runtime& rt) {
    return rt.Clock().Now();
}
// Sleeps until the clock reads at least `tick` minus `before` ticks, from below: the last step of the way
// is a whole number of nanoseconds, so the reading ends within 4 ticks of the target.
void SleepToTick(Runtime& rt, u64 tick) {
    const u64 now = Now(rt);
    if (tick > now) {
        SleepNs(rt, TicksToNs(tick - now));
    }
}

// The results of the applet module (errors.h, applet_manager.cpp).
constexpr Result kTimeout = 0x09401BFE;
constexpr Result kNoData = 0xC8A0CFEF;
constexpr Result kParameterPresent = 0xC8A0CC02;
constexpr Result kInvalidAppletSlot = 0xC8A0CC04;
constexpr Result kAlreadyExists = 0xC8A0CFFC;
constexpr Result kNotFound = 0xC880CFFA;
constexpr Result kNotSupported = 0xD8C0CFFA;

constexpr u32 kApplication = 0x300;
constexpr u32 kKeyboard = 0x401;  // SoftwareKeyboard2: the id of the library applet (0x201: the system one)

enum Signal : u32 {
    kWakeup = 1,
    kRequest = 2,
    kResponse = 3,
    kMessage = 5,
    kWakeupByExit = 0xA,
    kWakeupByPause = 0xB,
    kWakeupByCancel = 0xC,
};

struct Guest {
    Runtime& rt;
    explicit Guest(Runtime& runtime) : rt(runtime) {
        // The 39 microseconds that every reply takes would move the clock by themselves: with none the
        // time is only what the test sleeps.
        FindServiceManager(rt)->SetReplyDelayNs(0);
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
        if (bytes.empty()) {
            return;
        }
        REQUIRE(rt.Memory().WriteBlock(address, bytes.data(), static_cast<u32>(bytes.size())));
    }
    std::vector<u8> GetBytes(u32 address, std::size_t count) {
        std::vector<u8> bytes(count);
        if (count != 0) {
            REQUIRE(rt.Memory().ReadBlock(bytes.data(), address, static_cast<u32>(count)));
        }
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
    std::vector<u32> Request(Handle session, const std::vector<u32>& words, std::size_t reply_words) {
        PutWords(Cmd(), words);
        CallSvc(rt, 0x32, {session});
        return GetWords(Cmd(), reply_words);
    }

    // srv: GetServiceHandle of `name` (flags 1: do not wait): the result and the handle (0 if refused).
    Result ServiceResult(const std::string& name, Handle* out) {
        Handle srv = 0;
        REQUIRE(ConnectToPort("srv:", &srv) == kResultSuccess);
        std::array<u32, 2> words{};
        std::memcpy(words.data(), name.data(), std::min<std::size_t>(name.size(), 8));
        const std::vector<u32> reply = Request(
            srv, {ipc::MakeHeader(0x0005, 4, 0), words[0], words[1], static_cast<u32>(name.size()), 1}, 4);
        Close(srv);
        *out = reply[1] == kResultSuccess ? reply[3] : 0;
        return reply[1];
    }
    Handle Service(const std::string& name) {
        Handle h = 0;
        ServiceResult(name, &h);
        return h;
    }
};

// What a parameter reply holds.
struct Param {
    Result result = 0;
    u32 sender = 0, signal = 0, size = 0;
    Handle object = 0;
    std::vector<u8> data;  // `size` bytes
    std::vector<u32> raw;
};

// One session of an APT service, and the commands of the game.
struct Apt {
    Guest& g;
    Handle h;

    // GetLockHandle: [header, result, attributes, state, translate header, handle]
    std::vector<u32> GetLockHandle(u32 attributes) {
        return g.Request(h, {ipc::MakeHeader(0x0001, 1, 0), attributes}, 6);
    }
    struct Registered {
        Result result = 0;
        Handle notification = 0, parameter = 0;
    };
    Registered Initialize(u32 app_id, u32 attributes) {
        const std::vector<u32> r = g.Request(h, {ipc::MakeHeader(0x0002, 2, 0), app_id, attributes}, 5);
        Registered out;
        out.result = r[1];
        if (r[0] == ipc::MakeHeader(0x0002, 1, 3)) {
            CHECK(r[2] == ipc::CopyHandleDesc(2));
            out.notification = r[3];
            out.parameter = r[4];
        } else {
            CHECK(r[0] == ipc::MakeHeader(0x0002, 1, 0));
        }
        return out;
    }
    Result Simple(u16 command, std::vector<u32> params) {
        std::vector<u32> words = {ipc::MakeHeader(command, static_cast<u32>(params.size()), 0)};
        words.insert(words.end(), params.begin(), params.end());
        const std::vector<u32> r = g.Request(h, words, 2);
        CHECK(r[0] == ipc::MakeHeader(command, 1, 0));
        return r[1];
    }
    Result Enable(u32 attributes) { return Simple(0x0003, {attributes}); }
    u32 IsRegistered(u32 app_id) {
        const std::vector<u32> r = g.Request(h, {ipc::MakeHeader(0x0009, 1, 0), app_id}, 3);
        CHECK(r[0] == ipc::MakeHeader(0x0009, 2, 0));
        CHECK(r[1] == kResultSuccess);
        return r[2];
    }

    // The receive buffer 0 of the next request (a static buffer descriptor and its address).
    void SetReceiveBuffer(u32 size, u32 address) {
        g.PutWords(g.Cmd() + 0x100, {ipc::StaticBufferDesc(size, 0), address});
    }
    Param Receive(u32 app_id, u32 buffer_size, bool glance = false) {
        const u16 command = glance ? 0x000E : 0x000D;
        const u32 buffer = g.Scratch(0x400);
        g.PutBytes(buffer, std::vector<u8>(buffer_size, 0xEE));
        SetReceiveBuffer(buffer_size, buffer);
        Param p;
        p.raw = g.Request(h, {ipc::MakeHeader(command, 2, 0), app_id, buffer_size}, 9);
        p.result = p.raw[1];
        if (p.raw[0] == ipc::MakeHeader(command, 4, 4)) {
            p.sender = p.raw[2];
            p.signal = p.raw[3];
            p.size = p.raw[4];
            CHECK(p.raw[5] == ipc::MoveHandleDesc(1));
            p.object = p.raw[6];
            CHECK(p.raw[7] == ipc::StaticBufferDesc(buffer_size, 0));
            CHECK(p.raw[8] == buffer);
            const std::vector<u8> all = g.GetBytes(buffer, buffer_size);
            p.data.assign(all.begin(), all.begin() + std::min<u32>(p.size, buffer_size));
            // the rest of the buffer is zeros: APT always pushes a buffer of the size asked for
            for (u32 i = p.size; i < buffer_size; ++i) {
                CHECK(all[i] == 0);
            }
        } else {
            CHECK(p.raw[0] == ipc::MakeHeader(command, 1, 0));
        }
        return p;
    }
    Result Send(u32 source, u32 destination, u32 signal, Handle object, const std::vector<u8>& data) {
        const u32 buffer = g.Scratch(0x800);
        g.PutBytes(buffer, data);
        const std::vector<u32> r = g.Request(
            h,
            {ipc::MakeHeader(0x000C, 4, 4), source, destination, signal, static_cast<u32>(data.size()),
             ipc::CopyHandleDesc(1), object, ipc::StaticBufferDesc(data.size(), 0), buffer},
            2);
        CHECK(r[0] == ipc::MakeHeader(0x000C, 1, 0));
        return r[1];
    }
    // CancelParameter: the result and whether a parameter was cancelled.
    std::pair<Result, u32> Cancel(bool check_sender, u32 sender, bool check_receiver, u32 receiver) {
        const std::vector<u32> r = g.Request(
            h, {ipc::MakeHeader(0x000F, 4, 0), check_sender, sender, check_receiver, receiver}, 3);
        CHECK(r[0] == ipc::MakeHeader(0x000F, 2, 0));
        return {r[1], r[2]};
    }

    // Registers the application as the game does (GetLockHandle, Initialize, Enable) and takes the
    // Wakeup APT sends it.
    Registered StartApplication() {
        const std::vector<u32> lock = GetLockHandle(0);
        CHECK(lock[1] == kResultSuccess);
        const Registered registered = Initialize(kApplication, 0);
        CHECK(registered.result == kResultSuccess);
        CHECK(Enable(0) == kResultSuccess);
        const Param wakeup = Receive(kApplication, 0x10);
        CHECK(wakeup.result == kResultSuccess);
        CHECK(wakeup.signal == kWakeup);
        return registered;
    }

    // The library applet: PrepareToStartLibraryApplet.
    Result PrepareToStartLibraryApplet(u32 applet) { return Simple(0x0018, {applet}); }
    // StartLibraryApplet(applet, a copy of `object`, the static buffer `data`).
    Result StartLibraryApplet(u32 applet, Handle object, const std::vector<u8>& data) {
        const u32 buffer = g.Scratch(0xC00);
        g.PutBytes(buffer, data);
        const std::vector<u32> r = g.Request(
            h,
            {ipc::MakeHeader(0x001E, 2, 4), applet, static_cast<u32>(data.size()), ipc::CopyHandleDesc(1),
             object, ipc::StaticBufferDesc(data.size(), 0), buffer},
            2);
        CHECK(r[0] == ipc::MakeHeader(0x001E, 1, 0));
        return r[1];
    }
};

// ---- the keyboard's configuration, byte by byte (3dbrew, Software Keyboard) ----------------------------------

namespace swkbd_offsets {
constexpr std::size_t kButtons = 0x04;       // 0: one button, 1: two, 2: three, 3: none
constexpr std::size_t kValidInput = 0x08;
constexpr std::size_t kFilterFlags = 0x18;
constexpr std::size_t kMaxTextLength = 0x20;
constexpr std::size_t kMaxDigits = 0x24;
constexpr std::size_t kSharedMemorySize = 0x130;
constexpr std::size_t kReturnCode = 0x138;
constexpr std::size_t kTextOffset = 0x144;
constexpr std::size_t kTextLength = 0x148;
constexpr std::size_t kCallbackResult = 0x14C;
constexpr std::size_t kCallbackMessage = 0x150;
}  // namespace swkbd_offsets

std::vector<u8> MakeConfig(u32 buttons, u32 max_text = 16, u32 valid_input = 0, u32 filter_flags = 0) {
    std::vector<u8> c(0x400, 0);
    Put32(c, swkbd_offsets::kButtons, buttons);
    Put32(c, swkbd_offsets::kValidInput, valid_input);
    Put32(c, swkbd_offsets::kFilterFlags, filter_flags);
    Put16(c, swkbd_offsets::kMaxTextLength, max_text);
    Put16(c, swkbd_offsets::kMaxDigits, 2);
    Put32(c, swkbd_offsets::kSharedMemorySize, 0x2000);
    return c;
}

std::vector<u8> MakeCaptureInfo(u32 framebuffer_size) {
    std::vector<u8> info(0x20, 0);
    Put32(info, 0, framebuffer_size);
    return info;
}

// The text in the block as UTF-16 until the null terminator.
std::u16string ReadText(Guest& g, u32 address, std::size_t max_units = 64) {
    const std::vector<u8> bytes = g.GetBytes(address, max_units * 2);
    std::u16string text;
    for (std::size_t i = 0; i < max_units; ++i) {
        const u32 unit = Get16(bytes, 2 * i);
        if (unit == 0) {
            break;
        }
        text.push_back(static_cast<char16_t>(unit));
    }
    return text;
}

// A memory block of the application's own memory for the keyboard's text (svcControlMemory commit of
// the heap once, then svcCreateMemoryBlock over a part of it): the address and the handle.
constexpr u32 kHeap = 0x08000000;
struct TextBlock {
    u32 address = 0;
    Handle handle = 0;
};
TextBlock MakeTextBlock(Guest& g, u32 index) {
    if (index == 0) {
        REQUIRE(CallSvc(g.rt, 0x01, {3, kHeap, 0, 0x10000, 3}).r[0] == kResultSuccess);
    }
    TextBlock block;
    block.address = kHeap + 0x2000 * index;
    // fill with a pattern so that what the keyboard writes (and does not) can be told
    g.PutBytes(block.address, std::vector<u8>(0x2000, 0x5A));
    const Regs out = CallSvc(g.rt, 0x1E, {3, block.address, 0x2000, 3});
    REQUIRE(out.r[0] == kResultSuccess);
    block.handle = out.r[1];
    return block;
}

// The whole handshake of a keyboard up to the start: PrepareToStartLibraryApplet, the Request message with
// the framebuffer's size and its Response, StartLibraryApplet with the configuration and the text block.
struct Launch {
    Result prepare = 0, request = 0, start = 0;
    Param response;
};
Launch LaunchKeyboard(Apt& apt, const std::vector<u8>& config, const TextBlock& block,
                      u32 framebuffer_size = 0x8000, u32 applet = kKeyboard) {
    Launch launch;
    launch.prepare = apt.PrepareToStartLibraryApplet(applet);
    launch.request = apt.Send(kApplication, applet, kRequest, 0, MakeCaptureInfo(framebuffer_size));
    launch.response = apt.Receive(kApplication, 0x20);
    launch.start = apt.StartLibraryApplet(applet, block.handle, config);
    return launch;
}

// The keyboard of `config`, run to the end: the closing parameter, and what is left in the block.
struct Finished {
    Launch launch;
    Param closing;
    std::u16string text;
    std::vector<u8> config;  // the configuration the game gets back
};
Finished RunKeyboard(Apt& apt, Guest& g, const std::vector<u8>& config, u32 index,
                     std::size_t max_units = 64) {
    Finished out;
    const TextBlock block = MakeTextBlock(g, index);
    out.launch = LaunchKeyboard(apt, config, block);
    SleepNs(g.rt, TicksToNsCeil(kAppletUpdateIntervalTicks) + 1000);
    out.closing = apt.Receive(kApplication, 0x400);
    out.text = ReadText(g, block.address, max_units);
    out.config = out.closing.data;
    return out;
}

}  // namespace

// ---- registration ----------------------------------------------------------------------------------------------

TEST_CASE("APT:U, APT:A and APT:S exist, with two sessions each") {
    Fixture f;
    TempDir dir;
    Handle sessions[3][3] = {};
    Result third[3] = {};
    Handle again = 0;
    Result again_result = 0;
    const int status = BootWith(f, dir, {"--ms", "20000"}, [&] {
        Guest g(*f.runtime);
        const char* names[3] = {"APT:U", "APT:A", "APT:S"};
        for (int n = 0; n < 3; ++n) {
            g.ServiceResult(names[n], &sessions[n][0]);
            g.ServiceResult(names[n], &sessions[n][1]);
            third[n] = g.ServiceResult(names[n], &sessions[n][2]);
        }
        // closing one session makes room for another
        g.Close(sessions[0][0]);
        again_result = g.ServiceResult("APT:U", &again);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    for (int n = 0; n < 3; ++n) {
        INFO("service " << n);
        CHECK(sessions[n][0] != 0);
        CHECK(sessions[n][1] != 0);
        CHECK(sessions[n][2] == 0);
        CHECK(third[n] == kResultMaxConnectionsReached);
    }
    CHECK(again_result == kResultSuccess);
    CHECK(again != 0);
}

TEST_CASE("the three APT services share one applet manager") {
    Fixture f;
    TempDir dir;
    u32 registered_through_u = 0, registered_through_a = 0;
    const int status = BootWith(f, dir, {"--ms", "20000"}, [&] {
        Guest g(*f.runtime);
        Apt u{g, g.Service("APT:U")};
        Apt a{g, g.Service("APT:A")};
        REQUIRE(u.h != 0);
        REQUIRE(a.h != 0);
        u.StartApplication();
        registered_through_u = u.IsRegistered(kApplication);
        registered_through_a = a.IsRegistered(kApplication);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(registered_through_u == 1);
    CHECK(registered_through_a == 1);
}

// ---- the application's registration and the parameters ----------------------------------------------------------

TEST_CASE("the first applet to register gets the Wakeup signal, and Receive takes it") {
    Fixture f;
    TempDir dir;
    std::vector<u32> lock;
    Apt::Registered registered;
    Result parameter_event_before = 0, parameter_event_after = 0, notification_event = 0;
    Param wakeup, second, glance_empty;
    u32 is_registered_before = 9, is_registered = 0, is_other = 9;
    std::vector<u32> info_before, info_after;
    std::vector<u32> notification;
    const int status = BootWith(f, dir, {"--ms", "20000"}, [&] {
        Guest g(*f.runtime);
        Apt apt{g, g.Service("APT:U")};
        is_registered_before = apt.IsRegistered(kApplication);
        info_before = g.Request(apt.h, {ipc::MakeHeader(0x0005, 1, 0), 0}, 6);
        lock = apt.GetLockHandle(0);
        registered = apt.Initialize(kApplication, 0);
        // Initialize enabled the first applet and sent it the Wakeup
        parameter_event_before = g.Wait(registered.parameter, 0);
        notification_event = g.Wait(registered.notification, 0);
        is_registered = apt.IsRegistered(kApplication);
        is_other = apt.IsRegistered(kKeyboard);
        info_after = g.Request(apt.h, {ipc::MakeHeader(0x0005, 1, 0), 0}, 6);
        notification = g.Request(apt.h, {ipc::MakeHeader(0x000B, 1, 0), kApplication}, 3);
        glance_empty = apt.Receive(0x123, 0x10, true);  // not for this applet
        wakeup = apt.Receive(kApplication, 0x20);
        second = apt.Receive(kApplication, 0x20);
        parameter_event_after = g.Wait(registered.parameter, 0);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(is_registered_before == 0);
    // GetAppletManInfo(Application): nothing active yet
    CHECK(info_before[0] == ipc::MakeHeader(0x0005, 5, 0));
    CHECK(info_before[1] == kResultSuccess);
    CHECK(info_before[2] == 0xFF);   // AppletPos::Invalid
    CHECK(info_before[3] == 0);      // no applet requested
    CHECK(info_before[4] == 0x101);  // the HOME menu's id
    // GetLockHandle: [header, result, corrected attributes, state, translate header, the lock]
    CHECK(lock[0] == ipc::MakeHeader(0x0001, 3, 2));
    CHECK(lock[1] == kResultSuccess);
    CHECK(lock[2] == 0);
    CHECK(lock[3] == 0);
    CHECK(lock[4] == ipc::CopyHandleDesc(1));
    CHECK(lock[5] != 0);
    CHECK(registered.result == kResultSuccess);
    CHECK(registered.notification != 0);
    CHECK(registered.parameter != 0);
    CHECK(registered.notification != registered.parameter);
    CHECK(parameter_event_before == kResultSuccess);  // the Wakeup is waiting
    CHECK(notification_event == kTimeout);      // no notification was sent
    CHECK(is_registered == 1);
    CHECK(is_other == 0);
    CHECK(info_after[1] == kResultSuccess);
    CHECK(info_after[2] == 0);          // the Application is active
    CHECK(info_after[3] == kApplication);  // and is the one asked for
    CHECK(info_after[4] == 0x101);
    CHECK(info_after[5] == kApplication);
    // InquireNotification: nothing
    CHECK(notification[0] == ipc::MakeHeader(0x000B, 2, 0));
    CHECK(notification[1] == kResultSuccess);
    CHECK(notification[2] == 0);
    // a parameter for another applet is not for us
    CHECK(glance_empty.result == kNotFound);
    CHECK(wakeup.result == kResultSuccess);
    CHECK(wakeup.sender == 0);
    CHECK(wakeup.signal == kWakeup);
    CHECK(wakeup.size == 0);
    CHECK(wakeup.object == 0);
    CHECK(second.result == kNoData);  // received: gone
    CHECK(parameter_event_after == kTimeout);  // a one-shot event: the first wait consumed the signal
}

TEST_CASE("SendParameter, GlanceParameter, ReceiveParameter and CancelParameter") {
    Fixture f;
    TempDir dir;
    Result first = 0, blocked = 0;
    Param glanced, wrong_destination, received, gone;
    std::pair<Result, u32> cancel_nothing, cancel_wrong_sender, cancel_wrong_receiver, cancel_ok, cancel_again;
    Result after_cancel = 0;
    Param cut;
    std::vector<u8> payload(0x30);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<u8>(i + 1);
    }
    const int status = BootWith(f, dir, {"--ms", "20000"}, [&] {
        Guest g(*f.runtime);
        Apt apt{g, g.Service("APT:U")};
        apt.StartApplication();
        cancel_nothing = apt.Cancel(false, 0, false, 0);
        // some applet sends the application a message with a buffer
        first = apt.Send(kKeyboard, kApplication, kMessage, 0, payload);
        blocked = apt.Send(kKeyboard, kApplication, kMessage, 0, payload);  // one at a time
        glanced = apt.Receive(kApplication, 0x40, true);
        wrong_destination = apt.Receive(kKeyboard, 0x40);
        received = apt.Receive(kApplication, 0x40);
        gone = apt.Receive(kApplication, 0x40);
        // a parameter is sent again, and cancelled with the checks
        apt.Send(0x201, kApplication, kMessage, 0, payload);
        cancel_wrong_sender = apt.Cancel(true, 0x202, false, 0);
        cancel_wrong_receiver = apt.Cancel(false, 0, true, 0x123);
        cancel_ok = apt.Cancel(true, 0x201, true, kApplication);
        cancel_again = apt.Cancel(false, 0, false, 0);
        after_cancel = apt.Send(0x201, kApplication, kMessage, 0, payload);
        // the receive buffer is smaller than the parameter: the size says so, the buffer holds the start
        cut = apt.Receive(kApplication, 0x10);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(cancel_nothing == std::pair<Result, u32>{kResultSuccess, 0});
    CHECK(first == kResultSuccess);
    CHECK(blocked == kParameterPresent);
    CHECK(glanced.result == kResultSuccess);
    CHECK(glanced.sender == kKeyboard);
    CHECK(glanced.signal == kMessage);
    CHECK(glanced.size == 0x30);
    CHECK(glanced.data == payload);
    CHECK(wrong_destination.result == kNotFound);
    CHECK(received.result == kResultSuccess);  // Glance did not take it
    CHECK(received.data == payload);
    CHECK(gone.result == kNoData);
    CHECK(cancel_wrong_sender == std::pair<Result, u32>{kResultSuccess, 0});
    CHECK(cancel_wrong_receiver == std::pair<Result, u32>{kResultSuccess, 0});
    CHECK(cancel_ok == std::pair<Result, u32>{kResultSuccess, 1});
    CHECK(cancel_again == std::pair<Result, u32>{kResultSuccess, 0});
    CHECK(after_cancel == kResultSuccess);
    CHECK(cut.result == kResultSuccess);
    CHECK(cut.size == 0x10);
    CHECK(cut.data == std::vector<u8>(payload.begin(), payload.begin() + 0x10));
}

TEST_CASE("Initialize and Enable refuse what Azahar refuses") {
    Fixture f;
    TempDir dir;
    Apt::Registered twice, bad_position;
    Result enable_bad = 0, prepare_while_pending = 0, prepare_after = 0;
    const int status = BootWith(f, dir, {"--ms", "20000"}, [&] {
        Guest g(*f.runtime);
        Apt apt{g, g.Service("APT:U")};
        apt.StartApplication();
        twice = apt.Initialize(kApplication, 0);  // the slot is taken
        bad_position = apt.Initialize(0x555, 4);   // Resident has no slot (Azahar asserts)
        enable_bad = apt.Enable(7);                // neither does position 7
        // a library applet cannot be prepared while a parameter waits for somebody
        apt.Send(kKeyboard, kApplication, kMessage, 0, {});
        prepare_while_pending = apt.PrepareToStartLibraryApplet(kKeyboard);
        apt.Receive(kApplication, 0x10);
        prepare_after = apt.PrepareToStartLibraryApplet(kKeyboard);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(twice.result == kAlreadyExists);
    CHECK(bad_position.result == kInvalidAppletSlot);
    CHECK(enable_bad == kInvalidAppletSlot);
    CHECK(prepare_while_pending == kParameterPresent);
    CHECK(prepare_after == kResultSuccess);
}

// ---- the library applet flow -------------------------------------------------------------------------------------

TEST_CASE("the software keyboard answers with the user name after one update tick, with the Response and "
          "the closing parameter as Azahar sends them") {
    Fixture f;
    TempDir dir;
    Launch launch;
    Param before_tick, closing;
    std::u16string text_before;
    std::u16string text_after;
    std::vector<u8> block_tail;
    Result second_prepare = 0;
    u64 created_at = 0, awake_at = 0;
    std::shared_ptr<SharedMemory> response_memory;
    Result close_response_handle = 0;
    std::vector<u32> app_info;
    const int status = BootWith(f, dir, {"--ms", "20000"}, [&] {
        Guest g(*f.runtime);
        Apt apt{g, g.Service("APT:U")};
        apt.StartApplication();
        const TextBlock block = MakeTextBlock(g, 0);

        created_at = Now(g.rt);
        launch = LaunchKeyboard(apt, MakeConfig(0), block, 0x8000);
        response_memory = g.rt.GetKernel().GetObject<SharedMemory>(launch.response.object);
        // a second PrepareToStartLibraryApplet finds the slot taken
        second_prepare = apt.PrepareToStartLibraryApplet(kKeyboard);
        // just before the first update tick nothing has happened
        SleepToTick(g.rt, created_at + kAppletUpdateIntervalTicks - 1);
        text_before = ReadText(g, block.address, 8);
        before_tick = apt.Receive(kApplication, 0x400);
        // and just after it the keyboard has answered
        SleepNs(g.rt, 40);  // at least 10 ticks
        awake_at = Now(g.rt);
        closing = apt.Receive(kApplication, 0x400);
        text_after = ReadText(g, block.address, 8);
        block_tail = g.GetBytes(block.address + 14, 2);
        close_response_handle = g.Close(launch.response.object);
        app_info = g.Request(apt.h, {ipc::MakeHeader(0x0005, 1, 0), 0}, 6);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    // PrepareToStartLibraryApplet, the Request message, the Response and StartLibraryApplet
    CHECK(launch.prepare == kResultSuccess);
    CHECK(launch.request == kResultSuccess);
    CHECK(launch.response.result == kResultSuccess);
    CHECK(launch.response.sender == kKeyboard);
    CHECK(launch.response.signal == kResponse);
    CHECK(launch.response.size == 0);
    CHECK(launch.response.object != 0);  // the shared memory for the framebuffer info
    REQUIRE(response_memory != nullptr);
    CHECK(response_memory->Size() == 0x8000);
    CHECK(close_response_handle == kResultSuccess);
    CHECK(launch.start == kResultSuccess);
    CHECK(second_prepare == kAlreadyExists);
    // not before the update tick
    CHECK(text_before.empty() == false);  // still the pattern the test wrote (0x5A5A)
    CHECK(text_before == std::u16string(8, char16_t(0x5A5A)));
    CHECK(before_tick.result == kNoData);
    // the closing parameter: WakeupByExit from the keyboard with the 0x400 configuration
    CHECK(awake_at >= created_at + kAppletUpdateIntervalTicks);
    CHECK(closing.result == kResultSuccess);
    CHECK(closing.sender == kKeyboard);
    CHECK(closing.signal == kWakeupByExit);
    CHECK(closing.size == 0x400);
    CHECK(closing.object == 0);
    REQUIRE(closing.data.size() == 0x400);
    CHECK(static_cast<s32>(Get32(closing.data, swkbd_offsets::kReturnCode)) == 0);  // D0Click
    CHECK(Get16(closing.data, swkbd_offsets::kTextLength) == 6);
    CHECK(Get32(closing.data, swkbd_offsets::kTextOffset) == 0);
    // the text: the cfg user name, UTF-16 with a terminator, at the start of the block
    CHECK(text_after == u"AZAHAR");
    CHECK(block_tail == std::vector<u8>{0x5A, 0x5A});  // nothing after the terminator is touched
    // the application is the active applet again
    CHECK(app_info[2] == 0);
    CHECK(app_info[5] == kApplication);
}

TEST_CASE("the keyboard's updates come exactly one period after the applet was made, and a period apart") {
    Fixture f;
    TempDir dir;
    // The Message of the callback filter comes at each update tick: the first, and the one a period later
    // when the game answers Continue.
    Param first_message, second_message;
    std::vector<u32> arrival_before_first, arrival_before_second;
    u64 created_at = 0;
    Result before_first = 0, before_second = 0, send_continue = 0;
    const int status = BootWith(f, dir, {"--ms", "20000"}, [&] {
        Guest g(*f.runtime);
        Apt apt{g, g.Service("APT:U")};
        apt.StartApplication();
        const TextBlock block = MakeTextBlock(g, 0);
        created_at = Now(g.rt);
        LaunchKeyboard(apt, MakeConfig(0, 16, 0, 0x20), block);  // filter flag: callback
        SleepToTick(g.rt, created_at + kAppletUpdateIntervalTicks - 1);
        before_first = apt.Receive(kApplication, 0x400).result;
        SleepNs(g.rt, 40);
        first_message = apt.Receive(kApplication, 0x400);
        // The game answers "continue" half a period later. The next update is due at 2 periods after the
        // creation, whenever the game answers.
        SleepNs(g.rt, TicksToNs(kAppletUpdateIntervalTicks / 2));
        std::vector<u8> verdict = first_message.data;
        Put32(verdict, swkbd_offsets::kCallbackResult, 2);  // Continue
        send_continue = apt.Send(kApplication, kKeyboard, kMessage, 0, verdict);
        SleepToTick(g.rt, created_at + 2 * kAppletUpdateIntervalTicks - 1);
        before_second = apt.Receive(kApplication, 0x400).result;
        SleepNs(g.rt, 40);
        second_message = apt.Receive(kApplication, 0x400);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(before_first == kNoData);
    CHECK(first_message.result == kResultSuccess);
    CHECK(first_message.signal == kMessage);
    CHECK(first_message.sender == kKeyboard);
    CHECK(first_message.size == 0x400);
    CHECK(send_continue == kResultSuccess);
    CHECK(before_second == kNoData);
    CHECK(second_message.result == kResultSuccess);
    CHECK(second_message.signal == kMessage);
}

TEST_CASE("each button configuration answers with the button of Azahar's default keyboard") {
    struct Case {
        u32 buttons;
        s32 return_code;
    };
    // one button: D0Click; two: the right one, D1Click1; three: the right one, D2Click2; none: -1
    for (const Case& c : {Case{0, 0}, Case{1, 2}, Case{2, 5}, Case{3, -1}}) {
        Fixture f;
        TempDir dir;
        Finished result;
        const int status = BootWith(f, dir, {"--ms", "20000"}, [&] {
            Guest g(*f.runtime);
            Apt apt{g, g.Service("APT:U")};
            apt.StartApplication();
            result = RunKeyboard(apt, g, MakeConfig(c.buttons), 0);
            ExitProcess(*f.runtime);
        });
        INFO("buttons " << c.buttons);
        CHECK(status == 0);
        CHECK(result.launch.start == kResultSuccess);
        CHECK(result.closing.signal == kWakeupByExit);
        REQUIRE(result.config.size() == 0x400);
        CHECK(static_cast<s32>(Get32(result.config, swkbd_offsets::kReturnCode)) == c.return_code);
        CHECK(Get16(result.config, swkbd_offsets::kTextLength) == 6);
        CHECK(result.text == u"AZAHAR");
    }
}

TEST_CASE("a text longer than the configuration's maximum is cut down") {
    Fixture f;
    TempDir dir;
    Finished result;
    std::vector<u8> after;
    const int status = BootWith(f, dir, {"--ms", "20000", "--swkbd-text", "ABCDEFGHIJKL"}, [&] {
        Guest g(*f.runtime);
        Apt apt{g, g.Service("APT:U")};
        apt.StartApplication();
        result = RunKeyboard(apt, g, MakeConfig(1, 5), 0);
        after = g.GetBytes(kHeap, 16);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(static_cast<s32>(Get32(result.config, swkbd_offsets::kReturnCode)) == 2);
    CHECK(Get16(result.config, swkbd_offsets::kTextLength) == 5);
    CHECK(result.text == u"ABCDE");
    // five units, the terminator, and then the block is as it was
    CHECK(Get16(after, 10) == 0);
    CHECK(Get16(after, 12) == 0x5A5A);
}

TEST_CASE("--swkbd-text gives the Nth keyboard the Nth text, and the user name is the default") {
    Fixture f;
    TempDir dir;
    // a user whose name is "Hunter" (the cfg savegame of the user folder)
    {
        std::vector<u8> file(0x8000, 0);
        Put16(file, 0, 1);
        Put16(file, 2, 0x455C);
        Put32(file, 4, 0x000A0000);  // the user name block
        Put32(file, 8, 0x455C);
        Put16(file, 12, 0x1C);
        Put16(file, 14, 14);
        const std::u16string name = u"Hunter";
        for (std::size_t i = 0; i < name.size(); ++i) {
            Put16(file, 0x455C + 2 * i, name[i]);
        }
        WriteAll(dir.File("user/nand/data/00000000000000000000000000000000/sysdata/00010017/00000000/config"),
                 file);
    }
    std::vector<Finished> results(4);
    const int status = BootWith(f, dir, {"--ms", "20000", "--swkbd-text", "First", "--swkbd-text", "caf\xC3\xA9"},
                                [&] {
                                    Guest g(*f.runtime);
                                    Apt apt{g, g.Service("APT:U")};
                                    apt.StartApplication();
                                    for (u32 i = 0; i < 4; ++i) {
                                        results[i] = RunKeyboard(apt, g, MakeConfig(0), i);
                                    }
                                    ExitProcess(*f.runtime);
                                });
    CHECK(status == 0);
    for (const Finished& r : results) {
        CHECK(r.launch.prepare == kResultSuccess);
        CHECK(r.launch.start == kResultSuccess);
        CHECK(r.closing.signal == kWakeupByExit);
    }
    CHECK(results[0].text == u"First");
    CHECK(results[1].text == u"café");
    CHECK(Get16(results[1].config, swkbd_offsets::kTextLength) == 4);
    CHECK(results[2].text == u"Hunter");  // no more texts: the cfg user name
    CHECK(results[3].text == u"Hunter");
}

TEST_CASE("the keyboard never finishes when the default keyboard's checks reject the text, as in Azahar") {
    Fixture f;
    TempDir dir;
    Param after_a_long_time;
    std::vector<u8> block_after;
    Result start = 0;
    const int status = BootWith(f, dir, {"--ms", "20000", "--swkbd-text", "has@sign"}, [&] {
        Guest g(*f.runtime);
        Apt apt{g, g.Service("APT:U")};
        apt.StartApplication();
        const TextBlock block = MakeTextBlock(g, 0);
        // filter flag 2: no '@'
        start = LaunchKeyboard(apt, MakeConfig(0, 16, 0, 2), block).start;
        SleepNs(g.rt, 10 * TicksToNsCeil(kAppletUpdateIntervalTicks));
        after_a_long_time = apt.Receive(kApplication, 0x400);
        block_after = g.GetBytes(block.address, 4);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(start == kResultSuccess);
    CHECK(after_a_long_time.result == kNoData);
    CHECK(block_after == std::vector<u8>(4, 0x5A));
    CHECK(f.Logged("the default keyboard's text is not accepted"));
}

TEST_CASE("the callback filter: the Message goes to the game, and its verdict OK, Close or Continue is "
          "handled as Azahar's applet handles it") {
    struct Verdict {
        u32 value;  // callback_result
        bool finishes;
        s32 return_code;  // of the closing parameter
        u32 text_length;
    };
    for (const Verdict& v : {Verdict{0, true, 0, 6}, Verdict{1, true, 30, 0}}) {
        Fixture f;
        TempDir dir;
        Param message, closing, nothing_more;
        Result send = 0;
        const int status = BootWith(f, dir, {"--ms", "20000"}, [&] {
            Guest g(*f.runtime);
            Apt apt{g, g.Service("APT:U")};
            apt.StartApplication();
            const TextBlock block = MakeTextBlock(g, 0);
            LaunchKeyboard(apt, MakeConfig(0, 16, 0, 0x20), block);
            SleepNs(g.rt, TicksToNsCeil(kAppletUpdateIntervalTicks) + 1000);
            message = apt.Receive(kApplication, 0x400);
            std::vector<u8> verdict = message.data;
            Put32(verdict, swkbd_offsets::kCallbackResult, v.value);
            Put16(verdict, swkbd_offsets::kCallbackMessage, 'n');
            Put16(verdict, swkbd_offsets::kCallbackMessage + 2, 'o');
            send = apt.Send(kApplication, kKeyboard, kMessage, 0, verdict);
            closing = apt.Receive(kApplication, 0x400);
            SleepNs(g.rt, 3 * TicksToNsCeil(kAppletUpdateIntervalTicks));
            nothing_more = apt.Receive(kApplication, 0x400);
            ExitProcess(*f.runtime);
        });
        INFO("verdict " << v.value);
        CHECK(status == 0);
        CHECK(message.result == kResultSuccess);
        CHECK(message.signal == kMessage);
        CHECK(message.sender == kKeyboard);
        REQUIRE(message.data.size() == 0x400);
        CHECK(static_cast<s32>(Get32(message.data, swkbd_offsets::kReturnCode)) == 0);
        CHECK(Get16(message.data, swkbd_offsets::kTextLength) == 6);
        CHECK(send == kResultSuccess);
        CHECK(closing.result == kResultSuccess);
        CHECK(closing.signal == kWakeupByExit);
        CHECK(closing.sender == kKeyboard);
        CHECK(static_cast<s32>(Get32(closing.data, swkbd_offsets::kReturnCode)) == v.return_code);
        CHECK(Get16(closing.data, swkbd_offsets::kTextLength) == v.text_length);
        CHECK(nothing_more.result == kNoData);
        if (v.value == 1) {
            CHECK(f.Logged("Default keyboard text is unaccepted! error: no"));
        }
    }
}

TEST_CASE("CancelLibraryApplet stops the keyboard before its update, and a new one can start") {
    Fixture f;
    TempDir dir;
    Param cancelled, nothing_more;
    Result cancel_result = 0, cancel_without = 0, cancel_twice = 0, second_prepare = 0;
    Finished second;
    const int status = BootWith(f, dir, {"--ms", "20000", "--swkbd-text", "one", "--swkbd-text", "two"}, [&] {
        Guest g(*f.runtime);
        Apt apt{g, g.Service("APT:U")};
        apt.StartApplication();
        cancel_without = apt.Simple(0x003B, {0});  // no library applet is registered
        const TextBlock block = MakeTextBlock(g, 0);
        LaunchKeyboard(apt, MakeConfig(0), block);
        SleepNs(g.rt, TicksToNsCeil(kAppletUpdateIntervalTicks) / 4);
        cancel_result = apt.Simple(0x003B, {0});
        cancel_twice = apt.Simple(0x003B, {0});  // the first one's parameter has not been received
        cancelled = apt.Receive(kApplication, 0x400);
        SleepNs(g.rt, 3 * TicksToNsCeil(kAppletUpdateIntervalTicks));
        nothing_more = apt.Receive(kApplication, 0x400);
        second_prepare = apt.PrepareToStartLibraryApplet(kKeyboard);
        // finish that one properly: the second keyboard gets the second text
        apt.Send(kApplication, kKeyboard, kRequest, 0, MakeCaptureInfo(0x4000));
        apt.Receive(kApplication, 0x20);
        const TextBlock block2 = MakeTextBlock(g, 1);
        apt.StartLibraryApplet(kKeyboard, block2.handle, MakeConfig(0));
        SleepNs(g.rt, TicksToNsCeil(kAppletUpdateIntervalTicks) + 1000);
        second.closing = apt.Receive(kApplication, 0x400);
        second.text = ReadText(g, block2.address);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(cancel_without == kInvalidAppletSlot);
    CHECK(cancel_result == kResultSuccess);
    CHECK(cancel_twice == kParameterPresent);
    // the applet closed at once, and the game got the exit message with the configuration as it was
    CHECK(cancelled.result == kResultSuccess);
    CHECK(cancelled.signal == kWakeupByExit);
    CHECK(cancelled.sender == kKeyboard);
    CHECK(cancelled.size == 0x400);
    CHECK(nothing_more.result == kNoData);  // and it never answered
    CHECK(second_prepare == kResultSuccess);
    CHECK(second.closing.signal == kWakeupByExit);
    // the cancelled keyboard had started and taken the first text; this is the second
    CHECK(second.text == u"two");
}

TEST_CASE("the keyboard does not write past the shared memory block the game gave") {
    Fixture f;
    TempDir dir;
    Finished result;
    std::vector<u8> after;
    // a page holds 4096 units: with the terminator, 4095 characters of a text of 5000
    const int status = BootWith(f, dir, {"--ms", "20000", "--swkbd-text", std::string(5000, 'a')}, [&] {
        Guest g(*f.runtime);
        Apt apt{g, g.Service("APT:U")};
        apt.StartApplication();
        result = RunKeyboard(apt, g, MakeConfig(0, 0xFFFF), 0, 5000);
        after = g.GetBytes(kHeap + 0x2000, 8);  // what follows the block (never written by the test)
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(result.text == std::u16string(4095, u'a'));
    CHECK(Get16(result.config, swkbd_offsets::kTextLength) == 4095);
    CHECK(after == std::vector<u8>(8, 0));
    CHECK(f.Logged("does not fit the shared memory block"));
}

TEST_CASE("SoftwareKeyboard1 (0x201) is a keyboard too, and GetLockHandle corrects a library position") {
    Fixture f;
    TempDir dir;
    std::vector<u32> before, during, after;
    Launch launch;
    Param closing;
    const int status = BootWith(f, dir, {"--ms", "20000"}, [&] {
        Guest g(*f.runtime);
        Apt apt{g, g.Service("APT:U")};
        apt.StartApplication();
        // AutoLibrary (5), asked for by the applet itself: with nobody having started a library applet it
        // is a system library applet (3); after the application started one, a library applet (1)
        before = apt.GetLockHandle(5);
        const TextBlock block = MakeTextBlock(g, 0);
        launch = LaunchKeyboard(apt, MakeConfig(0), block, 0x4000, 0x201);
        during = apt.GetLockHandle(5);
        SleepNs(g.rt, TicksToNsCeil(kAppletUpdateIntervalTicks) + 1000);
        closing = apt.Receive(kApplication, 0x400);
        after = apt.GetLockHandle(2);  // other positions are left as they are
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(before[1] == kResultSuccess);
    CHECK(before[2] == 3);  // the last launcher slot is still the error slot: not the application's
    CHECK(launch.prepare == kResultSuccess);
    CHECK(launch.response.sender == 0x201);
    CHECK(launch.start == kResultSuccess);
    CHECK(during[2] == 1);
    CHECK(closing.sender == 0x201);
    CHECK(closing.signal == kWakeupByExit);
    CHECK(after[2] == 2);
}

TEST_CASE("a keyboard started with a parameter of the wrong size does not start, and nothing crashes") {
    Fixture f;
    TempDir dir;
    Result prepare = 0, bad_request = 0, start = 0;
    Param response, closing;
    const int status = BootWith(f, dir, {"--ms", "20000"}, [&] {
        Guest g(*f.runtime);
        Apt apt{g, g.Service("APT:U")};
        apt.StartApplication();
        const TextBlock block = MakeTextBlock(g, 0);
        prepare = apt.PrepareToStartLibraryApplet(kKeyboard);
        // a Request message that is not a capture buffer info: Azahar asserts, here it is refused
        bad_request = apt.Send(kApplication, kKeyboard, kRequest, 0, std::vector<u8>(0x10, 1));
        response = apt.Receive(kApplication, 0x20);
        // a configuration of 0x100 bytes
        start = apt.StartLibraryApplet(kKeyboard, block.handle, std::vector<u8>(0x100, 0));
        SleepNs(g.rt, 5 * TicksToNsCeil(kAppletUpdateIntervalTicks));
        closing = apt.Receive(kApplication, 0x400);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(prepare == kResultSuccess);
    CHECK(bad_request == kResultSuccess);  // SendParameter itself worked
    CHECK(response.result == kNoData);     // and the applet did not answer
    CHECK(start == kResultSuccess);
    CHECK(closing.result == kNoData);
    CHECK(f.Logged("The Request parameter has 16 bytes"));
    CHECK(f.Logged("The size of the parameter (SoftwareKeyboardConfig) is wrong: 256 bytes"));
}

TEST_CASE("another library applet cannot be made, and the system applet starters fail as Azahar's do without "
          "the title") {
    Fixture f;
    TempDir dir;
    Result mii = 0, error_applet = 0, prepare_system = 0, start_system = 0, then_keyboard = 0;
    const int status = BootWith(f, dir, {"--ms", "20000"}, [&] {
        Guest g(*f.runtime);
        Apt apt{g, g.Service("APT:U")};
        apt.StartApplication();
        mii = apt.PrepareToStartLibraryApplet(0x402);  // the Mii selector (Azahar has an HLE one)
        error_applet = apt.PrepareToStartLibraryApplet(0x406);
        prepare_system = apt.Simple(0x0019, {0x101});
        const std::vector<u32> r = g.Request(
            apt.h, {ipc::MakeHeader(0x001F, 2, 4), 0x101, 0, ipc::CopyHandleDesc(1), 0, ipc::StaticBufferDesc(0, 0), g.Scratch(0xC00)},
            2);
        start_system = r[1];
        then_keyboard = apt.PrepareToStartLibraryApplet(kKeyboard);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(mii == kNotSupported);
    CHECK(error_applet == kNotSupported);
    CHECK(prepare_system == kResultSuccess);
    CHECK(start_system == kNotSupported);
    CHECK(then_keyboard == kResultSuccess);
}

// ---- the other commands the game sends --------------------------------------------------------------------------------

TEST_CASE("the small commands: capture info, sleep replies, NotifyToWait, AppletUtility, the screenshot "
          "permission, the platform") {
    Fixture f;
    TempDir dir;
    Result capture = 0, sleep_query = 0, sleep_done = 0, wait = 0, permission = 0;
    std::vector<u8> utility6, utility7;
    std::vector<u32> platform, mode_before, mode_after;
    const int status = BootWith(f, dir, {"--ms", "20000"}, [&] {
        Guest g(*f.runtime);
        Apt apt{g, g.Service("APT:U")};
        mode_before = g.Request(apt.h, {ipc::MakeHeader(0x0103 | 0, 0, 0)}, 3);
        apt.StartApplication();
        // SendCaptureBufferInfo(size, static buffer)
        g.PutBytes(g.Scratch(0xC00), MakeCaptureInfo(0x4000));
        const std::vector<u32> r = g.Request(
            apt.h,
            {ipc::MakeHeader(0x0040, 1, 2), 0x20, ipc::StaticBufferDesc(0x20, 0), g.Scratch(0xC00)}, 2);
        CHECK(r[0] == ipc::MakeHeader(0x0040, 1, 0));
        capture = r[1];
        sleep_query = apt.Simple(0x003E, {kApplication, 1});
        sleep_done = apt.Simple(0x003F, {kApplication});
        wait = apt.Simple(0x0043, {kApplication});
        permission = apt.Simple(0x0055, {2});
        // AppletUtility(command, input size, output size, input buffer): TryLockTransition says yes
        const auto utility = [&](u32 command, u32 output_size) -> std::pair<std::vector<u32>, std::vector<u8>> {
            g.PutBytes(g.Scratch(0x400), std::vector<u8>(output_size, 0xEE));
            g.PutWords(g.Cmd() + 0x100, {ipc::StaticBufferDesc(output_size, 0), g.Scratch(0x400)});
            g.PutBytes(g.Scratch(0xC00), std::vector<u8>(4, 1));
            const std::vector<u32> reply = g.Request(
                apt.h,
                {ipc::MakeHeader(0x004B, 3, 2), command, 4, output_size, ipc::StaticBufferDesc(4, 1),
                 g.Scratch(0xC00)},
                5);
            CHECK(reply[0] == ipc::MakeHeader(0x004B, 2, 2));
            CHECK(reply[1] == kResultSuccess);
            CHECK(reply[2] == kResultSuccess);
            return std::make_pair(reply, g.GetBytes(g.Scratch(0x400), output_size));
        };
        const auto six = utility(6, 4);
        const auto seven = utility(7, 4);
        utility6 = six.second;
        utility7 = seven.second;
        platform = g.Request(apt.h, {ipc::MakeHeader(0x0101, 0, 0)}, 3);
        mode_after = g.Request(apt.h, {ipc::MakeHeader(0x0103, 0, 0)}, 3);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(mode_before[0] == ipc::MakeHeader(0x0103, 2, 0));
    CHECK(mode_before[2] == 0);  // no application
    CHECK(capture == kResultSuccess);
    CHECK(sleep_query == kResultSuccess);
    CHECK(sleep_done == kResultSuccess);
    CHECK(wait == kResultSuccess);
    CHECK(permission == kResultSuccess);
    CHECK(utility6 == std::vector<u8>{1, 0, 0, 0});  // TryLockTransition succeeds
    CHECK(utility7 == std::vector<u8>(4, 0));
    CHECK(platform[0] == ipc::MakeHeader(0x0101, 2, 0));
    CHECK(platform[1] == kResultSuccess);
    CHECK(platform[2] == 0);  // an Old 3DS
    CHECK(mode_after[1] == kResultSuccess);
    CHECK(mode_after[2] == 1);  // Old3dsRegistered
}

TEST_CASE("SetAppCpuTimeLimit and GetAppCpuTimeLimit go to the core-1 limiter") {
    Fixture f;
    TempDir dir;
    std::vector<u32> get_initial, set_20, get_20, set_too_big, get_after_big, set_off, get_off;
    u32 limiter_value = 99;
    bool active_after_set = false, active_after_off = true;
    const int status = BootWith(f, dir, {"--ms", "20000"}, [&] {
        Guest g(*f.runtime);
        Apt apt{g, g.Service("APT:U")};
        // A limit of 30 % and a current value of 0 (a multi-mode title that asks for 30)
        g.rt.Threads().ApplyAppMaxCpuSetting(0, 1, 30);
        get_initial = g.Request(apt.h, {ipc::MakeHeader(0x0050, 1, 0), 1}, 3);
        set_20 = g.Request(apt.h, {ipc::MakeHeader(0x004F, 2, 0), 1, 20}, 2);
        get_20 = g.Request(apt.h, {ipc::MakeHeader(0x0050, 1, 0), 1}, 3);
        limiter_value = g.rt.Threads().AppCpuTimeLimit();
        active_after_set = g.rt.Threads().Core1LimiterActive();
        // above the limit value: nothing happens, and the result is still a success
        set_too_big = g.Request(apt.h, {ipc::MakeHeader(0x004F, 2, 0), 1, 31}, 2);
        get_after_big = g.Request(apt.h, {ipc::MakeHeader(0x0050, 1, 0), 1}, 3);
        // as an unsigned number: -1 is above too
        g.Request(apt.h, {ipc::MakeHeader(0x004F, 2, 0), 1, 0xFFFFFFFF}, 2);
        set_off = g.Request(apt.h, {ipc::MakeHeader(0x004F, 2, 0), 1, 0}, 2);
        get_off = g.Request(apt.h, {ipc::MakeHeader(0x0050, 1, 0), 1}, 3);
        active_after_off = g.rt.Threads().Core1LimiterActive();
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(get_initial[0] == ipc::MakeHeader(0x0050, 2, 0));
    CHECK(get_initial[1] == kResultSuccess);
    CHECK(get_initial[2] == 0);
    CHECK(set_20[0] == ipc::MakeHeader(0x004F, 1, 0));
    CHECK(set_20[1] == kResultSuccess);
    CHECK(get_20[2] == 20);
    CHECK(limiter_value == 20);
    CHECK(active_after_set);
    CHECK(set_too_big[1] == kResultSuccess);
    CHECK(get_after_big[2] == 20);
    CHECK(set_off[1] == kResultSuccess);
    CHECK(get_off[2] == 0);
    CHECK_FALSE(active_after_off);
}

TEST_CASE("jumping to the HOME menu leaves a parameter nobody receives, as in Azahar") {
    Fixture f;
    TempDir dir;
    Result prepare = 0, jump = 0, blocked = 0;
    const int status = BootWith(f, dir, {"--ms", "20000"}, [&] {
        Guest g(*f.runtime);
        Apt apt{g, g.Service("APT:U")};
        apt.StartApplication();
        prepare = apt.Simple(0x002B, {});
        const std::vector<u32> r = g.Request(
            apt.h, {ipc::MakeHeader(0x002C, 1, 4), 0, ipc::CopyHandleDesc(1), 0, ipc::StaticBufferDesc(0, 0), g.Scratch(0xC00)}, 2);
        jump = r[1];
        blocked = apt.Send(kApplication, 0x201, kMessage, 0, {});
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(prepare == kResultSuccess);
    CHECK(jump == kResultSuccess);
    // JumpToHomeMenu sent WakeupByPause to the HOME menu, which is not there to take it
    CHECK(blocked == kParameterPresent);
    CHECK(f.Logged("The Home Menu failed to launch"));
}

TEST_CASE("closing the application with no HOME menu ends the run, and an application jump does too") {
    for (const bool jump : {false, true}) {
        Fixture f;
        TempDir dir;
        Result prepare = 0, close = 0;
        const int status = BootWith(f, dir, {"--ms", "20000"}, [&] {
            Guest g(*f.runtime);
            Apt apt{g, g.Service("APT:U")};
            apt.StartApplication();
            if (jump) {
                // PrepareToDoApplicationJump(flags 0, title id, media type 2)
                prepare = apt.Simple(0x0031, {0, 0x12345678, 0x00040000, 2});
                g.PutBytes(g.Scratch(0xC00), std::vector<u8>(8, 0));
                const std::vector<u32> r = g.Request(
                    apt.h,
                    {ipc::MakeHeader(0x0032, 2, 4), 8, 0x20, ipc::StaticBufferDesc(8, 0), g.Scratch(0xC00),
                     ipc::StaticBufferDesc(0x20, 1), g.Scratch(0xC00)},
                    2);
                close = r[1];
            } else {
                // PrepareToCloseApplication(return to the system), CloseApplication(0, handle 0, no buffer)
                prepare = apt.Simple(0x0022, {1});
                const std::vector<u32> r = g.Request(
                    apt.h,
                    {ipc::MakeHeader(0x0027, 1, 4), 0, ipc::CopyHandleDesc(1), 0, ipc::StaticBufferDesc(0, 0), g.Scratch(0xC00)},
                    2);
                close = r[1];
            }
            ExitProcess(*f.runtime);
        });
        INFO((jump ? "DoApplicationJump" : "CloseApplication"));
        CHECK(status == 0);
        CHECK(prepare == kResultSuccess);
        CHECK(close == kResultSuccess);
        REQUIRE(f.runtime->ExitStatus().has_value());
        CHECK(*f.runtime->ExitStatus() == 0);
        CHECK(f.Logged(jump ? "DoApplicationJump asks for a system reset"
                            : "the application closed and there is no HOME menu"));
    }
}

TEST_CASE("PrepareToCloseApplication needs the application to be the active applet") {
    Fixture f;
    TempDir dir;
    Result before_registering = 0, jump_stored_flags = 0;
    const int status = BootWith(f, dir, {"--ms", "20000"}, [&] {
        Guest g(*f.runtime);
        Apt apt{g, g.Service("APT:U")};
        before_registering = apt.Simple(0x0022, {1});
        apt.StartApplication();
        jump_stored_flags = apt.Simple(0x0031, {1, 0, 0, 2});  // flags 1: Azahar asserts
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK(before_registering == kInvalidAppletSlot);
    CHECK(jump_stored_flags == 0xFFFFFFFF);
}

TEST_CASE("the commands the game does not send are answered as unimplemented ones") {
    Fixture f;
    TempDir dir;
    std::vector<u32> shared_font, finalize, sleep_system;
    const int status = BootWith(f, dir, {"--ms", "20000"}, [&] {
        Guest g(*f.runtime);
        Apt apt{g, g.Service("APT:U")};
        shared_font = g.Request(apt.h, {ipc::MakeHeader(0x0044, 0, 0)}, 3);
        finalize = g.Request(apt.h, {ipc::MakeHeader(0x0004, 1, 0), kApplication}, 3);
        sleep_system = g.Request(apt.h, {ipc::MakeHeader(0x0042, 2, 0), 0, 0}, 3);
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    for (const std::vector<u32>* r : {&shared_font, &finalize, &sleep_system}) {
        CHECK((*r)[1] == 0);
    }
    CHECK(shared_font[0] == ipc::MakeHeader(0x0044, 1, 0));
    CHECK(finalize[0] == ipc::MakeHeader(0x0004, 1, 0));
    CHECK(sleep_system[0] == ipc::MakeHeader(0x0042, 1, 0));
    CHECK(f.Logged("unimplemented APT:U 0x00440000"));
    CHECK(f.Logged("unimplemented APT:U 0x00040040"));
    CHECK(f.Logged("unimplemented APT:U 0x00420080"));
}

// ---- the default keyboard's checks, directly -------------------------------------------------------------------------

namespace {

KeyboardConfig Config(u32 buttons, u32 accept_mode, u16 max_text, u16 max_digits = 2) {
    KeyboardConfig c;
    c.button_config = buttons;
    c.accept_mode = accept_mode;
    c.max_text_length = max_text;
    c.max_digits = max_digits;
    return c;
}

}  // namespace

TEST_CASE("the default keyboard validates as Azahar's frontend does") {
    Fixture f;
    TempDir dir;
    struct Row {
        const char* text;
        KeyboardConfig config;
        bool accepted;
        std::u16string expected;
    };
    std::vector<Row> rows;
    {
        KeyboardConfig no_at = Config(0, 0, 16);
        no_at.prevent_at = true;
        KeyboardConfig no_percent = Config(0, 0, 16);
        no_percent.prevent_percent = true;
        KeyboardConfig no_backslash = Config(0, 0, 16);
        no_backslash.prevent_backslash = true;
        KeyboardConfig digits = Config(0, 0, 16, 2);
        digits.prevent_digit = true;
        rows = {
            {"hello", Config(0, 0, 16), true, u"hello"},
            {"", Config(0, 1, 16), false, u""},            // not empty
            {"hello", Config(0, 1, 16), true, u"hello"},
            {"   ", Config(0, 3, 16), false, u""},         // not blank
            {"", Config(0, 3, 16), false, u""},            // the empty text is blank too
            {"  x ", Config(0, 3, 16), true, u"  x "},
            {"  ", Config(0, 2, 16), false, u""},          // not empty and not blank
            {"ab", Config(0, 2, 16), true, u"ab"},
            {"abcd", Config(0, 4, 4), true, u"abcd"},      // fixed length
            {"abc", Config(0, 4, 4), false, u""},
            {"abcdef", Config(0, 4, 4), true, u"abcd"},    // cut to the fixed length first
            {"abcdef", Config(0, 0, 3), true, u"abc"},     // cut to the maximum
            {"a@b", no_at, false, u""},
            {"a%b", no_percent, false, u""},
            {"a\\b", no_backslash, false, u""},
            {"a1b2", digits, true, u"a1b2"},
            {"a1b2c3", digits, false, u""},
            // the text is checked only when the button is the "OK" one: with no buttons it is not
            {"", Config(3, 1, 16), true, u""},
            {"a@", no_at, false, u""},
            {"", Config(1, 1, 16), false, u""},
            {"", Config(2, 1, 16), false, u""},
            {"ab", Config(2, 0, 16), true, u"ab"},
            // outside the BMP: a cut never leaves half a pair
            {"ab\xF0\x9F\x98\x80", Config(0, 0, 3), true, u"ab"},
            {"ab\xF0\x9F\x98\x80", Config(0, 0, 4), true, std::u16string(u"ab\xD83D\xDE00")},
        };
    }
    std::vector<u8> button_of_triple;
    std::vector<bool> accepted;
    std::vector<std::u16string> texts;
    std::vector<u8> buttons;
    const int status = BootWith(f, dir,
                                [&] {
                                    std::vector<std::string> args = {"--ms", "20000"};
                                    for (const Row& row : rows) {
                                        args.push_back(std::string("--swkbd-text=") + row.text);
                                    }
                                    return args;
                                }(),
                                [&] {
                                    Guest g(*f.runtime);
                                    ServiceContext& context = FindServiceManager(*f.runtime)->Context();
                                    for (const Row& row : rows) {
                                        DefaultKeyboard keyboard(context);
                                        keyboard.Open();
                                        keyboard.Execute(row.config);
                                        accepted.push_back(keyboard.DataReady());
                                        if (keyboard.DataReady()) {
                                            const KeyboardData data = keyboard.ReceiveData();
                                            texts.push_back(data.text);
                                            buttons.push_back(data.button);
                                            CHECK_FALSE(keyboard.DataReady());
                                        } else {
                                            texts.push_back(u"");
                                            buttons.push_back(0xFF);
                                        }
                                    }
                                    ExitProcess(*f.runtime);
                                });
    CHECK(status == 0);
    REQUIRE(accepted.size() == rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        INFO("row " << i << " text '" << rows[i].text << "'");
        CHECK(accepted[i] == rows[i].accepted);
        if (rows[i].accepted) {
            CHECK(texts[i] == rows[i].expected);
            // 1 button: 0; 2 buttons: 1; 3 buttons: 2; none: 0
            const u32 b = rows[i].config.button_config;
            CHECK(buttons[i] == (b == 3 ? 0u : b));
        }
    }
}

TEST_CASE("an unknown button or input configuration does not crash the keyboard, which then never finishes") {
    Fixture f;
    TempDir dir;
    bool bad_buttons = true, bad_mode = true;
    const int status = BootWith(f, dir, {"--ms", "20000"}, [&] {
        Guest g(*f.runtime);
        ServiceContext& context = FindServiceManager(*f.runtime)->Context();
        DefaultKeyboard a(context);
        a.Open();
        a.Execute(Config(9, 0, 16));
        bad_buttons = a.DataReady();
        DefaultKeyboard b(context);
        b.Open();
        b.Execute(Config(0, 9, 16));
        bad_mode = b.DataReady();
        ExitProcess(*f.runtime);
    });
    CHECK(status == 0);
    CHECK_FALSE(bad_buttons);
    CHECK_FALSE(bad_mode);
    CHECK(f.Logged("Unknown button config 9"));
    CHECK(f.Logged("unknown validation method"));
}

#endif  // recomp.h
