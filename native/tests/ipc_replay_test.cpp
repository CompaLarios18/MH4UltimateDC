// mh4n_ipc_replay (N1-11): the trace readers, the time-dependent list and the replay of synthetic IPC
// traces against a fake service that this file registers. The traces are written by the tests (no game
// data): their words are what the fake service answers, worked out here from its code.
#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ipc_replay/replay.h"
#include "ipc_replay/replay_clock.h"
#include "ipc_replay/time_dependent.h"
#include "ipc_replay/tool.h"
#include "ipc_replay/trace.h"
#include "mh4n/crc32.h"
#include "mh4n/ipc.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"
#include "mh4n/runtime.h"
#include "mh4n/service.h"

using namespace mh4n;
using namespace mh4n::ipc_replay;

namespace {

constexpr Result kFailure = 0xC8804464;
constexpr u32 kEventRef = 0xAAAA;     // the reference's handle for the event the fake service hands out
constexpr u32 kSubRef = 0xBBBB;       // ... for the sub-session
constexpr u32 kSvcEventRef = 0x7000;  // ... for an event the program created itself

std::atomic<u32> g_ticks{0};
std::atomic<u32> g_timer_fired{0};  // set by the timer command 0x11 sets, cleared by the tests
std::atomic<u32> g_post_ran{0};     // set by the work command 0x15 posts
std::array<std::atomic<u32>, 2> g_slot_fired{};  // set by the timers command 0x17 sets
// What the timers of command 0x19 saw: their tag and the clock's ticks inside the callback. Touched only
// on the scheduler's thread (the replay's) and by the tests while no replay runs.
std::vector<std::pair<u32, u64>> g_stamps;
std::vector<std::shared_ptr<Request>> g_hung;  // requests the fake service never answers

// The service a session of rpl:tst hands out (command 0x000C).
class SubService final : public Service {
public:
    // `serial` counts the sub-sessions one session of FakeService has handed out.
    SubService(ServiceContext& context, u32 serial) : Service(context, "rpl:tst"), serial_(serial) {
        Register(0x0001, "Answer", &SubService::Answer);
    }
    const char* SessionKind() const override { return "sub"; }

private:
    void Answer(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(u32{0x5555} + serial_);
    }
    u32 serial_;
};

// Commands:
//   0x01 Add          a u32 and a u32 come back added
//   0x02 Fail         the result kFailure
//   0x03 StaticOut    eight bytes 1..8 in the static buffer 0
//   0x04 NewEvent     a copy handle to a new event
//   0x05 UseEvent     one handle: 1 if it is an event, 0 if not
//   0x06 Defer        the reply comes 20 ms later from another host thread
//   0x07 Ticks        a counter that grows with every call
//   0x08 GameFolder   the length of the game folder the service was given
//   0x09 MappedOut    writes 8 bytes 9.. into the mapped buffer it is given
//   0x0A StaticIn     the sum of the bytes of a static buffer
//   0x0D Hang         defers and never replies
//   0x0C NewSub       a move handle to a session of SubService (its Answer, 0x01, gives 0x5555 + how many
//                     this session handed out before it)
//   0x0E UserFolder   the length of the user folder the service was given
//   0x0F NullHandle   a copy handle that is 0
//   0x10 KindOf       the HandleType of the object a handle stands for, 0xFFFF for none
//   0x11 ArmTimer     a timer on the game clock: `lo hi` ticks from now it sets g_timer_fired
//   0x12 Fired        1 if that timer has run, else 0
//   0x13 Now          the game clock's ticks, low word then high word
//   0x14 ReplyAfter   defers; the reply is written by a timer `lo hi` ticks from now (no host thread)
//   0x15 PostLater    posts work to the scheduler that sets g_post_ran
//   0x16 PostRan      1 if that work has run, else 0
//   0x17 ArmSlot      `slot lo hi`: like 0x11 for one of two flags
//   0x18 FiredSlot    `slot`: 1 if that timer has run, else 0
//   0x19 ArmStamp     `tag lo hi rearm`: `lo hi` ticks from now a timer records (tag, the clock's ticks); with
//                     a rearm it arms a second one `rearm` ticks later from its own callback (tag + 10)
//   0x1A ReadStamp    `n`: tag, then the ticks (low word, high word) of the nth record; zeros if none
//   0x1B DeferBoth    `lo hi`: defers; the reply is written when `lo hi` ticks of game time have passed AND
//                     another host thread (20 ms) has said it is done (a file read with its latency)
class FakeService final : public Service {
public:
    explicit FakeService(ServiceContext& context) : Service(context, "rpl:tst") {
        Register(0x0001, "Add", &FakeService::Add);
        Register(0x0002, "Fail", &FakeService::Fail);
        Register(0x0003, "StaticOut", &FakeService::StaticOut);
        Register(0x0004, "NewEvent", &FakeService::NewEvent);
        Register(0x0005, "UseEvent", &FakeService::UseEvent);
        Register(0x0006, "Defer", &FakeService::Defer);
        Register(0x0007, "Ticks", &FakeService::Ticks);
        Register(0x0008, "GameFolder", &FakeService::GameFolder);
        Register(0x0009, "MappedOut", &FakeService::MappedOut);
        Register(0x000A, "StaticIn", &FakeService::StaticIn);
        Register(0x000C, "NewSub", &FakeService::NewSub);
        Register(0x000D, "Hang", &FakeService::Hang);
        Register(0x000E, "UserFolder", &FakeService::UserFolder);
        Register(0x000F, "NullHandle", &FakeService::NullHandle);
        Register(0x0010, "KindOf", &FakeService::KindOf);
        Register(0x0011, "ArmTimer", &FakeService::ArmTimer);
        Register(0x0012, "Fired", &FakeService::Fired);
        Register(0x0013, "Now", &FakeService::Now);
        Register(0x0014, "ReplyAfter", &FakeService::ReplyAfter);
        Register(0x0015, "PostLater", &FakeService::PostLater);
        Register(0x0016, "PostRan", &FakeService::PostRan);
        Register(0x0017, "ArmSlot", &FakeService::ArmSlot);
        Register(0x0018, "FiredSlot", &FakeService::FiredSlot);
        Register(0x0019, "ArmStamp", &FakeService::ArmStamp);
        Register(0x001A, "ReadStamp", &FakeService::ReadStamp);
        Register(0x001B, "DeferBoth", &FakeService::DeferBoth);
    }

private:
    void Add(Request& request) {
        const u32 a = request.Pop<u32>();
        const u32 b = request.Pop<u32>();
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(a + b);
    }
    void Fail(Request& request) { request.MakeReply(1, 0).Push(kFailure); }
    void StaticOut(Request& request) {
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushStaticBuffer({1, 2, 3, 4, 5, 6, 7, 8}, 0);
    }
    void NewEvent(Request& request) {
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushCopyObjects(Context().GetKernel().NewEvent(ResetType::OneShot));
    }
    void UseEvent(Request& request) {
        const std::shared_ptr<Event> event = request.PopObject<Event>();
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(event != nullptr ? u32{1} : u32{0});
    }
    Handled Defer(Request& request) {
        std::thread([shared = request.Share()] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            Reply reply(shared->CommandId(), 2, 0);
            reply.Push(kResultSuccess);
            reply.Push(u32{0x77});
            Complete(shared, std::move(reply));
        }).detach();
        return Handled::Deferred;
    }
    void Ticks(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(g_ticks.fetch_add(1));
    }
    void GameFolder(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(static_cast<u32>(Context().GameFolder().size()));
    }
    void MappedOut(Request& request) {
        const MappedBuffer* buffer = request.PopMappedBuffer();
        const u8 bytes[8] = {9, 10, 11, 12, 13, 14, 15, 16};
        const bool ok = buffer != nullptr && buffer->Write(bytes, 0, 8);
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(ok ? kResultSuccess : kFailure);
        if (buffer != nullptr) {
            reply.PushMappedBuffer(*buffer);
        } else {
            reply.Push(u32{0}, u32{0});
        }
    }
    void StaticIn(Request& request) {
        const StaticBuffer* buffer = request.PopStaticBuffer();
        u32 sum = 0;
        if (buffer != nullptr) {
            for (const u8 byte : buffer->data) {
                sum += byte;
            }
        }
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(sum);
    }
    Handled Hang(Request& request) {
        g_hung.push_back(request.Share());  // never completed
        return Handled::Deferred;
    }
    void NewSub(Request& request) {
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushMoveObjects(Context().CreateSession(std::make_unique<SubService>(Context(), subs_++)));
    }
    void UserFolder(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(static_cast<u32>(Context().UserFolder().size()));
    }
    void NullHandle(Request& request) {
        Reply& reply = request.MakeReply(1, 2);
        reply.Push(kResultSuccess);
        reply.PushCopyObjects(std::shared_ptr<Event>());
    }
    void KindOf(Request& request) {
        const std::shared_ptr<Object> object = request.PopGenericObject();
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(object ? static_cast<u32>(object->GetHandleType()) : u32{0xFFFF});
    }

    void ArmTimer(Request& request) {
        const u64 lo = request.Pop<u32>();
        const u64 hi = request.Pop<u32>();
        Kernel& kernel = Context().GetKernel();
        kernel.ScheduleAt(kernel.NowTicks() + (lo | (hi << 32)), [] { g_timer_fired.store(1); });
        request.MakeReply(1, 0).Push(kResultSuccess);
    }
    void Fired(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(g_timer_fired.load());
    }
    void Now(Request& request) {
        const u64 now = Context().GetKernel().NowTicks();
        Reply& reply = request.MakeReply(3, 0);
        reply.Push(kResultSuccess);
        reply.Push(static_cast<u32>(now));
        reply.Push(static_cast<u32>(now >> 32));
    }
    Handled ReplyAfter(Request& request) {
        const u64 lo = request.Pop<u32>();
        const u64 hi = request.Pop<u32>();
        Kernel& kernel = Context().GetKernel();
        Reply reply(request.CommandId(), 1, 0);
        reply.Push(kResultSuccess);
        auto shared = std::make_shared<Reply>(std::move(reply));
        kernel.ScheduleAt(kernel.NowTicks() + (lo | (hi << 32)),
                          [pending = request.Share(), shared] { pending->Deliver(std::move(*shared)); });
        return Handled::Deferred;
    }
    void PostLater(Request& request) {
        Context().GetKernel().Post([] { g_post_ran.store(1); });
        request.MakeReply(1, 0).Push(kResultSuccess);
    }
    void PostRan(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(g_post_ran.load());
    }

    void ArmSlot(Request& request) {
        const u32 slot = request.Pop<u32>() & 1;
        const u64 lo = request.Pop<u32>();
        const u64 hi = request.Pop<u32>();
        Kernel& kernel = Context().GetKernel();
        kernel.ScheduleAt(kernel.NowTicks() + (lo | (hi << 32)), [slot] { g_slot_fired[slot].store(1); });
        request.MakeReply(1, 0).Push(kResultSuccess);
    }
    void FiredSlot(Request& request) {
        const u32 slot = request.Pop<u32>() & 1;
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(g_slot_fired[slot].load());
    }
    static void Stamp(Kernel* kernel, u32 tag, u32 rearm) {
        g_stamps.emplace_back(tag, kernel->NowTicks());
        if (rearm != 0) {
            kernel->ScheduleAt(kernel->NowTicks() + rearm, [kernel, tag] { Stamp(kernel, tag + 10, 0); });
        }
    }
    void ArmStamp(Request& request) {
        const u32 tag = request.Pop<u32>();
        const u64 lo = request.Pop<u32>();
        const u64 hi = request.Pop<u32>();
        const u32 rearm = request.Pop<u32>();
        Kernel& kernel = Context().GetKernel();
        kernel.ScheduleAt(kernel.NowTicks() + (lo | (hi << 32)),
                          [kernel = &kernel, tag, rearm] { Stamp(kernel, tag, rearm); });
        request.MakeReply(1, 0).Push(kResultSuccess);
    }
    void ReadStamp(Request& request) {
        const std::size_t n = request.Pop<u32>();
        const std::pair<u32, u64> stamp = n < g_stamps.size() ? g_stamps[n] : std::pair<u32, u64>{0, 0};
        Reply& reply = request.MakeReply(4, 0);
        reply.Push(kResultSuccess);
        reply.Push(stamp.first);
        reply.Push(static_cast<u32>(stamp.second));
        reply.Push(static_cast<u32>(stamp.second >> 32));
    }
    Handled DeferBoth(Request& request) {
        const u64 lo = request.Pop<u32>();
        const u64 hi = request.Pop<u32>();
        Kernel& kernel = Context().GetKernel();
        // Both marks come on the scheduler's thread, in either order, as the file system's gate does.
        struct Gate {
            bool due = false;
            bool done = false;
            std::shared_ptr<Request> pending;
            void Check() {
                if (due && done && pending) {
                    Reply reply(pending->CommandId(), 1, 0);
                    reply.Push(kResultSuccess);
                    std::shared_ptr<Request> request = std::move(pending);
                    request->Deliver(std::move(reply));
                }
            }
        };
        auto gate = std::make_shared<Gate>();
        gate->pending = request.Share();
        kernel.ScheduleAt(kernel.NowTicks() + (lo | (hi << 32)), [gate] {
            gate->due = true;
            gate->Check();
        });
        std::thread([kernel = &kernel, gate] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            kernel->Post([gate] {
                gate->done = true;
                gate->Check();
            });
        }).detach();
        return Handled::Deferred;
    }

    u32 subs_ = 0;
};

MH4N_REGISTER(ServiceFactory, "rpl:tst",
              ServiceFactory([](ServiceContext& c) { return std::make_unique<FakeService>(c); }));

// ---- synthetic traces -----------------------------------------------------------------------------------------

std::string Hex(u32 value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::nouppercase;
    out.fill('0');
    out.width(8);
    out << value;
    return out.str();
}

std::string Words(const std::vector<u32>& words) {
    std::string text;
    for (std::size_t i = 0; i < words.size(); ++i) {
        text += (i != 0 ? "," : "") + Hex(words[i]);
    }
    return text.empty() ? "-" : text;
}

std::string CrcOf(const std::vector<u8>& bytes) {
    return Hex(Crc32(bytes.data(), bytes.size()));
}

std::string HexBytes(const std::vector<u8>& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string text;
    for (const u8 byte : bytes) {
        text += digits[byte >> 4];
        text += digits[byte & 15];
    }
    return text;
}

struct Req {
    std::string thread = "1";
    u32 seq = 1;
    std::string session = "rpl:tst";
    std::vector<u32> in;
    std::string desc = "-";
    std::vector<u32> out;  // empty: no reply
    std::string outdesc = "-";
    std::string data;      // "idx:hex" or empty
    bool no_request = false;
    u64 ticks = 0;         // the t= of the record
};

std::string IpcLine(const Req& r) {
    std::string line = r.thread + " seq=" + std::to_string(r.seq) + " session=" + r.session +
                       " cmd=" + (r.no_request || r.in.empty() ? "-" : Hex(r.in[0])) +
                       " in=" + (r.no_request ? "-" : Words(r.in)) + " desc=" + r.desc +
                       " out=" + Words(r.out) + " outdesc=" + r.outdesc + " t=" + std::to_string(r.ticks);
    if (!r.data.empty()) {
        line += " data=" + r.data;
    }
    return line;
}

std::string IpcTraceText(const std::vector<Req>& requests) {
    std::string text = "# ipctrace v1 2026-01-01T00:00:00 game=0x0 cpu=native new3ds=off\n";
    for (const Req& r : requests) {
        text += IpcLine(r) + "\n";
    }
    return text;
}

// The commands the tests send, with the replies the fake service gives.
Req Add(u32 seq, u32 a, u32 b) {
    Req r;
    r.seq = seq;
    r.in = {ipc::MakeHeader(1, 2, 0), a, b};
    r.out = {ipc::MakeHeader(1, 2, 0), 0, a + b};
    return r;
}

const std::vector<u8> kStaticOutBytes = {1, 2, 3, 4, 5, 6, 7, 8};

Req StaticOut(u32 seq, const std::string& crc) {
    Req r;
    r.seq = seq;
    r.in = {ipc::MakeHeader(3, 0, 0)};
    r.out = {ipc::MakeHeader(3, 1, 2), 0, ipc::StaticBufferDesc(8, 0), 0xDEADC0DE};
    r.outdesc = "static:2:8:" + crc;
    return r;
}

Req NewEvent(u32 seq) {
    Req r;
    r.seq = seq;
    r.in = {ipc::MakeHeader(4, 0, 0)};
    r.out = {ipc::MakeHeader(4, 1, 2), 0, ipc::CopyHandleDesc(1), kEventRef};
    r.outdesc = "copy:2:1:rpl:tst/event#0";
    return r;
}

Req UseEvent(u32 seq, u32 handle, const std::string& name, u32 answer) {
    Req r;
    r.seq = seq;
    r.in = {ipc::MakeHeader(5, 0, 2), ipc::CopyHandleDesc(1), handle};
    r.desc = "copy:1:1:" + name;
    r.out = {ipc::MakeHeader(5, 2, 0), 0, answer};
    return r;
}

Req KindOf(u32 seq, u32 handle, HandleType type, const std::string& thread = "1") {
    Req r;
    r.thread = thread;
    r.seq = seq;
    r.in = {ipc::MakeHeader(0x10, 0, 2), ipc::CopyHandleDesc(1), handle};
    r.out = {ipc::MakeHeader(0x10, 2, 0), 0, static_cast<u32>(type)};
    return r;
}

// One call of an SVC trace: the registers it was made with and the ones it returned.
std::string SvcLine(const std::string& thread, u32 svc, std::array<u32, 4> args, std::array<u32, 4> out) {
    const auto reg = [](u32 value) {
        char text[16];
        std::snprintf(text, sizeof(text), "%08x", value);
        return std::string(text);
    };
    char number[8];
    std::snprintf(number, sizeof(number), "%x", svc);
    return thread + " " + number + " pc=00100000 lr=00100004 " + reg(args[0]) + " " + reg(args[1]) + " " +
           reg(args[2]) + " " + reg(args[3]) + " t=5 in4=0,0,0 out=" + reg(out[0]) + "," + reg(out[1]) + "," +
           reg(out[2]) + "," + reg(out[3]) + "\n";
}

const char* const kSvcHeader = "# svctrace v1 2026-01-01 game=0x0 cpu=native new3ds=off\n";

// A SendSyncRequest of the program and what it returned.
std::string SvcSend(const std::string& thread, u32 result = 0) {
    return SvcLine(thread, 0x32, {0x1234, 0, 0, 0}, {result, 0, 0, 0});
}

// A creating call that returned the handle `handle`.
std::string SvcCreate(const std::string& thread, u32 svc, std::array<u32, 4> args, u32 handle) {
    return SvcLine(thread, svc, args, {0, handle, 0, 0});
}

class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("mh4n_ipc_replay_test_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                 std::to_string(counter++));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    std::string Write(const std::string& name, const std::string& text) const {
        const std::string path = (path_ / name).string();
        std::ofstream(path, std::ios::binary) << text;
        return path;
    }
    std::string Path(const std::string& name = {}) const { return (path_ / name).string(); }

private:
    std::filesystem::path path_;
};

struct Run {
    int status = -1;
    std::string out, err;
};

Run RunTool(const std::vector<std::string>& args) {
    std::ostringstream out, err;
    Run run;
    run.status = RunIpcReplay(args, out, err);
    run.out = out.str();
    run.err = err.str();
    return run;
}

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

}  // namespace

// ---- the readers ----------------------------------------------------------------------------------------------

TEST_CASE("an IPC trace is read as tracefmt.py reads it") {
    Req r = StaticOut(7, "0x12345678");
    r.session = "fs:USER/file#3";
    r.data = "1:00ff";
    IpcTrace trace;
    std::string error;
    REQUIRE_MESSAGE(ParseIpcTrace(IpcTraceText({r}) + "# a comment\n\n", &trace, &error), error);
    REQUIRE(trace.records.size() == 1);
    const IpcRecord& got = trace.records[0];
    CHECK(got.thread == "1");
    CHECK(got.seq == 7);
    CHECK(got.session == "fs:USER/file#3");
    CHECK(*got.cmd == ipc::MakeHeader(3, 0, 0));
    CHECK(got.in->size() == 1);
    CHECK(got.out->size() == 4);
    REQUIRE(got.outdesc.size() == 1);
    CHECK(got.outdesc[0].kind == "static");
    CHECK(got.outdesc[0].index == 2);
    CHECK(got.outdesc[0].size == 8);
    CHECK(got.outdesc[0].token == "0x12345678");
    REQUIRE(got.data.count(1) == 1);
    CHECK(got.data.at(1) == std::vector<u8>{0x00, 0xFF});
}

TEST_CASE("the token of a handle descriptor keeps its colons") {
    Req r = NewEvent(1);
    r.outdesc = "copy:2:1:fs:USER/file#3";
    IpcTrace trace;
    std::string error;
    REQUIRE_MESSAGE(ParseIpcTrace(IpcTraceText({r}), &trace, &error), error);
    CHECK(trace.records[0].outdesc[0].token == "fs:USER/file#3");
}

TEST_CASE("a request with no reply or no contents is read as such") {
    Req no_reply = Add(1, 1, 2);
    no_reply.out.clear();
    Req unseen;
    unseen.seq = 2;
    unseen.no_request = true;
    unseen.out = {ipc::MakeHeader(1, 1, 0), 0};
    IpcTrace trace;
    std::string error;
    REQUIRE_MESSAGE(ParseIpcTrace(IpcTraceText({no_reply, unseen}), &trace, &error), error);
    REQUIRE(trace.records.size() == 2);
    CHECK_FALSE(trace.records[0].out.has_value());
    CHECK(trace.records[0].cmd.has_value());
    CHECK_FALSE(trace.records[1].cmd.has_value());
    CHECK_FALSE(trace.records[1].in.has_value());
    CHECK(trace.records[1].out.has_value());
}

TEST_CASE("an IPC trace that cannot be read is refused, with the line") {
    IpcTrace trace;
    std::string error;
    SUBCASE("no header") {
        CHECK_FALSE(ParseIpcTrace("1 seq=1 session=a cmd=- in=- desc=- out=- outdesc=- t=0\n", &trace, &error));
        CHECK(Contains(error, "line 1"));
    }
    SUBCASE("an SVC trace") {
        CHECK_FALSE(ParseIpcTrace("# svctrace v1 2026-01-01 game=0x0\n", &trace, &error));
        CHECK(Contains(error, "ipctrace"));
    }
    SUBCASE("empty") {
        CHECK_FALSE(ParseIpcTrace("", &trace, &error));
        CHECK(Contains(error, "empty"));
    }
    SUBCASE("a line without seq") {
        CHECK_FALSE(ParseIpcTrace("# ipctrace v1 x\n1 session=a cmd=- in=- desc=- out=- outdesc=- t=0\n", &trace, &error));
        CHECK(Contains(error, "line 2"));
        CHECK(Contains(error, "seq"));
    }
    SUBCASE("a word that is not a number") {
        CHECK_FALSE(ParseIpcTrace("# ipctrace v1 x\n1 seq=1 session=a cmd=0x1 in=0xZZ desc=- out=- outdesc=- t=0\n", &trace, &error));
        CHECK(Contains(error, "line 2"));
    }
    SUBCASE("a descriptor outside the command buffer") {
        CHECK_FALSE(ParseIpcTrace("# ipctrace v1 x\n1 seq=1 session=a cmd=0x1 in=0x1 desc=copy:64:1:- out=- outdesc=- t=0\n", &trace, &error));
        CHECK(Contains(error, "outside"));
    }
    SUBCASE("a descriptor without a size") {
        CHECK_FALSE(ParseIpcTrace("# ipctrace v1 x\n1 seq=1 session=a cmd=0x1 in=0x1 desc=copy:1 out=- outdesc=- t=0\n", &trace, &error));
    }
    SUBCASE("odd hex bytes") {
        CHECK_FALSE(ParseIpcTrace("# ipctrace v1 x\n1 seq=1 session=a cmd=0x1 in=0x1 desc=- out=- outdesc=- t=0 data=1:abc\n", &trace, &error));
    }
    SUBCASE("a field that is not key=value") {
        CHECK_FALSE(ParseIpcTrace("# ipctrace v1 x\n1 seq=1 session=a junk\n", &trace, &error));
    }
}

TEST_CASE("an SVC trace is read for its creating calls") {
    const std::string text =
        "# svctrace v1 2026-01-01 game=0x0 cpu=native new3ds=off\n"
        "# thread 1 parent=0 entry=00100000 arg=0 stack=0 prio=48 core=0/0\n"
        "1 17 pc=00100000 lr=00100004 00000000 00000001 00000000 00000000 t=5 in4=0,0,0 out=00000000,00007000,00000000,00000000\n"
        "1 32 pc=00100008 lr=0010000C 00001234 00000000 00000000 00000000 t=6 out=-\n";
    SvcTrace trace;
    std::string error;
    REQUIRE_MESSAGE(ParseSvcTrace(text, &trace, &error), error);
    REQUIRE(trace.calls.size() == 2);
    CHECK(trace.calls[0].svc == 0x17);
    CHECK(trace.calls[0].args[1] == 1);
    CHECK(trace.calls[0].has_out);
    CHECK(trace.calls[0].out[1] == 0x7000);
    CHECK(trace.calls[1].svc == 0x32);
    CHECK_FALSE(trace.calls[1].has_out);
    CHECK(trace.calls[1].args[0] == 0x1234);

    CHECK_FALSE(ParseSvcTrace("1 17 pc=1 lr=1 0 0 0 0 t=0\n", &trace, &error));
    CHECK_FALSE(ParseSvcTrace("# ipctrace v1 x\n", &trace, &error));
    CHECK_FALSE(ParseSvcTrace("# svctrace v1 x\n1 17 pc=1\n", &trace, &error));
    CHECK(Contains(error, "line 2"));
    CHECK_FALSE(ParseSvcTrace("# svctrace v1 x\n1 17 pc=1 lr=1 0 zz 0 0 t=0\n", &trace, &error));
    CHECK_FALSE(ParseSvcTrace("# svctrace v1 x\n1 17 pc=1 lr=1 0 0 0 0 t=0 out=1,2\n", &trace, &error));
}

// ---- the time-dependent list ----------------------------------------------------------------------------------

TEST_CASE("the time-dependent list reads <session> <header> # reason") {
    TimeDependent list;
    std::string error;
    REQUIRE_MESSAGE(list.LoadText("# a comment only\n\nfs:USER 00010000  # a tick count\nfs:USER/file 0x00040080\n",
                                  "list", &error),
                    error);
    CHECK(list.size() == 2);
    CHECK(list.Matches("fs:USER", 0x00010000));
    CHECK_FALSE(list.Matches("fs:USER", 0x00010001));
    CHECK_FALSE(list.Matches("cfg:u", 0x00010000));
    CHECK(list.Matches("fs:USER/file#3", 0x00040080));  // by the name without the instance number
    CHECK_FALSE(list.Matches("fs:USER/dir#3", 0x00040080));
}

TEST_CASE("a time-dependent list that cannot be read is refused") {
    TempDir dir;
    TimeDependent list;
    std::string error;
    CHECK_FALSE(list.LoadFile(dir.Path("missing.txt"), &error));
    CHECK(Contains(error, "missing.txt"));
    CHECK_FALSE(list.LoadText("fs:USER\n", "list", &error));
    CHECK(Contains(error, "list:1"));
    CHECK_FALSE(list.LoadText("fs:USER 1 2\n", "list", &error));
    CHECK_FALSE(list.LoadText("ok:srv 00000001\nfs:USER zz\n", "list", &error));
    CHECK(Contains(error, "list:2"));
    CHECK(list.size() == 0);  // a file with a bad line adds nothing
    CHECK_FALSE(list.LoadText(std::string("fs:USER 1\n\xff\xfe\n"), "list", &error));
    CHECK(Contains(error, "UTF-8"));
    // a directory reads as an empty file: it is refused
    CHECK_FALSE(list.LoadFile(dir.Path(), &error));
    CHECK(Contains(error, "cannot read"));
}

TEST_CASE("a time-dependent list is UTF-8 as strictly as Python's decoder demands") {
    TimeDependent list;
    std::string error;
    // what Python's utf-8 codec refuses: overlong forms, surrogates, above U+10FFFF, stray bytes
    for (const std::string& bad : {std::string("\xC0\x80"), std::string("\xC1\xBF"), std::string("\xE0\x80\x80"),
                                  std::string("\xE0\x9F\xBF"), std::string("\xED\xA0\x80"),
                                  std::string("\xED\xBF\xBF"), std::string("\xF0\x80\x80\x80"),
                                  std::string("\xF0\x8F\xBF\xBF"), std::string("\xF4\x90\x80\x80"),
                                  std::string("\xF5\x80\x80\x80"), std::string("\xF8\x88\x80\x80\x80"),
                                  std::string("\x80"), std::string("\xC2"), std::string("\xE2\x82"),
                                  std::string("\xF0\x9F\x98")}) {
        INFO("bytes: " << bad.size() << " starting 0x" << std::hex << (static_cast<int>(bad[0]) & 0xFF));
        CHECK_FALSE(list.LoadText("srv:" + bad + " 00010002\n", "list", &error));
        CHECK(Contains(error, "UTF-8"));
        CHECK_FALSE(list.LoadText("srv: 00010002 # " + bad + "\n", "list", &error));
    }
    // what it takes, at the edges of each range
    for (const std::string& good : {std::string("\xC2\x80"), std::string("\xDF\xBF"), std::string("\xE0\xA0\x80"),
                                   std::string("\xEC\xBF\xBF"), std::string("\xED\x80\x80"),
                                   std::string("\xED\x9F\xBF"), std::string("\xEE\x80\x80"),
                                   std::string("\xEF\xBF\xBF"), std::string("\xF0\x90\x80\x80"),
                                   std::string("\xF3\xBF\xBF\xBF"), std::string("\xF4\x8F\xBF\xBF"),
                                   std::string("\xC3\xA9"), std::string("\xE2\x82\xAC")}) {
        INFO("bytes: " << good.size() << " starting 0x" << std::hex << (static_cast<int>(good[0]) & 0xFF));
        CHECK_MESSAGE(list.LoadText("srv:" + good + " 00010002\n", "list", &error), error);
    }
}

TEST_CASE("--time-dependent and --game refuse what they cannot examine or read") {
    TempDir dir;
    const std::string good = dir.Write("ipc.txt", IpcTraceText({Add(1, 1, 2)}));
    SUBCASE("a directory as the list") {
        const Run run = RunTool({good, "--time-dependent", dir.Path()});
        CHECK(run.status == 2);
        CHECK(Contains(run.err, "cannot read"));
    }
    SUBCASE("a file that is not UTF-8 text as the list") {
        const Run run = RunTool({good, "--time-dependent", dir.Write("td.txt", "srv:\xED\xA0\x80 1\n")});
        CHECK(run.status == 2);
        CHECK(Contains(run.err, "UTF-8"));
    }
    SUBCASE("a game folder that is a symbolic link loop") {
        std::error_code ec;
        std::filesystem::create_directory_symlink("loop", dir.Path("loop"), ec);
        if (ec) {
            MESSAGE("symbolic links are not available here: skipped");
            return;
        }
        const Run run = RunTool({good, "--game", dir.Path("loop")});
        CHECK(run.status == 2);
        CHECK(Contains(run.err, "cannot examine --game"));
    }
    SUBCASE("a default time-dependent list that cannot be examined") {
        std::error_code ec;
        std::filesystem::create_directory_symlink("config", dir.Path("config"), ec);
        if (ec) {
            MESSAGE("symbolic links are not available here: skipped");
            return;
        }
        // the default list is looked for relative to the current directory
        struct ChangeDirectory {
            std::filesystem::path before = std::filesystem::current_path();
            explicit ChangeDirectory(const std::string& to) { std::filesystem::current_path(to); }
            ~ChangeDirectory() {
                std::error_code ignored;
                std::filesystem::current_path(before, ignored);
            }
        } change(dir.Path());
        const Run run = RunTool({good, "--service", "rpl:tst"});
        CHECK(run.status == 2);
        CHECK(Contains(run.err, "cannot examine config/native/ipc_time_dependent.txt"));
    }
    SUBCASE("no default list at all is not an error") {
        struct ChangeDirectory {
            std::filesystem::path before = std::filesystem::current_path();
            explicit ChangeDirectory(const std::string& to) { std::filesystem::current_path(to); }
            ~ChangeDirectory() {
                std::error_code ignored;
                std::filesystem::current_path(before, ignored);
            }
        } change(dir.Path());
        const Run run = RunTool({good, "--service", "rpl:tst"});
        CHECK_MESSAGE(run.status == 0, run.out << run.err);
    }
}

// ---- the replay -------------------------------------------------------------------------------------------------

TEST_CASE("requests that match are replayed without a difference") {
    TempDir dir;
    const std::string trace = dir.Write("ipc.txt", IpcTraceText({Add(1, 40, 2), Add(2, 7, 8),
                                                                  StaticOut(3, CrcOf(kStaticOutBytes))}));
    const Run run = RunTool({trace, "--service", "rpl:tst"});
    CHECK_MESSAGE(run.status == 0, run.out << run.err);
    CHECK(Contains(run.out, "replayed 3 requests: 3 matched, 0 differ, 0 skipped"));
    CHECK(Contains(run.out, "rpl:tst: 3 replayed, 0 differ"));
}

TEST_CASE("a different result is reported with both results and nothing else of the reply") {
    TempDir dir;
    Req wrong = Add(2, 1, 2);
    wrong.in = {ipc::MakeHeader(2, 0, 0)};  // Fail answers kFailure
    wrong.out = {ipc::MakeHeader(2, 1, 0), 0};
    const std::string trace = dir.Write("ipc.txt", IpcTraceText({Add(1, 1, 1), wrong}));
    const Run run = RunTool({trace, "--service", "rpl:tst"});
    CHECK(run.status == 1);
    CHECK(Contains(run.out, "DIFF rpl:tst seq=2 session=rpl:tst header=0x00020000"));
    CHECK(Contains(run.out, "result: expected 0x00000000, got 0xc8804464"));
    CHECK(Contains(run.out, "replayed 2 requests: 1 matched, 1 differ, 0 skipped"));
}

TEST_CASE("a different normal word is named, not shown") {
    TempDir dir;
    Req wrong = Add(1, 1, 2);
    wrong.out[2] = 0x1234;
    const std::string trace = dir.Write("ipc.txt", IpcTraceText({wrong}));
    const Run run = RunTool({trace, "--service", "rpl:tst"});
    CHECK(run.status == 1);
    CHECK(Contains(run.out, "normal word 2 differs"));
    CHECK_FALSE(Contains(run.out, "0x00001234"));
    CHECK_FALSE(Contains(run.out, "0x1234"));
}

TEST_CASE("a different header is reported") {
    TempDir dir;
    Req wrong = Add(1, 1, 2);
    wrong.out[0] = ipc::MakeHeader(1, 3, 0);
    wrong.out.push_back(0);
    const std::string trace = dir.Write("ipc.txt", IpcTraceText({wrong}));
    const Run run = RunTool({trace, "--service", "rpl:tst"});
    CHECK(run.status == 1);
    CHECK(Contains(run.out, "header: expected 0x000100c0, got 0x00010080"));
}

TEST_CASE("a different output CRC is reported with the descriptor, the size and both CRCs") {
    TempDir dir;
    const std::string trace = dir.Write("ipc.txt", IpcTraceText({StaticOut(1, "0x12345678")}));
    const Run run = RunTool({trace, "--service", "rpl:tst"});
    CHECK(run.status == 1);
    CHECK(Contains(run.out, "descriptor 0 (static, index 2)"));
    CHECK(Contains(run.out, "crc expected 0x12345678, got " + CrcOf(kStaticOutBytes)));
}

TEST_CASE("a different size of an output buffer is reported") {
    TempDir dir;
    Req wrong = StaticOut(1, CrcOf(kStaticOutBytes));
    wrong.outdesc = "static:2:16:" + CrcOf(kStaticOutBytes);
    const Run run = RunTool({dir.Write("ipc.txt", IpcTraceText({wrong})), "--service", "rpl:tst"});
    CHECK(run.status == 1);
    CHECK(Contains(run.out, "size expected 16, got 8"));
}

TEST_CASE("a mapped output buffer is compared after the reply") {
    TempDir dir;
    const std::vector<u8> written = {9, 10, 11, 12, 13, 14, 15, 16};
    Req good;
    good.seq = 1;
    good.in = {ipc::MakeHeader(9, 0, 2), ipc::MappedBufferDesc(8, ipc::W), 0x08100000};
    good.desc = "mapped-w:1:8:-";
    good.out = {ipc::MakeHeader(9, 1, 2), 0, ipc::MappedBufferDesc(8, ipc::W), 0};
    good.outdesc = "mapped-w:1:8:" + CrcOf(written);
    Req bad = good;
    bad.seq = 2;
    bad.outdesc = "mapped-w:1:8:0x00000001";
    SUBCASE("equal") {
        const Run run = RunTool({dir.Write("ipc.txt", IpcTraceText({good})), "--service", "rpl:tst"});
        CHECK_MESSAGE(run.status == 0, run.out << run.err);
    }
    SUBCASE("different") {
        const Run run = RunTool({dir.Write("ipc.txt", IpcTraceText({bad})), "--service", "rpl:tst"});
        CHECK(run.status == 1);
        CHECK(Contains(run.out, "crc expected 0x00000001, got " + CrcOf(written)));
    }
}

TEST_CASE("an input buffer reaches the service from the trace's data") {
    TempDir dir;
    const std::vector<u8> bytes = {1, 2, 3, 4};
    Req r;
    r.seq = 1;
    r.in = {ipc::MakeHeader(10, 0, 2), ipc::StaticBufferDesc(4, 0), 0x08200000};
    r.desc = "static:1:4:" + CrcOf(bytes);
    r.data = "1:" + HexBytes(bytes);
    r.out = {ipc::MakeHeader(10, 2, 0), 0, 10};  // the sum of the bytes
    const Run run = RunTool({dir.Write("ipc.txt", IpcTraceText({r})), "--service", "rpl:tst"});
    CHECK_MESSAGE(run.status == 0, run.out << run.err);
}

TEST_CASE("handles are remapped from reply to request") {
    TempDir dir;
    // The reply of request 1 introduces the reference handle 0xAAAA; request 2 sends it back and the
    // service must see an event.
    const std::string trace = dir.Write(
        "ipc.txt", IpcTraceText({NewEvent(1), UseEvent(2, kEventRef, "rpl:tst/event#0", 1)}));
    const Run run = RunTool({trace, "--service", "rpl:tst"});
    CHECK_MESSAGE(run.status == 0, run.out << run.err);
    CHECK(Contains(run.out, "replayed 2 requests: 2 matched, 0 differ, 0 skipped"));
}

TEST_CASE("a reply handle of another kind than the trace's is reported") {
    TempDir dir;
    Req wrong = NewEvent(1);
    wrong.outdesc = "copy:2:1:rpl:tst/session#0";
    const Run run = RunTool({dir.Write("ipc.txt", IpcTraceText({wrong})), "--service", "rpl:tst"});
    CHECK(run.status == 1);
    CHECK(Contains(run.out, "kind expected session, got event"));
}

TEST_CASE("a handle nothing introduced skips the request") {
    TempDir dir;
    const Run run = RunTool({dir.Write("ipc.txt", IpcTraceText({UseEvent(1, 0x9999, "-", 1), Add(2, 1, 2)})),
                             "--service", "rpl:tst"});
    CHECK(run.status == 0);
    CHECK(Contains(run.out, "replayed 1 requests: 1 matched, 0 differ, 1 skipped"));
    CHECK(Contains(run.out, "skipped 1 x rpl:tst: a handle that no reply introduced (no SVC trace given)"));
}

TEST_CASE("an event created in the SVC trace reaches the service as an event") {
    TempDir dir;
    const Req request = UseEvent(1, kSvcEventRef, "-", 1);
    const std::string svc =
        "# svctrace v1 2026-01-01 game=0x0 cpu=native new3ds=off\n"
        "1 17 pc=00100000 lr=00100004 00000000 00000000 00000000 00000000 t=5 in4=0,0,0 "
        "out=00000000,00007000,00000000,00000000\n"
        "1 32 pc=00100008 lr=0010000C 00001234 00000000 00000000 00000000 t=6 in4=0,0,0 "
        "out=00000000,00000000,00000000,00000000\n";
    const std::string trace = dir.Write("ipc.txt", IpcTraceText({request}));
    SUBCASE("with the SVC trace") {
        const Run run = RunTool({trace, "--service", "rpl:tst", "--svc-trace", dir.Write("svc.txt", svc)});
        CHECK_MESSAGE(run.status == 0, run.out << run.err);
        CHECK(Contains(run.out, "replayed 1 requests: 1 matched, 0 differ, 0 skipped"));
    }
    SUBCASE("a handle created after the request is not its object") {
        const std::string late =
            "# svctrace v1 2026-01-01 game=0x0 cpu=native new3ds=off\n"
            "1 32 pc=00100008 lr=0010000C 00001234 00000000 00000000 00000000 t=6 in4=0,0,0 "
            "out=00000000,00000000,00000000,00000000\n"
            "1 17 pc=00100000 lr=00100004 00000000 00000000 00000000 00000000 t=7 in4=0,0,0 "
            "out=00000000,00007000,00000000,00000000\n";
        const Run run = RunTool({trace, "--service", "rpl:tst", "--svc-trace", dir.Write("late.txt", late)});
        CHECK(Contains(run.out, "1 skipped"));
    }
}

TEST_CASE("a deferred reply is waited for") {
    TempDir dir;
    Req r;
    r.seq = 1;
    r.in = {ipc::MakeHeader(6, 0, 0)};
    r.out = {ipc::MakeHeader(6, 2, 0), 0, 0x77};
    Req after = Add(2, 3, 4);
    const Run run = RunTool({dir.Write("ipc.txt", IpcTraceText({r, after})), "--service", "rpl:tst"});
    CHECK_MESSAGE(run.status == 0, run.out << run.err);
    CHECK(Contains(run.out, "replayed 2 requests: 2 matched, 0 differ, 0 skipped"));
}

TEST_CASE("a reply that never comes is a difference, and the next request still goes through") {
    TempDir dir;
    Req hang;
    hang.seq = 1;
    hang.in = {ipc::MakeHeader(13, 0, 0)};
    hang.out = {ipc::MakeHeader(13, 1, 0), 0};
    const std::string trace = dir.Write("ipc.txt", IpcTraceText({hang, Add(2, 1, 2)}));
    const auto started = std::chrono::steady_clock::now();
    const Run run = RunTool({trace, "--service", "rpl:tst", "--continue", "--reply-timeout", "150"});
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(20));
    CHECK(run.status == 1);
    CHECK(Contains(run.out, "no reply within 150 ms"));
    CHECK(Contains(run.out, "replayed 2 requests: 1 matched, 1 differ, 0 skipped"));
    // the thread that waited was ended, which cancelled its request and let go of the thread
    REQUIRE(g_hung.size() == 1);
    CHECK(g_hung[0]->Cancelled());
    CHECK(g_hung[0]->ClientThreadPtr() == nullptr);
    g_hung.clear();
}

TEST_CASE("a CloseHandle in the SVC trace closes the handle a reply introduced") {
    TempDir dir;
    const std::string svc =
        "# svctrace v1 2026-01-01 game=0x0 cpu=native new3ds=off\n"
        "1 32 pc=00100008 lr=0010000C 00001234 00000000 00000000 00000000 t=6 in4=0,0,0 "
        "out=00000000,00000000,00000000,00000000\n"
        "1 23 pc=00100008 lr=0010000C 0000AAAA 00000000 00000000 00000000 t=7 in4=0,0,0 "
        "out=00000000,00000000,00000000,00000000\n"
        "1 32 pc=00100008 lr=0010000C 00001234 00000000 00000000 00000000 t=8 in4=0,0,0 "
        "out=00000000,00000000,00000000,00000000\n";
    const std::string trace = dir.Write(
        "ipc.txt", IpcTraceText({NewEvent(1), UseEvent(2, kEventRef, "rpl:tst/event#0", 0)}));
    const Run run = RunTool({trace, "--service", "rpl:tst", "--svc-trace", dir.Write("svc.txt", svc)});
    // the handle was closed before the second request: it is not an event any more
    CHECK(Contains(run.out, "replayed 1 requests: 1 matched, 0 differ, 1 skipped"));
}

TEST_CASE("--reply-timeout takes a number of milliseconds") {
    TempDir dir;
    const std::string trace = dir.Write("ipc.txt", IpcTraceText({Add(1, 1, 2)}));
    CHECK(RunTool({trace, "--service", "rpl:tst", "--reply-timeout", "abc"}).status == 2);
    CHECK(RunTool({trace, "--service", "rpl:tst", "--reply-timeout", "0"}).status == 2);
    CHECK(RunTool({trace, "--service", "rpl:tst", "--reply-timeout", "5000"}).status == 0);
}

TEST_CASE("a time-dependent command is compared by result only, with the list from a second file") {
    TempDir dir;
    Req ticks;
    ticks.seq = 1;
    ticks.in = {ipc::MakeHeader(7, 0, 0)};
    ticks.out = {ipc::MakeHeader(7, 2, 0), 0, 0xFFFFFF};  // the fake service counts from 0
    const std::string trace = dir.Write("ipc.txt", IpcTraceText({ticks}));
    SUBCASE("without the list") {
        const Run run = RunTool({trace, "--service", "rpl:tst"});
        CHECK(run.status == 1);
        CHECK(Contains(run.out, "normal word 2 differs"));
    }
    SUBCASE("with the list") {
        const std::string list = dir.Write("extra.txt", "rpl:tst 00070000   # a counter\n");
        const Run run = RunTool({trace, "--service", "rpl:tst", "--time-dependent", list});
        CHECK_MESSAGE(run.status == 0, run.out << run.err);
    }
    SUBCASE("a different result is still a difference") {
        Req failing = ticks;
        failing.out = {ipc::MakeHeader(7, 2, 0), kFailure, 0};
        const std::string list = dir.Write("extra.txt", "rpl:tst 00070000\n");
        const Run run = RunTool({dir.Write("ipc2.txt", IpcTraceText({failing})), "--service", "rpl:tst",
                                 "--time-dependent", list});
        CHECK(run.status == 1);
        CHECK(Contains(run.out, "result: expected 0xc8804464, got 0x00000000"));
    }
}

TEST_CASE("without --continue a service stops at its first difference") {
    TempDir dir;
    Req wrong1 = Add(1, 1, 1);
    wrong1.out[2] = 99;
    Req wrong2 = Add(2, 2, 2);
    wrong2.out[2] = 99;
    const std::string trace = dir.Write("ipc.txt", IpcTraceText({wrong1, wrong2, Add(3, 3, 3)}));
    SUBCASE("stops") {
        const Run run = RunTool({trace, "--service", "rpl:tst"});
        CHECK(run.status == 1);
        CHECK(Contains(run.out, "seq=1"));
        CHECK_FALSE(Contains(run.out, "seq=2"));
        CHECK(Contains(run.out, "skipped 2 x rpl:tst: after the first difference"));
    }
    SUBCASE("continues") {
        const Run run = RunTool({trace, "--service", "rpl:tst", "--continue"});
        CHECK(run.status == 1);
        CHECK(Contains(run.out, "seq=1"));
        CHECK(Contains(run.out, "seq=2"));
        CHECK(Contains(run.out, "replayed 3 requests: 1 matched, 2 differ, 0 skipped"));
    }
}

TEST_CASE("--service selects the sessions a service hands out") {
    TempDir dir;
    Req sub;
    sub.seq = 1;
    sub.in = {ipc::MakeHeader(12, 0, 0)};
    sub.out = {ipc::MakeHeader(12, 1, 2), 0, ipc::MoveHandleDesc(1), kSubRef};
    sub.outdesc = "move:2:1:rpl:tst/sub#0";
    Req answer;
    answer.seq = 2;
    answer.session = "rpl:tst/sub#0";
    answer.in = {ipc::MakeHeader(1, 0, 0)};
    answer.out = {ipc::MakeHeader(1, 2, 0), 0, 0x5555};
    Req other;
    other.seq = 3;
    other.session = "err:f";
    other.in = {ipc::MakeHeader(0x99, 0, 0)};
    other.out = {ipc::MakeHeader(0x99, 1, 0), 0};
    const std::string trace = dir.Write("ipc.txt", IpcTraceText({sub, answer, other}));
    const Run run = RunTool({trace, "--service", "rpl:tst"});
    CHECK_MESSAGE(run.status == 0, run.out << run.err);
    CHECK(Contains(run.out, "replayed 2 requests: 2 matched, 0 differ, 0 skipped"));
    CHECK_FALSE(Contains(run.out, "err:f"));
}

TEST_CASE("by default the services registered natively are replayed and the others are counted") {
    TempDir dir;
    Req unknown;
    unknown.seq = 2;
    unknown.session = "xyz:none";
    unknown.in = {ipc::MakeHeader(1, 0, 0)};
    unknown.out = {ipc::MakeHeader(1, 1, 0), 0};
    const Run run = RunTool({dir.Write("ipc.txt", IpcTraceText({Add(1, 1, 2), unknown}))});
    CHECK_MESSAGE(run.status == 0, run.out << run.err);
    CHECK(Contains(run.out, "replayed 1 requests"));
    CHECK(Contains(run.out, "skipped 1 x xyz:none: service not registered natively"));
}

TEST_CASE("requests without a recorded request or reply are skipped") {
    TempDir dir;
    Req no_reply = Add(1, 1, 2);
    no_reply.out.clear();
    Req unseen;
    unseen.seq = 2;
    unseen.no_request = true;
    unseen.out = {ipc::MakeHeader(1, 1, 0), 0};
    const Run run = RunTool({dir.Write("ipc.txt", IpcTraceText({no_reply, unseen, Add(3, 1, 1)})),
                             "--service", "rpl:tst"});
    CHECK(run.status == 0);
    CHECK(Contains(run.out, "skipped 1 x rpl:tst: the trace has no reply for it"));
    CHECK(Contains(run.out, "skipped 1 x rpl:tst: the trace has no request for it"));
}

TEST_CASE("--game is the game folder the service is given") {
    TempDir dir;
    const std::string folder = dir.Path("game_folder_for_the_test");
    std::filesystem::create_directories(folder);
    Req r;
    r.seq = 1;
    r.in = {ipc::MakeHeader(8, 0, 0)};
    r.out = {ipc::MakeHeader(8, 2, 0), 0, static_cast<u32>(folder.size())};
    const std::string trace = dir.Write("ipc.txt", IpcTraceText({r}));
    SUBCASE("given") {
        const Run run = RunTool({trace, "--service", "rpl:tst", "--game", folder});
        CHECK_MESSAGE(run.status == 0, run.out << run.err);
    }
    SUBCASE("not given: the folder is empty") {
        const Run run = RunTool({trace, "--service", "rpl:tst"});
        CHECK(run.status == 1);
    }
}

// ---- errors of the tool --------------------------------------------------------------------------------------------

TEST_CASE("the tool refuses what it cannot use") {
    TempDir dir;
    const std::string good = dir.Write("ipc.txt", IpcTraceText({Add(1, 1, 2)}));
    SUBCASE("--help") {
        const Run run = RunTool({"--help"});
        CHECK(run.status == 0);
        CHECK(Contains(run.out, "--time-dependent"));
        CHECK(Contains(run.out, "--svc-trace"));
    }
    SUBCASE("no trace") {
        const Run run = RunTool({});
        CHECK(run.status == 2);
        CHECK(Contains(run.err, "expected one TRACE"));
    }
    SUBCASE("two traces") {
        CHECK(RunTool({good, good}).status == 2);
    }
    SUBCASE("an unknown option") {
        const Run run = RunTool({good, "--bogus"});
        CHECK(run.status == 2);
        CHECK(Contains(run.err, "--bogus"));
    }
    SUBCASE("a missing trace") {
        const Run run = RunTool({dir.Path("missing.txt")});
        CHECK(run.status == 2);
        CHECK(Contains(run.err, "cannot read"));
    }
    SUBCASE("an empty file") {
        CHECK(RunTool({dir.Write("empty.txt", "")}).status == 2);
    }
    SUBCASE("an SVC trace where the IPC trace belongs") {
        const Run run = RunTool({dir.Write("svc.txt", "# svctrace v1 x\n")});
        CHECK(run.status == 2);
        CHECK(Contains(run.err, "ipctrace"));
    }
    SUBCASE("a trace with a header and no request") {
        const Run run = RunTool({dir.Write("none.txt", "# ipctrace v1 x\n")});
        CHECK(run.status == 2);
        CHECK(Contains(run.err, "no request"));
    }
    SUBCASE("a damaged line") {
        const Run run = RunTool({dir.Write("bad.txt", "# ipctrace v1 x\nbroken\n")});
        CHECK(run.status == 2);
        CHECK(Contains(run.err, "line 2"));
    }
    SUBCASE("an error message never repeats the trace's text") {
        const Run field = RunTool({dir.Write("tok.txt", "# ipctrace v1 x\nT0 SECRETTOKEN\n")});
        CHECK(field.status == 2);
        CHECK(Contains(field.err, "line 2: field 1 is not key=value"));
        CHECK_FALSE(Contains(field.err, "SECRETTOKEN"));
        const Run value = RunTool({dir.Write("val.txt", "# ipctrace v1 x\nT0 seq=SECRETTOKEN\n")});
        CHECK(value.status == 2);
        CHECK_FALSE(Contains(value.err, "SECRETTOKEN"));
        const Run bytes = RunTool({dir.Write("bin.txt", std::string("# ipctrace v1 x\nT0 seq=1 \xC3\xFF\xFE", 28))});
        CHECK(bytes.status == 2);
        CHECK_FALSE(Contains(bytes.err, "\xFF"));
        CHECK_FALSE(Contains(bytes.err, "\xFE"));
        const std::string list = dir.Write("td.txt", "SECRETTOKEN zz\n");
        const Run header = RunTool({good, "--time-dependent", list});
        CHECK(header.status == 2);
        CHECK(Contains(header.err, "td.txt:1"));
        CHECK_FALSE(Contains(header.err, "zz"));
    }
    SUBCASE("a missing SVC trace") {
        CHECK(RunTool({good, "--svc-trace", dir.Path("missing.txt")}).status == 2);
    }
    SUBCASE("an IPC trace as the SVC trace") {
        const Run run = RunTool({good, "--svc-trace", good});
        CHECK(run.status == 2);
        CHECK(Contains(run.err, "svctrace"));
    }
    SUBCASE("a service nobody registered") {
        const Run run = RunTool({good, "--service", "nope:srv"});
        CHECK(run.status == 2);
        CHECK(Contains(run.err, "no service `nope:srv` is registered natively"));
        CHECK(Contains(run.err, "rpl:tst"));
    }
    SUBCASE("a service the trace has no request for") {
        const Run run = RunTool({good, "--service", "srv:"});
        CHECK(run.status == 2);
        CHECK(Contains(run.err, "no request was replayed"));
    }
    SUBCASE("a missing time-dependent file") {
        CHECK(RunTool({good, "--time-dependent", dir.Path("missing.txt")}).status == 2);
    }
    SUBCASE("a time-dependent file with a bad line") {
        const Run run = RunTool({good, "--time-dependent", dir.Write("td.txt", "rpl:tst\n")});
        CHECK(run.status == 2);
        CHECK(Contains(run.err, "td.txt:1"));
    }
    SUBCASE("a game folder that is not a folder") {
        const Run run = RunTool({good, "--game", dir.Path("missing_dir")});
        CHECK(run.status == 2);
        CHECK(Contains(run.err, "is not a directory"));
    }
}

TEST_CASE("the shipped time-dependent list is a list the tool reads") {
    const std::filesystem::path list_path =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "config" / "native" /
        "ipc_time_dependent.txt";
    if (!std::filesystem::exists(list_path)) {
        MESSAGE("config/native/ipc_time_dependent.txt is not next to native/: skipped");
        return;
    }
    TimeDependent list;
    std::string error;
    CHECK_MESSAGE(list.LoadFile(list_path.string(), &error), error);
}

// ---- what the report can print of the trace --------------------------------------------------------------------

TEST_CASE("a trace with text the report could print is refused when it is read, without the text") {
    TempDir dir;
    // Each line puts text of its own where the report prints the trace's: a descriptor kind, a buffer's
    // token, the kind in a handle's name, a session name. They are refused with the line number and
    // nothing of the text; no output is written.
    struct Case {
        const char* what;
        std::string session, desc, outdesc;
    };
    const std::vector<Case> cases = {
        {"a descriptor kind", "rpl:tst", "-", "SECRETKIND:2:1:-"},
        {"a descriptor kind in the request", "rpl:tst", "SECRETKIND:2:1:-", "-"},
        {"the token of a buffer", "rpl:tst", "-", "static:2:1:SECRETTOKEN"},
        {"a CRC with too few digits", "rpl:tst", "-", "static:2:1:0x1234"},
        {"a CRC with a letter that is not a digit", "rpl:tst", "-", "static:2:1:0x1234567g"},
        {"a CRC without 0x", "rpl:tst", "-", "static:2:1:12345678"},
        {"the token of a pid descriptor", "rpl:tst", "-", "pid:2:1:SECRETTOKEN"},
        {"the token of a PXI descriptor", "rpl:tst", "pxi:1:4:SECRETTOKEN", "-"},
        {"the kind in a handle's name", "rpl:tst", "-", "copy:2:1:rpl:tst/SECRETKIND#0"},
        {"the number in a handle's name", "rpl:tst", "-", "copy:2:1:rpl:tst/event#SECRET"},
        {"a handle's name without a kind", "rpl:tst", "-", "copy:2:1:SECRET NAME"},
        {"a handle's name among several", "rpl:tst", "-", "copy:2:2:rpl:tst/event#0+SECRET/x"},
        {"a session without a number", "rpl:tst/SECRETKIND", "-", "-"},
        {"a session with a kind in capitals", "rpl:tst/SECRETKIND#0", "-", "-"},
        {"a session that is not a name", "SECRET@NAME", "-", "-"},
        {"an empty session", "", "-", "-"},
        {"an unnamed session with letters", "unnamed#SECRET", "-", "-"},
        {"a session that is too long", std::string(80, 'a'), "-", "-"},
    };
    for (const Case& c : cases) {
        INFO("case: " << c.what);
        Req r = Add(1, 1, 2);
        r.session = c.session;
        r.desc = c.desc;
        r.outdesc = c.outdesc;
        const std::string trace = IpcTraceText({r});
        IpcTrace parsed;
        std::string error;
        CHECK_FALSE(ParseIpcTrace(trace, &parsed, &error));
        CHECK(Contains(error, "line 2"));
        CHECK_FALSE(Contains(error, "SECRET"));
        const Run run = RunTool({dir.Write("secret.txt", trace)});
        CHECK(run.status == 2);
        CHECK(run.out.empty());
        CHECK_FALSE(Contains(run.err, "SECRET"));
    }
}

TEST_CASE("the descriptors and sessions the tracers write are read") {
    Req r = Add(1, 1, 2);
    r.session = "gsp::Gpu";
    r.desc = "copy:1:3:0+-+unnamed#3,move:5:1:fs:USER/file#12,static:7:16:0x0000abcd,mapped-r:9:4:0xDEADBEEF,"
             "mapped-w:11:4:-,mapped-rw:13:4:-,mapped-0:15:4:-,pxi:17:4:-,pid:19:1:-";
    r.outdesc = "copy:2:2:rpl:tst/event#0+rpl:tst/sub-type_2#10";
    IpcTrace trace;
    std::string error;
    REQUIRE_MESSAGE(ParseIpcTrace(IpcTraceText({r}), &trace, &error), error);
    CHECK(trace.records[0].desc.size() == 9);
    CHECK(trace.records[0].outdesc.size() == 1);
    for (const char* session : {"srv:", "fs:USER/dir#0", "unnamed#0", "boss:U", "a.b$c-d_e"}) {
        Req named = Add(1, 1, 2);
        named.session = session;
        CHECK_MESSAGE(ParseIpcTrace(IpcTraceText({named}), &trace, &error), session << ": " << error);
    }
}

// ---- what the comparison and the alignment must notice -------------------------------------------------------

TEST_CASE("the number of the reply's descriptors is compared") {
    TempDir dir;
    SUBCASE("the trace lists none, the reply has one") {
        Req r = StaticOut(1, CrcOf(kStaticOutBytes));
        r.outdesc = "-";
        const Run run = RunTool({dir.Write("ipc.txt", IpcTraceText({r})), "--service", "rpl:tst"});
        CHECK(run.status == 1);
        CHECK(Contains(run.out, "the reply has 1 descriptors, expected 0"));
    }
    SUBCASE("the trace lists two, the reply has one") {
        Req r = StaticOut(1, CrcOf(kStaticOutBytes));
        r.outdesc += ",static:5:8:" + CrcOf(kStaticOutBytes);
        const Run run = RunTool({dir.Write("ipc.txt", IpcTraceText({r})), "--service", "rpl:tst"});
        CHECK(run.status == 1);
        CHECK(Contains(run.out, "the reply has 1 descriptors, expected 2"));
    }
}

TEST_CASE("the index of a descriptor is compared") {
    TempDir dir;
    Req r = StaticOut(1, CrcOf(kStaticOutBytes));
    r.outdesc = "static:3:8:" + CrcOf(kStaticOutBytes);
    const Run run = RunTool({dir.Write("ipc.txt", IpcTraceText({r})), "--service", "rpl:tst"});
    CHECK(run.status == 1);
    CHECK(Contains(run.out, "descriptor 0 (static, index 3): got it at index 2"));
}

TEST_CASE("a reply handle that is null in one reply and not in the other is reported") {
    TempDir dir;
    SUBCASE("the trace has a handle, the service gives none") {
        Req r;
        r.seq = 1;
        r.in = {ipc::MakeHeader(0x0F, 0, 0)};
        r.out = {ipc::MakeHeader(0x0F, 1, 2), 0, ipc::CopyHandleDesc(1), kEventRef};
        r.outdesc = "copy:2:1:rpl:tst/event#0";
        const Run run = RunTool({dir.Write("ipc.txt", IpcTraceText({r})), "--service", "rpl:tst"});
        CHECK(run.status == 1);
        CHECK(Contains(run.out, "handle 0 expected a handle, got null"));
    }
    SUBCASE("the trace has none, the service gives one") {
        Req r = NewEvent(1);
        r.out[3] = 0;
        r.outdesc = "copy:2:1:0";
        const Run run = RunTool({dir.Write("ipc.txt", IpcTraceText({r})), "--service", "rpl:tst"});
        CHECK(run.status == 1);
        CHECK(Contains(run.out, "handle 0 expected null, got a handle"));
    }
    SUBCASE("both are null") {
        Req r;
        r.seq = 1;
        r.in = {ipc::MakeHeader(0x0F, 0, 0)};
        r.out = {ipc::MakeHeader(0x0F, 1, 2), 0, ipc::CopyHandleDesc(1), 0};
        r.outdesc = "copy:2:1:0";
        const Run run = RunTool({dir.Write("ipc.txt", IpcTraceText({r})), "--service", "rpl:tst"});
        CHECK_MESSAGE(run.status == 0, run.out << run.err);
    }
}

TEST_CASE("requests are sent in the order of their seq, not of the lines") {
    TempDir dir;
    // the lines come in the order the replies were written; the second request was sent first
    const std::string trace = dir.Write(
        "ipc.txt", IpcTraceText({UseEvent(2, kEventRef, "rpl:tst/event#0", 1), NewEvent(1)}));
    const Run run = RunTool({trace, "--service", "rpl:tst"});
    CHECK_MESSAGE(run.status == 0, run.out << run.err);
    CHECK(Contains(run.out, "replayed 2 requests: 2 matched, 0 differ, 0 skipped"));
}

TEST_CASE("a SendSyncRequest that failed with 'invalid handle' is not one of the requests") {
    TempDir dir;
    const std::string trace = dir.Write("ipc.txt", IpcTraceText({UseEvent(1, kSvcEventRef, "-", 1)}));
    // The call that failed comes before the event is made; the request is the call after it. Counted as
    // a request it would take the record, and the event would not be one of the request's handles.
    const std::string svc = std::string(kSvcHeader) + SvcSend("1", kResultInvalidHandle) +
                            SvcCreate("1", 0x17, {0, 0, 0, 0}, kSvcEventRef) + SvcSend("1");
    SUBCASE("skipped") {
        const Run run = RunTool({trace, "--service", "rpl:tst", "--svc-trace", dir.Write("svc.txt", svc)});
        CHECK_MESSAGE(run.status == 0, run.out << run.err);
        CHECK(Contains(run.out, "replayed 1 requests: 1 matched, 0 differ, 0 skipped"));
    }
    SUBCASE("any other result is a request") {
        const std::string other = std::string(kSvcHeader) + SvcSend("1", kResultSessionClosed) +
                                  SvcCreate("1", 0x17, {0, 0, 0, 0}, kSvcEventRef) + SvcSend("1");
        const Run run = RunTool({trace, "--service", "rpl:tst", "--svc-trace", dir.Write("other.txt", other)});
        CHECK(Contains(run.out, "1 skipped"));
    }
}

TEST_CASE("the SVC trace and the IPC trace are lined up for each thread") {
    TempDir dir;
    // thread 2's request carries the event thread 2 made; thread 1's request was sent first
    Req carries = UseEvent(1, kSvcEventRef, "-", 1);
    carries.thread = "2";
    Req plain = Add(2, 3, 4);
    plain.thread = "1";
    const std::string svc = std::string(kSvcHeader) + SvcSend("1") +
                            SvcCreate("2", 0x17, {0, 0, 0, 0}, kSvcEventRef) + SvcSend("2");
    const Run run = RunTool({dir.Write("ipc.txt", IpcTraceText({carries, plain})), "--service", "rpl:tst",
                             "--svc-trace", dir.Write("svc.txt", svc)});
    CHECK_MESSAGE(run.status == 0, run.out << run.err);
    CHECK(Contains(run.out, "replayed 2 requests: 2 matched, 0 differ, 0 skipped"));
}

TEST_CASE("the newest session of a name answers to it") {
    TempDir dir;
    constexpr u32 kSecondSubRef = 0xCCCC;
    const auto new_sub = [](u32 seq, u32 handle) {
        Req r;
        r.seq = seq;
        r.in = {ipc::MakeHeader(12, 0, 0)};
        r.out = {ipc::MakeHeader(12, 1, 2), 0, ipc::MoveHandleDesc(1), handle};
        r.outdesc = "move:2:1:rpl:tst/sub#0";  // the same name both times
        return r;
    };
    Req answer;
    answer.seq = 3;
    answer.session = "rpl:tst/sub#0";
    answer.in = {ipc::MakeHeader(1, 0, 0)};
    answer.out = {ipc::MakeHeader(1, 2, 0), 0, 0x5556};  // the second session the service handed out
    const Run run = RunTool({dir.Write("ipc.txt", IpcTraceText({new_sub(1, kSubRef), new_sub(2, kSecondSubRef),
                                                                 answer})),
                             "--service", "rpl:tst"});
    CHECK_MESSAGE(run.status == 0, run.out << run.err);
    CHECK(Contains(run.out, "replayed 3 requests: 3 matched, 0 differ, 0 skipped"));
}

TEST_CASE("an object an SVC call made is an object of that kind") {
    TempDir dir;
    struct Kind {
        const char* name;
        u32 svc;
        std::array<u32, 4> args;
        HandleType type;
    };
    // CreateMutex(initially locked), CreateSemaphore(initial, max), CreateEvent(reset type),
    // CreateTimer(reset type), CreateMemoryBlock(other permission, address, size, permission),
    // CreateAddressArbiter
    const Kind kinds[] = {
        {"event", 0x17, {0, 0, 0, 0}, HandleType::Event},
        {"mutex", 0x13, {0, 0, 0, 0}, HandleType::Mutex},
        {"semaphore", 0x15, {0, 0, 1, 0}, HandleType::Semaphore},
        {"timer", 0x1A, {0, 0, 0, 0}, HandleType::Timer},
        {"memory block", 0x1E, {3, 0, 0x1000, 3}, HandleType::SharedMemory},
        {"address arbiter", 0x21, {0, 0, 0, 0}, HandleType::AddressArbiter},
    };
    for (const Kind& kind : kinds) {
        INFO("object: " << kind.name);
        const std::string trace =
            dir.Write("ipc.txt", IpcTraceText({KindOf(1, kSvcEventRef, kind.type)}));
        const std::string svc = std::string(kSvcHeader) + SvcCreate("1", kind.svc, kind.args, kSvcEventRef) +
                                SvcSend("1");
        const Run run = RunTool({trace, "--service", "rpl:tst", "--svc-trace", dir.Write("svc.txt", svc)});
        CHECK_MESSAGE(run.status == 0, run.out << run.err);
        CHECK(Contains(run.out, "replayed 1 requests: 1 matched, 0 differ, 0 skipped"));
    }
}

TEST_CASE("--user is the user folder the service is given") {
    TempDir dir;
    const std::string folder = dir.Path("user_folder_for_the_test");
    std::filesystem::create_directories(folder);
    Req r;
    r.seq = 1;
    r.in = {ipc::MakeHeader(0x0E, 0, 0)};
    r.out = {ipc::MakeHeader(0x0E, 2, 0), 0, static_cast<u32>(folder.size())};
    const std::string trace = dir.Write("ipc.txt", IpcTraceText({r}));
    SUBCASE("given") {
        const Run run = RunTool({trace, "--service", "rpl:tst", "--user", folder});
        CHECK_MESSAGE(run.status == 0, run.out << run.err);
    }
    SUBCASE("given with a game folder, which makes a second service manager") {
        const Run run = RunTool({trace, "--service", "rpl:tst", "--user", folder, "--game", dir.Path()});
        CHECK_MESSAGE(run.status == 0, run.out << run.err);
    }
    SUBCASE("not given: the folder is empty") {
        const Run run = RunTool({trace, "--service", "rpl:tst"});
        CHECK(run.status == 1);
    }
}

// ---- game time (N1-11b) -------------------------------------------------------------------------------------

namespace {

constexpr u32 kApt = 4468352;  // the ticks of the timer APT starts for the software keyboard

Req ArmTimerReq(u32 seq, u64 at, u64 ticks) {
    Req r;
    r.seq = seq;
    r.ticks = at;
    r.in = {ipc::MakeHeader(0x11, 2, 0), static_cast<u32>(ticks), static_cast<u32>(ticks >> 32)};
    r.out = {ipc::MakeHeader(0x11, 1, 0), 0};
    return r;
}

Req FiredReq(u32 seq, u64 at, u32 expected) {
    Req r;
    r.seq = seq;
    r.ticks = at;
    r.in = {ipc::MakeHeader(0x12, 0, 0)};
    r.out = {ipc::MakeHeader(0x12, 2, 0), 0, expected};
    return r;
}

Req NowReq(u32 seq, u64 at, u64 expected) {
    Req r;
    r.seq = seq;
    r.ticks = at;
    r.in = {ipc::MakeHeader(0x13, 0, 0)};
    r.out = {ipc::MakeHeader(0x13, 3, 0), 0, static_cast<u32>(expected), static_cast<u32>(expected >> 32)};
    return r;
}

Req ReplyAfterReq(u32 seq, u64 at, u64 ticks) {
    Req r;
    r.seq = seq;
    r.ticks = at;
    r.in = {ipc::MakeHeader(0x14, 2, 0), static_cast<u32>(ticks), static_cast<u32>(ticks >> 32)};
    r.out = {ipc::MakeHeader(0x14, 1, 0), 0};
    return r;
}

Req PostReq(u32 seq, u64 at, bool ask, u32 expected = 0) {
    Req r;
    r.seq = seq;
    r.ticks = at;
    if (ask) {
        r.in = {ipc::MakeHeader(0x16, 0, 0)};
        r.out = {ipc::MakeHeader(0x16, 2, 0), 0, expected};
    } else {
        r.in = {ipc::MakeHeader(0x15, 0, 0)};
        r.out = {ipc::MakeHeader(0x15, 1, 0), 0};
    }
    return r;
}

void ResetFake() {
    g_timer_fired.store(0);
    g_post_ran.store(0);
    g_slot_fired[0].store(0);
    g_slot_fired[1].store(0);
    g_stamps.clear();
}

Req SlotReq(u32 seq, u64 at, u32 slot, u64 ticks) {
    Req r;
    r.seq = seq;
    r.ticks = at;
    r.in = {ipc::MakeHeader(0x17, 3, 0), slot, static_cast<u32>(ticks), static_cast<u32>(ticks >> 32)};
    r.out = {ipc::MakeHeader(0x17, 1, 0), 0};
    return r;
}

Req SlotFiredReq(u32 seq, u64 at, u32 slot, u32 expected) {
    Req r;
    r.seq = seq;
    r.ticks = at;
    r.in = {ipc::MakeHeader(0x18, 1, 0), slot};
    r.out = {ipc::MakeHeader(0x18, 2, 0), 0, expected};
    return r;
}

Req StampArmReq(u32 seq, u64 at, u32 tag, u64 ticks, u32 rearm) {
    Req r;
    r.seq = seq;
    r.ticks = at;
    r.in = {ipc::MakeHeader(0x19, 4, 0), tag, static_cast<u32>(ticks), static_cast<u32>(ticks >> 32), rearm};
    r.out = {ipc::MakeHeader(0x19, 1, 0), 0};
    return r;
}

Req StampReadReq(u32 seq, u64 at, u32 n, u32 tag, u64 ticks) {
    Req r;
    r.seq = seq;
    r.ticks = at;
    r.in = {ipc::MakeHeader(0x1A, 1, 0), n};
    r.out = {ipc::MakeHeader(0x1A, 4, 0), 0, tag, static_cast<u32>(ticks), static_cast<u32>(ticks >> 32)};
    return r;
}

Req DeferBothReq(u32 seq, u64 at, u64 ticks) {
    Req r;
    r.seq = seq;
    r.ticks = at;
    r.in = {ipc::MakeHeader(0x1B, 2, 0), static_cast<u32>(ticks), static_cast<u32>(ticks >> 32)};
    r.out = {ipc::MakeHeader(0x1B, 1, 0), 0};
    return r;
}

}  // namespace

TEST_CASE("the replay advances game time to each record's tick, so a timer a request set has fired") {
    ResetFake();
    TempDir dir;
    // The timer is due kApt ticks after the first request, which sets the origin (tick 1000).
    const std::string trace = dir.Write(
        "ipc.txt", IpcTraceText({ArmTimerReq(1, 1000, kApt), FiredReq(2, 1000 + kApt - 1, 0),
                                 FiredReq(3, 1000 + kApt, 1), FiredReq(4, 1000 + kApt + 10, 1)}));
    SUBCASE("by default: one tick before it is due the timer has not run, at its tick it has") {
        const Run run = RunTool({trace, "--service", "rpl:tst"});
        CHECK_MESSAGE(run.status == 0, run.out << run.err);
        CHECK(Contains(run.out, "replayed 4 requests: 4 matched, 0 differ, 0 skipped"));
    }
    SUBCASE("with --no-time: no time passes between the requests, so the timer never fires") {
        const Run run = RunTool({trace, "--service", "rpl:tst", "--no-time", "--continue"});
        CHECK(run.status == 1);
        CHECK(Contains(run.out, "DIFF rpl:tst seq=3"));
        CHECK(Contains(run.out, "DIFF rpl:tst seq=4"));
        CHECK(!Contains(run.out, "DIFF rpl:tst seq=2"));
        CHECK(Contains(run.out, "replayed 4 requests: 2 matched, 2 differ, 0 skipped"));
    }
}

TEST_CASE("a difference is still a difference with game time advancing") {
    ResetFake();
    TempDir dir;
    // The reference says the timer had not fired after kApt + 5 ticks, which the replay's has.
    const std::string trace =
        dir.Write("ipc.txt", IpcTraceText({ArmTimerReq(1, 0, kApt), FiredReq(2, kApt + 5, 0)}));
    const Run run = RunTool({trace, "--service", "rpl:tst"});
    CHECK(run.status == 1);
    CHECK(Contains(run.out, "DIFF rpl:tst seq=2"));
}

TEST_CASE("the first record sets the origin of replay time") {
    ResetFake();
    TempDir dir;
    // Absolute ticks of a recording are large; only the distances from the first record count.
    const u64 first = 5'000'000'000ull;
    const std::string trace = dir.Write(
        "ipc.txt", IpcTraceText({NowReq(1, first, 0), NowReq(2, first + 1234, 1234),
                                 NowReq(3, first + 10'000'000, 10'000'000)}));
    const Run run = RunTool({trace, "--service", "rpl:tst"});
    CHECK_MESSAGE(run.status == 0, run.out << run.err);
}

TEST_CASE("a record whose tick goes backward does not move the clock back") {
    ResetFake();
    TempDir dir;
    // Replies are written when they complete, so the ticks of consecutive requests can go down.
    const std::string trace = dir.Write(
        "ipc.txt", IpcTraceText({NowReq(1, 5000, 0), NowReq(2, 9000, 4000), NowReq(3, 2000, 4000),
                                 NowReq(4, 5000, 4000), NowReq(5, 9500, 4500)}));
    const Run run = RunTool({trace, "--service", "rpl:tst"});
    CHECK_MESSAGE(run.status == 0, run.out << run.err);
    CHECK(Contains(run.out, "replayed 5 requests: 5 matched, 0 differ, 0 skipped"));
}

TEST_CASE("a gap of hours takes no wall time and runs the timers inside it") {
    ResetFake();
    TempDir dir;
    const u64 hour = 3600ull * kTicksPerSecond;
    const std::string trace = dir.Write(
        "ipc.txt", IpcTraceText({ArmTimerReq(1, 100, 3 * hour), FiredReq(2, 100 + 3 * hour - 1, 0),
                                 FiredReq(3, 100 + 3 * hour, 1), NowReq(4, 100 + 5 * hour, 5 * hour)}));
    const auto before = std::chrono::steady_clock::now();
    const Run run = RunTool({trace, "--service", "rpl:tst"});
    const auto elapsed = std::chrono::steady_clock::now() - before;
    CHECK_MESSAGE(run.status == 0, run.out << run.err);
    CHECK(elapsed < std::chrono::seconds(20));
}

TEST_CASE("a trace that spans more than a day of game time is refused") {
    CHECK(ReplayConfig{}.max_span_ticks == 24ull * 3600ull * kTicksPerSecond);
    ResetFake();
    TempDir dir;
    const u64 day = 24ull * 3600ull * kTicksPerSecond;
    SUBCASE("beyond the limit") {
        const std::string trace =
            dir.Write("ipc.txt", IpcTraceText({NowReq(1, 0, 0), NowReq(2, day + 1, day + 1)}));
        const Run run = RunTool({trace, "--service", "rpl:tst"});
        CHECK(run.status == 2);
        CHECK(Contains(run.err, "line 3"));
        CHECK(Contains(run.err, "damaged"));
    }
    SUBCASE("with --no-time the ticks are not used") {
        Req second = Add(2, 1, 2);
        second.ticks = ~u64{0};
        const std::string trace = dir.Write("ipc.txt", IpcTraceText({Add(1, 1, 2), second}));
        const Run run = RunTool({trace, "--service", "rpl:tst", "--no-time"});
        CHECK_MESSAGE(run.status == 0, run.out << run.err);
    }
}

TEST_CASE("posted work runs before the next request, and only with game time") {
    ResetFake();
    TempDir dir;
    const std::string trace = dir.Write(
        "ipc.txt", IpcTraceText({PostReq(1, 10, false), PostReq(2, 20, true, 1)}));
    SUBCASE("by default") {
        const Run run = RunTool({trace, "--service", "rpl:tst"});
        CHECK_MESSAGE(run.status == 0, run.out << run.err);
    }
    SUBCASE("with --no-time the scheduler is not run between requests") {
        const Run run = RunTool({trace, "--service", "rpl:tst", "--no-time"});
        CHECK(run.status == 1);
    }
}

TEST_CASE("a reply that waits for a timer arrives at the timer's tick, with no wall wait") {
    ResetFake();
    TempDir dir;
    const u64 delay = 30ull * kTicksPerSecond;  // half a minute of game time
    const std::string trace = dir.Write(
        "ipc.txt", IpcTraceText({ReplyAfterReq(1, 0, delay), NowReq(2, 1, delay),
                                 NowReq(3, delay + 50, delay + 50)}));
    // 5 s: the half minute cannot have been waited for in real time.
    const Run run = RunTool({trace, "--service", "rpl:tst", "--reply-timeout", "5000"});
    CHECK_MESSAGE(run.status == 0, run.out << run.err);
    CHECK(Contains(run.out, "replayed 3 requests: 3 matched, 0 differ, 0 skipped"));
}

TEST_CASE("a reply that needs more than a minute of game time is a difference, and the clock stops") {
    ResetFake();
    TempDir dir;
    const u64 delay = 90ull * kTicksPerSecond;
    // Two timers set before the request, one inside the minute and one beyond it: the clock goes on up to
    // the first (the reply's own timer is beyond the cap) and no farther.
    const std::string trace = dir.Write(
        "ipc.txt", IpcTraceText({SlotReq(1, 0, 0, 59ull * kTicksPerSecond),
                                 SlotReq(2, 0, 1, 61ull * kTicksPerSecond), ReplyAfterReq(3, 0, delay),
                                 SlotFiredReq(4, 10, 0, 1), SlotFiredReq(5, 10, 1, 0), Add(6, 1, 2)}));
    const Run run = RunTool({trace, "--service", "rpl:tst", "--reply-timeout", "300", "--continue"});
    CHECK(run.status == 1);
    CHECK(Contains(run.out, "DIFF rpl:tst seq=3"));
    CHECK(Contains(run.out, "no reply within 300 ms"));
    CHECK(!Contains(run.out, "DIFF rpl:tst seq=4"));  // the timer at 59 s ran
    CHECK(!Contains(run.out, "DIFF rpl:tst seq=5"));  // the timer at 61 s did not
    CHECK(Contains(run.out, "replayed 6 requests: 5 matched, 1 differ, 0 skipped"));
}

TEST_CASE("a deferred reply a host thread completes sees no game time pass") {
    ResetFake();
    TempDir dir;
    // The fake service's Defer answers 20 ms later from another host thread and arms no timer: the clock
    // stands where the request put it, so the Now that follows reads the recorded distance exactly
    // (never the time the host thread took, which the clock would keep for good if it had moved).
    auto defer = [](u32 seq, u64 at) {
        Req r;
        r.seq = seq;
        r.ticks = at;
        r.in = {ipc::MakeHeader(6, 0, 0)};
        r.out = {ipc::MakeHeader(6, 2, 0), 0, 0x77};
        return r;
    };
    const std::string trace = dir.Write(
        "ipc.txt", IpcTraceText({defer(1, 7), NowReq(2, 7, 0), NowReq(3, 100, 93), defer(4, 200),
                                 NowReq(5, 200, 193), NowReq(6, 250, 243), Add(7, 1, 2)}));
    for (int i = 0; i < 3; ++i) {
        const Run run = RunTool({trace, "--service", "rpl:tst"});
        CHECK_MESSAGE(run.status == 0, run.out << run.err);
        CHECK(Contains(run.out, "replayed 7 requests: 7 matched, 0 differ, 0 skipped"));
    }
}

TEST_CASE("a reply that waits for a timer and a host thread arrives at the timer's tick") {
    ResetFake();
    TempDir dir;
    // The shape of a file read with its latency: the reply needs 2 s of game time and a 20 ms thread. Whichever
    // comes last, the clock is at the timer's tick afterwards, not wherever the thread's speed took it.
    const u64 delay = 2ull * kTicksPerSecond;
    const std::string trace = dir.Write(
        "ipc.txt", IpcTraceText({DeferBothReq(1, 0, delay), NowReq(2, 1, delay), NowReq(3, delay + 77, delay + 77),
                                 DeferBothReq(4, delay + 100, 500), NowReq(5, delay + 101, delay + 600)}));
    for (int i = 0; i < 3; ++i) {
        const Run run = RunTool({trace, "--service", "rpl:tst", "--reply-timeout", "10000"});
        CHECK_MESSAGE(run.status == 0, run.out << run.err);
        CHECK(Contains(run.out, "replayed 5 requests: 5 matched, 0 differ, 0 skipped"));
    }
}

TEST_CASE("a skipped request moves the clock like any other") {
    ResetFake();
    TempDir dir;
    // Each Now asks for a tick that is behind the record skipped before it, so what it reads is where that
    // record put the clock. Three ways to be skipped: a service that is not selected, a record the trace
    // gives no reply for, and a session the trace never saw handed out.
    Req not_selected = Add(2, 1, 2);
    not_selected.session = "other:svc";
    not_selected.ticks = 10'000;
    Req no_reply = Add(4, 1, 2);
    no_reply.out.clear();
    no_reply.ticks = 30'000;
    Req no_session = Add(6, 1, 2);
    no_session.session = "rpl:tst/sub#9";
    no_session.ticks = 50'000;
    const std::string trace = dir.Write(
        "ipc.txt", IpcTraceText({NowReq(1, 0, 0), not_selected, NowReq(3, 1, 10'000), no_reply,
                                 NowReq(5, 2, 30'000), no_session, NowReq(7, 3, 50'000)}));
    const Run run = RunTool({trace, "--service", "rpl:tst"});
    CHECK_MESSAGE(run.status == 0, run.out << run.err);
    CHECK(Contains(run.out, "replayed 4 requests: 4 matched, 0 differ, 2 skipped"));
}

TEST_CASE("a timer runs with the clock at its own tick, in (tick, id) order") {
    ResetFake();
    TempDir dir;
    // Four timers fall due inside the one gap before the first read (tick 9000): tag 2 at 3000 (which arms
    // tag 12 from its own callback, 1000 ticks on, so at 4000), then tags 1 and 3 together at 5000, tag 1
    // first because it was armed first.
    const std::string trace = dir.Write(
        "ipc.txt", IpcTraceText({StampArmReq(1, 0, 1, 5000, 0), StampArmReq(2, 0, 2, 3000, 1000),
                                 StampArmReq(3, 0, 3, 5000, 0), StampReadReq(4, 9000, 0, 2, 3000),
                                 StampReadReq(5, 9000, 1, 12, 4000), StampReadReq(6, 9000, 2, 1, 5000),
                                 StampReadReq(7, 9000, 3, 3, 5000), StampReadReq(8, 9000, 4, 0, 0)}));
    const Run run = RunTool({trace, "--service", "rpl:tst"});
    CHECK_MESSAGE(run.status == 0, run.out << run.err);
    CHECK(Contains(run.out, "replayed 8 requests: 8 matched, 0 differ, 0 skipped"));
}

TEST_CASE("the span limit is a value of ReplayConfig, exact to the tick") {
    ResetFake();
    const u64 limit = 2ull * kTicksPerSecond;  // two seconds, so that the boundary is cheap to replay
    auto replay = [&](u64 last) {
        const std::string text = IpcTraceText({NowReq(1, 0, 0), NowReq(2, last, last)});
        IpcTrace trace;
        std::string error;
        REQUIRE_MESSAGE(ParseIpcTrace(text, &trace, &error), error);
        const Options options = ParseOptions("ipc-replay", {});
        REQUIRE(options.ok());
        Runtime runtime(MakeReplayRuntimeConfig());
        REQUIRE_MESSAGE(runtime.Prepare(options, {Phase::Devices, Phase::Services}, &error), error);
        ServiceManager* manager = FindServiceManager(runtime);
        REQUIRE(manager != nullptr);
        ReplayConfig config;
        config.services = {"rpl:tst"};
        config.max_span_ticks = limit;
        return Replay(runtime, *manager, trace, nullptr, config);
    };
    SUBCASE("exactly the limit is replayed") {
        const ReplayReport report = replay(limit);
        CHECK_MESSAGE(report.error.empty(), report.error);
        CHECK(report.ExitStatus() == 0);
    }
    SUBCASE("one tick more is refused, with the line") {
        const ReplayReport report = replay(limit + 1);
        CHECK(report.ExitStatus() == 2);
        CHECK(report.error.find("line 3") != std::string::npos);
    }
}

TEST_CASE("Replay needs the replay clock to advance time") {
    ResetFake();
    TempDir dir;
    const std::string text = IpcTraceText({Add(1, 1, 2)});
    IpcTrace trace;
    std::string error;
    REQUIRE_MESSAGE(ParseIpcTrace(text, &trace, &error), error);
    const Options options = ParseOptions("ipc-replay", {});
    REQUIRE(options.ok());
    SUBCASE("a runtime with the host's clock: an error, never a silent no-op") {
        Runtime runtime;
        REQUIRE_MESSAGE(runtime.Prepare(options, {Phase::Devices, Phase::Services}, &error), error);
        ServiceManager* manager = FindServiceManager(runtime);
        REQUIRE(manager != nullptr);
        ReplayConfig config;
        config.services = {"rpl:tst"};
        const ReplayReport report = Replay(runtime, *manager, trace, nullptr, config);
        CHECK(!report.error.empty());
        CHECK(report.ExitStatus() == 2);
        config.advance_time = false;
        const ReplayReport plain = Replay(runtime, *manager, trace, nullptr, config);
        CHECK(plain.error.empty());
        CHECK(plain.ExitStatus() == 0);
    }
    SUBCASE("a runtime made with MakeReplayRuntimeConfig") {
        Runtime runtime(MakeReplayRuntimeConfig());
        REQUIRE_MESSAGE(runtime.Prepare(options, {Phase::Devices, Phase::Services}, &error), error);
        ServiceManager* manager = FindServiceManager(runtime);
        REQUIRE(manager != nullptr);
        ReplayConfig config;
        config.services = {"rpl:tst"};
        const ReplayReport report = Replay(runtime, *manager, trace, nullptr, config);
        CHECK(report.error.empty());
        CHECK(report.ExitStatus() == 0);
    }
}

TEST_CASE("the replay clock stands still until it is set and never goes back") {
    ReplayTimeSource source;
    CHECK(source.NowNs() == 0);
    CHECK(!source.NextDeadlineNs());
    source.SetNs(500);
    CHECK(source.NowNs() == 500);
    source.SetNs(100);
    CHECK(source.NowNs() == 500);
    std::unique_ptr<PreemptionSource> preemption = source.MakePreemption();
    preemption->Arm(900);
    CHECK(*source.NextDeadlineNs() == 900);
    preemption->Arm(std::nullopt);
    CHECK(!source.NextDeadlineNs());
}

TEST_CASE("--help lists --no-time") {
    const Run run = RunTool({"--help"});
    CHECK(run.status == 0);
    CHECK(Contains(run.out, "--no-time"));
}
