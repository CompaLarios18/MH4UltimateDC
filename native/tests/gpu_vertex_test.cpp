// Derived from Azahar src/video_core/pica/vertex_loader.cpp, vertex_loader.h, regs_pipeline.h,
// regs_shader.h, shader_unit.cpp and pica_core.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later; every one of those files says "Licensed under GPLv2 or any later version"):
// the CPU reference loader below is a port of VertexLoader (its constructor and LoadVertex) with
// ShaderUnit::LoadInput's attribute-to-register mapping, and the register positions and bit
// fields the tests write are those of PipelineRegs and ShaderRegs.
// Copyright 2023 Citra Emulator Project (vertex_loader.cpp, vertex_loader.h)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_pipeline.h,
//   regs_shader.h)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (shader_unit.cpp, pica_core.cpp)
//
// mh4n_gpu_vertex (N2-05): vertex loading.
//
// Every expectation is worked out here from the PICA's register layout and Azahar's loader rules,
// not by calling the code under test; the comparison with the reference port (RefLoader) is the
// oracle that the dump check uses on the game's captures too.
//
// The dump check: with MH4N_VERTEX_CAPTURES naming .mgpu capture files (or directories that hold
// them, several entries separated by the platform's path-list separator), the last test case
// replays each capture through the PICA processor (N2-04) and, at every draw, loads the vertices
// with the module and with the reference and compares them. It prints counts only.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "gpu/pica/command_processor.h"
#include "gpu/pica/recording_backend.h"
#include "gpu/vertex/vertex_loader.h"
#include "gpu_capture/capture_host.h"
#include "gpu_capture/mgpu.h"
#include "gpu_capture/player.h"
#include "mh4n/gpu.h"
#include "mh4n/gpu_debug.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"

#if defined(_WIN32)
#include <process.h>
#define MH4N_GETPID _getpid
#else
#include <unistd.h>
#define MH4N_GETPID getpid
#endif

using namespace mh4n;
using namespace mh4n::gpu;
using namespace mh4n::gpu::vertex;
using mh4n::gpu_capture::CaptureHost;
namespace pica = mh4n::gpu::pica;
namespace fs = std::filesystem;

namespace {

constexpr u32 kFcram = 0x20000000;
constexpr u32 kBase = kFcram + 0x100000; // the vertex arrays' base address (a multiple of 16)

constexpr u32 kPicaFloat = 3;

u32 BitsOfWord(u32 word, u32 first, u32 count) {
    return (word >> first) & ((1u << count) - 1u);
}

u32 WithField(u32 word, u32 first, u32 count, u32 value) {
    const u32 mask = ((1u << count) - 1u) << first;
    return (word & ~mask) | ((value << first) & mask);
}

// ---- a draw, written with the registers the PICA has -----------------------------------------------

// PipelineRegs: 0x200 base_address (bits 1-28, x16); 0x201 formats/sizes of attributes 0-7 (format
// bits 0-1, size-1 bits 2-3, 4 bits each); 0x202 attributes 8-11 the same, mask bits 16-27,
// max_attribute_index bits 28-31; 0x203 + 3n the attribute loader n: [data_offset], [comp0..7],
// [comp8..11 bits 0-15, byte_count 16-23, component_count 28-31]. ShaderRegs of the vertex shader
// (0x2B0): +9 max input attribute (bits 0-3), +0xB/+0xC the attribute-to-register map.
struct Scene {
    CaptureHost host;
    std::unique_ptr<pica::PicaState> state = std::make_unique<pica::PicaState>();
    pica::DrawCall draw;

    Scene() {
        draw.state = state.get();
        draw.vertex_base_address = kBase;
        state->regs[0x200] = (kBase >> 4) << 1;
        MapIdentity(1);
        SetNumAttributes(1, 0);
    }

    u32& R(u32 id) {
        return state->regs[id];
    }
    void Put(u32 offset, const std::vector<u8>& bytes) {
        REQUIRE(host.WriteBoth(kBase + offset, bytes.data(), static_cast<u32>(bytes.size())));
    }
    void SetAttr(u32 attr, u32 format, u32 count) {
        const u32 word = attr < 8 ? 0x201 : 0x202;
        const u32 shift = (attr % 8) * 4;
        R(word) = WithField(R(word), shift, 2, format);
        R(word) = WithField(R(word), shift + 2, 2, count - 1);
    }
    void SetNumAttributes(u32 total, u32 default_mask) {
        R(0x202) = WithField(R(0x202), 28, 4, total - 1);
        R(0x202) = WithField(R(0x202), 16, 12, default_mask);
    }
    void SetLoader(u32 loader, u32 data_offset, const std::vector<u32>& comps, u32 byte_count) {
        const u32 base = 0x203 + 3 * loader;
        R(base) = data_offset;
        R(base + 1) = 0;
        R(base + 2) = 0;
        for (u32 c = 0; c < comps.size(); ++c) {
            if (c < 8) {
                R(base + 1) = WithField(R(base + 1), 4 * c, 4, comps[c]);
            } else {
                R(base + 2) = WithField(R(base + 2), 4 * (c - 8), 4, comps[c]);
            }
        }
        R(base + 2) = WithField(R(base + 2), 16, 8, byte_count);
        R(base + 2) = WithField(R(base + 2), 28, 4, static_cast<u32>(comps.size()));
    }
    // reg_of[a] is the input register of attribute a; the vertex shader's last input is the last.
    void Map(const std::vector<u32>& reg_of) {
        R(0x2B0 + 9) = WithField(R(0x2B0 + 9), 0, 4, static_cast<u32>(reg_of.size() - 1));
        R(0x2B0 + 0xB) = 0;
        R(0x2B0 + 0xC) = 0;
        for (u32 a = 0; a < reg_of.size(); ++a) {
            const u32 word = a < 8 ? 0x2B0 + 0xB : 0x2B0 + 0xC;
            R(word) = WithField(R(word), (a % 8) * 4, 4, reg_of[a]);
        }
    }
    void MapIdentity(u32 count) {
        std::vector<u32> m(count);
        for (u32 i = 0; i < count; ++i) {
            m[i] = i;
        }
        Map(m);
    }
    void SetTopology(u32 topology) {
        R(0x25E) = WithField(R(0x25E), 8, 2, topology);
    }
    void Arrays(u32 first, u32 count) {
        draw.kind = pica::DrawKind::Arrays;
        draw.vertex_offset = first;
        draw.num_vertices = count;
    }
    void Indexed(const std::vector<u8>& index_bytes, bool u16, u32 count, u32 index_offset = 0x80000) {
        draw.kind = pica::DrawKind::Indexed;
        draw.index_u16 = u16;
        draw.index_offset = index_offset;
        draw.num_vertices = count;
        Put(index_offset, index_bytes);
    }
};

std::vector<u8> Random(size_t n, u32 seed) {
    std::vector<u8> bytes(n);
    u32 x = seed * 2654435761u + 12345u;
    for (u8& b : bytes) {
        x = x * 1664525u + 1013904223u;
        b = static_cast<u8>(x >> 24);
    }
    return bytes;
}

std::vector<u8> Indices16(const std::vector<u32>& v) {
    std::vector<u8> b;
    for (u32 i : v) {
        b.push_back(static_cast<u8>(i));
        b.push_back(static_cast<u8>(i >> 8));
    }
    return b;
}

std::vector<u8> Indices8(const std::vector<u32>& v) {
    std::vector<u8> b;
    for (u32 i : v) {
        b.push_back(static_cast<u8>(i));
    }
    return b;
}

// ---- the CPU reference: a port of Azahar's VertexLoader and ShaderUnit::LoadInput ------------------

using Vec4 = std::array<float, 4>;

Vec4 ToVec4(const pica::Vec4f& v) {
    return {v.c[0], v.c[1], v.c[2], v.c[3]};
}

class RefLoader {
public:
    explicit RefLoader(const pica::PicaState& s) : s_(s) {
        const auto& r = s.regs;
        num_total_ = BitsOfWord(r[0x202], 28, 4) + 1;
        sources_.fill(0xdeadbeef);
        for (u32 i = 0; i < 16; ++i) {
            is_default_[i] = i >= 12 || ((BitsOfWord(r[0x202], 16, 12) >> i) & 1u) != 0;
        }
        for (u32 loader = 0; loader < 12; ++loader) {
            const u32 base = 0x203 + 3 * loader;
            const u32 data_offset = BitsOfWord(r[base], 0, 28);
            const u32 byte_count = BitsOfWord(r[base + 2], 16, 8);
            const u32 component_count = BitsOfWord(r[base + 2], 28, 4);
            if (byte_count == 0 && component_count != 0) {
                stride_zero_loader_ = true;
            }
            u32 offset = 0;
            for (u32 component = 0; component < component_count; ++component) {
                if (component >= 12) {
                    continue;
                }
                const u32 attribute =
                    component < 8 ? BitsOfWord(r[base + 1], 4 * component, 4)
                                  : BitsOfWord(r[base + 2], 4 * (component - 8), 4);
                if (attribute < 12) {
                    const u32 word = attribute < 8 ? r[0x201] : r[0x202];
                    const u32 shift = (attribute % 8) * 4;
                    const u32 format = BitsOfWord(word, shift, 2);
                    const u32 elements = BitsOfWord(word, shift + 2, 2) + 1;
                    const u32 size = format == kPicaFloat ? 4 : (format == 2 ? 2 : 1);
                    offset = (offset + size - 1) / size * size;
                    sources_[attribute] = data_offset + offset;
                    strides_[attribute] = byte_count;
                    formats_[attribute] = format;
                    elements_[attribute] = elements;
                    offset += elements * size;
                } else {
                    offset = (offset + 3) / 4 * 4;
                    offset += (attribute - 11) * 4;
                }
            }
        }
    }

    bool stride_zero_loader() const {
        return stride_zero_loader_;
    }

    // The value of every input register for `vertex`; nullopt for a register the interpreter does
    // not define (nothing was loaded for it). `ok` is false when the memory is not there.
    std::array<std::optional<Vec4>, 16> Load(gpu::Host& host, u32 base, u32 vertex, bool& ok) const {
        std::array<std::optional<Vec4>, 16> buffer;
        for (u32 i = 0; i < num_total_ && i < 16; ++i) {
            if (is_default_[i]) {
                buffer[i] = ToVec4(s_.default_attributes[i]);
                continue;
            }
            if (elements_[i] == 0) {
                continue; // "Vertex retension unimplemented"
            }
            const u32 size = formats_[i] == kPicaFloat ? 4 : (formats_[i] == 2 ? 2 : 1);
            const u32 addr = base + sources_[i] + strides_[i] * vertex;
            const u8* p = host.PhysicalPointer(addr, elements_[i] * size);
            if (p == nullptr) {
                ok = false;
                continue;
            }
            Vec4 v{};
            for (u32 comp = 0; comp < elements_[i]; ++comp) {
                switch (formats_[i]) {
                case 0: v[comp] = static_cast<float>(static_cast<s8>(p[comp])); break;
                case 1: v[comp] = static_cast<float>(p[comp]); break;
                case 2: {
                    s16 x;
                    std::memcpy(&x, p + 2 * comp, 2);
                    v[comp] = static_cast<float>(x);
                    break;
                }
                default: std::memcpy(&v[comp], p + 4 * comp, 4); break;
                }
            }
            for (u32 comp = elements_[i]; comp < 4; ++comp) {
                v[comp] = comp == 3 ? 1.0f : 0.0f;
            }
            buffer[i] = v;
        }
        // ShaderUnit::LoadInput
        std::array<std::optional<Vec4>, 16> input;
        const u64 map = (static_cast<u64>(s_.regs[0x2B0 + 0xC]) << 32) | s_.regs[0x2B0 + 0xB];
        const u32 max_attribute = BitsOfWord(s_.regs[0x2B0 + 9], 0, 4);
        for (u32 attr = 0; attr <= max_attribute; ++attr) {
            input[(map >> (attr * 4)) & 0xF] = buffer[attr];
        }
        return input;
    }

    std::array<std::optional<Vec4>, 16> LoadImmediate() const {
        std::array<std::optional<Vec4>, 16> input;
        const u64 map = (static_cast<u64>(s_.regs[0x2B0 + 0xC]) << 32) | s_.regs[0x2B0 + 0xB];
        const u32 max_attribute = BitsOfWord(s_.regs[0x2B0 + 9], 0, 4);
        for (u32 attr = 0; attr <= max_attribute; ++attr) {
            input[(map >> (attr * 4)) & 0xF] = ToVec4(s_.immediate.input_vertex[attr]);
        }
        return input;
    }

private:
    const pica::PicaState& s_;
    u32 num_total_ = 0;
    std::array<u32, 16> sources_{};
    std::array<u32, 16> strides_{};
    std::array<u32, 16> formats_{};
    std::array<u32, 16> elements_{};
    std::array<bool, 16> is_default_{};
    bool stride_zero_loader_ = false;
};

// ---- what the GPU does with the description: fetch, convert, and pick up the defaults -------------

// The value the vertex shader gets for input register `reg` of buffer vertex `v` (the fetch of the
// format, the lanes the format lacks as (0, 0, 0, 1), the conversion, and w = 1 for an alias).
bool Fetch(const LoadedDraw& ld, u32 reg, u32 v, Vec4& out) {
    const VertexAttribute& a = ld.attributes[reg];
    if (a.binding >= ld.buffers.size()) {
        return false;
    }
    const VertexBuffer& b = ld.buffers[a.binding];
    const FormatInfo info = GetFormatInfo(a.format);
    const size_t at = a.offset + (b.per_instance ? 0 : static_cast<size_t>(v) * b.stride);
    if (at + info.size_bytes > b.data.size() || (!b.per_instance && v >= b.vertex_count)) {
        return false;
    }
    out = {0.0f, 0.0f, 0.0f, 1.0f};
    const u8* p = b.data.data() + at;
    for (u32 c = 0; c < info.lanes; ++c) {
        switch (info.kind) {
        case ComponentKind::S8: out[c] = static_cast<float>(static_cast<s8>(p[c])); break;
        case ComponentKind::U8: out[c] = static_cast<float>(p[c]); break;
        case ComponentKind::S16: {
            s16 x;
            std::memcpy(&x, p + 2 * c, 2);
            out[c] = static_cast<float>(x);
            break;
        }
        case ComponentKind::F32: std::memcpy(&out[c], p + 4 * c, 4); break;
        }
    }
    // The description must agree with itself: a float format needs no conversion, an integer one
    // needs the matching one.
    const bool is_float = info.kind == ComponentKind::F32;
    const Conversion want = is_float ? Conversion::None
                                     : (info.kind == ComponentKind::U8 ? Conversion::FromUint
                                                                       : Conversion::FromSint);
    if (a.conversion != want && a.binding + 1 != ld.buffers.size()) {
        return false;
    }
    if (a.alias_w) {
        out[3] = 1.0f;
    }
    return true;
}

struct Stats {
    u32 draws_checked = 0;
    std::map<std::string, u32> skipped; // by reason
    u64 vertices = 0;
    u64 registers = 0;
    u64 mismatches = 0;
    u64 index_mismatches = 0;
};

bool SameBits(const Vec4& a, const Vec4& b) {
    return std::memcmp(a.data(), b.data(), sizeof(Vec4)) == 0;
}

constexpr u32 kFirstVertices = 64;

// Compares the description's vertices with the reference's. `limit` caps the vertices compared.
void CompareDraw(gpu::Host& host, const pica::DrawCall& draw, const LoadedDraw& ld, u32 limit,
                 Stats& stats) {
    const pica::PicaState& s = *draw.state;
    const RefLoader ref(s);
    if (ld.status != LoadStatus::Ok) {
        ++stats.skipped[std::string("not loaded: ") + ToString(ld.status)];
        return;
    }
    if (ref.stride_zero_loader() && draw.kind != pica::DrawKind::Immediate) {
        ++stats.skipped["a loader with a stride of 0 (the module skips it, like Azahar's hardware renderers)"];
        return;
    }
    ++stats.draws_checked;

    // The source vertex of each draw position.
    std::vector<u32> source;
    if (draw.kind == pica::DrawKind::Immediate) {
        source = {0};
    } else if (draw.kind == pica::DrawKind::Arrays) {
        for (u32 i = 0; i < draw.num_vertices; ++i) {
            source.push_back(draw.vertex_offset + i);
        }
    } else {
        const u32 size = draw.index_u16 ? 2 : 1;
        const u8* p = host.PhysicalPointer(draw.vertex_base_address + draw.index_offset,
                                           draw.num_vertices * size);
        for (u32 i = 0; p != nullptr && i < draw.num_vertices; ++i) {
            source.push_back(draw.index_u16 ? (p[2 * i] | (p[2 * i + 1] << 8)) : p[i]);
        }
    }

    // The primitives: the vertices the draw names must be the source vertices, in the order the
    // topology needs (a fan that was converted is the list (v0, vi, vi+1)).
    if (draw.kind != pica::DrawKind::Immediate) {
        std::vector<u32> expected = source;
        if (ld.fan_converted) {
            expected.clear();
            for (size_t i = 1; i + 1 < source.size(); ++i) {
                expected.insert(expected.end(), {source[0], source[i], source[i + 1]});
            }
        }
        std::vector<u32> got;
        for (u32 i = 0; i < ld.count; ++i) {
            u32 index = i;
            if (ld.index_type != IndexType::None) {
                const u32 width = ld.index_type == IndexType::U8 ? 1 : (ld.index_type == IndexType::U16 ? 2 : 4);
                index = 0;
                for (u32 b = 0; b < width; ++b) {
                    index |= static_cast<u32>(ld.index_data[static_cast<size_t>(i) * width + b]) << (8 * b);
                }
            }
            got.push_back(static_cast<u32>(static_cast<s64>(index) + ld.base_vertex) + ld.first_vertex);
        }
        if (ld.index_type == IndexType::None && draw.kind == pica::DrawKind::Indexed) {
            ++stats.index_mismatches;
        } else if (got != expected) {
            ++stats.index_mismatches;
        }
    }

    for (u32 i = 0; i < source.size() && i < limit; ++i) {
        ++stats.vertices;
        std::array<std::optional<Vec4>, 16> want;
        if (draw.kind == pica::DrawKind::Immediate) {
            want = ref.LoadImmediate();
        } else {
            bool ok = true;
            want = ref.Load(host, draw.vertex_base_address, source[i], ok);
            if (!ok) {
                ++stats.skipped["reference could not read the vertex"];
                continue;
            }
        }
        for (u32 reg = 0; reg < 16; ++reg) {
            if (!want[reg]) {
                continue;
            }
            ++stats.registers;
            Vec4 got{};
            const u32 buffer_vertex = draw.kind == pica::DrawKind::Immediate ? 0 : source[i] - ld.first_vertex;
            if (!Fetch(ld, reg, buffer_vertex, got) || !SameBits(got, *want[reg])) {
                ++stats.mismatches;
            }
        }
    }
}

Stats LoadAndCompare(Scene& sc, const VertexCaps& caps, LoadedDraw* keep = nullptr,
                     VertexLoader* loader_out = nullptr) {
    VertexLoader loader(caps);
    LoadedDraw ld = loader.Load(sc.host, sc.draw);
    Stats stats;
    CompareDraw(sc.host, sc.draw, ld, kFirstVertices, stats);
    if (keep != nullptr) {
        *keep = std::move(ld);
    }
    if (loader_out != nullptr) {
        *loader_out = loader;
    }
    return stats;
}

u32 Read32(const std::vector<u8>& d, size_t at) {
    u32 v;
    std::memcpy(&v, d.data() + at, 4);
    return v;
}

} // namespace

// ================================================================================================
// Attribute formats
// ================================================================================================

TEST_CASE("every component type and count 1-4 loads as the interpreter's loader does") {
    struct Expect {
        VertexFormat format;
        Conversion conversion;
        bool alias;
    };
    // PICA format 0 = BYTE, 1 = UBYTE, 2 = SHORT, 3 = FLOAT. Integers go as integer formats with
    // the shader converting; the 3-component integer cases are aliased to the 4-component format
    // (w is not data); float is native with any count.
    const Expect table[4][4] = {
        {{VertexFormat::R8_SINT, Conversion::FromSint, false},
         {VertexFormat::R8G8_SINT, Conversion::FromSint, false},
         {VertexFormat::R8G8B8A8_SINT, Conversion::FromSint, true},
         {VertexFormat::R8G8B8A8_SINT, Conversion::FromSint, false}},
        {{VertexFormat::R8_UINT, Conversion::FromUint, false},
         {VertexFormat::R8G8_UINT, Conversion::FromUint, false},
         {VertexFormat::R8G8B8A8_UINT, Conversion::FromUint, true},
         {VertexFormat::R8G8B8A8_UINT, Conversion::FromUint, false}},
        {{VertexFormat::R16_SINT, Conversion::FromSint, false},
         {VertexFormat::R16G16_SINT, Conversion::FromSint, false},
         {VertexFormat::R16G16B16A16_SINT, Conversion::FromSint, true},
         {VertexFormat::R16G16B16A16_SINT, Conversion::FromSint, false}},
        {{VertexFormat::R32_FLOAT, Conversion::None, false},
         {VertexFormat::R32G32_FLOAT, Conversion::None, false},
         {VertexFormat::R32G32B32_FLOAT, Conversion::None, false},
         {VertexFormat::R32G32B32A32_FLOAT, Conversion::None, false}},
    };
    const u32 sizes[4] = {1, 1, 2, 4};
    for (u32 type = 0; type < 4; ++type) {
        for (u32 count = 1; count <= 4; ++count) {
            CAPTURE(type);
            CAPTURE(count);
            Scene sc;
            const u32 stride = count * sizes[type];
            sc.SetAttr(0, type, count);
            sc.SetLoader(0, 0x40, {0}, stride);
            sc.Put(0x40, Random(stride * 8, type * 8 + count));
            sc.Arrays(1, 5);
            LoadedDraw ld;
            const Stats stats = LoadAndCompare(sc, VertexCaps{}, &ld);
            CHECK(stats.mismatches == 0);
            CHECK(stats.vertices == 5);
            CHECK(stats.draws_checked == 1);
            const VertexAttribute& a = ld.attributes[0];
            const Expect& e = table[type][count - 1];
            CHECK(a.format == e.format);
            CHECK(a.conversion == e.conversion);
            CHECK(a.alias_w == e.alias);
            CHECK(a.components == count);
            CHECK(a.binding == 0);
            CHECK(a.offset == 0);
            REQUIRE(ld.buffers.size() == 2);
            CHECK(ld.buffers[0].stride == stride);
            CHECK(ld.buffers[0].vertex_count == 5);
            CHECK(ld.buffers[0].data.size() >= stride * 5 + 16);
            CHECK_FALSE(ld.buffers[0].per_instance);
            CHECK(ld.buffers[1].per_instance);
            CHECK(ld.first_vertex == 1);
            CHECK(ld.count == 5);
            CHECK(ld.index_type == IndexType::None);
            // Every other register reads (0, 0, 0, 1) from the constant buffer.
            for (u32 r = 1; r < 16; ++r) {
                CHECK(ld.attributes[r].binding == 1);
                CHECK(ld.attributes[r].offset == 0);
                CHECK(ld.attributes[r].format == VertexFormat::R32G32B32A32_FLOAT);
            }
            CHECK(Read32(ld.buffers[1].data, 12) == std::bit_cast<u32>(1.0f));
        }
    }
}

TEST_CASE("signed and unsigned bytes and shorts are integers, not normalised") {
    // BYTE x3 {0xFF, 0x01, 0x80} is (-1, 1, -128, w) with w = 1 from the alias rule; SHORT x2
    // {0x8000, 0x7FFF} is (-32768, 32767, 0, 1); UBYTE x2 {0xFF, 0x80} is (255, 128, 0, 1).
    Scene sc;
    sc.SetAttr(0, 0, 3);
    sc.SetAttr(1, 2, 2);
    sc.SetAttr(2, 1, 2);
    sc.SetLoader(0, 0, {0, 1, 2}, 10);
    // offsets: attr0 at 0 (3 bytes), attr1 aligned to 2 -> 4 (4 bytes), attr2 at 8 (2 bytes): 10 bytes
    sc.SetNumAttributes(3, 0);
    sc.MapIdentity(3);
    sc.Put(0, {0xFF, 0x01, 0x80, 0, 0x00, 0x80, 0xFF, 0x7F, 0xFF, 0x80});
    sc.Arrays(0, 1);
    LoadedDraw ld;
    const Stats stats = LoadAndCompare(sc, VertexCaps{}, &ld);
    CHECK(stats.mismatches == 0);
    CHECK(ld.attributes[0].offset == 0);
    CHECK(ld.attributes[1].offset == 4);
    CHECK(ld.attributes[2].offset == 8);
    Vec4 v;
    REQUIRE(Fetch(ld, 0, 0, v));
    CHECK(v == Vec4{-1.0f, 1.0f, -128.0f, 1.0f});
    REQUIRE(Fetch(ld, 1, 0, v));
    CHECK(v == Vec4{-32768.0f, 32767.0f, 0.0f, 1.0f});
    REQUIRE(Fetch(ld, 2, 0, v));
    CHECK(v == Vec4{255.0f, 128.0f, 0.0f, 1.0f});
}

TEST_CASE("loader layout: alignment, paddings, several loaders and the register map") {
    // Loader 0: attr 2 (UBYTE x3, offset 0, 3 bytes), padding id 12 (4 bytes: aligns to 4 first,
    // so it covers 4..8), attr 0 (FLOAT x3, aligned to 4: offset 8, 12 bytes) -> 20 bytes.
    // Loader 1: attr 1 (SHORT x2, 4 bytes) at data_offset 0x100.
    // The map sends attr 0 to register 5, attr 1 to register 2 and attr 2 to register 9.
    Scene sc;
    sc.SetAttr(0, kPicaFloat, 3);
    sc.SetAttr(1, 2, 2);
    sc.SetAttr(2, 1, 3);
    sc.SetNumAttributes(3, 0);
    sc.SetLoader(0, 0, {2, 12, 0}, 20);
    sc.SetLoader(1, 0x100, {1}, 4);
    sc.Map({5, 2, 9});
    sc.Put(0, Random(20 * 6, 1));
    sc.Put(0x100, Random(4 * 6, 2));
    sc.Arrays(2, 3);
    LoadedDraw ld;
    const Stats stats = LoadAndCompare(sc, VertexCaps{}, &ld);
    CHECK(stats.mismatches == 0);
    CHECK(stats.registers == 9);
    REQUIRE(ld.buffers.size() == 3);
    CHECK(ld.attributes[9].binding == 0);
    CHECK(ld.attributes[9].offset == 0);
    CHECK(ld.attributes[9].alias_w);
    CHECK(ld.attributes[5].binding == 0);
    CHECK(ld.attributes[5].offset == 8);
    CHECK(ld.attributes[2].binding == 1);
    CHECK(ld.attributes[2].offset == 0);
    CHECK(ld.buffers[0].stride == 20);
    CHECK(ld.buffers[1].stride == 4);
    CHECK(ld.attributes[0].binding == 2); // nothing maps to register 0: the constant
    CHECK(ld.attributes[5].location == 5);
}

// ================================================================================================
// Strides
// ================================================================================================

TEST_CASE("a stride that is not a multiple of the alignment is repacked to the next multiple") {
    // 10 bytes per vertex (SHORT x2 at 0, FLOAT x1 at 4, UBYTE x2 at 8), alignment 4 -> 12.
    for (u32 alignment : {0u, 1u, 2u, 4u, 8u, 16u}) {
        CAPTURE(alignment);
        Scene sc;
        sc.SetAttr(0, 2, 2);
        sc.SetAttr(1, kPicaFloat, 1);
        sc.SetAttr(2, 1, 2);
        sc.SetNumAttributes(3, 0);
        sc.SetLoader(0, 0x20, {0, 1, 2}, 10);
        sc.MapIdentity(3);
        const std::vector<u8> data = Random(10 * 7, 5 + alignment);
        sc.Put(0x20, data);
        sc.Arrays(0, 7);
        VertexCaps caps;
        caps.stride_alignment = alignment;
        LoadedDraw ld;
        const Stats stats = LoadAndCompare(sc, caps, &ld);
        CHECK(stats.mismatches == 0);
        CHECK(stats.vertices == 7);
        const u32 a = std::max(alignment, 1u);
        const u32 expected_stride = (10 + a - 1) / a * a;
        CHECK(ld.buffers[0].stride == expected_stride);
        // Vertex 3's bytes are where the new stride puts them.
        CHECK(std::memcmp(ld.buffers[0].data.data() + 3 * expected_stride, data.data() + 30, 10) == 0);
        CHECK(ld.attributes[2].offset == 8); // attributes do not move
        // The constant buffer's stride is aligned too.
        CHECK(ld.buffers[1].stride % a == 0);
    }
}

TEST_CASE("12 attributes in 12 buffers") {
    Scene sc;
    sc.SetNumAttributes(12, 0);
    std::vector<u32> map;
    for (u32 i = 0; i < 12; ++i) {
        const u32 type = i % 4;
        const u32 count = i % 3 + 1;
        sc.SetAttr(i, type, count);
        const u32 size = (type == 3 ? 4 : (type == 2 ? 2 : 1)) * count;
        sc.SetLoader(i, 0x1000 * i, {i}, size);
        sc.Put(0x1000 * i, Random(size * 4, 100 + i));
        map.push_back(11 - i); // reversed
    }
    sc.Map(map);
    sc.Arrays(0, 4);
    LoadedDraw ld;
    const Stats stats = LoadAndCompare(sc, VertexCaps{}, &ld);
    CHECK(stats.mismatches == 0);
    CHECK(stats.registers == 12 * 4);
    REQUIRE(ld.buffers.size() == 13);
    for (u32 i = 0; i < 12; ++i) {
        CHECK(ld.attributes[11 - i].binding == i);
        CHECK(ld.attributes[11 - i].offset == 0);
    }
    CHECK(ld.buffers[12].per_instance);
    for (u32 r = 12; r < 16; ++r) {
        CHECK(ld.attributes[r].binding == 12);
    }
}

// ================================================================================================
// Default, fixed and immediate attributes
// ================================================================================================

TEST_CASE("default attributes come from the constant buffer and win over a loader") {
    // attr 0: a loader float x2 on register 3; attr 1: flagged default (value (1, 2, 3, 4)) and
    // also covered by the loader, on register 6; attr 2: flagged default (5, 6, 7, 8) on
    // register 3 as well: the later attribute wins, so register 3 is attr 2's default.
    // attr 3: no loader, not default: register 1 reads the constant (0, 0, 0, 1).
    Scene sc;
    sc.SetAttr(0, kPicaFloat, 2);
    sc.SetAttr(1, kPicaFloat, 1);
    sc.SetNumAttributes(4, 0b0110);
    sc.SetLoader(0, 0, {0, 1}, 12);
    sc.Map({3, 6, 3, 1});
    sc.state->default_attributes[1] = {{1.0f, 2.0f, 3.0f, 4.0f}};
    sc.state->default_attributes[2] = {{5.0f, 6.0f, 7.0f, 8.0f}};
    sc.Put(0, Random(12 * 3, 9));
    sc.Arrays(0, 3);
    LoadedDraw ld;
    const Stats stats = LoadAndCompare(sc, VertexCaps{}, &ld);
    CHECK(stats.mismatches == 0);
    CHECK(stats.registers == 3 * 2); // registers 3 and 6 are defined; register 1 is not
    Vec4 v;
    REQUIRE(Fetch(ld, 6, 2, v));
    CHECK(v == Vec4{1.0f, 2.0f, 3.0f, 4.0f});
    REQUIRE(Fetch(ld, 3, 0, v));
    CHECK(v == Vec4{5.0f, 6.0f, 7.0f, 8.0f});
    REQUIRE(Fetch(ld, 1, 0, v));
    CHECK(v == Vec4{0.0f, 0.0f, 0.0f, 1.0f});
    // Only the constant buffer is bound for the default ones, and the loader (attr 0) is unused:
    // its register was overwritten, so the loader's buffer is not even built.
    REQUIRE(ld.buffers.size() == 1);
    CHECK(ld.buffers[0].per_instance);
}

TEST_CASE("an attribute past the vertex shader's last input or the attribute count is not loaded") {
    Scene sc;
    sc.SetAttr(0, kPicaFloat, 1);
    sc.SetAttr(1, kPicaFloat, 1);
    sc.SetAttr(2, kPicaFloat, 1);
    sc.SetNumAttributes(2, 0); // 2 attributes, but the shader reads up to 3
    sc.SetLoader(0, 0, {0, 1, 2}, 12);
    sc.Map({0, 1, 2});
    sc.Put(0, Random(36, 3));
    sc.Arrays(0, 3);
    LoadedDraw ld;
    const Stats stats = LoadAndCompare(sc, VertexCaps{}, &ld);
    CHECK(stats.mismatches == 0);
    CHECK(stats.registers == 6);
    CHECK(ld.attributes[2].binding == 1); // the constant
    CHECK(ld.attributes[0].binding == 0);
}

TEST_CASE("immediate mode: the vertex's attributes sit in the constant buffer by register") {
    Scene sc;
    sc.draw.kind = pica::DrawKind::Immediate;
    sc.draw.num_vertices = 1;
    sc.draw.vertex_base_address = 0;
    sc.Map({4, 0, 7});
    sc.state->immediate.input_vertex[0] = {{1.0f, 2.0f, 3.0f, 4.0f}};
    sc.state->immediate.input_vertex[1] = {{-5.0f, 0.5f, 0.0f, 1.0f}};
    sc.state->immediate.input_vertex[2] = {{9.0f, 8.0f, 7.0f, 6.0f}};
    LoadedDraw ld;
    const Stats stats = LoadAndCompare(sc, VertexCaps{}, &ld);
    CHECK(stats.mismatches == 0);
    CHECK(stats.registers == 3);
    REQUIRE(ld.buffers.size() == 1);
    CHECK(ld.buffers[0].per_instance);
    CHECK(ld.count == 1);
    CHECK(ld.index_type == IndexType::None);
    Vec4 v;
    REQUIRE(Fetch(ld, 4, 0, v));
    CHECK(v == Vec4{1.0f, 2.0f, 3.0f, 4.0f});
    REQUIRE(Fetch(ld, 7, 0, v));
    CHECK(v == Vec4{9.0f, 8.0f, 7.0f, 6.0f});
    REQUIRE(Fetch(ld, 5, 0, v));
    CHECK(v == Vec4{0.0f, 0.0f, 0.0f, 1.0f});
}

// ================================================================================================
// Topology and indices
// ================================================================================================

namespace {

// One FLOAT x1 attribute, 4 bytes per vertex, 300 vertices of data.
void SimpleScene(Scene& sc) {
    sc.SetAttr(0, kPicaFloat, 1);
    sc.SetLoader(0, 0, {0}, 4);
    sc.Put(0, Random(4 * 300, 77));
}

std::vector<u32> ReadIndices(const LoadedDraw& ld) {
    std::vector<u32> out;
    const u32 width = ld.index_type == IndexType::U8 ? 1 : (ld.index_type == IndexType::U16 ? 2 : 4);
    for (u32 i = 0; i < ld.count; ++i) {
        u32 v = 0;
        for (u32 b = 0; b < width; ++b) {
            v |= static_cast<u32>(ld.index_data[static_cast<size_t>(i) * width + b]) << (8 * b);
        }
        out.push_back(v);
    }
    return out;
}

} // namespace

TEST_CASE("a fan of an Arrays draw becomes an indexed triangle list when the device cannot draw fans") {
    for (int variant = 0; variant < 3; ++variant) {
        // 0: no fans; 1: fans, but apple; 2: native fans.
        VertexCaps caps;
        caps.triangle_fans = variant != 0;
        caps.apple = variant == 1;
        CAPTURE(variant);
        Scene sc;
        SimpleScene(sc);
        sc.SetTopology(pica::kTopologyFan);
        sc.Arrays(2, 5);
        LoadedDraw ld;
        VertexLoader loader(caps);
        const Stats stats = LoadAndCompare(sc, caps, &ld, &loader);
        CHECK(stats.mismatches == 0);
        CHECK(stats.index_mismatches == 0);
        CHECK(ld.first_vertex == 2);
        if (variant == 2) {
            CHECK(ld.topology == Topology::TriangleFan);
            CHECK(ld.index_type == IndexType::None);
            CHECK(ld.count == 5);
            CHECK_FALSE(ld.fan_converted);
        } else {
            CHECK(ld.topology == Topology::TriangleList);
            CHECK(ld.fan_converted);
            CHECK(ld.index_type == IndexType::U16);
            CHECK(ld.base_vertex == 0);
            CHECK(ReadIndices(ld) == std::vector<u32>{0, 1, 2, 0, 2, 3, 0, 3, 4});
            CHECK(loader.counters().fans_converted == 1);
        }
    }
    // Fewer than three vertices: nothing to draw.
    Scene sc;
    SimpleScene(sc);
    sc.SetTopology(pica::kTopologyFan);
    sc.Arrays(0, 2);
    VertexCaps caps;
    caps.triangle_fans = false;
    LoadedDraw ld;
    LoadAndCompare(sc, caps, &ld);
    CHECK(ld.count == 0);
    CHECK(ld.topology == Topology::TriangleList);
}

TEST_CASE("strips, lists and the shader topology are not touched") {
    for (u32 topology : {pica::kTopologyList, pica::kTopologyStrip, pica::kTopologyShader}) {
        Scene sc;
        SimpleScene(sc);
        sc.SetTopology(topology);
        sc.Arrays(0, 6);
        VertexCaps caps;
        caps.triangle_fans = false;
        caps.apple = true;
        LoadedDraw ld;
        const Stats stats = LoadAndCompare(sc, caps, &ld);
        CHECK(stats.mismatches == 0);
        CHECK(stats.index_mismatches == 0);
        CHECK(ld.index_type == IndexType::None);
        CHECK(ld.count == 6);
        CHECK(ld.topology == (topology == pica::kTopologyList ? Topology::TriangleList
                              : topology == pica::kTopologyStrip ? Topology::TriangleStrip
                                                                 : Topology::Shader));
    }
}

TEST_CASE("8-bit indices stay 8-bit only where the device draws them") {
    // Indices 9, 3, 7, 3: the vertices loaded are 3..9 (7 of them) and the base vertex is -3.
    const std::vector<u32> list = {9, 3, 7, 3};
    for (int variant = 0; variant < 3; ++variant) {
        // 0: native; 1: index_uint8 off; 2: apple (index_uint8 claimed but converted anyway)
        VertexCaps caps;
        caps.index_uint8 = variant != 1;
        caps.apple = variant == 2;
        CAPTURE(variant);
        Scene sc;
        SimpleScene(sc);
        sc.Indexed(Indices8(list), false, 4);
        LoadedDraw ld;
        const Stats stats = LoadAndCompare(sc, caps, &ld);
        CHECK(stats.mismatches == 0);
        CHECK(stats.index_mismatches == 0);
        CHECK(ld.first_vertex == 3);
        CHECK(ld.base_vertex == -3);
        CHECK(ld.buffers[0].vertex_count == 7);
        CHECK(ld.count == 4);
        CHECK(ld.index_type == (variant == 0 ? IndexType::U8 : IndexType::U16));
        CHECK(ld.indices_widened_u8 == (variant != 0));
        CHECK(ReadIndices(ld) == list);
    }
}

TEST_CASE("an indexed fan keeps the indices' order and the base vertex") {
    const std::vector<u32> list = {5, 7, 9, 11, 13};
    for (int variant = 0; variant < 2; ++variant) {
        VertexCaps caps;
        caps.triangle_fans = variant == 1;
        CAPTURE(variant);
        Scene sc;
        SimpleScene(sc);
        sc.SetTopology(pica::kTopologyFan);
        sc.Indexed(Indices8(list), false, 5);
        LoadedDraw ld;
        const Stats stats = LoadAndCompare(sc, caps, &ld);
        CHECK(stats.mismatches == 0);
        CHECK(stats.index_mismatches == 0);
        CHECK(ld.base_vertex == -5);
        if (variant == 0) {
            CHECK(ld.topology == Topology::TriangleList);
            CHECK(ld.index_type == IndexType::U8);
            CHECK(ReadIndices(ld) == std::vector<u32>{5, 7, 9, 5, 9, 11, 5, 11, 13});
        } else {
            CHECK(ld.topology == Topology::TriangleFan);
            CHECK(ReadIndices(ld) == list);
        }
    }
    // Apple, 16-bit source: the fan list is 16-bit.
    VertexCaps apple;
    apple.apple = true;
    Scene sc;
    SimpleScene(sc);
    sc.SetTopology(pica::kTopologyFan);
    sc.Indexed(Indices16({1, 2, 3, 4}), true, 4);
    LoadedDraw ld;
    LoadAndCompare(sc, apple, &ld);
    CHECK(ld.index_type == IndexType::U16);
    CHECK(ReadIndices(ld) == std::vector<u32>{1, 2, 3, 1, 3, 4});
}

TEST_CASE("a 16-bit index list with 0xFFFF is widened to 32-bit where a restart is always on") {
    // Vertices 0 .. 65535 are read for the list {0, 2, 0xFFFF, 1}: byte_count 1, so 64 KiB.
    const std::vector<u32> with_restart = {0, 2, 0xFFFF, 1};
    const std::vector<u32> without = {0, 2, 0xFFFE, 1};
    auto make = [](Scene& sc, const std::vector<u32>& list) {
        sc.SetAttr(0, 1, 1);
        sc.SetLoader(0, 0, {0}, 1);
        sc.Indexed(Indices16(list), true, static_cast<u32>(list.size()));
    };
    {
        Scene sc;
        make(sc, with_restart);
        VertexCaps caps;
        caps.primitive_restart_always_on = true;
        LoadedDraw ld;
        VertexLoader loader(caps);
        const Stats stats = LoadAndCompare(sc, caps, &ld, &loader);
        CHECK(stats.mismatches == 0);
        CHECK(stats.index_mismatches == 0);
        CHECK(ld.index_type == IndexType::U32);
        CHECK(ld.restart_widened);
        CHECK(ReadIndices(ld) == with_restart);
        CHECK(loader.counters().restart_widened == 1);
        CHECK(ld.base_vertex == 0);
        CHECK(ld.buffers[0].vertex_count == 65536);
    }
    {
        // The counter keeps counting on the same loader, and a list without the value is not widened.
        Scene sc;
        make(sc, with_restart);
        Scene sc2;
        make(sc2, without);
        VertexCaps caps;
        caps.primitive_restart_always_on = true;
        VertexLoader loader(caps);
        loader.Load(sc.host, sc.draw);
        loader.Load(sc.host, sc.draw);
        const LoadedDraw ld = loader.Load(sc2.host, sc2.draw);
        CHECK(ld.index_type == IndexType::U16);
        CHECK_FALSE(ld.restart_widened);
        CHECK(loader.counters().restart_widened == 2);
    }
    {
        // Restarts are not always on: the list is left alone.
        Scene sc;
        make(sc, with_restart);
        LoadedDraw ld;
        VertexLoader loader(VertexCaps{});
        LoadAndCompare(sc, VertexCaps{}, &ld, &loader);
        CHECK(ld.index_type == IndexType::U16);
        CHECK(loader.counters().restart_widened == 0);
    }
    {
        // 8-bit indices that stay 8-bit while 0xFF restarts: widened to 16-bit, and counted.
        Scene sc;
        sc.SetAttr(0, 1, 1);
        sc.SetLoader(0, 0, {0}, 1);
        sc.Indexed(Indices8({0xFF, 1, 2}), false, 3);
        VertexCaps caps;
        caps.primitive_restart_always_on = true;
        LoadedDraw ld;
        VertexLoader loader(caps);
        const Stats stats = LoadAndCompare(sc, caps, &ld, &loader);
        CHECK(stats.mismatches == 0);
        CHECK(ld.index_type == IndexType::U16);
        CHECK(loader.counters().restart_widened == 1);
        // On apple the 8-bit list is 16-bit anyway and has no value that restarts.
        caps.apple = true;
        VertexLoader apple(caps);
        const LoadedDraw ld2 = apple.Load(sc.host, sc.draw);
        CHECK(ld2.index_type == IndexType::U16);
        CHECK(apple.counters().restart_widened == 0);
    }
}

TEST_CASE("draws that cannot be loaded are reported, not drawn") {
    VertexLoader loader(VertexCaps{});
    {
        Scene sc;
        SimpleScene(sc);
        sc.Arrays(0, 0);
        CHECK(loader.Load(sc.host, sc.draw).status == LoadStatus::NoVertices);
    }
    {
        Scene sc;
        SimpleScene(sc);
        sc.draw.vertex_base_address = 0x70000000; // outside every memory
        sc.Arrays(0, 3);
        CHECK(loader.Load(sc.host, sc.draw).status == LoadStatus::Unmapped);
    }
    {
        Scene sc;
        SimpleScene(sc);
        sc.draw.kind = pica::DrawKind::Indexed; // the two indices straddle the end of FCRAM
        sc.draw.index_u16 = false;
        sc.draw.num_vertices = 2;
        sc.draw.vertex_base_address = 0x2FFFFFFF;
        sc.draw.index_offset = 0;
        CHECK(loader.Load(sc.host, sc.draw).status == LoadStatus::Unmapped);
    }
    CHECK(loader.counters().draws_dropped == 3);
    CHECK(loader.counters().draws_loaded == 0);
}

TEST_CASE("the comparison itself notices a damaged buffer and a wrong index list") {
    Scene sc;
    SimpleScene(sc);
    sc.SetTopology(pica::kTopologyFan);
    sc.Arrays(0, 5);
    VertexCaps caps;
    caps.triangle_fans = false;
    VertexLoader loader(caps);
    LoadedDraw ld = loader.Load(sc.host, sc.draw);
    Stats good;
    CompareDraw(sc.host, sc.draw, ld, 64, good);
    CHECK(good.mismatches == 0);
    CHECK(good.index_mismatches == 0);
    ld.buffers[0].data[9] ^= 0x40;
    Stats bad;
    CompareDraw(sc.host, sc.draw, ld, 64, bad);
    CHECK(bad.mismatches == 1);
    ld.index_data[2] = 4;
    Stats bad2;
    CompareDraw(sc.host, sc.draw, ld, 64, bad2);
    CHECK(bad2.index_mismatches == 1);
}

// ================================================================================================
// Through the PICA processor and the capture player
// ================================================================================================

namespace {

// ---- a Gpu for the capture player that loads and compares at every draw ------------------------------

struct ReplayState {
    VertexCaps caps;
    Stats stats;
    u64 draws_seen = 0;
};
ReplayState g_replay;

class CheckingBackend final : public pica::Backend {
public:
    explicit CheckingBackend(gpu::Host& host) : host_(host) {}

    void Draw(const pica::DrawCall& draw) override {
        VertexLoader loader(g_replay.caps);
        const LoadedDraw ld = loader.Load(host_, draw);
        ++g_replay.draws_seen;
        CompareDraw(host_, draw, ld, kFirstVertices, g_replay.stats);
    }

private:
    gpu::Host& host_;
};

class VertexCheckGpu final : public gpu::Gpu, public gpu::GpuDebug {
public:
    explicit VertexCheckGpu(gpu::Host& host) : host_(host), backend_(host), processor_(host, backend_) {}

    void Execute(const gpu::GxCommand& command) override {
        if ((command.words[0] & 0xFF) != 1) {
            return;
        }
        const u32 vaddr = command.words[1];
        const u32 paddr = vaddr == 0 ? 0 : host_.VirtualToPhysical(vaddr).value_or(vaddr);
        processor_.SubmitGxCmdList(paddr, command.words[2]);
    }
    void WriteReg(u32 addr, u32 value) override {
        const u32 window = addr & 0xFFFFF000;
        if (window == pica::kGpuMmioBase || window == pica::kGpuMmioBase + 0x1000) {
            const u32 index = (addr - pica::kGpuMmioBase) / 4;
            if (index >= pica::kExternalRegs && index - pica::kExternalRegs < pica::kNumRegs) {
                processor_.WriteHardwareRegister(index - pica::kExternalRegs, value);
            }
        }
    }
    u32 ReadReg(u32) override {
        return 0;
    }
    void SetBufferSwap(u32, const gpu::FrameBufferInfo&) override {}
    void SetColorFill(u32) override {}
    void FlushRegion(u32, u32) override {}
    void InvalidateRegion(u32, u32) override {}
    void VBlank(u32) override {}
    void LoadInitState(const gpu::InitState& init) override {
        processor_.LoadInitState(init);
    }

private:
    gpu::Host& host_;
    CheckingBackend backend_;
    pica::CommandProcessor processor_;
};

} // namespace

MH4N_REGISTER(gpu::GpuFactory, "vertex-check",
              [](gpu::Host& host, const Options&) -> std::unique_ptr<gpu::Gpu> {
                  return std::make_unique<VertexCheckGpu>(host);
              });

namespace {

// Four device profiles the replay loads every draw with: a full device, and the three kinds of
// device the module has to convert for.
std::vector<std::pair<std::string, VertexCaps>> Profiles() {
    std::vector<std::pair<std::string, VertexCaps>> p;
    p.push_back({"full", VertexCaps{}});
    VertexCaps nofan;
    nofan.triangle_fans = false;
    nofan.index_uint8 = false;
    nofan.stride_alignment = 4;
    p.push_back({"no-fans-no-u8-stride4", nofan});
    VertexCaps apple;
    apple.apple = true;
    apple.stride_alignment = 4;
    apple.primitive_restart_always_on = true;
    p.push_back({"apple", apple});
    VertexCaps restart;
    restart.primitive_restart_always_on = true;
    restart.stride_alignment = 3;
    p.push_back({"restart-stride3", restart});
    return p;
}

gpu_capture::PlayerResult Replay(const std::string& capture) {
    const Options options = ParseOptions(
        "capture-player", std::vector<std::string>{"--gpu", "vertex-check", "--judge", "none", capture});
    REQUIRE_MESSAGE(options.ok(), options.error());
    gpu_capture::PlayerConfig config;
    std::string error;
    REQUIRE_MESSAGE(gpu_capture::ConfigFromOptions(options, config, &error), error);
    return gpu_capture::RunPlayer(config, options);
}

// A command list: [value, header, extra words...] commands (pica_core.cpp's loop).
class List {
public:
    List& Write(u32 id, u32 value) {
        if (words_.size() % 2 != 0) {
            words_.push_back(0xDEADBEEF);
        }
        words_.push_back(value);
        words_.push_back(id | (0xFu << 16));
        return *this;
    }
    std::vector<u8> Bytes() const {
        std::vector<u32> w = words_;
        if (w.size() % 2 != 0) {
            w.push_back(0);
        }
        std::vector<u8> b(w.size() * 4);
        std::memcpy(b.data(), w.data(), b.size());
        return b;
    }

private:
    std::vector<u32> words_;
};

void Put32(std::vector<u8>& b, u32 v) {
    for (int i = 0; i < 4; ++i) {
        b.push_back(static_cast<u8>(v >> (8 * i)));
    }
}

// The smallest capture that draws: an all-zero INIT, the memory, one command list in a GX command.
struct CaptureFile {
    std::vector<u8> out;
    u64 ticks = 0;
    u32 frame = 0;

    CaptureFile() {
        out = {'M', 'G', 'P', 'U'};
        Put32(out, 1);
    }
    void Record(u32 type, const std::vector<u8>& payload) {
        Put32(out, type);
        Put32(out, frame);
        Put32(out, static_cast<u32>(ticks += 1000));
        Put32(out, static_cast<u32>(ticks >> 32));
        Put32(out, static_cast<u32>(payload.size()));
        out.insert(out.end(), payload.begin(), payload.end());
    }
    void Init() {
        std::vector<u8> payload;
        Put32(payload, 11);
        for (u32 kind = 1; kind <= 11; ++kind) {
            const u32 size = gpu_capture::InitBlockSize(kind);
            Put32(payload, kind);
            Put32(payload, size);
            payload.resize(payload.size() + size, 0);
        }
        Record(gpu_capture::kInit, payload);
    }
    void Mem(u32 addr, const std::vector<u8>& data) {
        std::vector<u8> payload;
        Put32(payload, addr);
        Put32(payload, static_cast<u32>(data.size()));
        payload.insert(payload.end(), data.begin(), data.end());
        Record(gpu_capture::kMem, payload);
    }
    void SubmitCmdList(u32 vaddr, u32 size) {
        std::vector<u8> payload;
        for (u32 w : {1u, vaddr, size, 0u, 0u, 0u, 0u, 0u}) {
            Put32(payload, w);
        }
        Record(gpu_capture::kGx, payload);
    }
    // The record that follows a command list; empty, because nothing here judges the registers.
    void NoRegisterWrites() {
        std::vector<u8> payload;
        Put32(payload, 0);
        Record(gpu_capture::kPicaReg, payload);
    }
    void Frame(u32 n) {
        frame = n;
        std::vector<u8> payload;
        Put32(payload, n);
        Record(gpu_capture::kFrame, payload);
    }
    std::string Save(const std::string& name) const {
        const auto path = fs::temp_directory_path() /
                          ("mh4n_gpu_vertex_test_" + std::to_string(MH4N_GETPID()) + "_" + name + ".mgpu");
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
        return path.string();
    }
};

// A capture whose one command list sets up two attributes (a UBYTE x3 position-like attribute and
// a FLOAT x2 one in a second loader), a default attribute and the vertex shader's map, then draws
// arrays as a list and as a fan, and indexed (as a fan) with 8-bit and with 16-bit indices.
std::string SyntheticCapture(const std::string& name) {
    List list;
    list.Write(0x200, (kBase >> 4) << 1);
    // attr 0: UBYTE x3 (format 1, size-1 2), attr 1: FLOAT x2 (format 3, size-1 1), attr 2: SHORT x4
    list.Write(0x201, (1u | (2u << 2)) | ((3u | (1u << 2)) << 4) | ((2u | (3u << 2)) << 8));
    list.Write(0x202, (2u << 28) | (0u << 16)); // 3 attributes, none default
    // loader 0: attr 0 then attr 2 (SHORT aligns to 2: offset 4): 12 bytes; loader 1: attr 1, 8 bytes
    list.Write(0x203, 0);
    list.Write(0x204, 0u | (2u << 4));
    list.Write(0x205, (12u << 16) | (2u << 28));
    list.Write(0x206, 0x1000);
    list.Write(0x207, 1u);
    list.Write(0x208, (8u << 16) | (1u << 28));
    list.Write(0x2B0 + 9, 2);
    list.Write(0x2B0 + 0xB, (3u << 0) | (1u << 4) | (0u << 8));
    for (u32 topology : {0u, 2u}) {
        list.Write(0x25E, topology << 8);
        list.Write(0x228, 6);
        list.Write(0x22A, 1);
        list.Write(0x22E, 0); // arrays
    }
    for (u32 format : {0u, 1u}) {
        list.Write(0x227, (format << 31) | 0x80000);
        list.Write(0x228, 5);
        list.Write(0x22F, 0); // indexed
    }
    const std::vector<u8> bytes = list.Bytes();

    CaptureFile capture;
    capture.Init();
    capture.Mem(kFcram + 0x1000, bytes);
    capture.Mem(kBase, Random(12 * 20, 31));
    capture.Mem(kBase + 0x1000, Random(8 * 20, 32));
    capture.Mem(kBase + 0x80000, Indices16({0, 1, 2, 3, 4, 5, 6}));
    capture.SubmitCmdList(0x14001000, static_cast<u32>(bytes.size()));
    capture.NoRegisterWrites();
    capture.Frame(1);
    return capture.Save(name);
}

} // namespace

namespace {

std::vector<std::string> CapturePathsFromEnvironment(const char* value) {
#if defined(_WIN32)
    const char separator = ';';
#else
    const char separator = ':';
#endif
    std::vector<std::string> paths;
    std::string text = value;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find(separator, at);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string entry = text.substr(at, end - at);
        at = end + 1;
        if (entry.empty()) {
            continue;
        }
        std::error_code ec;
        if (fs::is_directory(entry, ec)) {
            std::vector<std::string> found;
            for (const auto& item : fs::recursive_directory_iterator(entry, ec)) {
                if (item.is_regular_file() && item.path().extension() == ".mgpu") {
                    found.push_back(item.path().string());
                }
            }
            std::sort(found.begin(), found.end());
            paths.insert(paths.end(), found.begin(), found.end());
        } else {
            paths.push_back(entry);
        }
    }
    return paths;
}


struct DumpTotals {
    Stats stats;
    u64 draws_seen = 0;
    size_t captures = 0;
};

// Replays every capture the list names with every device profile, and prints counts (never
// vertex data) when `print` is set.
DumpTotals DumpCheck(const char* value, bool print) {
    const std::vector<std::string> paths = CapturePathsFromEnvironment(value);
    REQUIRE_MESSAGE(!paths.empty(), "the capture list names no capture");
    DumpTotals totals;
    totals.captures = paths.size();
    for (const std::string& path : paths) {
        for (const auto& [profile, caps] : Profiles()) {
            g_replay = ReplayState{};
            g_replay.caps = caps;
            const gpu_capture::PlayerResult result = Replay(path);
            CHECK_MESSAGE(result.error.empty(), "a capture could not be replayed: " << result.error);
            const Stats& one = g_replay.stats;
            Stats& total = totals.stats;
            totals.draws_seen += g_replay.draws_seen;
            total.draws_checked += one.draws_checked;
            total.vertices += one.vertices;
            total.registers += one.registers;
            total.mismatches += one.mismatches;
            total.index_mismatches += one.index_mismatches;
            for (const auto& [reason, n] : one.skipped) {
                total.skipped[reason] += n;
            }
        }
    }
    if (print) {
        const Stats& t = totals.stats;
        std::printf("vertex dump check: %zu captures x %zu device profiles; draws seen %llu, draws checked %u, "
                    "vertices compared %llu, registers compared %llu, mismatches %llu, index mismatches %llu\n",
                    paths.size(), Profiles().size(), static_cast<unsigned long long>(totals.draws_seen),
                    t.draws_checked, static_cast<unsigned long long>(t.vertices),
                    static_cast<unsigned long long>(t.registers),
                    static_cast<unsigned long long>(t.mismatches),
                    static_cast<unsigned long long>(t.index_mismatches));
        for (const auto& [reason, n] : t.skipped) {
            std::printf("vertex dump check: skipped %u draws: %s\n", n, reason.c_str());
        }
        std::fflush(stdout);
    }
    return totals;
}

} // namespace

TEST_CASE("the replay path: a synthetic capture goes through the processor and the loader") {
    const std::string path = SyntheticCapture("replay");
    for (const auto& [profile, caps] : Profiles()) {
        CAPTURE(profile);
        g_replay = ReplayState{};
        g_replay.caps = caps;
        const gpu_capture::PlayerResult result = Replay(path);
        CHECK_MESSAGE(result.ExitStatus() == 0, gpu_capture::FormatReport(result));
        CHECK(g_replay.draws_seen == 4);
        CHECK(g_replay.stats.draws_checked == 4);
        CHECK(g_replay.stats.vertices == 6 + 6 + 5 + 5);
        CHECK(g_replay.stats.registers == 3 * (6 + 6 + 5 + 5));
        CHECK(g_replay.stats.mismatches == 0);
        CHECK(g_replay.stats.index_mismatches == 0);
        CHECK(g_replay.stats.skipped.empty());
    }
    fs::remove(path);
}

TEST_CASE("the capture list of the dump check: a directory, a file, and the counts") {
    // The same code the environment variable drives: a directory holding the synthetic capture is
    // searched for .mgpu files, and every draw of it is checked under every device profile.
    const fs::path dir = fs::temp_directory_path() / ("mh4n_gpu_vertex_test_dir_" + std::to_string(MH4N_GETPID()));
    fs::create_directories(dir / "sub");
    const std::string made = SyntheticCapture("listed");
    fs::rename(made, dir / "sub" / "one.mgpu");
    std::ofstream(dir / "readme.txt") << "not a capture";
    DumpTotals totals = DumpCheck(dir.string().c_str(), false);
    CHECK(totals.captures == 1);
    CHECK(totals.draws_seen == 4 * Profiles().size());
    CHECK(totals.stats.draws_checked == 4 * Profiles().size());
    CHECK(totals.stats.mismatches == 0);
    CHECK(totals.stats.index_mismatches == 0);
    const std::string both = dir.string() + std::string(1,
#if defined(_WIN32)
                                                        ';'
#else
                                                        ':'
#endif
                                                        ) + (dir / "sub" / "one.mgpu").string();
    totals = DumpCheck(both.c_str(), false);
    CHECK(totals.captures == 2);
    CHECK(totals.stats.draws_checked == 8 * Profiles().size());
    fs::remove_all(dir);
}

// ================================================================================================
// The dump check
// ================================================================================================

TEST_CASE("(with the dump) the first 64 vertices of every captured draw equal the reference's") {
    const char* value = std::getenv("MH4N_VERTEX_CAPTURES");
    if (value == nullptr || *value == '\0') {
        WARN_MESSAGE(false, "skipped: set MH4N_VERTEX_CAPTURES to .mgpu capture files or directories "
                            "(separated by the platform's path-list separator) to run the dump check");
        return;
    }
    const DumpTotals totals = DumpCheck(value, true);
    CHECK(totals.stats.draws_checked > 0);
    CHECK(totals.stats.mismatches == 0);
    CHECK(totals.stats.index_mismatches == 0);
}
