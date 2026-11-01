// Derived from Azahar src/video_core/rasterizer_cache/framebuffer_base.h (FramebufferHelper: the draw rectangle, the viewport
// and the scissor of a draw), src/video_core/renderer_vulkan/vk_rasterizer.cpp (Draw: which framebuffer surfaces a draw uses),
// src/video_core/pica/regs_framebuffer.h and regs_rasterizer.h (the register positions) and src/video_core/gpu.cpp (the
// register windows) at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: vk_rasterizer.cpp says "Licensed under GPLv3
// or any later version"; the others say "Licensed under GPLv2 or any later version").
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (vk_rasterizer.cpp, gpu.cpp)
// Copyright 2023-2025 Citra Emulator Project / Azahar Emulator Project (framebuffer_base.h)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_framebuffer.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (regs_rasterizer.h)
//
// `--gpu native` (N2-08): the native renderer assembled. A `gpu::Gpu` for the capture player and, later, the runtime:
//
//   GX engine (N2-03)        every GX command that is not a command list; the register windows other than the PICA's
//   command processor (N2-04) the command lists, the internal registers; every draw reaches `Draw` below
//   vertex loading (N2-05) + vertex stage (N2-06)   the accelerated draws
//   CPU path (N2-36, N2-37)  geometry-shader, immediate-mode and refused draws: shaded on the CPU, assembled into triangles
//                            with the quaternion fix-up, drawn with the trivial vertex stage (counted `cpu_path_draws`)
//   fragment stage (N2-33, N2-08)   the key, the uniforms, the generator; the specialised shader on a miss (a counted
//                            synchronous build) or the ubershader (`--force-ubershader`, every draw)
//   framebuffer operations (N2-13)  the fixed-function state and the dynamic state
//
// PER-DRAW ORDER (what N2-23, N2-09 to N2-12, N2-16 and N2-20 can rely on), in `Draw`:
//   1. the renderer is created on first use, on the thread that calls Execute (never in the constructor: under the capture
//      player's `--queue` the GPU is made on another thread);
//   2. `FragmentState::Prepare` (key, uniforms, tables) runs for EVERY draw, also for one that is dropped, so the dirty
//      flags the command processor clears after the draw are not lost;
//   3. the primitive assembler applies the draw's register side effects, then `NeedsCpuPath(draw, assembler.IsEmpty())`;
//   4. accelerated: N2-05's loader, N2-06's translation (a program that does not translate takes the CPU path, counted
//      `translate_failures`); CPU path: the runner, the assembler, the batch;
//   5. the render targets for the colour and depth buffers (`TargetRegistry` below), the fixed-function state and the
//      dynamic state, the vertex-stage and fragment uniforms;
//   6. `DrawComposer::Draw` (N2-33) with the `PipelineKey` made from all of it; the default provider builds the pipeline
//      on a miss, with `PipelineBuilder::Build`.
//
// RENDER TARGETS (replaced by N2-15's surface cache): a colour target is an RGBA8 image per (address, format, size), a depth
// target a D16 or D24S8 image; guest memory stays the truth. A target is loaded from guest memory the first time it is
// used and again after the CPU side (a GX command, a flush or invalidate of its range) may have changed it; the draws
// write the image, and `WriteBackColorBuffers`, every GX command and every flush or invalidate that touches a target's
// range write it back first (N2-28's ReadbackToGuest). Depth is NOT loaded from guest memory: a new depth target starts at 1.0.
//
// WHAT IS NOT HERE: textures (the four units give white: N2-23), lighting, fog, procedural textures and shadows (the stubs,
// each counted `stubbed:<feature>`); a shadow-write draw is counted and not drawn; resolution scaling (N2-15).
#pragma once

#include <memory>

#include "mh4n/gpu.h"
#include "mh4n/gpu_debug.h"
#include "mh4n/options.h"

namespace mh4n::gpu::native {

struct NativeOptions {
    // Every draw takes the ubershader (`--force-ubershader`).
    bool force_ubershader = false;
};

// The `--gpu native` Gpu. Constructed with the host and the options; it touches no device until its first draw.
class NativeGpu : public Gpu, public GpuDebug {
public:
    ~NativeGpu() override = default;

    // Counters since the last call (`TakeFrameCounters` of GpuDebug resets them); `Peek` leaves them, for the tests.
    virtual std::vector<Counter> PeekCounters() const = 0;
};

std::unique_ptr<NativeGpu> MakeNativeGpu(Host& host, const NativeOptions& options);

}  // namespace mh4n::gpu::native
