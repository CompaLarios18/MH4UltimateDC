// mh4n_ipc_replay (N1-11): the trace readers, the time-dependent list and the replay of synthetic IPC
// traces against a fake service that this file registers. The traces are written by the tests (no game
// data): their words are what the fake service answers, worked out here from its code.
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ipc_replay/replay.h"
#include "ipc_replay/time_dependent.h"
#include "ipc_replay/tool.h"
#include "ipc_replay/trace.h"
#include "mh4n/crc32.h"
#include "mh4n/ipc.h"
#include "mh4n/registry.h"
#include "mh4n/service.h"

using namespace mh4n;
using namespace mh4n::ipc_replay;

namespace {

constexpr Result kFailure = 0xC8804464;
constexpr u32 kEventRef = 0xAAAA;     // the reference's handle for the event the fake service hands out
constexpr u32 kSubRef = 0xBBBB;       // ... for the sub-session
constexpr u32 kSvcEventRef = 0x7000;  // ... for an event the program created itself

std::atomic<u32> g_ticks{0};
std::vector<std::shared_ptr<Request>> g_hung;  // requests the fake service never answers

// The service a session of rpl:tst hands out (command 0x000C).
class SubService final : public Service {
public:
    explicit SubService(ServiceContext& context) : Service(context, "rpl:tst") {
        Register(0x0001, "Answer", &SubService::Answer);
    }
    const char* SessionKind() const override { return "sub"; }

private:
    void Answer(Request& request) {
        Reply& reply = request.MakeReply(2, 0);
        reply.Push(kResultSuccess);
        reply.Push(u32{0x5555});
    }
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
//   0x0C NewSub       a move handle to a session of SubService
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
        reply.PushMoveObjects(Context().CreateSession(std::make_unique<SubService>(Context())));
    }
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
};

std::string IpcLine(const Req& r) {
    std::string line = r.thread + " seq=" + std::to_string(r.seq) + " session=" + r.session +
                       " cmd=" + (r.no_request || r.in.empty() ? "-" : Hex(r.in[0])) +
                       " in=" + (r.no_request ? "-" : Words(r.in)) + " desc=" + r.desc +
                       " out=" + Words(r.out) + " outdesc=" + r.outdesc + " t=0";
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
