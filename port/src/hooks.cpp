#include "hooks.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging/log.h"

// Hooks read and write guest memory through Azahar's page table, like recompiled code.
#define RC_PAGED
extern "C" {
#include "recomp.h"
}

namespace Port::Hooks {

namespace {

// USA v1.3.0 (docs/research/system-services.md, "Frame pacing").
constexpr u32 SMAIN_INSTANCE = 0x01057194;  // sMhMain*: the engine's main system (an sMain)
constexpr u32 SMAIN_RATE = 0x2c;            // f32: the unit of delta, in frames per second (60)
constexpr u32 SMAIN_CAP = 0x30;             // f32: frame-rate cap of the frame limiter
constexpr u32 SMAIN_FPS = 0x34;             // f32: measured frame rate, refreshed each second
constexpr u32 SMAIN_DELTA = 0x5c;           // f32: last frame's length in 1/rate s
constexpr u32 VBLANK_TOP = 0x0110d004;      // VBlank counters kept by the GX library
constexpr u32 VBLANK_BOTTOM = 0x0110d008;
constexpr u32 FRAMES_SUBMITTED = 0x010571d4;
constexpr u32 SLTRENDER_INSTANCE = 0x010572cc;
constexpr u32 SLTRENDER_DISPLAY_SKIP = 0x8aa8;  // u32[2] mDisplaySkip: top, bottom

float ReadF32(RCtx* ctx, u32 addr) {
    const u32 v = rd32(ctx, addr);
    float f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
}

void WriteF32(RCtx* ctx, u32 addr, float f) {
    u32 v;
    std::memcpy(&v, &f, sizeof(v));
    wr32(ctx, addr, v);
}

struct Options {
    float fps_cap = 0;    // MH4U_FPS: frame-rate cap, 0 = the game's own
    u32 trace_every = 0;  // MH4U_TRACE_FRAMES: log frame timing every n frames, 0 = off
};
Options options;
u64 frames = 0;

/// sMain::updateFrameTiming(this): the frame limiter. Delta time is counted in 1/60 s
/// whatever the cap, and gameplay scales with it, so the cap is what limits the frame rate.
void UpdateFrameTiming(RCtx* ctx, RFunc original) {
    if (options.fps_cap > 0) {
        WriteF32(ctx, ctx->r[0] + SMAIN_CAP, options.fps_cap);
    }
    original(ctx);
}

/// sMhMain::frame(this): one iteration of the main loop.
void Frame(RCtx* ctx, RFunc original) {
    original(ctx);
    ++frames;
    if (options.trace_every == 0 || frames % options.trace_every != 0) {
        return;
    }
    const u32 main = rd32(ctx, SMAIN_INSTANCE);
    const u32 render = rd32(ctx, SLTRENDER_INSTANCE);
    if (main == 0 || render == 0) {
        return;
    }
    LOG_INFO(Frontend,
             "frame {}: delta {:.3f} fps {:.2f} rate {:.1f} cap {:.1f} vblanks {}/{} "
             "submitted {} display skip {}/{}",
             frames, ReadF32(ctx, main + SMAIN_DELTA), ReadF32(ctx, main + SMAIN_FPS),
             ReadF32(ctx, main + SMAIN_RATE), ReadF32(ctx, main + SMAIN_CAP), rd32(ctx, VBLANK_TOP),
             rd32(ctx, VBLANK_BOTTOM), rd32(ctx, FRAMES_SUBMITTED),
             rd32(ctx, render + SLTRENDER_DISPLAY_SKIP),
             rd32(ctx, render + SLTRENDER_DISPLAY_SKIP + 4));
}

struct Hook {
    u32 addr;
    const char* name;
    RHookFn fn;
};
constexpr Hook HOOKS[] = {
    {0x006442f8, "sMain::updateFrameTiming", UpdateFrameTiming},
    {0x00af5250, "sMhMain::frame", Frame},
};

} // namespace

std::vector<std::string> Install(const RCodeInfo& info) {
    std::vector<std::string> log;
    if (!info.set_hook || info.hook_count == 0) {
        return log; // a test program, or a library from before hook points
    }
    if (const char* fps = std::getenv("MH4U_FPS")) {
        options.fps_cap = static_cast<float>(std::atof(fps));
        log.push_back(fmt::format("frame-rate cap {}", options.fps_cap));
    }
    if (const char* every = std::getenv("MH4U_TRACE_FRAMES")) {
        options.trace_every = static_cast<u32>(std::max(1, std::atoi(every)));
        log.push_back(fmt::format("tracing frame timing every {} frames", options.trace_every));
    }
    for (const Hook& hook : HOOKS) {
        if (!info.set_hook(hook.addr, hook.fn)) {
            log.push_back(fmt::format("no hook point for {} at {:08X} (rebuild the code library)",
                                      hook.name, hook.addr));
        }
    }
    return log;
}

} // namespace Port::Hooks
