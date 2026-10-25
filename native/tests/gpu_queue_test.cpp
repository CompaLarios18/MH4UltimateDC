// mh4n_gpu_queue (N2-22): the renderer thread and its queue, with a fake host and a fake wrapped GPU.
//
// Nothing here uses a device or a timer for ordering: the fake GPU's commands wait on gates and latches
// that the test opens, so each case has one possible interleaving of what it asserts. The only sleeps
// make a measured duration non-zero (they never order anything). The cases that need threads use real
// ones: the owner rule is about threads.
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "gpu/backend/rhi.h"
#include "gpu_capture/mgpu.h"
#include "gpu_capture/player.h"
#include "gpu/queue/queue_gpu.h"
#include "mh4n/gpu.h"
#include "mh4n/gpu_debug.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"

using namespace mh4n;
using namespace mh4n::gpu;
using namespace mh4n::gpu::queue;

namespace {

constexpr bool kRhiChecks = MH4N_RHI_CHECKS != 0;
constexpr u32 kMemBase = 0x20000000;
constexpr u32 kMemSize = 0x1000;

// ---- synchronisation helpers ---------------------------------------------------------------------

struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool open = false;
    void Wait() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return open; });
    }
    void Open() {
        {
            std::lock_guard lock(mutex);
            open = true;
        }
        cv.notify_all();
    }
};

// A counter that a thread can wait on.
struct Latch {
    std::mutex mutex;
    std::condition_variable cv;
    int count = 0;
    void Signal() {
        {
            std::lock_guard lock(mutex);
            ++count;
        }
        cv.notify_all();
    }
    void Wait(int n = 1) {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this, n] { return count >= n; });
    }
};

// Spins (yielding) until `n` callers are blocked inside the queue. The deadline is only a guard that
// turns a bug into a failure; it orders nothing.
void WaitUntilBlocked(QueueGpu& q, u32 n) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (q.BlockedCallers() < n) {
        REQUIRE_MESSAGE(std::chrono::steady_clock::now() < deadline, "no caller blocked in the queue");
        std::this_thread::yield();
    }
}

// ---- the fake host -------------------------------------------------------------------------------

struct Signalled {
    Interrupt id;
    u64 delay_ns;
    std::thread::id thread;
};

class FakeHost final : public Host {
public:
    std::vector<u8> memory = std::vector<u8>(kMemSize, 0);

    u8* PhysicalPointer(u32 paddr, u32 size) override {
        if (paddr < kMemBase || paddr + size > kMemBase + kMemSize) {
            return nullptr;
        }
        return memory.data() + (paddr - kMemBase);
    }
    std::optional<u32> VirtualToPhysical(u32 vaddr) override {
        return vaddr;
    }
    void SignalInterrupt(Interrupt id, u64 delay_ns) override {
        std::lock_guard lock(mutex_);
        signalled_.push_back({id, delay_ns, std::this_thread::get_id()});
    }
    void Post(std::function<void()> work) override {
        std::lock_guard lock(mutex_);
        posted_.push_back(std::move(work));
    }

    // The scheduler's safe point: runs what was posted, on the calling thread.
    size_t RunPosted() {
        std::vector<std::function<void()>> work;
        {
            std::lock_guard lock(mutex_);
            work.swap(posted_);
        }
        for (auto& w : work) {
            w();
        }
        return work.size();
    }
    size_t PostedCount() {
        std::lock_guard lock(mutex_);
        return posted_.size();
    }
    std::vector<Signalled> Signals() {
        std::lock_guard lock(mutex_);
        return signalled_;
    }

private:
    std::mutex mutex_;
    std::vector<Signalled> signalled_;
    std::vector<std::function<void()>> posted_;
};

// ---- the fake wrapped GPU ------------------------------------------------------------------------

// What the test and the fake GPU share.
struct Script {
    std::mutex mutex;
    std::vector<std::string> events;
    std::vector<std::thread::id> event_threads;
    std::vector<std::thread::id> write_threads; // who ran each posted write
    std::atomic<int> writes{0};
    FakeHost* host = nullptr;
    Gate gate;
    Latch entered;
    std::atomic<u64> draws{0};

    void Add(std::string event) {
        std::lock_guard lock(mutex);
        events.push_back(std::move(event));
        event_threads.push_back(std::this_thread::get_id());
    }
    // The thread of a call that has no event of its own (the counters read at every frame).
    void NoteThread() {
        std::lock_guard lock(mutex);
        event_threads.push_back(std::this_thread::get_id());
    }
    std::vector<std::string> Events() {
        std::lock_guard lock(mutex);
        return events;
    }
    std::set<std::thread::id> Threads() {
        std::lock_guard lock(mutex);
        return {event_threads.begin(), event_threads.end()};
    }
};

enum : u32 { kPlain = 0, kGated = 1, kSignal = 2, kPost = 3, kSlow = 4 };

GxCommand Cmd(u32 kind, u32 a, u32 b, u32 tag) {
    GxCommand c{};
    c.words[0] = kind;
    c.words[1] = a;
    c.words[2] = b;
    c.words[7] = tag;
    return c;
}

class FakeGpu final : public Gpu, public GpuDebug {
public:
    FakeGpu(Host& host, Script& script) : host_(host), s_(script) {}
    ~FakeGpu() override {
        s_.Add("~FakeGpu");
    }

    void Execute(const GxCommand& c) override {
        s_.Add(fmt::format("Execute {}", c.words[7]));
        ++s_.draws;
        switch (c.words[0] & 0xFF) {
        case kGated:
            s_.entered.Signal();
            s_.gate.Wait();
            break;
        case kSignal:
            host_.SignalInterrupt(static_cast<Interrupt>(c.words[1]), c.words[2]);
            break;
        case kPost: {
            // A write into guest memory: the data is made here, the store is handed to the scheduler.
            const u32 offset = c.words[1];
            const u8 value = static_cast<u8>(c.words[2]);
            Script* s = &s_;
            host_.Post([s, offset, value] {
                s->host->memory[offset] = value;
                std::lock_guard lock(s->mutex);
                s->write_threads.push_back(std::this_thread::get_id());
                ++s->writes;
            });
            break;
        }
        case kSlow:
            std::this_thread::sleep_for(std::chrono::milliseconds(2)); // work to measure, not ordering
            break;
        default:
            break;
        }
    }
    void WriteReg(u32 addr, u32 value) override {
        s_.Add(fmt::format("WriteReg {:08x}={}", addr, value));
        regs_[addr] = value;
    }
    u32 ReadReg(u32 addr) override {
        s_.Add(fmt::format("ReadReg {:08x}", addr));
        const auto it = regs_.find(addr);
        return it != regs_.end() ? it->second : (0xC0DE0000u | (addr & 0xFFFFu));
    }
    void SetBufferSwap(u32 screen, const FrameBufferInfo& info) override {
        s_.Add(fmt::format("SetBufferSwap {} {:08x} {}", screen, info.address_left, info.active_fb));
    }
    void SetColorFill(u32 fill) override {
        s_.Add(fmt::format("SetColorFill {:08x}", fill));
    }
    void FlushRegion(u32 vaddr, u32 size) override {
        s_.Add(fmt::format("FlushRegion {:08x}+{:x}", vaddr, size));
    }
    void InvalidateRegion(u32 vaddr, u32 size) override {
        s_.Add(fmt::format("InvalidateRegion {:08x}+{:x}", vaddr, size));
    }
    void VBlank(u32 screen) override {
        s_.Add(fmt::format("VBlank {}", screen));
    }

    void LoadInitState(const InitState& state) override {
        s_.Add(fmt::format("LoadInitState {}", state.Block(InitBlock::GpuRegs).size()));
    }
    std::optional<std::vector<RegisterWrite>> TakeRegisterWrites() override {
        s_.Add("TakeRegisterWrites");
        return std::vector<RegisterWrite>{{0x10, 0xF, 0x1234}};
    }
    void WriteBackColorBuffers() override {
        s_.Add("WriteBackColorBuffers");
    }
    std::vector<Counter> TakeFrameCounters() override {
        s_.NoteThread();
        return {{"draws", s_.draws.exchange(0)}};
    }
    std::optional<u64> TakeFrameGpuTimeNs() override {
        s_.NoteThread();
        return 700;
    }

private:
    Host& host_;
    Script& s_;
    std::map<u32, u32> regs_;
};

// A wrapped GPU with no GpuDebug.
class PlainGpu final : public Gpu {
public:
    explicit PlainGpu(Script& s) : s_(s) {}
    void Execute(const GxCommand& c) override { s_.Add(fmt::format("Execute {}", c.words[7])); }
    void WriteReg(u32, u32) override {}
    u32 ReadReg(u32) override { return 0; }
    void SetBufferSwap(u32, const FrameBufferInfo&) override {}
    void SetColorFill(u32) override {}
    void FlushRegion(u32, u32) override {}
    void InvalidateRegion(u32, u32) override {}
    void VBlank(u32) override {}

private:
    Script& s_;
};

struct Rig {
    Script script;
    FakeHost host;
    std::unique_ptr<QueueGpu> queue;

    explicit Rig(QueueConfig config = {}) {
        script.host = &host;
        queue = std::make_unique<QueueGpu>(
            host,
            [this](Host& h) { return std::unique_ptr<Gpu>(std::make_unique<FakeGpu>(h, script)); },
            config);
    }
    QueueGpu& q() { return *queue; }
    // Closes the wrapper's own thread and the queue's gate, so a failing check cannot hang the exit.
    ~Rig() { script.gate.Open(); }
};

// Counts the breaches of the owner rule while it lives.
struct BreachCapture {
    std::mutex mutex;
    std::vector<rhi::OwnerRuleBreach> breaches;
    rhi::OwnerRuleHandler previous;
    BreachCapture() {
        previous = rhi::SetOwnerRuleHandler([this](const rhi::OwnerRuleBreach& breach) {
            std::lock_guard lock(mutex);
            breaches.push_back(breach);
        });
    }
    ~BreachCapture() { rhi::SetOwnerRuleHandler(previous); }
    size_t Total() {
        std::lock_guard lock(mutex);
        return breaches.size();
    }
    std::vector<rhi::OwnerRuleBreach> All() {
        std::lock_guard lock(mutex);
        return breaches;
    }
};

FrameBufferInfo Info(u32 address) {
    FrameBufferInfo info{};
    info.active_fb = 1;
    info.address_left = address;
    return info;
}

Counter Find(const std::vector<Counter>& counters, const std::string& name) {
    for (const Counter& c : counters) {
        if (c.name == name) {
            return c;
        }
    }
    return {"<missing " + name + ">", ~u64{0}};
}

} // namespace

// ---- command order -------------------------------------------------------------------------------

TEST_CASE("queue/calls_return_at_once_and_reach_the_wrapped_gpu_in_order_on_the_renderer_thread") {
    Rig rig;
    QueueGpu& q = rig.q();
    // the first command holds the renderer thread: nothing after it can have run when the calls return
    q.Execute(Cmd(kGated, 0, 0, 0));
    rig.script.entered.Wait();
    q.WriteReg(0x1EF00010, 1);
    q.Execute(Cmd(kPlain, 0, 0, 1));
    q.SetBufferSwap(0, Info(0x18000000));
    q.SetColorFill(0x1234);
    q.FlushRegion(0x14000000, 0x100);
    q.VBlank(1);
    q.Execute(Cmd(kPlain, 0, 0, 2));
    q.VBlank(0);
    CHECK(rig.script.Events() == std::vector<std::string>{"Execute 0"});

    rig.script.gate.Open();
    q.WaitIdle();
    const std::vector<std::string> expected = {
        "Execute 0",
        "WriteReg 1ef00010=1",
        "Execute 1",
        "SetBufferSwap 0 18000000 1",
        "SetColorFill 00001234",
        "FlushRegion 14000000+100",
        "VBlank 1",
        "Execute 2",
        "VBlank 0",
    };
    CHECK(rig.script.Events() == expected);
    // every call reached the wrapped GPU on the renderer thread, and that is not this one
    const std::set<std::thread::id> threads = rig.script.Threads();
    CHECK(threads.size() == 1);
    CHECK(*threads.begin() == q.RendererThread());
    CHECK(q.RendererThread() != std::this_thread::get_id());
}

TEST_CASE("queue/a_buffer_swap_and_the_vblank_after_it_arrive_in_the_order_the_game_made_them") {
    Rig rig;
    QueueGpu& q = rig.q();
    q.Execute(Cmd(kGated, 0, 0, 0));
    rig.script.entered.Wait();
    // two frames' worth, swaps of both screens, then their VBlanks, as the GSP applies them
    q.SetBufferSwap(0, Info(0x18000000));
    q.SetBufferSwap(1, Info(0x18100000));
    q.VBlank(0);
    q.VBlank(1);
    rig.script.gate.Open();
    q.WaitIdle();
    const std::vector<std::string> expected = {"Execute 0", "SetBufferSwap 0 18000000 1", "SetBufferSwap 1 18100000 1",
                                               "VBlank 0", "VBlank 1"};
    CHECK(rig.script.Events() == expected);
}

TEST_CASE("queue/interrupts_go_back_in_the_order_the_commands_complete") {
    Rig rig;
    QueueGpu& q = rig.q();
    constexpr int kCommands = 40;
    std::vector<Interrupt> expected;
    for (int i = 0; i < kCommands; ++i) {
        // each command signals two interrupts; ids and delays tell them apart
        const auto first = static_cast<Interrupt>(i % 7);
        const auto second = static_cast<Interrupt>((i + 3) % 7);
        q.Execute(Cmd(kSignal, static_cast<u32>(first), 1000 + i, i));
        q.Execute(Cmd(kSignal, static_cast<u32>(second), 2000 + i, i));
        expected.push_back(first);
        expected.push_back(second);
    }
    q.WaitIdle();
    const std::vector<Signalled> signals = rig.host.Signals();
    REQUIRE(signals.size() == expected.size());
    for (size_t i = 0; i < signals.size(); ++i) {
        CHECK(signals[i].id == expected[i]);
        CHECK(signals[i].delay_ns == (i % 2 == 0 ? 1000 : 2000) + i / 2);
        CHECK(signals[i].thread == q.RendererThread());
    }
}

// ---- invalidate and flush ------------------------------------------------------------------------

TEST_CASE("queue/an_invalidate_waits_for_an_earlier_transfer_and_for_what_it_wrote_back") {
    Rig rig;
    QueueGpu& q = rig.q();
    q.Execute(Cmd(kGated, 0, 0, 0)); // the renderer is inside this command
    rig.script.entered.Wait();
    q.Execute(Cmd(kPost, 16, 0xAB, 1)); // an earlier "transfer": its write into guest memory is posted
    CHECK(rig.host.memory[16] == 0);

    std::atomic<bool> returned{false};
    std::atomic<int> seen{-1};
    std::thread::id scheduler;
    std::thread thread([&] {
        scheduler = std::this_thread::get_id(); // the scheduler thread of this case
        q.InvalidateRegion(0x14000000, 0x100);
        seen = rig.host.memory[16];
        returned = true;
    });
    WaitUntilBlocked(q, 1);
    // the gate is closed, so the transfer cannot be done: the invalidate has not returned
    CHECK_FALSE(returned.load());
    CHECK(rig.host.memory[16] == 0);

    rig.script.gate.Open();
    thread.join();
    CHECK(returned.load());
    CHECK(seen.load() == 0xAB); // written back before the invalidate returned
    const std::vector<std::string> expected = {"Execute 0", "Execute 1", "InvalidateRegion 14000000+100"};
    CHECK(rig.script.Events() == expected);
    // the write ran on the thread that asked (the scheduler's), once
    REQUIRE(rig.script.write_threads.size() == 1);
    CHECK(rig.script.write_threads[0] == scheduler);
    CHECK(rig.host.RunPosted() == 1); // the host's own safe point finds the work already done
    CHECK(rig.script.writes.load() == 1);
}

TEST_CASE("queue/writes_into_guest_memory_arrive_only_through_post") {
    Rig rig;
    QueueGpu& q = rig.q();
    q.Execute(Cmd(kPost, 0, 0x11, 0));
    q.Execute(Cmd(kPost, 1, 0x22, 1));
    q.WaitIdle();
    // the commands are done; the queue and the renderer thread wrote nothing into guest memory
    CHECK(std::all_of(rig.host.memory.begin(), rig.host.memory.end(), [](u8 b) { return b == 0; }));
    CHECK(rig.host.PostedCount() == 2);
    // the scheduler's safe point (this thread) applies them
    CHECK(rig.host.RunPosted() == 2);
    CHECK(rig.host.memory[0] == 0x11);
    CHECK(rig.host.memory[1] == 0x22);
    REQUIRE(rig.script.write_threads.size() == 2);
    CHECK(rig.script.write_threads[0] == std::this_thread::get_id());
    CHECK(rig.script.write_threads[1] == std::this_thread::get_id());
}

TEST_CASE("queue/a_wrapped_gpu_built_with_the_plain_host_still_posts_and_the_invalidate_still_waits") {
    Script script;
    FakeHost host;
    script.host = &host;
    QueueGpu q(std::make_unique<FakeGpu>(host, script));
    q.Execute(Cmd(kPost, 5, 0x77, 0));
    q.InvalidateRegion(0x14000000, 0x10);
    // earlier commands are done; the write was posted to the real host and waits for its safe point
    CHECK(script.Events() == std::vector<std::string>{"Execute 0", "InvalidateRegion 14000000+10"});
    CHECK(host.memory[5] == 0);
    CHECK(host.RunPosted() == 1);
    CHECK(host.memory[5] == 0x77);
    CHECK(q.Proxy() == nullptr);
}

TEST_CASE("queue/an_invalidate_runs_only_the_writes_posted_before_it_reached_the_wrapped_gpu") {
    FakeHost host;
    int ran = 0;
    QueueHost proxy(host);
    proxy.Post([&] { ran += 1; });
    proxy.Post([&] { ran += 10; });
    const u64 mark = proxy.PostedMark();
    CHECK(mark == 2);
    proxy.Post([&] { ran += 100; }); // posted after the mark
    proxy.RunPostedUpTo(mark);
    CHECK(ran == 11);
    proxy.RunPostedUpTo(mark); // nothing runs twice
    CHECK(ran == 11);
    CHECK(host.RunPosted() == 3); // the host's safe point runs the third, and finds the first two done
    CHECK(ran == 111);
    CHECK(proxy.PostedMark() == 3);
}

TEST_CASE("queue/a_flush_is_queued_in_order_with_the_commands_around_it") {
    Rig rig;
    QueueGpu& q = rig.q();
    q.Execute(Cmd(kGated, 0, 0, 0));
    rig.script.entered.Wait();
    q.Execute(Cmd(kPlain, 0, 0, 1));
    q.FlushRegion(0x14000000, 0x200);
    q.Execute(Cmd(kPlain, 0, 0, 2));
    CHECK(rig.script.Events().size() == 1); // returned at once
    rig.script.gate.Open();
    q.WaitIdle();
    const std::vector<std::string> expected = {"Execute 0", "Execute 1", "FlushRegion 14000000+200", "Execute 2"};
    CHECK(rig.script.Events() == expected);
}

// ---- frames in flight ----------------------------------------------------------------------------

TEST_CASE("queue/more_than_one_frame_in_flight_blocks_the_vblank_that_would_make_it_and_is_counted") {
    Rig rig;
    QueueGpu& q = rig.q();
    q.Execute(Cmd(kGated, 0, 0, 0)); // frame 1 is being rendered, and the renderer is held in it
    rig.script.entered.Wait();
    q.VBlank(0);                      // frame 1 ended: one frame in flight, returns at once
    q.VBlank(1);                      // the bottom screen's VBlank does not end a frame
    q.Execute(Cmd(kPlain, 0, 0, 1));  // frame 2 is being built

    std::atomic<int> progress{0};
    std::thread thread([&] {
        q.VBlank(0); // would make two frames in flight: waits
        progress = 1;
        q.Execute(Cmd(kPlain, 0, 0, 2));
        progress = 2;
    });
    WaitUntilBlocked(q, 1);
    CHECK(progress.load() == 0);
    CHECK(rig.script.Events() == std::vector<std::string>{"Execute 0"});
    // a measurable wait (the sleep orders nothing: the caller is blocked until the gate opens)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    rig.script.gate.Open();
    thread.join();
    q.WaitIdle();
    CHECK(progress.load() == 2);
    const std::vector<std::string> expected = {"Execute 0", "VBlank 0", "VBlank 1", "Execute 1", "VBlank 0", "Execute 2"};
    CHECK(rig.script.Events() == expected);

    const std::vector<Counter> counters = q.TakeFrameCounters();
    CHECK(Find(counters, "queue_frame_waits").value == 1);
    CHECK(Find(counters, "queue_wait_us").value >= 2000);
    // reading resets
    CHECK(Find(q.TakeFrameCounters(), "queue_frame_waits").value == 0);
}

TEST_CASE("queue/the_frame_limit_is_a_setting_and_a_renderer_that_keeps_up_never_waits") {
    {
        QueueConfig config;
        config.max_frames_in_flight = 2;
        Rig rig(config);
        QueueGpu& q = rig.q();
        q.Execute(Cmd(kGated, 0, 0, 0));
        rig.script.entered.Wait();
        q.VBlank(0);
        q.VBlank(0); // two in flight: both return at once
        std::atomic<bool> returned{false};
        std::thread thread([&] {
            q.VBlank(0);
            returned = true;
        });
        WaitUntilBlocked(q, 1);
        CHECK_FALSE(returned.load());
        rig.script.gate.Open();
        thread.join();
        q.WaitIdle();
        CHECK(Find(q.TakeFrameCounters(), "queue_frame_waits").value == 1);
    }
    {
        Rig rig;
        QueueGpu& q = rig.q();
        for (int frame = 0; frame < 20; ++frame) {
            q.Execute(Cmd(kPlain, 0, 0, frame));
            q.VBlank(0);
            q.WaitIdle(); // the renderer is done with the frame before the next ends
        }
        CHECK(Find(q.TakeFrameCounters(), "queue_frame_waits").value == 0);
    }
}

// ---- registers -----------------------------------------------------------------------------------

TEST_CASE("queue/read_reg_is_answered_at_once_from_what_was_written_through_the_wrapper") {
    Rig rig;
    QueueGpu& q = rig.q();
    q.Execute(Cmd(kGated, 0, 0, 0));
    rig.script.entered.Wait();
    q.WriteReg(0x1EF00010, 5);
    CHECK(q.ReadReg(0x1EF00010) == 5); // the wrapped GPU has not seen the write yet
    q.WriteReg(0x1EF00010, 9);
    CHECK(q.ReadReg(0x1EF00010) == 9);
    CHECK(rig.script.Events() == std::vector<std::string>{"Execute 0"});

    // a register never written through the wrapper is the wrapped GPU's to answer, after what is queued
    std::atomic<u32> value{0};
    std::thread thread([&] { value = q.ReadReg(0x1EF00020); });
    WaitUntilBlocked(q, 1);
    rig.script.gate.Open();
    thread.join();
    CHECK(value.load() == (0xC0DE0000u | 0x0020u));
    const std::vector<std::string> expected = {"Execute 0", "WriteReg 1ef00010=5", "WriteReg 1ef00010=9", "ReadReg 1ef00020"};
    CHECK(rig.script.Events() == expected);
}

// ---- counters and the debug interface --------------------------------------------------------------

TEST_CASE("queue/counters_report_the_renderers_busy_time_and_the_wrapped_gpus_own_counters_summed_per_frame") {
    Rig rig;
    QueueGpu& q = rig.q();
    q.Execute(Cmd(kPlain, 0, 0, 0));
    q.Execute(Cmd(kSlow, 0, 0, 1));
    q.Execute(Cmd(kPlain, 0, 0, 2));
    q.VBlank(0);
    q.WaitIdle(); // frame 1 is rendered before frame 2 ends: the frame limit never applies
    q.Execute(Cmd(kPlain, 0, 0, 3));
    q.Execute(Cmd(kPlain, 0, 0, 4));
    q.VBlank(0);
    q.WaitIdle();
    // the wrapper took the wrapped GPU's counters at each VBlank(0) on the renderer thread; reading
    // them here needs no wait for the queue and gives the two frames together
    std::vector<Counter> counters = q.TakeFrameCounters();
    CHECK(Find(counters, "draws").value == 5);
    CHECK(Find(counters, "translate_us").value >= 2000);
    CHECK(Find(counters, "queue_wait_us").name == "queue_wait_us");
    CHECK(Find(counters, "queue_frame_waits").value == 0);
    CHECK(q.TakeFrameGpuTimeNs() == std::optional<u64>(1400));
    // reading resets all of it
    counters = q.TakeFrameCounters();
    CHECK(Find(counters, "draws").name == "<missing draws>");
    CHECK(Find(counters, "translate_us").value < 2000);
    CHECK_FALSE(q.TakeFrameGpuTimeNs().has_value());
}

TEST_CASE("queue/the_debug_calls_run_on_the_renderer_thread_and_see_what_was_queued_before_them") {
    Rig rig;
    QueueGpu& q = rig.q();
    q.Execute(Cmd(kPlain, 0, 0, 0));
    q.WriteReg(0x1EF00040, 3);
    InitState state;
    q.LoadInitState(state);
    const auto writes = q.TakeRegisterWrites();
    REQUIRE(writes.has_value());
    CHECK(writes->size() == 1);
    CHECK((*writes)[0].reg == 0x10);
    CHECK((*writes)[0].value == 0x1234);
    q.WriteBackColorBuffers();
    const std::vector<std::string> expected = {"Execute 0", "WriteReg 1ef00040=3", "LoadInitState 0", "TakeRegisterWrites",
                                               "WriteBackColorBuffers"};
    CHECK(rig.script.Events() == expected);
    CHECK(rig.script.Threads() == std::set<std::thread::id>{q.RendererThread()});
    // the capture's init state replaces the register file the mirror was following
    CHECK(q.ReadReg(0x1EF00040) == 3); // forwarded now, and the wrapped GPU's own file has the write
}

TEST_CASE("queue/a_wrapped_gpu_without_the_debug_interface_gives_the_defaults") {
    Script script;
    QueueGpu q(std::make_unique<PlainGpu>(script));
    q.Execute(Cmd(kPlain, 0, 0, 0));
    q.LoadInitState(InitState{});
    CHECK_FALSE(q.TakeRegisterWrites().has_value());
    q.WriteBackColorBuffers();
    q.VBlank(0);
    q.WaitIdle();
    const std::vector<Counter> counters = q.TakeFrameCounters();
    CHECK(Find(counters, "translate_us").name == "translate_us"); // only the wrapper's own
    CHECK(counters.size() == 3);
    CHECK_FALSE(q.TakeFrameGpuTimeNs().has_value());
}

// ---- lockstep and the registered wrapper -----------------------------------------------------------

TEST_CASE("queue/in_lockstep_every_call_is_done_when_it_returns") {
    QueueConfig config;
    config.lockstep = true;
    Rig rig(config);
    QueueGpu& q = rig.q();
    q.Execute(Cmd(kSignal, static_cast<u32>(Interrupt::P3D), 7, 0));
    CHECK(rig.script.Events() == std::vector<std::string>{"Execute 0"});
    REQUIRE(rig.host.Signals().size() == 1);
    CHECK(rig.host.Signals()[0].id == Interrupt::P3D);
    q.Execute(Cmd(kPost, 3, 0x42, 1));
    CHECK(rig.host.PostedCount() == 1); // already posted
    q.WriteReg(0x1EF00010, 1);
    CHECK(rig.script.Events().back() == "WriteReg 1ef00010=1");
    q.VBlank(0);
    CHECK(rig.script.Events().back() == "VBlank 0");
    // the wrapped GPU's counters are read through, like the player does
    const std::vector<Counter> counters = q.TakeFrameCounters();
    CHECK(Find(counters, "draws").value == 2);
    CHECK(q.TakeFrameGpuTimeNs() == std::optional<u64>(700));
    CHECK(rig.script.Threads() == std::set<std::thread::id>{q.RendererThread()});
}

TEST_CASE("queue/the_wrapper_and_its_option_are_registered_for_the_capture_player") {
    const GpuWrapper* wrapper = Registry<GpuWrapper>::Instance().Find("queue");
    REQUIRE(wrapper != nullptr);
    const Options parsed = ParseOptions("capture-player", std::vector<std::string>{"--queue"});
    CHECK(parsed.ok());
    CHECK(parsed.Has("--queue"));
    CHECK(Usage("capture-player").find("--queue") != std::string::npos);

    Script script;
    FakeHost host;
    script.host = &host;
    std::unique_ptr<Gpu> wrapped =
        (*wrapper)(std::make_unique<FakeGpu>(host, script), host, parsed);
    REQUIRE(wrapped != nullptr);
    // the player finds the debug interface of the outermost object
    auto* debug = dynamic_cast<GpuDebug*>(wrapped.get());
    REQUIRE(debug != nullptr);
    // the player judges right after each call: the wrapper completes each before it returns
    wrapped->Execute(Cmd(kSignal, static_cast<u32>(Interrupt::PSC0), 0, 0));
    CHECK(script.Events() == std::vector<std::string>{"Execute 0"});
    CHECK(host.Signals().size() == 1);
    CHECK(script.Threads().size() == 1);
    CHECK(*script.Threads().begin() != std::this_thread::get_id());
    CHECK(debug->TakeRegisterWrites().has_value());
    wrapped.reset();
    CHECK(script.Events().back() == "~FakeGpu");
}

// ---- life cycle ----------------------------------------------------------------------------------

TEST_CASE("queue/the_destructor_finishes_the_queued_work_and_destroys_the_wrapped_gpu_on_the_renderer_thread") {
    Script script;
    FakeHost host;
    script.host = &host;
    std::thread::id renderer;
    {
        QueueGpu q(host, [&](Host& h) { return std::unique_ptr<Gpu>(std::make_unique<FakeGpu>(h, script)); });
        renderer = q.RendererThread();
        q.Execute(Cmd(kGated, 0, 0, 0));
        script.entered.Wait();
        for (int i = 1; i <= 5; ++i) {
            q.Execute(Cmd(kPlain, 0, 0, i));
        }
        script.gate.Open();
    } // destroyed with the queue not necessarily empty
    const std::vector<std::string> events = script.Events();
    REQUIRE(events.size() == 7);
    CHECK(events[0] == "Execute 0");
    CHECK(events[5] == "Execute 5");
    CHECK(events[6] == "~FakeGpu");
    CHECK(script.Threads() == std::set<std::thread::id>{renderer});
}

TEST_CASE("queue/a_wrapped_gpu_that_cannot_be_made_is_an_error_of_the_constructor") {
    FakeHost host;
    CHECK_THROWS(QueueGpu(host, [](Host&) { return std::unique_ptr<Gpu>(); }));
    CHECK_THROWS_AS(QueueGpu(host, [](Host&) -> std::unique_ptr<Gpu> { throw std::invalid_argument("no"); }),
                    std::invalid_argument);
    CHECK_THROWS(QueueGpu(std::unique_ptr<Gpu>()));
}

// ---- the owner rule --------------------------------------------------------------------------------

TEST_CASE("owner_rule/a_forwarded_call_from_another_thread_calls_the_handler") {
    if (!kRhiChecks) {
        WARN_MESSAGE(false, "skipped: the owner-rule checks are compiled out (MH4N_RHI_CHECKS=OFF)");
        return;
    }
    Rig rig;
    QueueGpu& q = rig.q();
    BreachCapture breaches;
    // every call the wrapper itself forwards runs on the renderer thread: no breach
    q.Execute(Cmd(kPlain, 0, 0, 0));
    q.WriteReg(0x1EF00010, 1);
    q.SetBufferSwap(0, Info(0x18000000));
    q.SetColorFill(0);
    q.FlushRegion(0x14000000, 0x10);
    q.InvalidateRegion(0x14000000, 0x10);
    q.VBlank(0);
    q.VBlank(1);
    q.ReadReg(0x1EF00099);
    q.LoadInitState(InitState{});
    q.TakeRegisterWrites();
    q.WriteBackColorBuffers();
    q.TakeFrameCounters();
    q.TakeFrameGpuTimeNs();
    q.WaitIdle();
    CHECK(breaches.Total() == 0);

    // a forwarded call made from another real thread is a breach, reported on that thread
    std::thread::id other_id;
    std::thread other([&] {
        other_id = std::this_thread::get_id();
        q.ForwardTarget("Execute");
    });
    other.join();
    REQUIRE(breaches.Total() == 1);
    const rhi::OwnerRuleBreach breach = breaches.All()[0];
    CHECK(breach.rule == rhi::OwnerRule::WrongThread);
    CHECK(breach.message.find("Execute") != std::string::npos);
    CHECK(other_id != q.RendererThread());

    // and from this thread too: the producer is not the renderer either
    q.ForwardTarget("FlushRegion");
    CHECK(breaches.Total() == 2);
    CHECK(breaches.All()[1].message.find("FlushRegion") != std::string::npos);
}

TEST_CASE("owner_rule/calls_from_many_threads_are_all_forwarded_by_the_renderer_thread") {
    if (!kRhiChecks) {
        WARN_MESSAGE(false, "skipped: the owner-rule checks are compiled out (MH4N_RHI_CHECKS=OFF)");
        return;
    }
    // calls made by several threads through the public interface are still forwarded by the renderer
    // thread alone
    Rig rig;
    QueueGpu& q = rig.q();
    BreachCapture breaches;
    constexpr int kThreads = 4;
    constexpr int kCalls = 50;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kCalls; ++i) {
                q.Execute(Cmd(kPlain, 0, 0, t * 1000 + i));
                q.WriteReg(0x1EF00000 + 4 * t, i);
                q.FlushRegion(0x14000000, 4);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    q.WaitIdle();
    CHECK(breaches.Total() == 0);
    CHECK(rig.script.Events().size() == static_cast<size_t>(kThreads * kCalls * 3));
    CHECK(rig.script.Threads() == std::set<std::thread::id>{q.RendererThread()});
    // each producer's own calls kept their order
    for (int t = 0; t < kThreads; ++t) {
        int last = -1;
        for (const std::string& event : rig.script.Events()) {
            const std::string prefix = "Execute ";
            if (event.rfind(prefix, 0) == 0) {
                const int tag = std::stoi(event.substr(prefix.size()));
                if (tag / 1000 == t) {
                    CHECK(tag % 1000 == last + 1);
                    last = tag % 1000;
                }
            }
        }
        CHECK(last == kCalls - 1);
    }
}

// ---- a long mixed run --------------------------------------------------------------------------------

TEST_CASE("queue/a_long_mixed_sequence_keeps_its_order_and_its_interrupts") {
    Rig rig;
    QueueGpu& q = rig.q();
    u32 state = 12345;
    const auto next = [&state] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    std::vector<std::string> expected;
    std::vector<Interrupt> expected_interrupts;
    std::map<u32, u32> mirror;
    int writes_posted = 0;
    constexpr int kOps = 3000;
    for (int i = 0; i < kOps; ++i) {
        switch (next() % 9) {
        case 0:
        case 1:
            q.Execute(Cmd(kPlain, 0, 0, i));
            expected.push_back(fmt::format("Execute {}", i));
            break;
        case 2: {
            const u32 id = next() % 7;
            q.Execute(Cmd(kSignal, id, 0, i));
            expected.push_back(fmt::format("Execute {}", i));
            expected_interrupts.push_back(static_cast<Interrupt>(id));
            break;
        }
        case 3: {
            q.Execute(Cmd(kPost, static_cast<u32>(i % 512), 1 + static_cast<u32>(i % 200), i));
            expected.push_back(fmt::format("Execute {}", i));
            ++writes_posted;
            break;
        }
        case 4: {
            const u32 addr = 0x1EF00000 + 4 * (next() % 8);
            const u32 value = next();
            q.WriteReg(addr, value);
            mirror[addr] = value;
            expected.push_back(fmt::format("WriteReg {:08x}={}", addr, value));
            CHECK(q.ReadReg(addr) == value); // from the mirror, in step with the call
            break;
        }
        case 5:
            q.VBlank(next() % 2);
            expected.push_back("VBlank");
            break;
        case 6:
            q.FlushRegion(0x14000000 + i, 4);
            expected.push_back(fmt::format("FlushRegion {:08x}+4", 0x14000000 + i));
            break;
        case 7:
            q.InvalidateRegion(0x14000000 + i, 4);
            expected.push_back(fmt::format("InvalidateRegion {:08x}+4", 0x14000000 + i));
            // everything before it is done, and so are the writes it posted
            CHECK(rig.script.Events().size() == expected.size());
            CHECK(rig.script.writes.load() == writes_posted);
            break;
        default:
            q.SetColorFill(static_cast<u32>(i));
            expected.push_back(fmt::format("SetColorFill {:08x}", i));
            break;
        }
    }
    q.WaitIdle();
    std::vector<std::string> events = rig.script.Events();
    // VBlank's screen is random in the run; compare them by kind
    for (std::string& e : events) {
        if (e.rfind("VBlank", 0) == 0) {
            e = "VBlank";
        }
    }
    CHECK(events == expected);
    const std::vector<Signalled> signals = rig.host.Signals();
    REQUIRE(signals.size() == expected_interrupts.size());
    for (size_t i = 0; i < signals.size(); ++i) {
        CHECK(signals[i].id == expected_interrupts[i]);
    }
    CHECK(rig.script.Threads() == std::set<std::thread::id>{q.RendererThread()});
}

// ---- the capture player, with and without --queue --------------------------------------------------------

namespace {

using Bytes = std::vector<u8>;
namespace fs = std::filesystem;

void Put32(Bytes& b, u32 v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(v >> (8 * i)));
}
void Put64(Bytes& b, u64 v) {
    Put32(b, static_cast<u32>(v));
    Put32(b, static_cast<u32>(v >> 32));
}

// A synthetic MGPU capture (the layout of docs/native/mgpu-format.md; nothing from a game).
class CaptureWriter {
public:
    CaptureWriter() {
        out.insert(out.end(), {'M', 'G', 'P', 'U'});
        Put32(out, 1);
    }
    void Rec(u32 type, const Bytes& payload) {
        Put32(out, type);
        Put32(out, frame);
        Put64(out, ticks += 1000);
        Put32(out, static_cast<u32>(payload.size()));
        out.insert(out.end(), payload.begin(), payload.end());
    }
    void Init() {
        Bytes payload;
        Put32(payload, 11);
        for (u32 kind = 1; kind <= 11; ++kind) {
            Bytes block(gpu_capture::InitBlockSize(kind), 0);
            Put32(payload, kind);
            Put32(payload, static_cast<u32>(block.size()));
            payload.insert(payload.end(), block.begin(), block.end());
        }
        Rec(gpu_capture::kInit, payload);
    }
    void Mem(u32 addr, const Bytes& data) {
        Bytes p;
        Put32(p, addr);
        Put32(p, static_cast<u32>(data.size()));
        p.insert(p.end(), data.begin(), data.end());
        Rec(gpu_capture::kMem, p);
    }
    static Bytes Pages(const std::vector<std::pair<u32, Bytes>>& pages) {
        Bytes p;
        Put32(p, static_cast<u32>(pages.size()));
        for (const auto& [addr, data] : pages) {
            Put32(p, addr);
            p.insert(p.end(), data.begin(), data.end());
        }
        return p;
    }
    void CpuPages(const std::vector<std::pair<u32, Bytes>>& pages) { Rec(gpu_capture::kCpuPages, Pages(pages)); }
    void GpuPages(const std::vector<std::pair<u32, Bytes>>& pages) { Rec(gpu_capture::kGpuPages, Pages(pages)); }
    void Gx(const std::array<u32, 8>& words) {
        Bytes p;
        for (u32 w : words) Put32(p, w);
        Rec(gpu_capture::kGx, p);
    }
    void Irq(u32 id) {
        Bytes p;
        Put32(p, id);
        Put64(p, 1000);
        Rec(gpu_capture::kIrq, p);
    }
    void Flush(u32 kind, u32 vaddr, u32 size) {
        Bytes p;
        Put32(p, kind);
        Put32(p, vaddr);
        Put32(p, size);
        Rec(gpu_capture::kFlush, p);
    }
    void Frame(u32 number) {
        frame = number;
        Bytes p;
        Put32(p, number);
        Rec(gpu_capture::kFrame, p);
    }
    bool Save(const fs::path& path) const {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
        return static_cast<bool>(f);
    }
    Bytes out;
    u32 frame = 0;
    u64 ticks = 0;
};

// A fill, a DMA, a flush and an invalidate of the pages the DMA wrote, two VBlanks and a frame. With
// `wrong_fill` the capture's pages for the fill say 0xAC, so a Gpu that fills with 0xAB fails.
std::string WriteScene(bool wrong_fill) {
    using namespace gpu_capture;
    const fs::path dir = fs::temp_directory_path() / fmt::format("mh4n_gpu_queue_test_{}_{:08x}", wrong_fill ? "bad" : "good", std::random_device{}());
    fs::create_directories(dir);
    const fs::path path = dir / "scene.mgpu";
    CaptureWriter w;
    w.Init();
    w.Mem(0x20000000, Bytes(0x200000, 0));
    Bytes pattern(3 * kPageSize);
    for (size_t i = 0; i < pattern.size(); ++i) pattern[i] = static_cast<u8>((i * 7 + (i >> 8)) % 200);
    w.CpuPages({{0x20010000, Bytes(pattern.begin(), pattern.begin() + kPageSize)},
                {0x20011000, Bytes(pattern.begin() + kPageSize, pattern.begin() + 2 * kPageSize)},
                {0x20012000, Bytes(pattern.begin() + 2 * kPageSize, pattern.end())}});
    w.Gx({kGxMemoryFill, 0x14000000, 0xABABABAB, 0x14001000, 0, 0, 0, 0x201});
    w.GpuPages({{0x20000000, Bytes(kPageSize, wrong_fill ? 0xAC : 0xAB)}});
    w.Irq(0); // PSC0
    w.Irq(2); // PDC0
    w.Gx({kGxRequestDma, 0x14010000, 0x14060000, 3 * kPageSize, 0, 0, 0, 0});
    w.GpuPages({{0x20060000, Bytes(pattern.begin(), pattern.begin() + kPageSize)},
                {0x20061000, Bytes(pattern.begin() + kPageSize, pattern.begin() + 2 * kPageSize)},
                {0x20062000, Bytes(pattern.begin() + 2 * kPageSize, pattern.end())}});
    w.Irq(6); // DMA
    w.Flush(kFlushDataCache, 0x14000000, 0x1000);
    w.Flush(kInvalidateDataCache, 0x14060000, 3 * kPageSize);
    w.Irq(3); // PDC1
    w.Frame(1);
    REQUIRE(w.Save(path));
    return path.string();
}

gpu_capture::PlayerResult RunPlayerWith(const std::string& capture, std::vector<std::string> args) {
    args.push_back(capture);
    const Options options = ParseOptions("capture-player", args);
    REQUIRE_MESSAGE(options.ok(), options.error());
    gpu_capture::PlayerConfig config;
    std::string error;
    REQUIRE_MESSAGE(gpu_capture::ConfigFromOptions(options, config, &error), error);
    return gpu_capture::RunPlayer(config, options);
}

// What a run says about the Gpu, without the timings.
std::string Summary(const gpu_capture::PlayerResult& r) {
    std::string text = fmt::format("error={} records={} failures={} frames={} unjudged={} gsp={}", r.error, r.records,
                                   r.failures.size(), r.frames.size(), r.unjudged_pages, r.gsp_queue_pages);
    for (const auto& [kind, count] : r.judged) text += fmt::format(" judged[{}]={}", kind, count);
    for (const auto& [kind, count] : r.skipped) text += fmt::format(" skipped[{}]={}", kind, count);
    for (const auto& f : r.failures) text += fmt::format(" | #{} {} {}", f.record, f.record_name, f.message);
    for (const auto& frame : r.frames) text += fmt::format(" | frame {} passed={}", frame.frame, frame.passed);
    return text;
}

} // namespace

TEST_CASE("queue/the_capture_player_in_chained_mode_gives_the_same_results_with_queue_as_without") {
    for (const bool wrong : {false, true}) {
        const std::string capture = WriteScene(wrong);
        for (const char* gpu_name : {"cpu-gx", "replay"}) {
            CAPTURE(gpu_name);
            CAPTURE(wrong);
            const std::vector<std::string> base = {"--gpu", gpu_name, "--chained", "--judge", "fill,transfer,copy,dma"};
            const gpu_capture::PlayerResult without = RunPlayerWith(capture, base);
            std::vector<std::string> with_queue = base;
            with_queue.push_back("--queue");
            const gpu_capture::PlayerResult with = RunPlayerWith(capture, with_queue);
            INFO("without: " << Summary(without));
            INFO("with:    " << Summary(with));
            CHECK(Summary(with) == Summary(without));
            CHECK(with.ExitStatus() == without.ExitStatus());
            if (std::string(gpu_name) == "cpu-gx") {
                // the fill and the DMA are real work there: the clean capture passes, the wrong one fails
                CHECK(without.ExitStatus() == (wrong ? 1 : 0));
            }
            // the queue's own counters are in the frame table next to the Gpu's
            REQUIRE(with.frames.size() == 1);
            bool has_busy = false;
            for (const gpu::Counter& c : with.frames[0].counters) has_busy = has_busy || c.name == "translate_us";
            CHECK(has_busy);
        }
        std::error_code ec;
        fs::remove_all(fs::path(capture).parent_path(), ec);
    }
}
