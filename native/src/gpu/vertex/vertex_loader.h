// Derived from Azahar src/video_core/pica/vertex_loader.cpp, vertex_loader.h, regs_pipeline.h,
// regs_shader.h, shader_unit.cpp, pica_core.cpp, src/video_core/rasterizer_accelerated.cpp,
// src/video_core/renderer_vulkan/vk_instance.cpp, vk_rasterizer.cpp, vk_graphics_pipeline.cpp,
// vk_pipeline_cache.cpp and src/video_core/shader/generator/shader_gen.h, glsl_shader_gen.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: vk_rasterizer.cpp,
// vk_graphics_pipeline.cpp and vk_pipeline_cache.cpp say "GPLv3 or any later version"; every other
// source named here says "GPLv2 or any later version"): the attribute-format table and its
// integer fallback with the 3-component alias (vk_instance.cpp, vk_graphics_pipeline.cpp), the
// per-attribute shader conversion flags (shader_gen.h, vk_pipeline_cache.cpp,
// glsl_shader_gen.cpp), and the names of the vertex-input description.
// Copyright 2023 Citra Emulator Project (vertex_loader.cpp, vertex_loader.h)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_pipeline.h,
//   regs_shader.h)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (shader_unit.cpp, pica_core.cpp,
//   rasterizer_accelerated.cpp, vk_instance.cpp, vk_rasterizer.cpp, vk_graphics_pipeline.cpp,
//   vk_pipeline_cache.cpp, shader_gen.h, glsl_shader_gen.cpp)
//
// Vertex loading (N2-05). `VertexLoader::Load` takes a draw call of the PICA command processor
// (gpu/pica/backend.h) and the guest memory it refers to, and returns what a backend needs to
// draw it: host vertex buffers, an index buffer and a description of the vertex input that
// mentions no RHI type. N2-06 (the vertex shader) reads the per-attribute conversion from the
// description, N2-08 and the backends read the buffers and the counters. Nothing here needs a
// device: what the device allows comes in as `VertexCaps`.
//
// ---- The description (layout contract for N2-06 and N2-08) -----------------------------------
//
// `LoadedDraw::attributes` has always 16 entries, indexed by the vertex shader's INPUT REGISTER
// (`location`, 0-15: the register the PICA maps the attribute to, GetRegisterForAttribute), so
// the shader declares one input per register and nothing else. Each entry says:
//
//   location    the input register (= the index of the entry)
//   binding     index into `LoadedDraw::buffers` of the buffer the attribute reads
//   offset      byte offset of the attribute inside one vertex of that buffer (before any stride
//               repacking; repacking only grows the stride, never moves an attribute)
//   format      the host vertex format (VertexFormat); the only ones produced are listed below
//   components  how many components the PICA attribute has (1-4); `format` may have more lanes
//               (see alias_w)
//   conversion  what the vertex shader must do to the fetched value to get the float the PICA
//               vertex shader sees:
//                 None      the format is float: use it as it is
//                 FromSint  the format is a signed integer format: convert each lane to float
//                           (vec4(ivec4)): the PICA's BYTE and SHORT attributes are NOT normalised
//                 FromUint  the same for an unsigned integer format (the PICA's UBYTE)
//   alias_w     the attribute has 3 components and `format` has 4 (the integer 3-component cases
//               are always aliased to the 4-component format of the same type, as Azahar's
//               vk_instance.cpp does when a device lacks the 3-component format; float 3 is
//               native). Lane 3 is then the first bytes of whatever follows in the vertex, not
//               data: the shader must overwrite w with 1.0. That is what Azahar's interpreter
//               does (ShaderUnit::LoadInput / VertexLoader fill lane 3 with One), and the
//               interpreter is N2-06's oracle. Azahar's GLSL generator writes 0 there (ZeroW):
//               a divergence between Azahar's own renderers that N2-06 must not copy.
//
// Note for N2-26's ToPlume: an aliased attribute that is the last one in its vertex can have
// offset + size_bytes > stride (3 shorts in a 6-byte vertex are read as 8 bytes). The buffer
// carries 16 zero tail bytes, so the fetch never leaves the buffer, but the 'apple' case (and any
// portability-subset device with `vertexAttributeAccessBeyondStride` false) must not rely on
// that: the backend has to pad the stride itself or take another route for such an attribute.
// The loader does not change its behaviour for this.
//
// Lanes the PICA attribute does not have (components < 4, no alias) are the fetch's defaults
// (0, 0, 0, 1) in float and in integer formats alike, as the PICA fills them (0 for x/y/z lanes
// above `components`, 1 for w), so the shader needs no code for them.
//
// An input register with no attribute (the PICA maps nothing to it, the attribute is past the
// vertex shader's last input or the number of attributes, or no loader covers it) reads from the
// constant buffer at offset 0 as (0, 0, 0, 1) (Azahar does the same). Default ("fixed")
// attributes and the vertices of immediate mode are float4 values in that buffer too.
//
// Formats produced (VertexFormat): R8_SINT, R8G8_SINT, R8G8B8A8_SINT, R8_UINT, R8G8_UINT,
// R8G8B8A8_UINT, R16_SINT, R16G16_SINT, R16G16B16A16_SINT, R32_FLOAT, R32G32_FLOAT,
// R32G32B32_FLOAT, R32G32B32A32_FLOAT. There are no scaled or normalised formats (plume has no
// SSCALED/USCALED, rhi-spike.md X3).
//
// `LoadedDraw::buffers[i]` is binding i: `data` (the bytes; at least 16 zero bytes follow the
// last vertex, so a 4-lane read of a 3-component attribute never leaves the buffer), `stride`
// (bytes per vertex; the PICA loader's byte_count rounded up to a multiple of
// `VertexCaps::stride_alignment`, the vertices repacked to it), `per_instance` (true only for
// the constant buffer, which is the last binding: draw one instance and its stride is irrelevant)
// and `vertex_count`. Buffer i < buffers.size()-1 is the PICA attribute loader's data for the
// vertices the draw uses; buffer 0's vertex 0 is source vertex `first_vertex`. A draw with no
// loader data has only the constant buffer.
//
// Drawing: `count` vertices (index_type None) or `count` indices (otherwise), `topology`,
// `base_vertex` added to each index (it is -first_vertex for the draws the PICA indexes; an
// index list this module generated from a fan of an Arrays draw is relative to buffer vertex 0
// and base_vertex is 0), one instance. `index_data` holds little-endian indices of `index_type`.
// `count == 0` means there is nothing to draw (a fan with fewer than 3 vertices).
//
// Topology: TriangleList, TriangleStrip, TriangleFan (only when the device draws fans natively
// and `apple` is not set), Shader (the PICA's "shader" topology: the primitives come from the
// geometry shader, N2-06; the vertices are plain). A fan is otherwise returned as TriangleList,
// as indexed triangles (v0, vi, vi+1).
//
// Immediate mode (DrawKind::Immediate) gives only the constant buffer, holding the vertex's
// attributes already mapped to input registers; count is 1; `topology` is the register's.
#pragma once

#include <array>
#include <vector>

#include "gpu/pica/backend.h"
#include "mh4n/gpu.h"
#include "mh4n/types.h"

namespace mh4n::gpu::vertex {

// What the device allows; the caller fills it from the backend's Capabilities (N2-26).
// The defaults are a full desktop Vulkan device.
struct VertexCaps {
    bool triangle_fans = true;  // draws TriangleFan natively
    bool index_uint8 = true;    // draws 8-bit indices natively
    bool apple = false;         // Metal-based device: fans and 8-bit indices are converted anyway
    u32 stride_alignment = 1;   // minVertexInputBindingStrideAlignment (0 is taken as 1)
    bool primitive_restart_always_on = false; // an index of all ones restarts the primitive
};

enum class VertexFormat : u8 {
    R8_SINT,
    R8G8_SINT,
    R8G8B8A8_SINT,
    R8_UINT,
    R8G8_UINT,
    R8G8B8A8_UINT,
    R16_SINT,
    R16G16_SINT,
    R16G16B16A16_SINT,
    R32_FLOAT,
    R32G32_FLOAT,
    R32G32B32_FLOAT,
    R32G32B32A32_FLOAT,
};

enum class ComponentKind : u8 { S8, U8, S16, F32 };

struct FormatInfo {
    ComponentKind kind;
    u32 lanes;      // components in the format
    u32 size_bytes; // lanes * the component size
};
FormatInfo GetFormatInfo(VertexFormat format);

enum class Conversion : u8 { None, FromSint, FromUint };

struct VertexAttribute {
    u32 location = 0;
    u32 binding = 0;
    u32 offset = 0;
    VertexFormat format = VertexFormat::R32G32B32A32_FLOAT;
    u32 components = 4;
    Conversion conversion = Conversion::None;
    bool alias_w = false;
};

struct VertexBuffer {
    std::vector<u8> data;
    u32 stride = 0;
    bool per_instance = false;
    u32 vertex_count = 0;
};

enum class Topology : u8 { TriangleList, TriangleStrip, TriangleFan, Shader };
enum class IndexType : u8 { None, U8, U16, U32 };

enum class LoadStatus : u8 {
    Ok,
    NoVertices, // an indexed draw with no indices
    Unmapped,   // the vertex or index data is (partly) outside guest memory: nothing to draw
    TooLarge,   // an Arrays draw of over 16 million vertices: not a draw
};
const char* ToString(LoadStatus status);

struct LoadedDraw {
    LoadStatus status = LoadStatus::Ok;
    Topology topology = Topology::TriangleList;
    std::array<VertexAttribute, 16> attributes{};
    std::vector<VertexBuffer> buffers;
    IndexType index_type = IndexType::None;
    std::vector<u8> index_data;
    u32 count = 0;
    s32 base_vertex = 0;
    u32 first_vertex = 0;
    // What was done to fit the device, for tests and counters.
    bool fan_converted = false;
    bool indices_widened_u8 = false; // 8-bit indices stored as 16-bit
    bool restart_widened = false;    // the index list was widened because it holds the restart value
};

struct VertexCounters {
    // Draws whose index list was widened (16 to 32 bits, or 8 to 16 bits) because an index equals
    // the value the device reads as a primitive restart. N2-08 publishes it as the counter
    // `vertex_restart_widened`.
    u64 restart_widened = 0;
    u64 fans_converted = 0;
    u64 draws_loaded = 0;
    u64 draws_dropped = 0; // status other than Ok
};
inline constexpr const char* kCounterRestartWidened = "vertex_restart_widened";

class VertexLoader {
public:
    explicit VertexLoader(const VertexCaps& caps) : caps_(caps) {}

    // Loads the vertices of `draw` (its state must be valid, i.e. call this from Backend::Draw)
    // from the guest memory behind `host`. Arrays: the vertices vertex_offset ... + num_vertices-1;
    // Indexed: the vertices between the smallest and the largest index.
    LoadedDraw Load(gpu::Host& host, const pica::DrawCall& draw);

    const VertexCaps& caps() const {
        return caps_;
    }
    const VertexCounters& counters() const {
        return counters_;
    }

private:
    VertexCaps caps_;
    VertexCounters counters_;
};

} // namespace mh4n::gpu::vertex
