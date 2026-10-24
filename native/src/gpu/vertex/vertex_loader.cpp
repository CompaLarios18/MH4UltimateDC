// Derived from Azahar src/video_core/pica/vertex_loader.cpp, vertex_loader.h, regs_pipeline.h,
// regs_shader.h, shader_unit.cpp, pica_core.cpp, src/video_core/rasterizer_accelerated.cpp,
// src/video_core/renderer_vulkan/vk_instance.cpp, vk_rasterizer.cpp and vk_graphics_pipeline.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: vk_rasterizer.cpp and
// vk_graphics_pipeline.cpp say "GPLv3 or any later version"; every other source named here says
// "GPLv2 or any later version"): the attribute loaders' layout rules (alignment, padding
// components, which register an attribute feeds), the vertex range of a draw
// (AnalyzeVertexArray), the repacking of a loader's data to an aligned stride, the fixed and
// default attribute buffer, the index buffer set-up and the base-vertex rule of the draw.
// Copyright 2023 Citra Emulator Project (vertex_loader.cpp, vertex_loader.h)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_pipeline.h,
//   regs_shader.h)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (shader_unit.cpp, pica_core.cpp,
//   rasterizer_accelerated.cpp, vk_instance.cpp, vk_rasterizer.cpp, vk_graphics_pipeline.cpp)
//
// Where the interpreter's VertexLoader and the hardware renderers differ, this follows the
// VertexLoader (what the interpreter feeds the vertex shader is the reference): a default
// attribute wins over a loader that also provides it, a later attribute that maps to the same
// register wins, and only the attributes up to the vertex shader's last input (and below the
// number of attributes) are described.
#include "gpu/vertex/vertex_loader.h"

#include <algorithm>
#include <cstring>

#include "gpu/pica/regs.h"
#include "gpu/pica/state.h"
#include "mh4n/log.h"

namespace mh4n::gpu::vertex {
namespace {

using pica::BitsOf;

constexpr const char* kLog = "Vertex";
constexpr u32 kTailPadding = 16;
constexpr u32 kNumLocations = 16;
constexpr u32 kNumLoaders = 12;
constexpr u32 kNumAttributes = 12;
// A draw of more vertices than this is not a draw (the largest an index can name is 65536).
constexpr u32 kMaxArraysVertices = 1u << 24;

// PipelineRegs::VertexAttributeFormat.
constexpr u32 kFmtByte = 0;
constexpr u32 kFmtUByte = 1;
constexpr u32 kFmtShort = 2;
constexpr u32 kFmtFloat = 3;

constexpr u32 kRegAttributeFormats = 0x201; // formats and sizes of attributes 0-7, 4 bits each
constexpr u32 kRegAttributeConfig = 0x202;  // 8-11, mask bits 16-27, max_attribute_index 28-31
constexpr u32 kRegLoaders = 0x203;          // 12 loaders, 3 words each
constexpr u32 kShaderInputMapLow = 0x0B;
constexpr u32 kShaderInputMapHigh = 0x0C;

constexpr u32 AlignUp(u32 value, u32 alignment) {
    return alignment <= 1 ? value : (value + alignment - 1) / alignment * alignment;
}

constexpr u32 ElementSize(u32 format) {
    return format == kFmtFloat ? 4 : (format == kFmtShort ? 2 : 1);
}

// MakeAttributeFormat's integer table (attrib_formats_int), with the 3-component integer cases
// aliased to the 4-component format (needs_emulation in Azahar), returned with the alias flag.
VertexFormat MakeFormat(u32 pica_format, u32 count, bool& alias_w) {
    alias_w = false;
    if (pica_format == kFmtFloat) {
        switch (count) {
        case 1: return VertexFormat::R32_FLOAT;
        case 2: return VertexFormat::R32G32_FLOAT;
        case 3: return VertexFormat::R32G32B32_FLOAT;
        default: return VertexFormat::R32G32B32A32_FLOAT;
        }
    }
    if (count == 3) {
        alias_w = true;
        count = 4;
    }
    const u32 count_index = count == 4 ? 2 : count - 1; // 1, 2, 4 -> 0, 1, 2
    switch (pica_format) {
    case kFmtByte:
        return static_cast<VertexFormat>(static_cast<u32>(VertexFormat::R8_SINT) + count_index);
    case kFmtUByte:
        return static_cast<VertexFormat>(static_cast<u32>(VertexFormat::R8_UINT) + count_index);
    default:
        return static_cast<VertexFormat>(static_cast<u32>(VertexFormat::R16_SINT) + count_index);
    }
}

Conversion ConversionFor(u32 pica_format) {
    // MakeAttribLoadFlag
    switch (pica_format) {
    case kFmtByte:
    case kFmtShort:
        return Conversion::FromSint;
    case kFmtUByte:
        return Conversion::FromUint;
    default:
        return Conversion::None;
    }
}

struct Attr {
    u32 format = 0;
    u32 elements = 0; // GetNumElements
    bool is_default = false;
    bool has_source = false;
    u32 loader = 0;
    u32 offset = 0; // inside the loader's vertex
};

struct Loader {
    u32 data_offset = 0;
    u32 byte_count = 0;
    u32 component_count = 0;
    std::array<u32, 12> components{};
};

enum class LocKind : u8 { Unset, Loader, Fixed };
struct Location {
    LocKind kind = LocKind::Unset;
    u32 attr = 0;
    const pica::Vec4f* value = nullptr;
    u32 fixed_offset = 0;
};

u32 RegisterFor(const pica::PicaState& s, u32 attr) {
    const u64 map = (static_cast<u64>(s.regs[pica::reg::kVs + kShaderInputMapHigh]) << 32) |
                    s.regs[pica::reg::kVs + kShaderInputMapLow];
    return static_cast<u32>((map >> (attr * 4)) & 0xF);
}

u32 ReadU16(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8);
}

void PutIndex(std::vector<u8>& out, u32 value, IndexType type) {
    out.push_back(static_cast<u8>(value));
    if (type == IndexType::U8) {
        return;
    }
    out.push_back(static_cast<u8>(value >> 8));
    if (type == IndexType::U16) {
        return;
    }
    out.push_back(static_cast<u8>(value >> 16));
    out.push_back(static_cast<u8>(value >> 24));
}

} // namespace

FormatInfo GetFormatInfo(VertexFormat format) {
    switch (format) {
    case VertexFormat::R8_SINT: return {ComponentKind::S8, 1, 1};
    case VertexFormat::R8G8_SINT: return {ComponentKind::S8, 2, 2};
    case VertexFormat::R8G8B8A8_SINT: return {ComponentKind::S8, 4, 4};
    case VertexFormat::R8_UINT: return {ComponentKind::U8, 1, 1};
    case VertexFormat::R8G8_UINT: return {ComponentKind::U8, 2, 2};
    case VertexFormat::R8G8B8A8_UINT: return {ComponentKind::U8, 4, 4};
    case VertexFormat::R16_SINT: return {ComponentKind::S16, 1, 2};
    case VertexFormat::R16G16_SINT: return {ComponentKind::S16, 2, 4};
    case VertexFormat::R16G16B16A16_SINT: return {ComponentKind::S16, 4, 8};
    case VertexFormat::R32_FLOAT: return {ComponentKind::F32, 1, 4};
    case VertexFormat::R32G32_FLOAT: return {ComponentKind::F32, 2, 8};
    case VertexFormat::R32G32B32_FLOAT: return {ComponentKind::F32, 3, 12};
    case VertexFormat::R32G32B32A32_FLOAT: return {ComponentKind::F32, 4, 16};
    }
    return {ComponentKind::F32, 4, 16};
}

const char* ToString(LoadStatus status) {
    switch (status) {
    case LoadStatus::Ok: return "ok";
    case LoadStatus::NoVertices: return "no vertices";
    case LoadStatus::Unmapped: return "vertex or index data outside guest memory";
    case LoadStatus::TooLarge: return "more vertices than any draw has";
    }
    return "?";
}

LoadedDraw VertexLoader::Load(gpu::Host& host, const pica::DrawCall& draw) {
    LoadedDraw out;
    const pica::PicaState& s = *draw.state;
    const auto& regs = s.regs;
    const u32 align = std::max<u32>(caps_.stride_alignment, 1);
    const bool native_u8 = caps_.index_uint8 && !caps_.apple;
    const bool native_fans = caps_.triangle_fans && !caps_.apple;

    switch (BitsOf(regs[pica::reg::kTriangleTopology], 8, 2)) {
    case pica::kTopologyList: out.topology = Topology::TriangleList; break;
    case pica::kTopologyStrip: out.topology = Topology::TriangleStrip; break;
    case pica::kTopologyFan: out.topology = Topology::TriangleFan; break;
    default: out.topology = Topology::Shader; break;
    }
    const bool convert_fan = out.topology == Topology::TriangleFan && !native_fans;

    const bool immediate = draw.kind == pica::DrawKind::Immediate;
    const u32 vs_max = BitsOf(regs[pica::reg::kVs + pica::reg::kShaderInputBufferConfig], 0, 4);

    // ---- the attributes and the loaders (VertexLoader's constructor) -------------------------------
    std::array<Attr, kNumLocations> attrs{};
    std::array<Loader, kNumLoaders> loaders{};
    const u32 num_total = BitsOf(regs[kRegAttributeConfig], 28, 4) + 1;
    const u32 default_mask = BitsOf(regs[kRegAttributeConfig], 16, 12);
    if (!immediate) {
        for (u32 i = 0; i < kNumAttributes; ++i) {
            const u32 word = regs[i < 8 ? kRegAttributeFormats : kRegAttributeConfig];
            const u32 shift = (i % 8) * 4;
            attrs[i].format = BitsOf(word, shift, 2);
            attrs[i].elements = BitsOf(word, shift + 2, 2) + 1;
        }
        for (u32 i = 0; i < kNumLocations; ++i) {
            attrs[i].is_default = i >= kNumAttributes || ((default_mask >> i) & 1u) != 0;
        }
        for (u32 l = 0; l < kNumLoaders; ++l) {
            const u32 base = kRegLoaders + 3 * l;
            Loader& loader = loaders[l];
            loader.data_offset = BitsOf(regs[base], 0, 28);
            loader.byte_count = BitsOf(regs[base + 2], 16, 8);
            loader.component_count = BitsOf(regs[base + 2], 28, 4);
            for (u32 c = 0; c < 8; ++c) {
                loader.components[c] = BitsOf(regs[base + 1], 4 * c, 4);
            }
            for (u32 c = 0; c < 4; ++c) {
                loader.components[8 + c] = BitsOf(regs[base + 2], 4 * c, 4);
            }
            // A loader without data or without a stride provides nothing (the accelerated
            // renderers skip it).
            if (loader.component_count == 0 || loader.byte_count == 0) {
                continue;
            }
            u32 offset = 0;
            for (u32 c = 0; c < loader.component_count; ++c) {
                if (c >= 12) {
                    MH4N_LOG(Error, kLog, "loader {} has more than 12 components", l);
                    break;
                }
                const u32 a = loader.components[c];
                if (a < kNumAttributes) {
                    offset = AlignUp(offset, ElementSize(attrs[a].format));
                    attrs[a].has_source = true; // a later loader overwrites an earlier one
                    attrs[a].loader = l;
                    attrs[a].offset = offset;
                    offset += attrs[a].elements * ElementSize(attrs[a].format);
                } else {
                    // Attribute ids 12-15 are paddings of 4, 8, 12 and 16 bytes.
                    offset = AlignUp(offset, 4) + (a - 11) * 4;
                }
            }
        }
    }

    // ---- which attribute feeds which input register (ShaderUnit::LoadInput) --------------------------
    std::array<Location, kNumLocations> locs{};
    for (u32 a = 0; a <= vs_max; ++a) {
        const u32 reg = RegisterFor(s, a);
        Location loc;
        if (immediate) {
            loc.kind = LocKind::Fixed;
            loc.value = &s.immediate.input_vertex[a];
        } else if (a >= num_total) {
            // not loaded: stays the constant (0, 0, 0, 1)
        } else if (attrs[a].is_default) {
            loc.kind = LocKind::Fixed;
            loc.value = &s.default_attributes[a];
        } else if (attrs[a].has_source) {
            loc.kind = LocKind::Loader;
        }
        loc.attr = a;
        locs[reg] = loc; // a later attribute overwrites an earlier one
    }

    // ---- the range of vertices ---------------------------------------------------------------------------
    u32 vertex_min = 0;
    u32 vertex_num = 0;
    std::vector<u32> indices; // Indexed draws: the index values
    bool source_u16 = false;
    if (draw.kind == pica::DrawKind::Arrays) {
        vertex_min = draw.vertex_offset;
        vertex_num = draw.num_vertices;
        if (vertex_num == 0) {
            out.status = LoadStatus::NoVertices;
        } else if (vertex_num > kMaxArraysVertices) {
            MH4N_LOG(Warning, kLog, "an Arrays draw of {} vertices is dropped", vertex_num);
            out.status = LoadStatus::TooLarge;
        }
    } else if (draw.kind == pica::DrawKind::Indexed) {
        source_u16 = draw.index_u16;
        const u32 index_size = source_u16 ? 2 : 1;
        const u64 bytes = static_cast<u64>(draw.num_vertices) * index_size;
        if (draw.num_vertices == 0) {
            out.status = LoadStatus::NoVertices;
        } else if (bytes > 0xFFFFFFFFull ||
                   static_cast<u64>(draw.vertex_base_address) + draw.index_offset + bytes >
                       0xFFFFFFFFull) {
            out.status = LoadStatus::Unmapped;
        } else {
            const u8* p = host.PhysicalPointer(draw.vertex_base_address + draw.index_offset,
                                               static_cast<u32>(bytes));
            if (p == nullptr) {
                out.status = LoadStatus::Unmapped;
            } else {
                indices.resize(draw.num_vertices);
                u32 lo = 0xFFFFFFFFu;
                u32 hi = 0;
                for (u32 i = 0; i < draw.num_vertices; ++i) {
                    const u32 v = source_u16 ? ReadU16(p + 2 * i) : p[i];
                    indices[i] = v;
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
                vertex_min = lo;
                vertex_num = hi - lo + 1;
            }
        }
    } else {
        vertex_num = 1;
    }
    if (out.status != LoadStatus::Ok) {
        ++counters_.draws_dropped;
        return out;
    }
    out.first_vertex = vertex_min;

    // ---- the buffers --------------------------------------------------------------------------------------
    std::array<bool, kNumLoaders> loader_used{};
    for (const Location& loc : locs) {
        if (loc.kind == LocKind::Loader) {
            loader_used[attrs[loc.attr].loader] = true;
        }
    }
    std::array<u32, kNumLoaders> loader_binding{};
    for (u32 l = 0; l < kNumLoaders; ++l) {
        if (!loader_used[l]) {
            continue;
        }
        const Loader& loader = loaders[l];
        const u64 size = static_cast<u64>(loader.byte_count) * vertex_num;
        const u64 start = static_cast<u64>(draw.vertex_base_address) + loader.data_offset +
                          static_cast<u64>(vertex_min) * loader.byte_count;
        const u8* src = nullptr;
        if (start + size <= 0xFFFFFFFFull) {
            src = host.PhysicalPointer(static_cast<u32>(start), static_cast<u32>(size));
        }
        if (src == nullptr) {
            MH4N_LOG(Warning, kLog, "loader {} reads {} bytes at {:#x}: outside guest memory", l, size,
                     start);
            out = LoadedDraw{};
            out.status = LoadStatus::Unmapped;
            ++counters_.draws_dropped;
            return out;
        }
        VertexBuffer buffer;
        buffer.stride = AlignUp(loader.byte_count, align);
        buffer.vertex_count = vertex_num;
        buffer.data.assign(static_cast<size_t>(buffer.stride) * vertex_num + kTailPadding, 0);
        for (u32 v = 0; v < vertex_num; ++v) {
            std::memcpy(buffer.data.data() + static_cast<size_t>(v) * buffer.stride,
                        src + static_cast<size_t>(v) * loader.byte_count, loader.byte_count);
        }
        loader_binding[l] = static_cast<u32>(out.buffers.size());
        out.buffers.push_back(std::move(buffer));
    }

    // The constant buffer: (0, 0, 0, 1) at offset 0, then each fixed value (SetupFixedAttribs).
    VertexBuffer fixed;
    fixed.per_instance = true;
    fixed.vertex_count = 1;
    auto push_float4 = [&fixed](const float* v) {
        const size_t at = fixed.data.size();
        fixed.data.resize(at + 16);
        std::memcpy(fixed.data.data() + at, v, 16);
    };
    const float one_w[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    push_float4(one_w);
    for (Location& loc : locs) {
        if (loc.kind == LocKind::Fixed) {
            loc.fixed_offset = static_cast<u32>(fixed.data.size());
            push_float4(loc.value->c);
        }
    }
    fixed.stride = AlignUp(static_cast<u32>(fixed.data.size()), align);
    fixed.data.resize(fixed.stride, 0);
    const u32 fixed_binding = static_cast<u32>(out.buffers.size());
    out.buffers.push_back(std::move(fixed));

    for (u32 reg = 0; reg < kNumLocations; ++reg) {
        VertexAttribute& va = out.attributes[reg];
        const Location& loc = locs[reg];
        va.location = reg;
        if (loc.kind == LocKind::Loader) {
            const Attr& a = attrs[loc.attr];
            va.binding = loader_binding[a.loader];
            va.offset = a.offset;
            va.format = MakeFormat(a.format, a.elements, va.alias_w);
            va.components = a.elements;
            va.conversion = ConversionFor(a.format);
        } else {
            va.binding = fixed_binding;
            va.offset = loc.kind == LocKind::Fixed ? loc.fixed_offset : 0;
            va.format = VertexFormat::R32G32B32A32_FLOAT;
            va.components = 4;
            va.conversion = Conversion::None;
        }
    }

    // ---- indices and topology ---------------------------------------------------------------------------------
    if (immediate) {
        out.count = 1;
        ++counters_.draws_loaded;
        return out;
    }

    const bool indexed = draw.kind == pica::DrawKind::Indexed;
    if (!indexed && !convert_fan) {
        out.count = draw.num_vertices;
        ++counters_.draws_loaded;
        return out;
    }

    std::vector<u32> list;
    if (indexed) {
        out.base_vertex = -static_cast<s32>(vertex_min);
        list = std::move(indices);
    } else {
        list.resize(draw.num_vertices);
        for (u32 i = 0; i < draw.num_vertices; ++i) {
            list[i] = i;
        }
    }
    if (convert_fan) {
        // A fan (v0, v1, v2, v3, ...) is the triangles (v0, vi, vi+1).
        std::vector<u32> triangles;
        if (list.size() >= 3) {
            triangles.reserve((list.size() - 2) * 3);
            for (size_t i = 1; i + 1 < list.size(); ++i) {
                triangles.push_back(list[0]);
                triangles.push_back(list[i]);
                triangles.push_back(list[i + 1]);
            }
        }
        list = std::move(triangles);
        out.topology = Topology::TriangleList;
        out.fan_converted = true;
        ++counters_.fans_converted;
    }

    u32 max_value = 0;
    for (u32 v : list) {
        max_value = std::max(max_value, v);
    }
    IndexType type = IndexType::U16;
    if (indexed && !source_u16 && native_u8) {
        type = IndexType::U8;
    } else if (max_value > 0xFFFF) {
        type = IndexType::U32;
    }
    out.indices_widened_u8 = indexed && !source_u16 && type != IndexType::U8;
    if (caps_.primitive_restart_always_on && type != IndexType::U32) {
        const u32 restart = type == IndexType::U8 ? 0xFFu : 0xFFFFu;
        if (std::find(list.begin(), list.end(), restart) != list.end()) {
            type = type == IndexType::U8 ? IndexType::U16 : IndexType::U32;
            out.restart_widened = true;
            ++counters_.restart_widened;
        }
    }
    out.index_type = type;
    out.count = static_cast<u32>(list.size());
    out.index_data.reserve(list.size() * (type == IndexType::U8 ? 1 : (type == IndexType::U16 ? 2 : 4)));
    for (u32 v : list) {
        PutIndex(out.index_data, v, type);
    }
    ++counters_.draws_loaded;
    return out;
}

} // namespace mh4n::gpu::vertex
