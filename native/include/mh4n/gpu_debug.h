// What a Gpu can also implement for the capture player (N2-02).
//
// The capture player (native/tools/capture_player, docs/native/capture-player.md) drives any
// gpu::Gpu from an MGPU capture. A Gpu that wants more checks than "the guest memory it wrote"
// also derives from GpuDebug; the player finds it with dynamic_cast<GpuDebug*>(gpu). Every
// member has a default that means "not supported", so a Gpu overrides only what it can do.
//
// A wrapper (GpuWrapper) that sits in front of another Gpu must implement GpuDebug too and
// forward each call to the Gpu it wraps (when that has it), because the player only sees the
// outermost object.
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "mh4n/gpu.h"
#include "mh4n/options.h"
#include "mh4n/types.h"

namespace mh4n::gpu {

// The kinds of the INIT record's blocks (docs/native/mgpu-format.md section 4, which has each
// block's layout; all little-endian, fixed size, no padding).
enum class InitBlock : u32 {
    GpuRegs = 1,          // words 0..0x431 of the GPU's register array (4296 bytes)
    LcdRegs = 2,          // 0x400 words
    PicaRegs = 3,         // the internal registers, 0x300 words
    VsSetup = 4,          // shader setup: program, swizzles, uniforms (34368 bytes)
    GsSetup = 5,          // the same layout
    DefaultAttrs = 6,     // 16 x Vec4<f32>
    LightingLut = 7,      // 24 tables x 256 x u32
    FogLut = 8,           // 128 x u32
    ProcTex = 9,          // 896 x u32
    Immediate = 10,       // immediate-mode state
    GeometryPipeline = 11 // geometry pipeline state
};
inline constexpr u32 kInitBlockCount = 11;

// The GPU state at the start of a capture. Every block is present; the spans point into the
// player's buffers and are valid only during LoadInitState.
struct InitState {
    std::array<std::span<const u8>, kInitBlockCount + 1> blocks{}; // indexed by InitBlock value

    std::span<const u8> Block(InitBlock kind) const {
        return blocks[static_cast<u32>(kind)];
    }
};

// One internal register write, in the form of a PICA_REG record (mgpu-format.md section 3.5):
// the new value of register `reg` is (old & ~M) | (value & M), M = `mask` expanded to bytes.
struct RegisterWrite {
    u16 reg;
    u8 mask; // bit n set = byte n of the register is written
    u32 value;

    friend bool operator==(const RegisterWrite&, const RegisterWrite&) = default;
};

struct Counter {
    std::string name; // a column of the player's frame table
    u64 value;
};

class GpuDebug {
public:
    virtual ~GpuDebug() = default;

    // The state at the start of the capture; called once, before the first command.
    virtual void LoadInitState(const InitState&) {}

    // The internal register writes decoded since the last call (the command lists executed in
    // between), in order, one entry per register written, exactly as the PICA_REG records list
    // them. nullopt: this Gpu does not decode registers (--check registers then refuses it).
    virtual std::optional<std::vector<RegisterWrite>> TakeRegisterWrites() {
        return std::nullopt;
    }

    // Writes the Gpu's colour buffers (and any display-transfer targets it holds back) into guest
    // memory, so the player can read the images. Called before the player judges a command list's
    // colour buffer and before it builds a frame's screens.
    virtual void WriteBackColorBuffers() {}

    // Named counters for the frame that just ended (draws through the ubershader, HD textures
    // ready, ...). Reading them resets them. The player prints each name as a column.
    virtual std::vector<Counter> TakeFrameCounters() {
        return {};
    }

    // Time the device spent on the frame that just ended, from timestamps, when the backend has
    // them. Reading it resets it.
    virtual std::optional<u64> TakeFrameGpuTimeNs() {
        return std::nullopt;
    }
};

// Registered by name: MH4N_REGISTER(gpu::GpuWrapper, "queue", ...). The player applies every
// wrapper whose flag of the same name is set on its command line (`--queue` for "queue"; the
// module registers the flag with MH4N_OPTION("capture-player", "--queue", "", "...")), in name
// order, each around the result of the one before.
using GpuWrapper =
    std::function<std::unique_ptr<Gpu>(std::unique_ptr<Gpu> inner, Host&, const Options&)>;

} // namespace mh4n::gpu
