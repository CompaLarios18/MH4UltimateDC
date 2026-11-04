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
//      `translate_failures`); CPU path: the runner, the assembler, the batch (all on the CPU: nothing is in a ring yet);
//   5. everything that can submit the recording, BEFORE the draw's first commit to a ring it reads: room in the vertex ring,
//      the render targets for the colour and depth buffers or the shadow target (`TargetRegistry` below: a write-back
//      submits), the textures (after the targets: a target's load begins a recording, whose TrimTextures must not release
//      an image the draw has bound), the fragment uniforms with their full-ring retry; then the vertices, indices or CPU
//      batch and the vertex-stage uniforms, the fixed-function state and the dynamic state. A ring's space is attached to
//      the recording open when it is committed and is handed out again once that recording has completed, so a draw
//      recorded after a submission that followed its commits would read another draw's bytes. The invariant is checked:
//      a draw whose recording changed after its first ring commit is dropped and counted `draws_split_by_submit` (also in
//      `native_draws_dropped`; the order makes it 0). A target is marked written only once its draw is going to be recorded;
//   6. `DrawComposer::Draw` (N2-33) with the `PipelineKey` made from all of it; the default provider builds the pipeline
//      on a miss, with `PipelineBuilder::Build`.
//
// RENDER TARGETS (replaced by N2-15's surface cache): a colour target is an RGBA8 image per (address, format, size), a depth
// target a D16 or D24S8 image; guest memory stays the truth. A target is loaded from guest memory the first time it is
// used and again after the CPU side (a GX command, a flush or invalidate of its range) may have changed it; the draws
// write the image, and `WriteBackColorBuffers`, every GX command and every flush or invalidate that touches a target's
// range write it back first (N2-28's ReadbackToGuest). Depth is NOT loaded from guest memory: a new depth target starts at 1.0.
//
// TEXTURES (N2-23): a draw binds the textures its TEV stages and lighting read (BindTextures: N2-14's cache and decoder, one image per
// texture or cube, level 0 only, uploaded flipped as sampling.h says; a unit that is off, has address 0 or cannot be decoded gets a
// 1x1 null image; the sampler objects are cached by value), made on the thread that calls Execute; the counters are `texture_binds`,
// `texture_uploads`, `texture_unavailable` and `sampler_creates`. A render target a texture overlaps is written back first.
// LIMITATION: only level 0 of a texture is bound, where Azahar's Vulkan renderer gives a texture `lod.max_level + 1` levels from
// guest memory. `texture_levels_missing` counts the draws that bind a unit whose max_level register is above 0 (a plain counter,
// not `stubbed:*`): a minified draw of such a texture samples level 0 and is not the game's picture.
//
// SHADOWS (N2-12): a draw in fragment operation mode Shadow draws in the composer's shadow-write pass (no attachments, no depth
// target) into a SHADOW TARGET: an RGBA8 image made with `shadow_source`, at the colour buffer's address with the size of 0x11E,
// always 4 bytes a texel; it is keyed apart from a colour target over the same range (the overlap rule carries the contents
// through guest memory), loaded and written back as a colour target in PICA format RGBA8. A unit 0 of type Shadow2D binds its
// texture, decoded from guest memory (any target over it written back first), uploaded into a `shadow_source` image and read
// through its R32_UINT view; a ShadowCube binds the plain RGBA8 cube of its six faces; a shadow unit that is off, has address 0 or
// cannot be decoded reads word 0 (a 1x1 shadow source, or the null cube). Guest memory stays the truth: a map is written back and
// uploaded again for each frame that looks it up (N2-15 may bind the target itself). Counters: `shadow_write_draws`,
// `shadow_lookups` (draws whose unit 0 bound a decoded shadow map) and `shadow_unavailable` (shadow-mode draws dropped because the
// shadow target could not be made, also in `native_draws_dropped`).
//
// WHAT IS NOT HERE: the features still behind a stub (each counted `stubbed:<feature>`); resolution scaling (N2-15).
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
