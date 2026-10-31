// Derived from Azahar src/video_core/pica/pica_core.h, pica_core.cpp, shader_setup.h,
// shader_setup.cpp, packed_attribute.h and src/video_core/pica_types.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; every one of those files says
// "Licensed under GPLv2 or any later version"). src/common/vector_math.h was read for the
// semantics of Vec4::operator== (a memcmp); no code of it is used.
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_core.h, pica_core.cpp,
//   shader_setup.h, shader_setup.cpp, packed_attribute.h)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (pica_types.h)
// Copyright 2015-2025 Citra Emulator Project / Azahar Emulator Project (vector_math.h)
// Copyright 2014 Tony Wasserka (vector_math.h, second notice block)
#include "gpu/pica/state.h"

#include <algorithm>
#include <bit>
#include <utility>

namespace mh4n::gpu::pica {

float Float24ToFloat(u32 hex) {
    // Pica::Float<16, 7>::FromRaw: bias = 127 - ((1 << (E - 1)) - 1) = 64.
    constexpr s32 kBias = 127 - 63;
    constexpr u32 kMantissaMask = (1u << 16) - 1u;
    constexpr u32 kExponentMask = (1u << 7) - 1u;

    s32 exponent = static_cast<s32>((hex >> 16) & kExponentMask);
    u32 mantissa = hex & kMantissaMask;
    const u32 sign = (hex >> 23) << 31;

    if (exponent == 0 && mantissa == 0) {
        hex = sign; // zero
    } else if (exponent == 0) {
        // The PICA flushes denormals; here they are converted exactly, which needs them
        // renormalised.
        exponent = kBias + 1;
        while ((mantissa & (1u << 16)) == 0) {
            exponent--;
            mantissa <<= 1;
        }
        mantissa &= kMantissaMask;
        hex = sign | (static_cast<u32>(exponent) << 23) | (mantissa << (23 - 16));
    } else if (exponent == static_cast<s32>(kExponentMask)) {
        hex = sign | (0xFFu << 23) | (mantissa << (23 - 16)); // infinity or NaN
    } else {
        hex = sign | (static_cast<u32>(kBias + exponent) << 23) | (mantissa << (23 - 16));
    }
    return std::bit_cast<float>(hex);
}

bool PackedAttribute::Push(u32 word, bool is_float32) {
    if (index >= buffer.size()) {
        index = 0;
    }
    buffer[index++] = word;
    return (index >= 4 && is_float32) || (index >= 3 && !is_float32);
}

Vec4f PackedAttribute::Get(bool is_float32) {
    Reset();
    Vec4f result;
    if (is_float32) {
        for (u32 i = 0; i < 4; ++i) {
            result.c[3 - i] = std::bit_cast<float>(buffer[i]);
        }
    } else {
        const u32 x = buffer[2] & 0xFFFFFF;
        const u32 y = ((buffer[1] & 0xFFFF) << 8) | ((buffer[2] >> 24) & 0xFF);
        const u32 z = ((buffer[0] & 0xFF) << 16) | ((buffer[1] >> 16) & 0xFFFF);
        const u32 w = buffer[0] >> 8;
        result.c[0] = Float24ToFloat(x);
        result.c[1] = Float24ToFloat(y);
        result.c[2] = Float24ToFloat(z);
        result.c[3] = Float24ToFloat(w);
    }
    return result;
}

void ShaderSetup::WriteUniformBoolReg(u32 value) {
    for (u32 i = 0; i < uniform_b.size(); ++i) {
        const bool bit = ((value >> i) & 1u) != 0;
        const bool prev = std::exchange(uniform_b[i], bit);
        uniforms_dirty |= prev != bit;
    }
}

void ShaderSetup::WriteUniformIntReg(u32 index, const std::array<u8, 4>& values) {
    const auto prev = std::exchange(uniform_i[index & 3], values);
    uniforms_dirty |= prev != values;
}

std::optional<u32> ShaderSetup::WriteUniformFloatReg(u32& index_reg, u32 value) {
    const bool is_float32 = (index_reg >> 31) != 0;
    if (!uniform_queue.Push(value, is_float32)) {
        return std::nullopt;
    }

    const Vec4f uniform = uniform_queue.Get(is_float32);
    const u32 index = BitsOf(index_reg, 0, 7);
    if (index >= uniform_f.size()) {
        // Some games submit out-of-range indices (mainly 0x7F), which real hardware ignores.
        return std::nullopt;
    }

    const Vec4f prev = std::exchange(uniform_f[index], uniform);
    uniforms_dirty |= !(prev == uniform);
    index_reg = WithBits(index_reg, 0, 7, index + 1);
    return index;
}

std::optional<ShaderSetup::UniformWriteRange>
ShaderSetup::WriteUniformFloatRegRange(u32& index_reg, std::span<const u32> values) {
    if (values.empty()) {
        return std::nullopt;
    }

    const bool is_float32 = (index_reg >> 31) != 0;
    std::optional<u32> first_index;
    u32 written = 0;

    for (const u32 word : values) {
        if (!uniform_queue.Push(word, is_float32)) {
            continue;
        }

        const Vec4f uniform = uniform_queue.Get(is_float32);
        const u32 index = BitsOf(index_reg, 0, 7);
        if (index >= uniform_f.size()) {
            // Unlike the one-word path, a batch gives up at the first out-of-range index.
            break;
        }

        const Vec4f prev = std::exchange(uniform_f[index], uniform);
        uniforms_dirty |= !(prev == uniform);
        index_reg = WithBits(index_reg, 0, 7, index + 1);

        if (!first_index) {
            first_index = index;
        }
        ++written;
    }

    if (written == 0) {
        return std::nullopt;
    }
    return UniformWriteRange{*first_index, written};
}

void ShaderSetup::UpdateProgramCode(u32 offset, u32 value) {
    u32& inst = program_code[offset];
    if (inst == value) {
        return;
    }
    inst = value;
    program_dirty = true;
    biggest_program_size = std::max(biggest_program_size, offset + 1);
}

void ShaderSetup::UpdateProgramCodeRange(u32 offset, std::span<const u32> values) {
    for (u32 i = 0; i < values.size(); ++i) {
        UpdateProgramCode(offset + i, values[i]);
    }
}

void ShaderSetup::UpdateSwizzleData(u32 offset, u32 value) {
    u32& data = swizzle_data[offset];
    if (data == value) {
        return;
    }
    data = value;
    swizzle_dirty = true;
    biggest_swizzle_size = std::max(biggest_swizzle_size, offset + 1);
}

void ShaderSetup::UpdateSwizzleDataRange(u32 offset, std::span<const u32> values) {
    for (u32 i = 0; i < values.size(); ++i) {
        UpdateSwizzleData(offset + i, values[i]);
    }
}

PicaState::PicaState() {
    // PicaCore::InitializeRegs, the part that is internal: values initialised by GSP, and the
    // geometry shader unit's defaults ("Tales of Abyss expects this register to have the
    // following default values").
    regs[reg::kIrqAutostop] = 1;
    regs[reg::kIrqMask] = 0xFFFFFFF0;
    // Older libctru versions did not set this; Azahar initialises it to avoid an endless black
    // screen.
    regs[reg::kIrqCompare] = 0x12345678;
    u32& gs_input = regs[reg::kGs + reg::kShaderInputBufferConfig];
    gs_input = WithBits(gs_input, 0, 4, 1);
    gs_input = WithBits(gs_input, 24, 8, kShaderModeVs);

    dirty.SetAll();
}

void PicaState::ClearDirty() {
    dirty.Reset();
    vs.ClearDirty();
    gs.ClearDirty();
    lighting.lut_dirty = 0;
    fog.lut_dirty = false;
    gas.table_dirty = false;
    proctex.table_dirty = 0;
}

void DelayGenerator::AddVertices(size_t vertices, u32 topology) {
    size_t triangles{};
    if (topology == kTopologyFan || topology == kTopologyStrip) {
        triangles = (vertices >= 3) ? (vertices - 2) : 1;
    } else {
        // Geometry shaders produce more vertices per given vertex, but they are not that
        // relevant for timing emulation.
        triangles = vertices / 3;
    }
    triangle_count_ += triangles;
}

u64 DelayGenerator::CalculateAndResetDelay() {
    // Azahar's estimate (pica_core.h): 800 / 2 ns per triangle, 35% of the triangles culled at
    // 20% of the cost, 22.4 ns per command.
    constexpr float nanoseconds_per_triangle = 800.f / 2;
    constexpr float culled_triangle_threshold = 0.35f;
    constexpr float culled_triangle_time_cost = 0.20f;
    constexpr float nanoseconds_per_command = 22.4f;

    float result = command_count_ * nanoseconds_per_command;

    result += (1.f - culled_triangle_threshold) * triangle_count_ * nanoseconds_per_triangle;
    result += culled_triangle_threshold * triangle_count_ *
              (nanoseconds_per_triangle * culled_triangle_time_cost);

    triangle_count_ = 0;
    command_count_ = 0;

    return static_cast<u64>(result);
}

} // namespace mh4n::gpu::pica
