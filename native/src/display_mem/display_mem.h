// The `null-display` GPU (N2b-07): the null GPU, plus the screens it would show.
//
// It wraps the `null` GPU found in the registry (nothing of it is copied) and forwards every call.
// It keeps a gpu_capture::DisplayShadow of the display registers, loaded at start from the inner
// GPU's register file and updated by WriteReg, SetBufferSwap and SetColorFill as the capture player
// updates it. At VBlank(0) it builds both screens from the runtime host's physical memory with
// DisplayShadow::BuildScreen (what Azahar's software renderer shows, upright, at 1x) and publishes a
// present::DisplayFrame of CPU pixels (RGBA8) to its sinks. Nothing is built while there is no sink.
//
// The null GPU skips display transfers and texture copies, so the game's own screens stay as the
// CPU left them: this is for programs that draw with the CPU and for the app's plumbing, not a
// picture of the game.
//
// Threads (principle 6): only the scheduler thread calls it; sinks are added before the run and
// removed after it.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "gpu_capture/image.h"
#include "mh4n/gpu.h"
#include "mh4n/present.h"
#include "mh4n/runtime.h"

namespace mh4n::display_mem {

// What the GPU learns from the runtime once the `display_mem.attach` boot step has run.
struct RuntimeLink {
    Runtime* runtime = nullptr;
    // The game's frame counter at its last change (0 until the first; a program that does not map
    // the counter never sees one). Written by the frame callback, read at VBlank: same thread.
    std::atomic<u32> game_frame{0};
};

class DisplayMemGpu final : public gpu::Gpu, public present::Display {
public:
    // `inner` is the GPU every call is forwarded to; its register file seeds the shadow.
    DisplayMemGpu(gpu::Host& host, std::unique_ptr<gpu::Gpu> inner);

    void Execute(const gpu::GxCommand& command) override;
    void WriteReg(u32 addr, u32 value) override;
    u32 ReadReg(u32 addr) override;
    void SetBufferSwap(u32 screen, const gpu::FrameBufferInfo& info) override;
    void SetColorFill(u32 fill) override;
    void FlushRegion(u32 vaddr, u32 size) override;
    void InvalidateRegion(u32 vaddr, u32 size) override;
    void VBlank(u32 screen) override;

    void AddScreenSink(present::ScreenSink* sink) override;
    void RemoveScreenSink(present::ScreenSink* sink) override;
    void ReadScreen(const present::ScreenImage& screen,
                    std::function<void(present::RgbImage)> done) override;

    // Gives the GPU the runtime's clock, the top screen's rows and the game's frame counter.
    void Attach(std::shared_ptr<RuntimeLink> link);
    gpu::Gpu& Inner() { return *inner_; }

private:
    std::optional<present::ScreenImage> BuildScreen(u32 screen);

    gpu::Host& host_;
    std::unique_ptr<gpu::Gpu> inner_;
    gpu_capture::DisplayShadow shadow_;
    std::vector<present::ScreenSink*> sinks_;
    std::shared_ptr<RuntimeLink> link_;
    u64 next_sequence_ = 0;
    std::string last_error_[2];
};

}  // namespace mh4n::display_mem
