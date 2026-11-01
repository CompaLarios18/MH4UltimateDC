// The present thread (N2b-06). Not derived from Azahar: the structure (a mailbox, one thread that owns the
// swapchain, recreation on out-of-date and suboptimal) follows the specification and rhi-spike.md 9.4; Azahar's
// vk_present_window.cpp and vk_swapchain.cpp were read for how it recovers from an out-of-date swapchain (recreate, then
// acquire again) and written again.
#include "swapchain/presenter.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <thread>

#include <fmt/format.h>

#include "mh4n/log.h"
#include "swapchain/policy.h"
#include "swapchain/present_log.h"
#include "swapchain/vk_swapchain.h"

namespace mh4n::swapchain {

namespace rhi = ::mh4n::gpu::rhi;
namespace sl = ::mh4n::screen_layout;

namespace {

constexpr const char* kLog = "Swapchain";

// Every wait on the present thread is cut into slices of this length, so that Stop is seen within one slice.
constexpr u64 kSliceNs = 20'000'000;
constexpr u64 kPresentWaitSliceNs = 5'000'000;
// How long the thread waits for one frame to be shown (VK_KHR_present_wait) before it gives up on that frame's time.
constexpr u64 kPresentWaitCapUs = 250'000;
// How long a frame's row waits for VK_GOOGLE_display_timing's answer.
constexpr u64 kTimingCapUs = 500'000;
constexpr int kAttempts = 4;

bool WritePpm(const std::filesystem::path& path, u32 width, u32 height, const std::vector<u8>& rgba) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file << "P6\n" << width << ' ' << height << "\n255\n";
    std::vector<char> rgb(static_cast<std::size_t>(width) * height * 3);
    for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
        rgb[i * 3 + 0] = static_cast<char>(rgba[i * 4 + 0]);
        rgb[i * 3 + 1] = static_cast<char>(rgba[i * 4 + 1]);
        rgb[i * 3 + 2] = static_cast<char>(rgba[i * 4 + 2]);
    }
    file.write(rgb.data(), static_cast<std::streamsize>(rgb.size()));
    return static_cast<bool>(file);
}

} // namespace

struct Presenter::Impl {
    // ---- fixed at construction --------------------------------------------------------------------------------------
    std::shared_ptr<rhi::Backend> backend;
    Target target;
    LayoutSource layout_source;
    Options options;
    TestHooks hooks;
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    std::thread thread;
    std::thread::id thread_id;  // written by the present thread before Create returns
    std::mutex join_mutex;
    std::atomic<bool> stop{false};

    // ---- shared with the callers (m guards all of it) ---------------------------------------------------------------
    mutable std::mutex m;
    std::condition_variable cv;
    FrameMailbox mailbox;
    bool redraw = false;
    u64 redraw_us = 0;
    bool have_last = false;  // a frame has been taken at least once (RequestRedraw has something to draw)
    bool vsync = true;
    u32 want_w = 0, want_h = 0;
    bool minimised = false;
    u64 dropped_since_row = 0;
    Stats stats;
    sl::Layout layout;
    std::string dump_dir;
    u32 dump_every = 1;
    std::string display_label = "none";

    // The layer the compositor calls. compose_mutex is held around Compose, so SetUiLayer(nullptr) waits for the
    // composition in progress.
    std::mutex compose_mutex;
    present::UiLayer* ui = nullptr;

    // The present log.
    std::mutex log_mutex;
    std::ofstream log_file;
    std::optional<double> refresh_hz;

    // ---- the present thread's own ------------------------------------------------------------------------------------
    std::unique_ptr<present::Compositor> compositor;
    std::unique_ptr<detail::VkSwapchain> sc;
    bool needs_recreate = false;
    u32 made_for_w = 0, made_for_h = 0;
    std::shared_ptr<rhi::Image> offscreen;
    present::DisplayFrame last_frame;
    u64 composed_count = 0;
    std::optional<u64> last_composed_sequence;
    DisplayTimeChoice choice;
    std::unique_ptr<DisplayTimeSource> time_source;
    u64 next_present_id = 1;
    bool timing_clock_warned = false;
    bool create_error_logged = false;

    struct PendingRow {
        PresentRow row;
        u64 present_id = 0;
    };
    std::deque<PendingRow> pending;
    std::map<u64, u64> timing_ns;  // presentID -> actualPresentTime, from VK_GOOGLE_display_timing

    struct Work {
        present::DisplayFrame frame;
        u64 published_us = 0;
        bool redraw = false;
        u64 dropped_before = 0;
        bool vsync = true;
        u32 width = 0, height = 0;
        bool minimised = false;
        std::string dump_dir;
        u32 dump_every = 1;
    };

    Impl(std::shared_ptr<rhi::Backend> b, Target t, LayoutSource source, Options o, TestHooks h)
        : backend(std::move(b)), target(t), layout_source(std::move(source)), options(std::move(o)), hooks(std::move(h)) {
        vsync = options.vsync;
        want_w = target.width;
        want_h = target.height;
        if (!target.IsOffscreen()) {
            surface = rhi::VulkanSurface(target.surface);
        }
    }

    // ---- small helpers ----------------------------------------------------------------------------------------------

    bool StopFlag() const {
        return stop.load(std::memory_order_acquire);
    }

    // Every own recording, submission and present of the presenter is on the present thread (rhi-spike.md 9.4 item 5).
    void CheckPresentThread(const char* what) const {
#if MH4N_RHI_CHECKS
        if (std::this_thread::get_id() != thread_id) {
            rhi::ReportOwnerRuleBreach(
                rhi::OwnerRule::WrongThread,
                fmt::format("swapchain::Presenter: {} ran on a thread other than the present thread", what));
        }
#else
        (void)what;
#endif
    }

    template <class F>
    void WithStats(F&& f) {
        std::lock_guard lock(m);
        f(stats);
    }

    void CountDropped(u64 n) {
        std::lock_guard lock(m);
        stats.dropped += n;
        dropped_since_row += n;
    }

    // ---- the thread -------------------------------------------------------------------------------------------------

    void Run(std::promise<rhi::Result<void>> ready) {
        thread_id = std::this_thread::get_id();

        rhi::Result<std::unique_ptr<present::Compositor>> made = present::Compositor::Create(backend);
        if (!made.ok()) {
            ready.set_value(rhi::Error{"the compositor: " + made.error()});
            return;
        }
        compositor = std::move(*made);

        if (!target.IsOffscreen()) {
            if (!MakeSwapchain(vsync, want_w, want_h, /*first=*/true)) {
                ready.set_value(rhi::Error{"the swapchain: " + first_error});
                return;
            }
        }
        SelectDisplayTime();
        {
            std::lock_guard lock(m);
            stats.display_time = display_label;
        }
        MH4N_LOG(Info, kLog, "present wait: {}", backend->PresentWaitAvailable() ? "on" : "off");
        MH4N_LOG(Info, kLog, "display time: {}", display_label);
        ready.set_value(rhi::Result<void>{});

        Loop();
        Teardown();
    }

    std::string first_error;

    void SelectDisplayTime() {
        bool measures = !target.IsOffscreen() && !hooks.disable_present_wait &&
                        (hooks.pretend_present_wait || (backend->PresentWaitMeasuresDisplay() && vkWaitForPresentKHR != nullptr));
        bool timing = !target.IsOffscreen() && !hooks.disable_display_timing &&
                      (hooks.past_presentation_times != nullptr ||
                       (backend->DisplayTimingAvailable() && vkGetPastPresentationTimingGOOGLE != nullptr));
        std::string_view registered;
        const DisplayTimeSourceFactory* factory = nullptr;
        if (!target.IsOffscreen() && !options.display_time_source.empty()) {
            factory = Registry<DisplayTimeSourceFactory>::Instance().Find(options.display_time_source);
            if (factory != nullptr && factory->make) {
                registered = options.display_time_source;
            } else {
                factory = nullptr;
            }
        }
        choice = ChooseDisplayTime(measures, timing, registered);
        if (choice.kind == DisplayTimeKind::Registered) {
            time_source = factory->make();
            if (time_source == nullptr) {
                choice = ChooseDisplayTime(false, false, {});
            }
        }
        display_label = DisplayTimeLabel(choice, time_source != nullptr ? time_source->Name() : std::string_view());
        if (choice.kind == DisplayTimeKind::DisplayTiming && sc != nullptr && !hooks.past_presentation_times) {
            const u64 cycle = sc->RefreshCycleNs();
            std::lock_guard lock(log_mutex);
            if (cycle > 0 && !refresh_hz.has_value()) {
                refresh_hz = 1e9 / static_cast<double>(cycle);
            }
        }
    }

    // Tries to make the swapchain for the request; false (and the frame dropped by the caller) when it cannot be made now.
    bool MakeSwapchain(bool want_vsync, u32 width, u32 height, bool first) {
        if (sc != nullptr) {
            sc->Destroy();
            sc.reset();
        }
        detail::VkSwapchain::Made made = detail::VkSwapchain::Create(backend, surface, want_vsync, width, height);
        if (made.status != detail::CreateStatus::Ok) {
            if (first) {
                first_error = made.message;
            } else if (made.status == detail::CreateStatus::NotPresentable) {
                // a minimised window: nothing to say
            } else if (!create_error_logged) {
                create_error_logged = true;
                MH4N_LOG(Error, kLog, "the swapchain could not be made: {} (frames are dropped until it can)", made.message);
            }
            return false;
        }
        create_error_logged = false;
        sc = std::move(made.swapchain);
        needs_recreate = false;
        made_for_w = width;
        made_for_h = height;
        WithStats([&](Stats& s) {
            ++s.swapchain_creations;
            s.extent_width = sc->Width();
            s.extent_height = sc->Height();
            s.present_mode = static_cast<s32>(sc->PresentMode());
        });
        MH4N_LOG(Info, kLog, "swapchain {}x{}, {} images, present mode {}", sc->Width(), sc->Height(), sc->ImageCount(),
                 PresentModeName(sc->PresentMode()));
        if (hooks.swapchain_created) {
            hooks.swapchain_created(sc->Width(), sc->Height(), static_cast<s32>(sc->PresentMode()), sc->ImageCount());
        }
        return true;
    }

    enum class Wake : u8 { Frame, Idle, Stop };

    Wake WaitForWork(Work& work) {
        std::unique_lock lock(m);
        const auto ready = [&] { return stop.load() || mailbox.Has() || (redraw && have_last); };
        if (pending.empty()) {
            cv.wait(lock, ready);
        } else {
            cv.wait_for(lock, std::chrono::milliseconds(4), ready);
        }
        if (stop.load()) {
            return Wake::Stop;
        }
        if (mailbox.Has()) {
            FrameMailbox::Entry entry = *mailbox.Take();
            work.frame = std::move(entry.frame);
            work.published_us = entry.published_us;
            work.redraw = false;
            redraw = false;
            have_last = true;
        } else if (redraw && have_last) {
            work.frame = last_frame;
            work.published_us = redraw_us;
            work.redraw = true;
            redraw = false;
        } else {
            return Wake::Idle;
        }
        work.dropped_before = dropped_since_row;
        dropped_since_row = 0;
        work.vsync = vsync;
        work.width = want_w;
        work.height = want_h;
        work.minimised = minimised;
        work.dump_dir = dump_dir;
        work.dump_every = dump_every;
        return Wake::Frame;
    }

    void Loop() {
        for (;;) {
            Work work;
            const Wake wake = WaitForWork(work);
            if (wake == Wake::Stop) {
                return;
            }
            if (wake == Wake::Frame) {
                Process(std::move(work));
            }
            FlushRows(false);
        }
    }

    void Process(Work work) {
        if (!work.redraw) {
            last_frame = work.frame;
        }
        if (work.minimised || work.width == 0 || work.height == 0) {
            // D60: nothing is presented; a published frame counts as dropped, a redraw is simply not done.
            if (!work.redraw) {
                CountDropped(1);
            }
            return;
        }
        PendingRow done;
        const bool shown = target.IsOffscreen() ? PresentOffscreen(work, done) : PresentToWindow(work, done);
        if (!shown) {
            if (!work.redraw) {
                CountDropped(1);
            }
            return;
        }
        {
            std::lock_guard lock(m);
            ++stats.presented;
            if (work.redraw) {
                ++stats.redraws;
            }
        }
        pending.push_back(std::move(done));
    }

    // ---- composing --------------------------------------------------------------------------------------------------

    // Composes `work.frame` into `image`, fills `row` and returns the timeline value of the submission. The recorder is
    // open from BeginRecording to the Submit of the compositor; `finish` runs on it after Compose (PrepareForPresent
    // for a swapchain image). Runs on the present thread (CheckPresentThread).
    template <class Finish>
    rhi::TimelineValue ComposeInto(rhi::Image& image, u32 width, u32 height, const Work& work, PresentRow& row,
                                   Finish&& finish) {
        CheckPresentThread("a recording");
        LayoutRequest request = layout_source();
        request.inputs.window_w = width;
        request.inputs.window_h = height;
        const sl::Layout computed = sl::Compute(request.inputs);

        rhi::Recorder recorder = backend->BeginRecording();
        present::ComposeResult result;
        {
            std::lock_guard lock(compose_mutex);
            if (ui != nullptr) {
                request.options.ui = ui;
            }
            result = compositor->Compose(recorder, image, work.frame, computed, request.options);
        }
        if (!result.drawn) {
            MH4N_LOG(Error, kLog, "the compositor drew nothing: the target's format cannot be drawn to");
        }
        {
            std::lock_guard lock(m);
            layout = computed;
        }
        if (hooks.in_composition) {
            hooks.in_composition(work.frame, width, height);
        }
        finish(recorder);
        CheckPresentThread("a submission");
        const rhi::TimelineValue value = compositor->Submit(std::move(recorder), std::move(result));
        row.sequence = work.frame.sequence;
        row.game_frame = work.frame.game_frame;
        row.published_us = work.published_us;
        row.composed_us = NowUs();
        row.dropped_before = work.dropped_before;
        ++composed_count;
        last_composed_sequence = work.frame.sequence;
        {
            std::lock_guard lock(m);
            ++stats.composed;
        }
        return value;
    }

    bool PresentOffscreen(const Work& work, PendingRow& out) {
        if (offscreen == nullptr || offscreen->Width() != work.width || offscreen->Height() != work.height) {
            if (offscreen != nullptr) {
                backend->WaitIdle();
                offscreen.reset();
            }
            rhi::ImageDesc desc;
            desc.format = rhi::Format::Rgba8Unorm;
            desc.width = work.width;
            desc.height = work.height;
            desc.name = "present offscreen";
            rhi::Result<std::shared_ptr<rhi::Image>> image = backend->CreateImage(desc);
            if (!image.ok()) {
                MH4N_LOG(Error, kLog, "the offscreen target could not be made: {}", image.error());
                return false;
            }
            offscreen = *image;
            WithStats([&](Stats& s) {
                s.extent_width = work.width;
                s.extent_height = work.height;
            });
        }
        rhi::TimelineValue value = 0;
        const auto run = [&] { value = ComposeInto(*offscreen, work.width, work.height, work, out.row, [](rhi::Recorder&) {}); };
        hooks.frame_runner ? hooks.frame_runner(run) : run();
        backend->WaitIdle(value);
        out.row.presented_us = NowUs();
        out.present_id = next_present_id++;
        if (!work.dump_dir.empty() && work.dump_every > 0 && (composed_count - 1) % work.dump_every == 0) {
            DumpImage(work.dump_dir, fmt::format("presented_{}.ppm", work.frame.sequence));
        }
        return true;
    }

    void DumpImage(const std::string& dir, const std::string& name) {
        if (offscreen == nullptr) {
            return;
        }
        rhi::Result<std::vector<u8>> pixels = backend->ReadImage(*offscreen, 0, 0, offscreen->Width(), offscreen->Height());
        {
            // The readback's staging memory is reused by a later upload. The backend waits for the readback on the host
            // (a timeline semaphore), which the validation layer 1.3.275 does not count as ordering (presentation.md,
            // "Run under the layer": present_test calls vkDeviceWaitIdle for the same reason); a queue wait idle
            // does count. Only the dump (a debugging aid, --present-dump) pays for it.
            rhi::QueueLock lock(*backend);
            vkQueueWaitIdle(rhi::Handles(*backend).queue);
        }
        if (!pixels.ok() || !WritePpm(std::filesystem::path(dir) / name, offscreen->Width(), offscreen->Height(), *pixels)) {
            MH4N_LOG(Error, kLog, "could not write {}/{}", dir, name);
        }
    }

    bool PresentToWindow(const Work& work, PendingRow& out) {
        for (int attempt = 0; attempt < kAttempts; ++attempt) {
            if (StopFlag()) {
                return false;
            }
            if (sc == nullptr || needs_recreate || sc->Vsync() != work.vsync || made_for_w != work.width ||
                made_for_h != work.height) {
                if (!MakeSwapchain(work.vsync, work.width, work.height, /*first=*/false)) {
                    return false;
                }
            }

            // ---- acquire: with the fence, in slices that check Stop
            detail::AcquireResult acquired;
            for (;;) {
                if (StopFlag()) {
                    return false;
                }
                const s32 forced = hooks.acquire_result ? hooks.acquire_result() : 0;
                acquired = sc->Acquire(kSliceNs, forced);
                if (acquired.status != detail::AcquireStatus::Timeout) {
                    break;
                }
                WithStats([](Stats& s) { ++s.acquire_timeouts; });
            }
            if (acquired.status == detail::AcquireStatus::OutOfDate) {
                WithStats([](Stats& s) { ++s.out_of_date; });
                needs_recreate = true;
                continue;
            }
            if (acquired.status != detail::AcquireStatus::Acquired) {
                return false;
            }
            if (acquired.suboptimal) {
                // The image is good to use ("can still be used for presentation"): this frame is shown, the next one
                // gets a new swapchain.
                WithStats([](Stats& s) { ++s.suboptimal; });
                needs_recreate = true;
            }
            while (!sc->WaitAcquired(kSliceNs)) {
                if (StopFlag()) {
                    return false;  // the image is left acquired; the swapchain's destruction waits for the fence
                }
            }

            // ---- record, submit, present
            detail::PresentStatus status = detail::PresentStatus::Failed;
            u64 present_id = 0;
            const auto run = [&] {
                rhi::Image& image = sc->Image(acquired.index);
                ComposeInto(image, sc->Width(), sc->Height(), work, out.row,
                            [&](rhi::Recorder& recorder) { recorder.PrepareForPresent(image); });
                CheckPresentThread("a present");
                detail::VkSwapchain::PresentParams params;
                params.index = acquired.index;
                present_id = next_present_id++;
                params.present_id = present_id;
                params.use_present_id = choice.kind == DisplayTimeKind::PresentWait && !hooks.pretend_present_wait;
                params.use_display_timing =
                    choice.kind == DisplayTimeKind::DisplayTiming && hooks.past_presentation_times == nullptr;
                params.forced = hooks.present_result ? hooks.present_result() : 0;
                status = sc->SubmitAndPresent(params);
            };
            hooks.frame_runner ? hooks.frame_runner(run) : run();
            out.row.presented_us = NowUs();
            out.present_id = present_id;

            if (status == detail::PresentStatus::OutOfDate) {
                WithStats([](Stats& s) { ++s.out_of_date; });
                needs_recreate = true;
                continue;  // the frame was not shown: recreate and compose it again
            }
            if (status == detail::PresentStatus::Failed) {
                return false;
            }
            if (status == detail::PresentStatus::Suboptimal) {
                WithStats([](Stats& s) { ++s.suboptimal; });
                needs_recreate = true;
            }
            if (choice.kind == DisplayTimeKind::PresentWait) {
                out.row.displayed_us = WaitDisplayed(present_id);
            }
            return true;
        }
        return false;
    }

    // VK_KHR_present_wait: the time vkWaitForPresentKHR returned for this frame, waited for in slices that check Stop and
    // give up after kPresentWaitCapUs. The wait is on the present thread (the swapchain is externally synchronised).
    std::optional<u64> WaitDisplayed(u64 present_id) {
        const u64 deadline = NowUs() + kPresentWaitCapUs;
        for (;;) {
            if (StopFlag()) {
                return std::nullopt;
            }
            const s32 forced = hooks.present_wait_result ? hooks.present_wait_result() : 0;
            const s32 result = forced != 0 ? forced
                               : hooks.pretend_present_wait ? static_cast<s32>(VK_SUCCESS)
                                                            : sc->WaitForPresent(present_id, kPresentWaitSliceNs);
            if (result == VK_SUCCESS) {
                return NowUs();
            }
            if (result == VK_TIMEOUT) {
                if (forced != 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));  // a real wait would have taken its slice
                }
                if (NowUs() >= deadline) {
                    return std::nullopt;
                }
                continue;
            }
            if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR) {
                needs_recreate = true;
            }
            return std::nullopt;
        }
    }

    // ---- the log ----------------------------------------------------------------------------------------------------

    void PollTiming() {
        if (choice.kind != DisplayTimeKind::DisplayTiming) {
            return;
        }
        if (hooks.past_presentation_times) {
            for (const auto& [id, ns] : hooks.past_presentation_times()) {
                timing_ns[id] = ns;
            }
        } else if (sc != nullptr) {
            for (const auto& [id, ns] : sc->PastPresentationTimes()) {
                timing_ns[id] = ns;
            }
        }
    }

    // Writes the rows whose displayed time is settled, in order. `final`: everything, with what is known.
    void FlushRows(bool final) {
        PollTiming();
        while (!pending.empty()) {
            PendingRow& head = pending.front();
            switch (choice.kind) {
            case DisplayTimeKind::DisplayTiming: {
                const auto found = timing_ns.find(static_cast<uint32_t>(head.present_id));
                if (found != timing_ns.end()) {
                    const u64 us = found->second / 1000;
                    const u64 near = head.row.presented_us;
                    // The extension's times are in the platform's presentation clock; they are taken to be the steady
                    // clock's (CLOCK_MONOTONIC), and a time a second away from the present call is not believed.
                    if ((us > near ? us - near : near - us) < 1'000'000) {
                        head.row.displayed_us = us;
                    } else if (!timing_clock_warned) {
                        timing_clock_warned = true;
                        MH4N_LOG(Warning, kLog, "display_timing's clock does not match the steady clock: displayed_us is left empty");
                    }
                    timing_ns.erase(found);
                } else if (!final && NowUs() - head.row.presented_us < kTimingCapUs) {
                    return;
                }
                break;
            }
            case DisplayTimeKind::Registered:
                if (time_source != nullptr) {
                    head.row.displayed_us = time_source->DisplayedUs(head.present_id, head.row.presented_us);
                }
                break;
            case DisplayTimeKind::PresentWait:
            case DisplayTimeKind::None: break;
            }
            WriteRow(head.row);
            pending.pop_front();
        }
        // Times of frames that were never asked for do not pile up.
        while (timing_ns.size() > 256) {
            timing_ns.erase(timing_ns.begin());
        }
    }

    void WriteRow(const PresentRow& row) {
        std::lock_guard lock(log_mutex);
        if (log_file.is_open()) {
            log_file << FormatPresentRow(row);
            log_file.flush();
        }
    }

    void Teardown() {
        FlushRows(true);
        backend->WaitIdle();
        if (!dump_dir.empty() && offscreen != nullptr && last_composed_sequence.has_value()) {
            DumpImage(dump_dir, "last.ppm");
        }
        if (sc != nullptr) {
            sc->Destroy();
            sc.reset();
        }
        offscreen.reset();
        compositor.reset();
        std::lock_guard lock(log_mutex);
        if (log_file.is_open()) {
            log_file.close();
        }
    }
};

// ---- the public class -----------------------------------------------------------------------------------------------

Presenter::Presenter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

rhi::Result<std::unique_ptr<Presenter>> Presenter::Create(std::shared_ptr<rhi::Backend> backend, Target target,
                                                          LayoutSource layout_source, Options options, TestHooks hooks) {
    if (backend == nullptr) {
        return rhi::Error{"Presenter::Create: no backend"};
    }
    if (!layout_source) {
        return rhi::Error{"Presenter::Create: no layout source"};
    }
    if (!target.IsOffscreen() && !backend->HasSurface()) {
        return rhi::Error{"Presenter::Create: the backend was made without a surface"};
    }
    auto impl = std::make_unique<Impl>(std::move(backend), target, std::move(layout_source), std::move(options),
                                       std::move(hooks));
    std::promise<rhi::Result<void>> ready;
    std::future<rhi::Result<void>> future = ready.get_future();
    Impl* raw = impl.get();
    raw->thread = std::thread([raw, ready = std::move(ready)]() mutable { raw->Run(std::move(ready)); });
    rhi::Result<void> started = future.get();
    if (!started.ok()) {
        raw->thread.join();
        return rhi::Error{started.error()};
    }
    return std::unique_ptr<Presenter>(new Presenter(std::move(impl)));
}

Presenter::~Presenter() {
    Stop();
}

void Presenter::Publish(const present::DisplayFrame& frame) {
    present::DisplayFrame copy = frame;  // the shared_ptrs: cheap, and outside the lock
    const u64 now = NowUs();
    {
        std::lock_guard lock(impl_->m);
        ++impl_->stats.published;
        if (impl_->stop.load()) {
            ++impl_->stats.dropped;
            return;
        }
        if (impl_->mailbox.Put(std::move(copy), now)) {
            ++impl_->stats.dropped;
            ++impl_->dropped_since_row;
        }
    }
    impl_->cv.notify_one();
}

void Presenter::SetUiLayer(present::UiLayer* layer) {
    std::lock_guard lock(impl_->compose_mutex);
    impl_->ui = layer;
}

void Presenter::RequestRedraw() {
    {
        std::lock_guard lock(impl_->m);
        if (!impl_->have_last) {
            return;
        }
        impl_->redraw = true;
        impl_->redraw_us = NowUs();
    }
    impl_->cv.notify_one();
}

sl::Layout Presenter::CurrentLayout() const {
    std::lock_guard lock(impl_->m);
    return impl_->layout;
}

void Presenter::SetVsync(bool vsync) {
    {
        std::lock_guard lock(impl_->m);
        impl_->vsync = vsync;
        impl_->redraw = impl_->have_last;  // the new mode takes effect with the next present: make one
        impl_->redraw_us = NowUs();
    }
    impl_->cv.notify_one();
}

void Presenter::Resize(u32 width, u32 height) {
    {
        std::lock_guard lock(impl_->m);
        impl_->want_w = width;
        impl_->want_h = height;
        impl_->redraw = impl_->have_last;
        impl_->redraw_us = NowUs();
    }
    impl_->cv.notify_one();
}

void Presenter::SetMinimised(bool minimised) {
    {
        std::lock_guard lock(impl_->m);
        impl_->minimised = minimised;
        if (!minimised) {
            impl_->redraw = impl_->have_last;
            impl_->redraw_us = NowUs();
        }
    }
    impl_->cv.notify_one();
}

void Presenter::SetRefreshHz(double hz) {
    std::lock_guard lock(impl_->log_mutex);
    impl_->refresh_hz = hz > 0.0 ? std::optional<double>(hz) : std::nullopt;
}

rhi::Result<void> Presenter::SetPresentLog(const std::string& path) {
    std::string label;
    {
        std::lock_guard lock(impl_->m);
        label = impl_->display_label;
    }
    std::lock_guard lock(impl_->log_mutex);
    if (impl_->log_file.is_open()) {
        impl_->log_file.close();
    }
    impl_->log_file.open(path, std::ios::binary | std::ios::trunc);
    if (!impl_->log_file) {
        return rhi::Error{"cannot open the present log " + path};
    }
    impl_->log_file << FormatPresentHeader(UtcDate(std::chrono::system_clock::now()), impl_->refresh_hz, label);
    impl_->log_file.flush();
    return rhi::Result<void>{};
}

rhi::Result<void> Presenter::DumpTo(const std::string& dir, u32 every_n) {
    if (!impl_->target.IsOffscreen()) {
        return rhi::Error{"DumpTo: only an offscreen target can be dumped"};
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return rhi::Error{"DumpTo: cannot create " + dir + ": " + ec.message()};
    }
    std::lock_guard lock(impl_->m);
    impl_->dump_dir = dir;
    impl_->dump_every = every_n == 0 ? 1 : every_n;
    return rhi::Result<void>{};
}

void Presenter::Stop() {
    std::lock_guard join(impl_->join_mutex);
    {
        std::lock_guard lock(impl_->m);
        impl_->stop.store(true);
    }
    impl_->cv.notify_all();
    if (impl_->thread.joinable() && impl_->thread.get_id() != std::this_thread::get_id()) {
        impl_->thread.join();
    }
}

bool Presenter::StopRequested() const {
    return impl_->StopFlag();
}

Stats Presenter::GetStats() const {
    std::lock_guard lock(impl_->m);
    return impl_->stats;
}

} // namespace mh4n::swapchain
