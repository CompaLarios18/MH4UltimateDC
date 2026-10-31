// The compositor (N2b-05, docs/native/presentation.md): draws one present::DisplayFrame into a backend
// image with a screen_layout::Layout, on the process's one backend, through rhi.h only.
//
//     auto compositor = present::Compositor::Create(backend);          // pipelines, samplers, rings
//     rhi::Recorder rec = backend->BeginRecording();
//     present::ComposeResult done = (*compositor)->Compose(rec, *target, frame, layout, options);
//     rhi::TimelineValue value = (*compositor)->Submit(std::move(rec), std::move(done));
//
// Compose clears the target to black, draws each shown screen into its rectangle (rotated upright, only the
// stored region), the bottom screen with the layout's opacity and crop, each further view with its crop, and
// calls the UiLayer last. The recorder is left with the target's pass open (the UiLayer's raw commands are
// valid inside it); a caller that presents records PrepareForPresent on a swapchain image after Compose.
//
// The owner rule (rhi-spike.md 9.4 item 5, docs/native/rhi.md): the compositor records only on images its
// thread owns (the target, its own uploaded textures) and only samples the images of the frame. It records no
// barrier, copy or layout change on them. A frame image is sampled in GENERAL, the backend's one layout, and
// the submission must wait for the value of the submission that drew it at the fragment-shader stage: Submit
// does that (ComposeResult::waits is the list, for a caller that submits some other way). A frame image of
// another thread must have been handed over (rhi::HandOver) or be settled in GENERAL, as the producer's code
// does.
//
// Threads: a Compositor, its recorders and its ComposeResults belong to one thread (the thread that
// composes: the present thread of N2b-06). Submit each result before the next Compose.
#pragma once

#include <array>
#include <map>
#include <memory>
#include <vector>

#include "gpu/backend/rhi.h"
#include "mh4n/present.h"
#include "screen_layout/screen_layout.h"

namespace mh4n::present {

struct ComposeOptions {
    // The app's `linear_filter`: bilinear sampling of the screens, else nearest.
    bool linear_filter = false;
    // Called last, with the recorder and the target's size, while the target's pass is open (N2b-11).
    UiLayer* ui = nullptr;
};

class Compositor;

// What Compose returns: the waits the submission needs and what must stay alive until it has passed.
class ComposeResult {
public:
    // False when nothing was drawn: the target's format cannot be drawn to (the recorder is untouched).
    bool drawn = false;
    // The waits of the submission: one Wait{value, Stage::FragmentShader} per backend-image screen whose
    // timeline_value is not 0 (the highest value if several screens share one submission).
    std::vector<gpu::rhi::Wait> waits;

private:
    friend class Compositor;
    struct CpuTexture;
    std::shared_ptr<std::vector<std::shared_ptr<const void>>> frame_images_;
    std::vector<CpuTexture*> cpu_textures_;
};

class Compositor {
public:
    ~Compositor();
    Compositor(const Compositor&) = delete;
    Compositor& operator=(const Compositor&) = delete;

    // Makes the pipelines for the usual target formats (Rgba8Unorm, Bgra8Unorm), the two samplers and the
    // vertex ring. An error when the shaders do not compile or the backend cannot make them.
    static gpu::rhi::Result<std::unique_ptr<Compositor>> Create(std::shared_ptr<gpu::rhi::Backend> backend);

    // Records the composition of `frame` into `target` on `recorder` (open on the calling thread). A screen
    // that is nullopt or not Valid() is left black; a rectangle outside the target is clipped to it (an
    // Overlay layout with a height of 0 has rectangles outside Layout::width/height, screen_layout.h).
    ComposeResult Compose(gpu::rhi::Recorder& recorder, gpu::rhi::Image& target, const DisplayFrame& frame,
                          const screen_layout::Layout& layout, const ComposeOptions& options);

    // Submits `recorder` with the result's waits, keeps what the GPU still reads alive until the submission
    // has passed, and returns its timeline value.
    gpu::rhi::TimelineValue Submit(gpu::rhi::Recorder&& recorder, ComposeResult&& result);

    // How many pipelines were made, how many CPU-pixel textures were created (pool misses).
    u32 PipelineCount() const;
    u32 TexturesCreated() const;

private:
    struct State;
    explicit Compositor(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

} // namespace mh4n::present
