// The backend's sync module (N2-28): Submit with waits (each wait with its own stage; the CPU wait in place of the GPU wait
// when the `timeline` bit is off), deferred destruction, the pipeline-cache blob, pipelines made from several threads
// and the two GPU timestamps of a frame. Run on whatever Vulkan device the machine has (lavapipe in the cloud and in
// Linux CI); a case that needs a device skips with a message without one, and fails under MH4N_REQUIRE_VULKAN=1.
//
// Lavapipe has ONE queue: a wait across queues cannot be shown here (`unverified`: the RX 6700 XT's families, MoltenVK).
//
// The waits are shown with a gate: the first submission of a case starts with a vkCmdWaitEvents on an event the test
// sets from the host, so it cannot finish until the test says so. With timeline semaphores the second submission
// returns at once and the GPU holds it; without them (`MH4N_RHI_MASK=timeline`, or the case's own masked backend) the
// submitter blocks until the gate opens. Neither depends on how fast the machine is.
//
// Run it as the PC's paths would: MH4N_RHI_MASK=timeline ctest --test-dir build/native -R '^gpu_backend_sync_test$'
// (docs/native/rhi.md, "Sync and readback (N2-28)", lists the masks). With MH4N_RHI_CHECKS off the owner-rule cases skip
// themselves with a message.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gpu/backend/sync/sync.h"
#include "gpu/backend/targets/impl.h"
#include "mh4n/log.h"
#include "support/vulkan_device.h"

#include "gpu_backend_sync_shaders.inc"

using mh4n::s32;
using mh4n::u32;
using mh4n::u64;
using mh4n::u8;
namespace test = mh4n::test;
using namespace mh4n::gpu::rhi;
using namespace std::chrono_literals;

namespace {

// ---- helpers ---------------------------------------------------------------------------------------------------

template <size_t N>
ShaderCode Spv(const uint32_t (&words)[N]) {
    ShaderCode code;
    code.spirv = std::span<const u32>(words, N);
    return code;
}

struct BreachCapture {
    std::mutex mutex;
    std::vector<OwnerRule> rules;
    OwnerRuleHandler previous;
    BreachCapture() {
        previous = SetOwnerRuleHandler([this](const OwnerRuleBreach& breach) {
            std::lock_guard lock(mutex);
            rules.push_back(breach.rule);
        });
    }
    ~BreachCapture() { SetOwnerRuleHandler(previous); }
    int Count(OwnerRule rule) {
        std::lock_guard lock(mutex);
        return static_cast<int>(std::count(rules.begin(), rules.end(), rule));
    }
    size_t Total() {
        std::lock_guard lock(mutex);
        return rules.size();
    }
};

// The lines the mh4n log receives while it lives (the pipeline-cache refusal is an Info line there).
struct LogCapture {
    struct Line {
        mh4n::LogLevel level;
        std::string category;
        std::string message;
    };
    std::mutex mutex;
    std::vector<Line> lines;
    LogCapture() {
        mh4n::SetLogSink([this](const mh4n::LogRecord& record) {
            std::lock_guard lock(mutex);
            lines.push_back(Line{record.level, std::string(record.category), std::string(record.message)});
        });
    }
    ~LogCapture() { mh4n::SetLogSink({}); }
    std::vector<Line> With(const std::string& part) {
        std::lock_guard lock(mutex);
        std::vector<Line> found;
        for (const Line& line : lines) {
            if (line.message.find(part) != std::string::npos) found.push_back(line);
        }
        return found;
    }
};

std::shared_ptr<Backend> MakeBackend(std::initializer_list<Cap> masked = {}, std::shared_ptr<Instance> instance = nullptr) {
    BackendOptions options;
    for (Cap cap : masked) options.mask = options.mask.With(cap);
    return test::RequireBackend(options, std::move(instance));
}

std::shared_ptr<Image> MakeImage(Backend& backend, Format format, u32 width, u32 height) {
    ImageDesc desc;
    desc.format = format;
    desc.width = width;
    desc.height = height;
    desc.name = "test image";
    Result<std::shared_ptr<Image>> image = backend.CreateImage(desc);
    REQUIRE_MESSAGE(image.ok(), image.error());
    return *image;
}

std::shared_ptr<Buffer> MakeBuffer(Backend& backend, u64 size, Heap heap, BufferUsage usage) {
    BufferDesc desc;
    desc.size = size;
    desc.heap = heap;
    desc.usage = usage;
    desc.name = "test buffer";
    Result<std::shared_ptr<Buffer>> buffer = backend.CreateBuffer(desc);
    REQUIRE_MESSAGE(buffer.ok(), buffer.error());
    return *buffer;
}

const BindingSetDesc kSampledAndSampler{{{0, BindingType::SampledImage}, {1, BindingType::Sampler}}};

std::shared_ptr<Pipeline> MakePipeline(Backend& backend, ShaderCode vertex, ShaderCode fragment, Format color, Format depth,
                                       const FixedFunctionState& state = {}, std::vector<BindingSetDesc> sets = {},
                                       std::string name = "test") {
    GraphicsPipelineDesc desc;
    desc.vertex = vertex;
    desc.fragment = fragment;
    desc.color_format = color;
    desc.depth_format = depth;
    desc.state = state;
    desc.sets = std::move(sets);
    desc.name = std::move(name);
    Result<std::shared_ptr<Pipeline>> pipeline = backend.CreateGraphicsPipeline(desc);
    REQUIRE_MESSAGE(pipeline.ok(), pipeline.error());
    return *pipeline;
}

std::shared_ptr<Sampler> NearestSampler(Backend& backend) {
    SamplerDesc desc;
    desc.min_filter = desc.mag_filter = desc.mip_filter = Filter::Nearest;
    Result<std::shared_ptr<Sampler>> sampler = backend.CreateSampler(desc);
    REQUIRE_MESSAGE(sampler.ok(), sampler.error());
    return *sampler;
}

BindingSet* SampleSet(BindingArena& arena, const Image& texture, const Sampler& sampler) {
    Result<BindingSet*> set = arena.Acquire(kSampledAndSampler);
    REQUIRE_MESSAGE(set.ok(), set.error());
    Result<void> a = (*set)->SetSampledImage(0, texture, ImageView::Sampled);
    REQUIRE_MESSAGE(a.ok(), a.error());
    REQUIRE((*set)->SetSampler(1, sampler).ok());
    return *set;
}

TargetPassDesc Pass(Image* color, std::array<float, 4> clear = {0, 0, 0, 1}) {
    TargetPassDesc pass;
    pass.color = color;
    pass.color_load = LoadOp::Clear;
    pass.clear_color = clear;
    return pass;
}

// Reads a 4x4 RGBA8 image back through CopyImageToBuffer and ReadBuffer (not through the readback module), on the
// calling thread, which must own the image.
std::vector<u8> ReadBack(Backend& backend, Image& image, u32 width = 4, u32 height = 4) {
    const u64 size = u64(width) * height * 4;
    std::shared_ptr<Buffer> buffer = MakeBuffer(backend, size, Heap::Readback, BufferUsage::Transfer);
    Recorder rec = backend.BeginRecording();
    BufferImageRegion region;
    region.image_rect = Rect{0, 0, width, height};
    rec.CopyImageToBuffer(image, *buffer, std::span<const BufferImageRegion>(&region, 1));
    const TimelineValue value = backend.Submit(std::move(rec));
    std::vector<u8> bytes(size);
    Result<void> read = backend.ReadBuffer(*buffer, 0, bytes, value);
    REQUIRE_MESSAGE(read.ok(), read.error());
    return bytes;
}

// Whether all `count` pixels of a tightly packed RGBA8 image are (r, g, b, a).
bool AllPixels(const std::vector<u8>& bytes, u8 r, u8 g, u8 b, u8 a) {
    if (bytes.empty() || bytes.size() % 4 != 0) return false;
    for (size_t i = 0; i < bytes.size(); i += 4) {
        if (bytes[i] != r || bytes[i + 1] != g || bytes[i + 2] != b || bytes[i + 3] != a) return false;
    }
    return true;
}

// ---- the gate --------------------------------------------------------------------------------------------------

// A submission that starts with Close(recorder) cannot finish until Open(): a vkCmdWaitEvents on a host-set event.
struct Gate {
    VkDevice device = VK_NULL_HANDLE;
    VkEvent event = VK_NULL_HANDLE;
    bool open = false;
    explicit Gate(Backend& backend) {
        device = Handles(backend).device;
        VkEventCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
        REQUIRE(vkCreateEvent(device, &info, nullptr, &event) == VK_SUCCESS);
    }
    Gate(const Gate&) = delete;
    Gate& operator=(const Gate&) = delete;
    void Close(Recorder& recorder) {
        vkCmdWaitEvents(CommandBuffer(recorder), 1, &event, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                        nullptr, 0, nullptr, 0, nullptr);
    }
    void Open() {
        if (!open) {
            open = true;
            vkSetEvent(device, event);
        }
    }
    ~Gate() {
        Open();
        vkDeviceWaitIdle(device);
        vkDestroyEvent(device, event, nullptr);
    }
};

bool WaitUntil(const std::function<bool()>& condition, std::chrono::milliseconds limit = 10000ms) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

// ---- the submissions seen through volk's vkQueueSubmit (as rhi-spike.md 6.9 traced them) ------------------------------

struct SubmitRecord {
    std::vector<VkPipelineStageFlags> masks;  // one per wait semaphore
    std::vector<u64> values;                  // the timeline wait values
};

struct SubmitTrace {
    std::mutex mutex;
    std::vector<SubmitRecord> records;
    PFN_vkQueueSubmit real = nullptr;
} gTrace;

VkResult VKAPI_CALL TraceQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    {
        std::lock_guard lock(gTrace.mutex);
        for (uint32_t i = 0; i < count; ++i) {
            SubmitRecord record;
            for (uint32_t j = 0; j < submits[i].waitSemaphoreCount; ++j) {
                record.masks.push_back(submits[i].pWaitDstStageMask[j]);
            }
            for (const VkBaseInStructure* p = static_cast<const VkBaseInStructure*>(submits[i].pNext); p != nullptr; p = p->pNext) {
                if (p->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) {
                    const auto* timeline = reinterpret_cast<const VkTimelineSemaphoreSubmitInfo*>(p);
                    record.values.assign(timeline->pWaitSemaphoreValues,
                                         timeline->pWaitSemaphoreValues + timeline->waitSemaphoreValueCount);
                }
            }
            gTrace.records.push_back(std::move(record));
        }
    }
    return gTrace.real(queue, count, submits, fence);
}

struct TraceGuard {
    TraceGuard() {
        std::lock_guard lock(gTrace.mutex);
        gTrace.records.clear();
        gTrace.real = vkQueueSubmit;
        vkQueueSubmit = TraceQueueSubmit;
    }
    ~TraceGuard() { vkQueueSubmit = gTrace.real; }
    // The submissions that waited on at least one semaphore.
    std::vector<SubmitRecord> Waiting() {
        std::lock_guard lock(gTrace.mutex);
        std::vector<SubmitRecord> found;
        for (const SubmitRecord& record : gTrace.records) {
            if (!record.masks.empty()) found.push_back(record);
        }
        return found;
    }
};

// The legs of a case: the environment's backend (which has timeline semaphores unless MH4N_RHI_MASK took them away) and
// one whose mask takes the `timeline` bit off whatever the environment says.
const bool kLegs[2] = {false, true};

std::shared_ptr<Backend> LegBackend(bool timeline_masked) {
    return timeline_masked ? MakeBackend({Cap::Timeline}) : MakeBackend();
}

#define REQUIRE_CHECKS_COMPILED_IN()                                                                                   \
    do {                                                                                                               \
        if (!kRhiChecks) {                                                                                             \
            WARN_MESSAGE(false, "skipped: the owner-rule checks are compiled out (MH4N_RHI_CHECKS=OFF)");              \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

}  // namespace

// ---- waits -----------------------------------------------------------------------------------------------------

TEST_CASE("waits/the_second_submission_waits_on_the_first_at_the_fragment_stage_and_samples_what_it_drew") {
    for (const bool masked : kLegs) {
        std::shared_ptr<Backend> backend = LegBackend(masked);
        if (!backend) return;
        const bool timeline = backend->Caps().timeline;
        CHECK((masked ? !timeline : true));
        const std::shared_ptr<Pipeline> red = MakePipeline(*backend, Spv(spv_fs_vert), Spv(spv_red_frag), Format::Rgba8Unorm, Format::Undefined);
        const std::shared_ptr<Pipeline> fetch = MakePipeline(*backend, Spv(spv_fs_vert), Spv(spv_fetch_frag), Format::Rgba8Unorm,
                                                              Format::Undefined, {}, {kSampledAndSampler});
        const std::shared_ptr<Sampler> sampler = NearestSampler(*backend);
        Gate gate(*backend);
        TraceGuard trace;

        std::promise<TimelineValue> first_submitted;
        std::atomic<bool> second_started{false}, second_returned{false}, completed_at_return_ok{false};
        std::atomic<TimelineValue> second_value{0};
        std::vector<u8> pixels;
        // One thread owns X and Y, draws X, and submits the sampling of it: this one is the one that may block.
        std::thread worker([&] {
            std::shared_ptr<Image> x = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
            std::shared_ptr<Image> y = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
            BindingArena arena(backend);
            // Lavapipe's queue thread holds the device while it waits for the event, and creating an image view,
            // a framebuffer or a command list then waits for it too: the first run through makes all of them,
            // with the gate open, so that the gated run makes none.
            for (const bool gated : {false, true}) {
                Recorder first = backend->BeginRecording();
                if (gated) gate.Close(first);  // nothing of this submission finishes until the test opens the gate
                first.BeginTargetPass(Pass(x.get(), {0, 0, 1, 1}));
                first.SetPipeline(*red);
                first.Draw(3);
                const TimelineValue v1 = backend->Submit(std::move(first));
                if (gated) first_submitted.set_value(v1);

                Recorder second = backend->BeginRecording();
                second.BeginTargetPass(Pass(y.get(), {0, 1, 0, 1}));
                BindingSet* set = SampleSet(arena, *x, *sampler);
                second.SetPipeline(*fetch);
                second.Bind(*set, 0);
                second.Draw(3);
                if (gated) second_started = true;
                const Wait wait{v1, Stage::FragmentShader};
                const TimelineValue v2 = backend->Submit(std::move(second), std::span<const Wait>(&wait, 1));
                if (gated) {
                    // Without timeline semaphores the CPU has waited for v1 before it submitted.
                    completed_at_return_ok = timeline || backend->CompletedValue() >= v1;
                    second_value = v2;
                    second_returned = true;
                }
                backend->WaitIdle(v2);
                if (gated) pixels = ReadBack(*backend, *y);
            }
        });
        const TimelineValue v1 = first_submitted.get_future().get();
        REQUIRE(WaitUntil([&] { return second_started.load(); }));
        std::this_thread::sleep_for(300ms);
        if (timeline) {
            // the GPU holds the second submission: the submitter is not blocked
            CHECK(WaitUntil([&] { return second_returned.load(); }, 5000ms));
        } else {
            // the CPU holds it: the submitter is blocked until the first submission has finished, and it cannot
            // have finished, the gate is closed
            CHECK_FALSE(second_returned.load());
        }
        CHECK(backend->CompletedValue() < v1);
        gate.Open();
        worker.join();
        CHECK(second_returned.load());
        CHECK(completed_at_return_ok.load());
        CHECK(second_value.load() == v1 + 1);
        // X is red where the draw covered it; Y is X: (255, 0, 0, 255) in every texel, not the clear colours
        CHECK(AllPixels(pixels, 255, 0, 0, 255));

        // (the warm-up run waited too: the gated run's is the last)
        const std::vector<SubmitRecord> waiting = trace.Waiting();
        if (timeline) {
            REQUIRE(waiting.size() == 2);
            REQUIRE(waiting.back().masks.size() == 1);
            CHECK((waiting.back().masks[0] & VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT) != 0);
            REQUIRE(waiting.back().values.size() == 1);
            CHECK(waiting.back().values[0] == v1);
        } else {
            CHECK(waiting.empty());  // no semaphore wait is submitted: the fence fallback waited on the CPU
        }
    }
}

TEST_CASE("waits/a_submission_with_two_waits_passes_two_stage_masks_each_its_own") {
    for (const bool masked : kLegs) {
        std::shared_ptr<Backend> backend = LegBackend(masked);
        if (!backend) return;
        const bool timeline = backend->Caps().timeline;
        const std::shared_ptr<Pipeline> red = MakePipeline(*backend, Spv(spv_fs_vert), Spv(spv_red_frag), Format::Rgba8Unorm, Format::Undefined);
        const std::shared_ptr<Pipeline> fetch = MakePipeline(*backend, Spv(spv_fs_vert), Spv(spv_fetch_frag), Format::Rgba8Unorm,
                                                              Format::Undefined, {}, {kSampledAndSampler});
        const std::shared_ptr<Sampler> sampler = NearestSampler(*backend);
        std::shared_ptr<Image> x = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
        std::shared_ptr<Image> y = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
        std::shared_ptr<Image> z = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
        BindingArena arena(backend);

        Recorder r1 = backend->BeginRecording();
        r1.BeginTargetPass(Pass(x.get()));
        r1.SetPipeline(*red);
        r1.Draw(3);
        const TimelineValue v1 = backend->Submit(std::move(r1));
        Recorder r2 = backend->BeginRecording();
        r2.ClearImage(*z, ClearValue{});  // an independent producer
        const TimelineValue v2 = backend->Submit(std::move(r2));
        REQUIRE(v2 == v1 + 1);

        TraceGuard trace;
        Recorder r3 = backend->BeginRecording();
        r3.BeginTargetPass(Pass(y.get()));
        BindingSet* set = SampleSet(arena, *x, *sampler);
        r3.SetPipeline(*fetch);
        r3.Bind(*set, 0);
        r3.Draw(3);
        // the fragment stage reads X; nothing reads what the second value produced, which is waited at compute only
        const std::array<Wait, 2> waits = {Wait{v1, Stage::FragmentShader}, Wait{v2, Stage::ComputeShader}};
        const TimelineValue v3 = backend->Submit(std::move(r3), waits);
        backend->WaitIdle(v3);

        const std::vector<SubmitRecord> waiting = trace.Waiting();
        if (timeline) {
            REQUIRE(waiting.size() == 1);
            REQUIRE(waiting[0].masks.size() == 2);
            // each wait has its own mask: the first blocks the fragment shader (the consumer), the second compute only
            CHECK((waiting[0].masks[0] & VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT) != 0);
            CHECK(waiting[0].masks[1] == VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            CHECK(waiting[0].masks[0] != waiting[0].masks[1]);
            REQUIRE(waiting[0].values.size() == 2);
            CHECK(waiting[0].values[0] == v1);
            CHECK(waiting[0].values[1] == v2);
        } else {
            CHECK(waiting.empty());
        }
        CHECK(AllPixels(ReadBack(*backend, *y), 255, 0, 0, 255));
    }
}

TEST_CASE("waits/a_wait_for_a_value_nobody_issued_is_left_out_with_a_message") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    LogCapture log;
    std::shared_ptr<Image> x = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
    Recorder rec = backend->BeginRecording();
    rec.ClearImage(*x, ClearValue{});
    const Wait wait{backend->LastSubmittedValue() + 100, Stage::FragmentShader};
    const TimelineValue value = backend->Submit(std::move(rec), std::span<const Wait>(&wait, 1));  // would block for ever if submitted
    backend->WaitIdle(value);  // it did not
    CHECK(value > 0);
    const std::vector<LogCapture::Line> lines = log.With("left out");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].level == mh4n::LogLevel::Error);
    CHECK(lines[0].category == "RHI");
}

// ---- hand-over between threads (6.9 D1 and D3) --------------------------------------------------------------------

namespace {

// Passes the turn between two threads together with the value the last submission returned.
struct Turn {
    std::mutex mutex;
    std::condition_variable cv;
    int who = 0;
    TimelineValue value = 0;
    TimelineValue Take(int me) {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return who == me; });
        return value;
    }
    void Give(int to, TimelineValue v) {
        {
            std::lock_guard lock(mutex);
            who = to;
            value = v;
        }
        cv.notify_all();
    }
};

}  // namespace

TEST_CASE("handover/an_image_drawn_on_one_thread_is_sampled_on_another_and_handed_back_by_a_value") {
    REQUIRE_CHECKS_COMPILED_IN();
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    BreachCapture breaches;
    const std::shared_ptr<Pipeline> red = MakePipeline(*backend, Spv(spv_fs_vert), Spv(spv_red_frag), Format::Rgba8Unorm, Format::Undefined);
    const std::shared_ptr<Pipeline> green = MakePipeline(*backend, Spv(spv_fs_vert), Spv(spv_green_frag), Format::Rgba8Unorm, Format::Undefined);
    const std::shared_ptr<Pipeline> fetch = MakePipeline(*backend, Spv(spv_fs_vert), Spv(spv_fetch_frag), Format::Rgba8Unorm,
                                                          Format::Undefined, {}, {kSampledAndSampler});
    const std::shared_ptr<Sampler> sampler = NearestSampler(*backend);
    constexpr int kRounds = 8;
    std::shared_ptr<Image> x = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);  // owned by this thread (the first owner)
    const std::thread::id sender_thread = std::this_thread::get_id();
    Turn turn;
    std::promise<std::thread::id> receiver_id;
    std::vector<std::vector<u8>> seen(kRounds);
    std::thread receiver([&] {
        std::shared_ptr<Image> y = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);  // this thread's own
        BindingArena arena(backend);
        receiver_id.set_value(std::this_thread::get_id());
        for (int round = 0; round < kRounds; ++round) {
            const TimelineValue produced = turn.Take(1);
            Recorder rec = backend->BeginRecording();
            rec.BeginTargetPass(Pass(y.get(), {0, 0, 1, 1}));
            BindingSet* set = SampleSet(arena, *x, *sampler);  // the first sample takes the image over
            rec.SetPipeline(*fetch);
            rec.Bind(*set, 0);
            rec.Draw(3);
            const Wait wait{produced, Stage::FragmentShader};
            const TimelineValue mine = backend->Submit(std::move(rec), std::span<const Wait>(&wait, 1));
            backend->WaitIdle(mine);
            seen[round] = ReadBack(*backend, *y);
            // hand the image back, on the value this thread's submission returned
            HandOver(*x, sender_thread, Wait{mine, Stage::FragmentShader});
            turn.Give(0, mine);
        }
    });
    const std::thread::id receiver_thread = receiver_id.get_future().get();
    for (int round = 0; round < kRounds; ++round) {
        const TimelineValue from_receiver = turn.Take(0);
        Recorder rec = backend->BeginRecording();
        rec.BeginTargetPass(Pass(x.get(), {0, 0, 1, 1}));  // after round 0 the first record takes the image back
        rec.SetPipeline(round % 2 == 0 ? *red : *green);
        rec.Draw(3);
        std::vector<Wait> waits;
        if (round > 0) waits.push_back(Wait{from_receiver, Stage::FragmentShader});
        // the pass is still open when the recorder is submitted
        const TimelineValue produced = backend->Submit(std::move(rec), waits);
        HandOver(*x, receiver_thread, Wait{produced, Stage::FragmentShader});
        turn.Give(1, produced);
    }
    receiver.join();
    backend->WaitIdle();
    for (int round = 0; round < kRounds; ++round) {
        // red on even rounds, green on odd ones: what the other thread sampled is what this one drew
        CHECK(AllPixels(seen[round], round % 2 == 0 ? 255 : 0, round % 2 == 0 ? 0 : 255, 0, 255));
    }
    CHECK(breaches.Total() == 0);
}

TEST_CASE("handover/the_receiver_must_wait_for_the_value_that_produced_the_image") {
    REQUIRE_CHECKS_COMPILED_IN();
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    BreachCapture breaches;
    const std::shared_ptr<Pipeline> red = MakePipeline(*backend, Spv(spv_fs_vert), Spv(spv_red_frag), Format::Rgba8Unorm, Format::Undefined);
    const std::shared_ptr<Pipeline> fetch = MakePipeline(*backend, Spv(spv_fs_vert), Spv(spv_fetch_frag), Format::Rgba8Unorm,
                                                          Format::Undefined, {}, {kSampledAndSampler});
    const std::shared_ptr<Sampler> sampler = NearestSampler(*backend);
    enum class Variant { Exact, Higher, Lower, OtherStage, None };
    struct Case {
        Variant variant;
        int breaches;
    };
    for (const Case c : {Case{Variant::Exact, 0}, Case{Variant::Higher, 0}, Case{Variant::Lower, 1},
                         Case{Variant::OtherStage, 1}, Case{Variant::None, 1}}) {
        const int before = breaches.Count(OwnerRule::RequiredWait);
        std::shared_ptr<Image> x = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
        {
            Recorder dummy = backend->BeginRecording();
            dummy.ClearImage(*x, ClearValue{});
            backend->Submit(std::move(dummy));  // so that "one below the producing value" has been issued
        }
        Recorder rec = backend->BeginRecording();
        rec.BeginTargetPass(Pass(x.get(), {0, 0, 1, 1}));
        rec.SetPipeline(*red);
        rec.Draw(3);
        const TimelineValue produced = backend->Submit(std::move(rec));
        std::shared_ptr<Image> spare = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
        {
            Recorder later = backend->BeginRecording();
            later.ClearImage(*spare, ClearValue{});
            backend->Submit(std::move(later));  // so that "one above" has been issued
        }
        std::promise<std::thread::id> id;
        std::promise<void> handed;
        std::vector<u8> pixels;
        std::thread receiver([&] {
            std::shared_ptr<Image> y = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
            BindingArena arena(backend);
            id.set_value(std::this_thread::get_id());
            handed.get_future().wait();
            Recorder mine = backend->BeginRecording();
            mine.BeginTargetPass(Pass(y.get(), {0, 0, 1, 1}));
            BindingSet* set = SampleSet(arena, *x, *sampler);
            mine.SetPipeline(*fetch);
            mine.Bind(*set, 0);
            mine.Draw(3);
            std::vector<Wait> waits;
            switch (c.variant) {
            case Variant::Exact: waits = {Wait{produced, Stage::FragmentShader}}; break;
            case Variant::Higher: waits = {Wait{produced + 1, Stage::FragmentShader}}; break;
            case Variant::Lower: waits = {Wait{produced - 1, Stage::FragmentShader}}; break;
            case Variant::OtherStage: waits = {Wait{produced, Stage::ComputeShader}}; break;
            case Variant::None: break;
            }
            backend->WaitIdle(backend->Submit(std::move(mine), waits));
            pixels = ReadBack(*backend, *y);
        });
        HandOver(*x, id.get_future().get(), Wait{produced, Stage::FragmentShader});
        handed.set_value();
        receiver.join();
        // a submission whose waits do not cover the hand-over (a lower value, another stage, none) calls the handler
        // once; the pixels are still right on lavapipe's one queue (the layer's barrier covers the memory)
        CHECK(breaches.Count(OwnerRule::RequiredWait) - before == c.breaches);
        CHECK(AllPixels(pixels, 255, 0, 0, 255));
        backend->WaitIdle();
    }
}

// ---- deferred destruction --------------------------------------------------------------------------------------------

namespace {

struct Probe {
    explicit Probe(std::atomic<bool>* flag) : destroyed(flag) {}
    ~Probe() { *destroyed = true; }
    std::atomic<bool>* destroyed;
};

}  // namespace

TEST_CASE("retire/an_object_retired_while_in_flight_is_destroyed_only_after_its_value") {
    for (const bool masked : kLegs) {
        std::shared_ptr<Backend> backend = LegBackend(masked);
        if (!backend) return;
        const std::shared_ptr<Pipeline> red = MakePipeline(*backend, Spv(spv_fs_vert), Spv(spv_red_frag), Format::Rgba8Unorm, Format::Undefined);
        Gate gate(*backend);
        std::atomic<bool> probe_gone{false}, image_probe_gone{false};

        std::shared_ptr<Image> image = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
        std::weak_ptr<Image> image_watch = image;
        Recorder rec = backend->BeginRecording();
        gate.Close(rec);
        rec.BeginTargetPass(Pass(image.get(), {0, 0, 1, 1}));
        rec.SetPipeline(*red);
        rec.Draw(3);
        const TimelineValue v = backend->Submit(std::move(rec));  // cannot finish: the gate is closed
        REQUIRE(backend->CompletedValue() < v);

        auto probe = std::make_shared<Probe>(&probe_gone);
        backend->Retire(std::move(probe), v);
        backend->Retire(std::move(image), v);  // the image the submission draws into
        CHECK_FALSE(probe_gone.load());
        CHECK_FALSE(image_watch.expired());
        CHECK(sync::RetiredCount(*backend) == 2);
        sync::DrainRetired(*backend);  // the GPU has not passed v
        CHECK_FALSE(probe_gone.load());
        CHECK_FALSE(image_watch.expired());

        gate.Open();
        backend->WaitIdle(v);
        // the value has passed; nothing is released until a drain point
        CHECK_FALSE(probe_gone.load());
        // a submission with waits is one: an empty list of waits is enough
        Recorder next = backend->BeginRecording();
        backend->WaitIdle(backend->Submit(std::move(next), std::span<const Wait>()));
        CHECK(probe_gone.load());
        CHECK(image_watch.expired());
        CHECK(sync::RetiredCount(*backend) == 0);

        // an object whose value has passed already is released at once
        auto late = std::make_shared<Probe>(&image_probe_gone);
        backend->Retire(std::move(late), backend->CompletedValue());
        CHECK(image_probe_gone.load());
        CHECK(sync::RetiredCount(*backend) == 0);
    }
}

TEST_CASE("retire/release_retired_waits_for_the_gpu_and_empties_the_list") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    Gate gate(*backend);
    std::atomic<bool> gone{false};
    std::shared_ptr<Image> image = MakeImage(*backend, Format::Rgba8Unorm, 4, 4);
    Recorder rec = backend->BeginRecording();
    gate.Close(rec);
    rec.ClearImage(*image, ClearValue{});
    const TimelineValue v = backend->Submit(std::move(rec));
    backend->Retire(std::make_shared<Probe>(&gone), v);
    CHECK_FALSE(gone.load());
    std::thread opener([&] {
        std::this_thread::sleep_for(200ms);
        gate.Open();
    });
    sync::ReleaseRetired(*backend);  // blocks until the gate opens and the value passes
    opener.join();
    CHECK(gone.load());
    CHECK(sync::RetiredCount(*backend) == 0);
    CHECK(backend->CompletedValue() >= v);
}

// ---- the pipeline cache ----------------------------------------------------------------------------------------------

namespace {

u32 ReadWord(const std::vector<u8>& blob, size_t offset) {
    u32 word = 0;
    std::memcpy(&word, blob.data() + offset, 4);
    return word;
}

}  // namespace

TEST_CASE("pipeline_cache/a_saved_blob_is_accepted_by_a_second_backend_and_a_foreign_one_is_refused_with_an_info_line") {
    std::shared_ptr<Instance> instance = test::RequireInstance();
    if (!instance) return;
    std::shared_ptr<Backend> first = MakeBackend({}, instance);
    if (!first) return;
    std::shared_ptr<Backend> second = MakeBackend({}, instance);  // another backend on the same device
    if (!second) return;
    // some pipelines first, so that the blob is whatever the driver keeps
    std::vector<std::shared_ptr<Pipeline>> pipelines;
    for (int i = 0; i < 4; ++i) {
        FixedFunctionState state;
        state.cull = i % 2 == 0 ? CullMode::None : CullMode::Back;
        state.blend_enabled = i >= 2;
        state.src_color = BlendFactor::SrcAlpha;
        state.dst_color = BlendFactor::OneMinusSrcAlpha;
        pipelines.push_back(MakePipeline(*first, Spv(spv_fs_vert), Spv(spv_red_frag), Format::Rgba8Unorm, Format::Undefined, state, {},
                                         "cache pipeline " + std::to_string(i)));
    }
    const std::vector<u8> blob = first->SavePipelineCache();
    REQUIRE(blob.size() >= sync::kPipelineCacheHeaderSize);

    // The header read from the device with the test's own calls: header size, version 1, vendor, device, the UUID.
    VkPhysicalDeviceProperties props = {};
    vkGetPhysicalDeviceProperties(Handles(*first).physical_device, &props);
    CHECK(ReadWord(blob, 0) == 32);
    CHECK(ReadWord(blob, 4) == 1);
    CHECK(ReadWord(blob, 8) == props.vendorID);
    CHECK(ReadWord(blob, 12) == props.deviceID);
    CHECK(std::memcmp(blob.data() + 16, props.pipelineCacheUUID, 16) == 0);

    LogCapture log;
    CHECK(second->LoadPipelineCache(blob));
    CHECK(log.With("pipeline cache").empty());  // accepted: no line
    // the second backend's cache still works and saves a blob of its own
    const std::shared_ptr<Pipeline> again =
        MakePipeline(*second, Spv(spv_fs_vert), Spv(spv_green_frag), Format::Rgba8Unorm, Format::Undefined);
    const std::vector<u8> blob2 = second->SavePipelineCache();
    REQUIRE(blob2.size() >= sync::kPipelineCacheHeaderSize);
    CHECK(std::memcmp(blob2.data() + 8, blob.data() + 8, 24) == 0);  // the same device: vendor, device, UUID

    struct Bad {
        const char* what;
        std::vector<u8> bytes;
    };
    std::vector<Bad> bad;
    {
        std::vector<u8> b = blob;
        b[16] ^= 0xFF;  // a UUID byte changed
        bad.push_back({"uuid", b});
    }
    {
        std::vector<u8> b = blob;
        b[31] ^= 0x01;  // the last UUID byte
        bad.push_back({"last uuid byte", b});
    }
    {
        std::vector<u8> b = blob;
        b[8] ^= 0x01;  // the vendor id
        bad.push_back({"vendor", b});
    }
    {
        std::vector<u8> b = blob;
        b[12] ^= 0x01;  // the device id
        bad.push_back({"device", b});
    }
    {
        std::vector<u8> b = blob;
        b[4] = 2;  // the header version
        bad.push_back({"version", b});
    }
    {
        std::vector<u8> b = blob;
        b[0] = 16;  // a header length below the version-one header
        bad.push_back({"header size", b});
    }
    bad.push_back({"truncated", std::vector<u8>(blob.begin(), blob.begin() + 31)});
    bad.push_back({"empty", {}});
    size_t expected_lines = 0;
    for (const Bad& b : bad) {
        INFO("blob changed: " << b.what);
        CHECK_FALSE(second->LoadPipelineCache(b.bytes));
        ++expected_lines;
        const std::vector<LogCapture::Line> lines = log.With("pipeline cache");
        REQUIRE(lines.size() == expected_lines);
        CHECK(lines.back().level == mh4n::LogLevel::Info);
        CHECK(lines.back().category == "RHI");
    }
    // an ignored blob leaves the device's cache as it was: pipelines can still be made and the blob saved is the same kind
    MakePipeline(*second, Spv(spv_fs_vert), Spv(spv_blue_frag), Format::Rgba8Unorm, Format::Undefined);
    const std::vector<u8> blob3 = second->SavePipelineCache();
    CHECK(blob3.size() >= sync::kPipelineCacheHeaderSize);
    // the pure check, with the numbers written out: a header of version one for vendor 0x10de, device 0x2204
    std::vector<u8> header(32, 0);
    header[0] = 32;
    header[4] = 1;
    header[8] = 0xDE;
    header[9] = 0x10;
    header[12] = 0x04;
    header[13] = 0x22;
    std::array<u8, 16> uuid{};
    for (u8 i = 0; i < 16; ++i) header[16 + i] = uuid[i] = static_cast<u8>(0xA0 + i);
    CHECK(sync::CheckPipelineCacheBlob(header, 0x10de, 0x2204, uuid).empty());
    CHECK_FALSE(sync::CheckPipelineCacheBlob(header, 0x1002, 0x2204, uuid).empty());
    CHECK_FALSE(sync::CheckPipelineCacheBlob(header, 0x10de, 0x73df, uuid).empty());
    header[20] ^= 4;
    CHECK_FALSE(sync::CheckPipelineCacheBlob(header, 0x10de, 0x2204, uuid).empty());
}

TEST_CASE("pipeline_cache/pipelines_made_from_four_threads_at_once_are_all_usable") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 6;
    std::atomic<int> ready{0};
    std::atomic<int> made{0}, drawn_right{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            ready.fetch_add(1);
            while (ready.load() < kThreads) std::this_thread::yield();  // start together
            // each thread: pipelines with different state over the same shaders, then a draw with each
            std::vector<std::shared_ptr<Pipeline>> mine;
            for (int i = 0; i < kPerThread; ++i) {
                FixedFunctionState state;
                state.cull = i % 3 == 0 ? CullMode::None : (i % 3 == 1 ? CullMode::Back : CullMode::Front);
                state.front_face = i % 2 == 0 ? FrontFace::CounterClockwise : FrontFace::Clockwise;
                state.blend_enabled = i >= 3;
                state.src_color = BlendFactor::One;
                state.dst_color = BlendFactor::Zero;
                GraphicsPipelineDesc desc;
                desc.vertex = Spv(spv_fs_vert);
                desc.fragment = t % 2 == 0 ? Spv(spv_red_frag) : Spv(spv_blue_frag);
                desc.color_format = Format::Rgba8Unorm;
                desc.state = state;
                desc.name = "thread " + std::to_string(t) + " pipeline " + std::to_string(i);
                Result<std::shared_ptr<Pipeline>> pipeline = backend->CreateGraphicsPipeline(desc);
                if (pipeline.ok()) {
                    mine.push_back(*pipeline);
                    made.fetch_add(1);
                }
            }
            // usable: a draw with each, into this thread's image; the culling of the full-screen triangle varies, so
            // only the pipelines that draw it count (the pipeline of i = 0 does: no culling)
            ImageDesc image_desc;
            image_desc.format = Format::Rgba8Unorm;
            image_desc.width = image_desc.height = 4;
            Result<std::shared_ptr<Image>> image = backend->CreateImage(image_desc);
            if (!image.ok() || mine.empty()) return;
            Recorder rec = backend->BeginRecording();
            rec.BeginTargetPass(Pass(image->get(), {0, 1, 0, 1}));
            rec.SetPipeline(*mine[0]);
            rec.Draw(3);
            backend->WaitIdle(backend->Submit(std::move(rec)));
            const std::vector<u8> pixels = ReadBack(*backend, **image);
            const bool right = t % 2 == 0 ? AllPixels(pixels, 255, 0, 0, 255) : AllPixels(pixels, 0, 0, 255, 255);
            if (right) drawn_right.fetch_add(1);
        });
    }
    for (std::thread& thread : threads) thread.join();
    CHECK(made.load() == kThreads * kPerThread);
    CHECK(drawn_right.load() == kThreads);
}

// ---- timestamps ------------------------------------------------------------------------------------------------------

TEST_CASE("timestamps/a_frame_is_read_only_after_its_value_has_passed") {
    for (const bool masked : kLegs) {
        std::shared_ptr<Backend> backend = LegBackend(masked);
        if (!backend) return;
        std::shared_ptr<Image> image = MakeImage(*backend, Format::Rgba8Unorm, 16, 16);
        CHECK(backend->FrameGpuTimeNs() == 0);  // no frame yet
        Gate gate(*backend);
        Recorder rec = backend->BeginRecording();
        rec.MarkFrame(FrameMark::Begin);
        gate.Close(rec);
        rec.ClearImage(*image, ClearValue{});
        rec.MarkFrame(FrameMark::End);
        const TimelineValue v = backend->Submit(std::move(rec));
        REQUIRE(backend->CompletedValue() < v);
        // the frame is in flight: nothing is read, nothing is reported
        CHECK(backend->FrameGpuTimeNs() == 0);
        CHECK(sync::FramesInFlight(*backend) == 1);
        gate.Open();
        backend->WaitIdle(v);
        const u64 ns = backend->FrameGpuTimeNs();
        CHECK(ns > 0);
        const sync::FrameTimestamps stamps = sync::LastFrameTimestamps(*backend);
        CHECK(stamps.end_ns > stamps.begin_ns);
        CHECK(stamps.end_ns - stamps.begin_ns == ns);
        CHECK(sync::FramesInFlight(*backend) == 0);

        // the next frame's timestamps follow the last's
        Recorder next = backend->BeginRecording();
        next.MarkFrame(FrameMark::Begin);
        next.ClearImage(*image, ClearValue{});
        next.MarkFrame(FrameMark::End);
        backend->WaitIdle(backend->Submit(std::move(next)));
        const sync::FrameTimestamps later = sync::LastFrameTimestamps(*backend);
        CHECK(later.begin_ns >= stamps.end_ns);
        CHECK(later.end_ns > later.begin_ns);
    }
}

TEST_CASE("timestamps/a_frame_with_no_end_a_dropped_recorder_and_an_end_with_no_begin_cost_nothing") {
    std::shared_ptr<Backend> backend = MakeBackend();
    if (!backend) return;
    LogCapture log;
    std::shared_ptr<Image> image = MakeImage(*backend, Format::Rgba8Unorm, 8, 8);
    {
        Recorder lone = backend->BeginRecording();
        lone.MarkFrame(FrameMark::End);  // no frame has begun: ignored with a message
        backend->WaitIdle(backend->Submit(std::move(lone)));
    }
    CHECK(log.With("MarkFrame(End) with no frame begun").size() == 1);
    CHECK(backend->FrameGpuTimeNs() == 0);
    {
        Recorder dropped = backend->BeginRecording();
        dropped.MarkFrame(FrameMark::Begin);  // never submitted
    }
    CHECK(sync::FramesInFlight(*backend) == 0);
    for (int i = 0; i < 20; ++i) {
        Recorder rec = backend->BeginRecording();
        rec.MarkFrame(FrameMark::Begin);  // a Begin with no End: the next Begin abandons it
        rec.ClearImage(*image, ClearValue{});
        backend->WaitIdle(backend->Submit(std::move(rec)));
    }
    CHECK(sync::FramesInFlight(*backend) <= 1);  // the abandoned ones gave their pools back
    CHECK(backend->FrameGpuTimeNs() == 0);       // none of them was a whole frame
    // The End closes the frame begun last, in whichever recorder it is: a frame can span two submissions.
    Recorder end = backend->BeginRecording();
    end.ClearImage(*image, ClearValue{});
    end.MarkFrame(FrameMark::End);
    backend->WaitIdle(backend->Submit(std::move(end)));
    CHECK(backend->FrameGpuTimeNs() > 0);
}
