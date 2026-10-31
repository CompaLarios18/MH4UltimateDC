// Derived from Azahar src/video_core/pica/primitive_assembly.cpp and primitive_assembly.h,
// pica_core.cpp, shader_unit.cpp, rasterizer_accelerated.cpp and rasterizer_accelerated.h,
// common/vector_math.h, renderer_vulkan/vk_rasterizer.cpp, shader/generator/glsl_shader_gen.cpp and
// glsl_fs_shader_gen.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later:
// vk_rasterizer.cpp and glsl_fs_shader_gen.cpp say "Licensed under GPLv3 or any later version"; the
// others say "Licensed under GPLv2 or any later version"): the expected triangles of each topology are
// worked out by hand from PrimitiveAssembler::SubmitVertex, the fix-up cases from AddTriangle and
// AreQuaternionsOpposite, the vertex layout from MakeSoftwareVertexLayout and the trivial stage's
// interface from the generated trivial vertex shader and DefineInterface.
// Copyright 2014-2024 Citra Emulator Project / Azahar Emulator Project (primitive_assembly.h)
// Copyright 2014 Citra Emulator Project (primitive_assembly.cpp)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_core.cpp, shader_unit.cpp,
//   rasterizer_accelerated.cpp, rasterizer_accelerated.h, vk_rasterizer.cpp, glsl_shader_gen.cpp,
//   glsl_fs_shader_gen.cpp)
// Copyright 2015-2025 Citra Emulator Project / Azahar Emulator Project (vector_math.h)
// vector_math.h also carries this notice:
//   Copyright 2014 Tony Wasserka
//   All rights reserved.
//
//   Redistribution and use in source and binary forms, with or without
//   modification, are permitted provided that the following conditions are met:
//
//       * Redistributions of source code must retain the above copyright
//         notice, this list of conditions and the following disclaimer.
//       * Redistributions in binary form must reproduce the above copyright
//         notice, this list of conditions and the following disclaimer in the
//         documentation and/or other materials provided with the distribution.
//       * Neither the name of the owner nor the names of its contributors may
//         be used to endorse or promote products derived from this software
//         without specific prior written permission.
//
//   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
//   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
//   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
//   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
//   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
//   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
//   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
//   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
//   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
//   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
//   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// mh4n_gpu_cpuvertex, N2-37: the primitive assembler, the quaternion fix-up, the triangle batch and
// the trivial vertex stage. Every expected triangle, flag and pixel is worked out by hand (the
// comments show the arithmetic), never by calling the code under test.
//
// The device cases draw into an 8x8 RGBA8 target on whatever Vulkan device the machine has (lavapipe
// in the cloud); without one they skip with a message, and fail under MH4N_REQUIRE_VULKAN=1.
#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "gpu/cpuvertex/cpu_vertex.h"
#include "gpu/cpuvertex/primitive_assembly.h"
#include "gpu/cpuvertex/triangle_batch.h"
#include "gpu/cpuvertex/trivial_vertex.h"
#include "gpu/pica/command_processor.h"
#include "gpu/pica/recording_backend.h"
#include "gpu_capture/capture_host.h"
#include "support/vulkan_device.h"

#include "gpu_cpuvertex_batch_shaders.inc"

using namespace mh4n;
using namespace mh4n::gpu;
using namespace mh4n::gpu::cpuvertex;
namespace rhi = mh4n::gpu::rhi;

namespace {

// ---- synthetic vertices ----------------------------------------------------------------------------------------

// A vertex that carries only an identity: pos.x = id.
OutputVertex Id(float id) {
    OutputVertex v{};
    v.pos[0] = id;
    v.pos[3] = 1.0f;
    return v;
}

using Tri = std::array<int, 3>;

struct Collector {
    std::vector<Tri> tris;
    void operator()(const OutputVertex& a, const OutputVertex& b, const OutputVertex& c) {
        tris.push_back({static_cast<int>(a.pos[0]), static_cast<int>(b.pos[0]), static_cast<int>(c.pos[0])});
    }
};

OutputStream Stream(int count, std::vector<u32> winding = {}) {
    OutputStream s;
    for (int i = 0; i < count; ++i) {
        s.vertices.push_back(Id(static_cast<float>(i)));
    }
    s.winding_before = std::move(winding);
    return s;
}

std::vector<Tri> Assemble(PrimitiveAssembler& a, const OutputStream& s) {
    Collector c;
    a.SubmitStream(s, c);
    return c.tris;
}

} // namespace

// ================================================================================================
// The primitive assembler
// ================================================================================================

TEST_CASE("assembler/list: every three vertices are a triangle, a remainder stays buffered") {
    PrimitiveAssembler a(Topology::List);
    CHECK(a.IsEmpty());
    // v0 v1 v2 -> (0,1,2); v3 v4 v5 -> (3,4,5); v6 and v7 wait (two buffered)
    CHECK(Assemble(a, Stream(8)) == std::vector<Tri>{{0, 1, 2}, {3, 4, 5}});
    CHECK_FALSE(a.IsEmpty());
    // the next vertex (id 0 again) completes (6,7,0)
    CHECK(Assemble(a, Stream(1)) == std::vector<Tri>{{6, 7, 0}} ); // ids restart at 0: 6 and 7 are the old ones
}

TEST_CASE("assembler/list: a multiple of three leaves it empty") {
    PrimitiveAssembler a(Topology::List);
    CHECK(Assemble(a, Stream(6)).size() == 2);
    CHECK(a.IsEmpty());
}

TEST_CASE("assembler/strip: the buffer alternates slots, as Azahar's code orders it") {
    // Trace of SubmitVertex (strip_ready false, index 0): v0 -> slot 0, index 1; v1 -> slot 1, strip_ready,
    // index 0; v2 -> (slot0 = v0, slot1 = v1, v2), slot 0 = v2, index 1; v3 -> (v2, v1, v3), slot 1 = v3,
    // index 0; v4 -> (v2, v3, v4), slot 0 = v4; v5 -> (v4, v3, v5).
    PrimitiveAssembler a(Topology::Strip);
    CHECK(Assemble(a, Stream(6)) == std::vector<Tri>{{0, 1, 2}, {2, 1, 3}, {2, 3, 4}, {4, 3, 5}});
    CHECK_FALSE(a.IsEmpty()); // a strip never ends empty once a vertex was submitted
}

TEST_CASE("assembler/strip: one or two vertices make no triangle and leave it non-empty") {
    PrimitiveAssembler a(Topology::Strip);
    CHECK(Assemble(a, Stream(1)).empty());
    CHECK_FALSE(a.IsEmpty());
    PrimitiveAssembler b(Topology::Strip);
    CHECK(Assemble(b, Stream(2)).empty());
    CHECK_FALSE(b.IsEmpty());
}

TEST_CASE("assembler/fan: slot 0 keeps the first vertex") {
    // v0 -> slot 0, index 1 (a fan always writes slot 1 from then on); v1 -> slot 1, strip_ready; v2 ->
    // (v0, v1, v2), slot 1 = v2; v3 -> (v0, v2, v3); v4 -> (v0, v3, v4).
    PrimitiveAssembler a(Topology::Fan);
    CHECK(Assemble(a, Stream(5)) == std::vector<Tri>{{0, 1, 2}, {0, 2, 3}, {0, 3, 4}});
    CHECK_FALSE(a.IsEmpty());
}

TEST_CASE("assembler/shader topology: as a list, the winding flag swaps the first two vertices of the next triangle") {
    // A B C, then a winding event, then D B C (the quad program of a geometry shader that emits two
    // triangles per point): ids A = 0, B = 1, C = 2, D = 3.
    {
        PrimitiveAssembler a(Topology::Shader);
        OutputStream s = Stream(6);
        s.vertices[3] = Id(3);
        s.vertices[4] = Id(1);
        s.vertices[5] = Id(2);
        s.winding_before = {3};
        // (0,1,2); winding: the third vertex of the next triangle gives (buffer[1], buffer[0], v) = (1, 3, 2)
        CHECK(Assemble(a, s) == std::vector<Tri>{{0, 1, 2}, {1, 3, 2}});
        CHECK(a.IsEmpty());
    }
    {
        // without the flag: (0,1,2), (3,1,2)
        PrimitiveAssembler a(Topology::Shader);
        OutputStream s = Stream(6);
        s.vertices[3] = Id(3);
        s.vertices[4] = Id(1);
        s.vertices[5] = Id(2);
        CHECK(Assemble(a, s) == std::vector<Tri>{{0, 1, 2}, {3, 1, 2}});
    }
}

TEST_CASE("assembler/shader topology: the flag is cleared by the triangle it flips") {
    PrimitiveAssembler a(Topology::Shader);
    // winding before vertex 0: the first triangle is flipped, the second is not
    CHECK(Assemble(a, Stream(6, {0})) == std::vector<Tri>{{1, 0, 2}, {3, 4, 5}});
}

TEST_CASE("assembler/a duplicate winding position does not hold back the later events") {
    PrimitiveAssembler a(Topology::Shader);
    CHECK(Assemble(a, Stream(6, {0, 0, 3})) == std::vector<Tri>{{1, 0, 2}, {4, 3, 5}});
}

TEST_CASE("assembler/shader topology: a winding event before a later vertex of the triangle applies at its third vertex") {
    // SetWinding before vertex 1 (the second of triangle 0): the flag is only read when the third arrives.
    PrimitiveAssembler a(Topology::Shader);
    CHECK(Assemble(a, Stream(3, {1})) == std::vector<Tri>{{1, 0, 2}});
}

TEST_CASE("assembler/list, strip and fan ignore the winding flag, and it stays set (Azahar) until Reset") {
    PrimitiveAssembler list(Topology::List);
    CHECK(Assemble(list, Stream(3, {0})) == std::vector<Tri>{{0, 1, 2}});
    // the flag is still set: switching to Shader without a Reset would flip; Reconfigure clears it
    list.Reconfigure(Topology::Shader);
    CHECK(Assemble(list, Stream(3)) == std::vector<Tri>{{0, 1, 2}});

    PrimitiveAssembler strip(Topology::Strip);
    CHECK(Assemble(strip, Stream(3, {0})) == std::vector<Tri>{{0, 1, 2}});
    PrimitiveAssembler fan(Topology::Fan);
    CHECK(Assemble(fan, Stream(4, {2})) == std::vector<Tri>{{0, 1, 2}, {0, 2, 3}});
}

TEST_CASE("assembler/Reset clears the winding flag") {
    PrimitiveAssembler a(Topology::Shader);
    a.SetWinding();
    a.Reset(); // clears the flag
    CHECK(Assemble(a, Stream(3)) == std::vector<Tri>{{0, 1, 2}});
}

TEST_CASE("assembler/reset and reconfigure empty the buffer") {
    PrimitiveAssembler a(Topology::List);
    Assemble(a, Stream(2));
    CHECK_FALSE(a.IsEmpty());
    a.Reset();
    CHECK(a.IsEmpty());
    CHECK(a.GetTopology() == Topology::List);
    // after the reset two vertices are needed again before a third completes a triangle
    CHECK(Assemble(a, Stream(3)) == std::vector<Tri>{{0, 1, 2}});

    Assemble(a, Stream(1));
    a.Reconfigure(Topology::Fan);
    CHECK(a.IsEmpty());
    CHECK(a.GetTopology() == Topology::Fan);
}

TEST_CASE("assembler/SubmitVertex hands the handler the three vertices of a List triangle") {
    PrimitiveAssembler a(Topology::List);
    Collector c;
    a.SubmitVertex(Id(5), c);
    a.SubmitVertex(Id(6), c);
    a.SubmitVertex(Id(7), c);
    CHECK(c.tris == std::vector<Tri>{{5, 6, 7}});
}

// ---- the register side effects --------------------------------------------------------------------------------

namespace {
constexpr u32 kTopologyReg = pica::reg::kTriangleTopology;
constexpr u32 kRestartReg = pica::reg::kRestartPrimitive;

pica::DrawCall DrawOf(const pica::PicaState& state, u32 vertices) {
    pica::DrawCall d;
    d.kind = pica::DrawKind::Arrays;
    d.num_vertices = vertices;
    d.state = &state;
    return d;
}
} // namespace

TEST_CASE("assembler/a fresh state configures it from the topology register at the first draw") {
    pica::PicaState state;                       // everything dirty
    state.regs[kTopologyReg] = 2u << 8;          // Fan
    PrimitiveAssembler a;                        // List
    a.ApplyRegisterWrites(state);
    CHECK(a.GetTopology() == Topology::Fan);
    CHECK(a.IsEmpty());
}

TEST_CASE("assembler/only bits 8-9 of the topology register count") {
    pica::PicaState state;
    state.regs[kTopologyReg] = 0xFFFFFCFFu | (1u << 8); // every other bit set, bits 8-9 = 1 (Strip)
    PrimitiveAssembler a;
    a.ApplyRegisterWrites(state);
    CHECK(a.GetTopology() == Topology::Strip);
    state.regs[kTopologyReg] = 0x00000300u;             // bits 8-9 = 3
    a.ApplyRegisterWrites(state);
    CHECK(a.GetTopology() == Topology::Shader);
}

TEST_CASE("assembler/a strip draw that leaves a vertex buffered keeps IsEmpty false until a restart or topology write") {
    pica::PicaState state;
    state.regs[kTopologyReg] = 1u << 8; // Strip
    PrimitiveAssembler a;
    a.ApplyRegisterWrites(state);
    state.ClearDirty();

    Assemble(a, Stream(4)); // the CPU path's draw leaves the strip buffered
    REQUIRE_FALSE(a.IsEmpty());

    // next draw, nothing written: still not empty, still a strip
    a.ApplyRegisterWrites(state);
    CHECK_FALSE(a.IsEmpty());
    CHECK(a.GetTopology() == Topology::Strip);

    // a write to the restart register (any value): empty, topology kept
    state.dirty.Set(kRestartReg);
    a.ApplyRegisterWrites(state);
    CHECK(a.IsEmpty());
    CHECK(a.GetTopology() == Topology::Strip);
    state.ClearDirty();

    // and a write to the topology register (even the same value) does too
    Assemble(a, Stream(4));
    REQUIRE_FALSE(a.IsEmpty());
    state.dirty.Set(kTopologyReg);
    a.ApplyRegisterWrites(state);
    CHECK(a.IsEmpty());
    CHECK(a.GetTopology() == Topology::Strip);
}

TEST_CASE("assembler/both writes between two draws: the order does not matter, the topology is the register's value") {
    pica::PicaState state;
    PrimitiveAssembler a;
    a.ApplyRegisterWrites(state);
    state.ClearDirty();
    Assemble(a, Stream(2)); // List, two buffered
    state.regs[kTopologyReg] = 2u << 8;
    state.dirty.Set(kTopologyReg);
    state.dirty.Set(kRestartReg);
    a.ApplyRegisterWrites(state);
    CHECK(a.IsEmpty());
    CHECK(a.GetTopology() == Topology::Fan);
}

TEST_CASE("assembler/NeedsCpuPath for a List draw of a multiple of three is true only while the assembler is not empty") {
    pica::PicaState state; // topology List (register 0), use_gs No
    PrimitiveAssembler a;
    a.ApplyRegisterWrites(state);
    state.ClearDirty();
    const pica::DrawCall draw = DrawOf(state, 6);

    CHECK_FALSE(NeedsCpuPath(draw, a.IsEmpty()));
    CHECK(CpuPathReason(draw, a.IsEmpty()) == CpuReason::None);

    // a List draw whose count is not a multiple of 3 is on the CPU path and leaves a vertex buffered
    const pica::DrawCall odd = DrawOf(state, 4);
    CHECK(NeedsCpuPath(odd, a.IsEmpty()));
    Assemble(a, Stream(4));
    REQUIRE_FALSE(a.IsEmpty());
    a.ApplyRegisterWrites(state); // nothing written
    CHECK(NeedsCpuPath(draw, a.IsEmpty()));
    CHECK(CpuPathReason(draw, a.IsEmpty()) == CpuReason::Refused);

    // the restart register's write empties it, and the same draw is accelerated again
    state.dirty.Set(kRestartReg);
    a.ApplyRegisterWrites(state);
    CHECK_FALSE(NeedsCpuPath(draw, a.IsEmpty()));
}

namespace {
constexpr u32 kFcram = 0x20000000;

// A command list as the command processor reads it: [value, header] pairs.
struct Cmds {
    std::vector<u32> words;
    Cmds& Write(u32 id, u32 value) {
        words.push_back(value);
        words.push_back(id | (0xFu << 16));
        return *this;
    }
    Cmds& Draw(u32 count) { return Write(0x228, count).Write(0x22A, 0).Write(0x22E, 0); }
};
} // namespace

TEST_CASE("assembler/through the command processor: the dirty bits of real register writes drive it") {
    gpu_capture::CaptureHost host;
    pica::RecordingBackend backend;
    pica::CommandProcessor proc(host, backend);
    const auto submit = [&](const Cmds& c) {
        std::vector<u8> bytes(c.words.size() * 4);
        std::memcpy(bytes.data(), c.words.data(), bytes.size());
        REQUIRE(host.WriteBoth(kFcram + 0x1000, bytes.data(), static_cast<u32>(bytes.size())));
        proc.ProcessCmdList(kFcram + 0x1000, static_cast<u32>(bytes.size()));
    };
    // draw 0: topology write (Strip) + draw. draw 1: draw only. draw 2: restart write + draw.
    // draw 3: draw only. draw 4: a write of the topology register with the value it has + draw.
    submit(Cmds().Write(kTopologyReg, 1u << 8).Draw(3));
    submit(Cmds().Draw(3));
    submit(Cmds().Write(kRestartReg, 1).Draw(3));
    submit(Cmds().Draw(3));
    submit(Cmds().Write(kTopologyReg, 1u << 8).Draw(3));
    REQUIRE(backend.draws.size() == 5);

    PrimitiveAssembler a;
    std::vector<bool> empty_at_draw;
    std::vector<Topology> topology_at_draw;
    for (const pica::RecordedDraw& d : backend.draws) {
        a.ApplyRegisterWrites(*d.call.state);
        empty_at_draw.push_back(a.IsEmpty());
        topology_at_draw.push_back(a.GetTopology());
        Assemble(a, Stream(3)); // every draw feeds three vertices (a strip keeps one buffered)
    }
    // draw 0: reconfigured to Strip (empty). draw 1: nothing written, the strip is still buffered.
    // draw 2: restart written, empty. draw 3: buffered again. draw 4: topology rewritten, empty.
    CHECK(empty_at_draw == std::vector<bool>{true, false, true, false, true});
    CHECK(topology_at_draw == std::vector<Topology>(5, Topology::Strip));
}

// ================================================================================================
// The fix-up
// ================================================================================================

namespace {

OutputVertex Q(float x, float y, float z, float w) {
    OutputVertex v{};
    v.quat[0] = x;
    v.quat[1] = y;
    v.quat[2] = z;
    v.quat[3] = w;
    v.pos[3] = 1.0f;
    return v;
}

bool Same(const float (&a)[4], float x, float y, float z, float w) {
    return std::bit_cast<u32>(a[0]) == std::bit_cast<u32>(x) && std::bit_cast<u32>(a[1]) == std::bit_cast<u32>(y) &&
           std::bit_cast<u32>(a[2]) == std::bit_cast<u32>(z) && std::bit_cast<u32>(a[3]) == std::bit_cast<u32>(w);
}

} // namespace

TEST_CASE("fixup/a triangle with an opposite quaternion: vertices 1 and 2 are flagged by their dot product with vertex 0") {
    // q0 = (1, 0, 0, 0). q1 = (-1, 0, 0, 0): dot = -1 < 0, flagged. q2 = (0.5, -0.5, 0.5, 0.5): dot = 0.5 >= 0, not.
    TriangleBatch batch;
    batch.AddTriangle(Q(1, 0, 0, 0), Q(-1, 0, 0, 0), Q(0.5f, -0.5f, 0.5f, 0.5f));
    REQUIRE(batch.VertexCount() == 3);
    REQUIRE(batch.TriangleCount() == 1);
    const auto& v = batch.Vertices();
    CHECK(Same(v[0].normquat, 1, 0, 0, 0));                 // vertex 0 is never flagged
    CHECK(Same(v[1].normquat, 1, -0.0f, -0.0f, -0.0f));     // negated lane by lane: -(-1) = 1, -0 = -0
    CHECK(Same(v[2].normquat, 0.5f, -0.5f, 0.5f, 0.5f));    // dot > 0: unchanged
}

TEST_CASE("fixup/a triangle without an opposite quaternion is left alone") {
    TriangleBatch batch;
    batch.AddTriangle(Q(0.2f, 0.4f, 0.6f, 0.8f), Q(0.2f, 0.4f, 0.6f, 0.8f), Q(0.8f, 0.6f, 0.4f, 0.2f));
    const auto& v = batch.Vertices();
    CHECK(Same(v[0].normquat, 0.2f, 0.4f, 0.6f, 0.8f));
    CHECK(Same(v[1].normquat, 0.2f, 0.4f, 0.6f, 0.8f));
    CHECK(Same(v[2].normquat, 0.8f, 0.6f, 0.4f, 0.2f));
}

TEST_CASE("fixup/vertex 2 is compared with vertex 0, not with vertex 1") {
    // q0 = (1,0,0,0), q1 = (-1,0,0,0) flagged, q2 = (-1,0,0,0): dot with q0 = -1, flagged too (a comparison with
    // the original q1 would also say "same side"; with q1 after the flip it would say "opposite" and not flag).
    TriangleBatch batch;
    batch.AddTriangle(Q(1, 0, 0, 0), Q(-1, 0, 0, 0), Q(-1, 0, 0, 0));
    const auto& v = batch.Vertices();
    CHECK(Same(v[1].normquat, 1, -0.0f, -0.0f, -0.0f));
    CHECK(Same(v[2].normquat, 1, -0.0f, -0.0f, -0.0f));
}

TEST_CASE("fixup/a zero dot product is not flagged, whatever the signs of the zeros") {
    const float a[4] = {1, 0, 0, 0};
    const float b[4] = {0, 1, 0, 0};       // 0 + 0 + 0 + 0
    const float c[4] = {0, -1, 0, 0};      // 1*0 + 0*(-1) = 0 + (-0) = +0
    const float d[4] = {-0.0f, -0.0f, -0.0f, -0.0f}; // -0 + -0 + -0 + -0 = -0, and -0 < 0 is false
    CHECK_FALSE(AreQuaternionsOpposite(a, b));
    CHECK_FALSE(AreQuaternionsOpposite(a, c));
    CHECK_FALSE(AreQuaternionsOpposite(a, d));
}

TEST_CASE("fixup/a NaN dot product is not flagged; an infinite one follows its sign") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const float a[4] = {1, 0, 0, 0};
    const float with_nan[4] = {nan, 0, 0, 0};
    const float minus_inf[4] = {-inf, 0, 0, 0};
    const float plus_inf[4] = {inf, 0, 0, 0};
    CHECK_FALSE(AreQuaternionsOpposite(a, with_nan));
    CHECK(AreQuaternionsOpposite(a, minus_inf)); // -inf < 0
    CHECK_FALSE(AreQuaternionsOpposite(a, plus_inf));
    // a NaN lane anywhere poisons the sum: 0 * inf = NaN in the float product
    const float zero_times_inf[4] = {0, 0, 0, 0};
    const float infs[4] = {inf, inf, inf, inf};
    CHECK_FALSE(AreQuaternionsOpposite(zero_times_inf, infs));
}

TEST_CASE("fixup/hand-worked dot products") {
    // (0.5, 0.5, 0.5, 0.5) . (-0.5, 0.5, -0.5, 0.25) = -0.25 + 0.25 - 0.25 + 0.125 = -0.125 < 0
    const float a[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    const float b[4] = {-0.5f, 0.5f, -0.5f, 0.25f};
    CHECK(AreQuaternionsOpposite(a, b));
    // (1, 2, 3, 4) . (-4, -3, 2, 1) = -4 - 6 + 6 + 4 = 0: not flagged
    const float c[4] = {1, 2, 3, 4};
    const float d[4] = {-4, -3, 2, 1};
    CHECK_FALSE(AreQuaternionsOpposite(c, d));
    // (1, 2, 3, 4) . (-4, -3, 2, 0.5) = -4 - 6 + 6 + 2 = -2 < 0
    const float e[4] = {-4, -3, 2, 0.5f};
    CHECK(AreQuaternionsOpposite(c, e));
}

TEST_CASE("fixup/the products are rounded to float one by one (no fused multiply-add)") {
    // c = 1 + 2^-12. c*c = 1 + 2^-11 + 2^-24 exactly, which is a tie between the floats 1 + 2^-11 and
    // 1 + 2^-11 + 2^-23: it rounds to the even one, 1 + 2^-11. So (c, c, 0, 0) . (c, -c, 0, 0) = round(c*c) -
    // round(c*c) = 0 exactly: not flagged. A fused multiply-add would add the exact -c*c to round(c*c) and
    // give -2^-24, flagged.
    const float c = 1.0f + std::ldexp(1.0f, -12);
    const float a[4] = {c, c, 0, 0};
    const float b[4] = {c, -c, 0, 0};
    CHECK_FALSE(AreQuaternionsOpposite(a, b));
}

TEST_CASE("batch/HardwareVertex takes every field of the output vertex") {
    OutputVertex o{};
    for (int i = 0; i < 4; ++i) {
        o.pos[i] = 1.0f + i;      // 1 2 3 4
        o.quat[i] = 10.0f + i;    // 10 11 12 13
        o.color[i] = 20.0f + i;   // 20 21 22 23
    }
    o.tc0[0] = 30; o.tc0[1] = 31;
    o.tc1[0] = 32; o.tc1[1] = 33;
    o.tc2[0] = 34; o.tc2[1] = 35;
    o.tc0_w = 36;
    o.view[0] = 37; o.view[1] = 38; o.view[2] = 39;
    o.padding0 = 99; o.padding1 = 98;
    const HardwareVertex h = MakeHardwareVertex(o, false);
    CHECK(Same(h.position, 1, 2, 3, 4));
    CHECK(Same(h.color, 20, 21, 22, 23));
    CHECK(h.tex_coord0[0] == 30); CHECK(h.tex_coord0[1] == 31);
    CHECK(h.tex_coord1[0] == 32); CHECK(h.tex_coord1[1] == 33);
    CHECK(h.tex_coord2[0] == 34); CHECK(h.tex_coord2[1] == 35);
    CHECK(h.tex_coord0_w == 36);
    CHECK(Same(h.normquat, 10, 11, 12, 13));
    CHECK(h.view[0] == 37); CHECK(h.view[1] == 38); CHECK(h.view[2] == 39);
    const HardwareVertex f = MakeHardwareVertex(o, true);
    CHECK(Same(f.normquat, -10, -11, -12, -13));
    CHECK(Same(f.position, 1, 2, 3, 4)); // only the quaternion is negated
}

TEST_CASE("batch/the vertex layout is Azahar's software layout") {
    static_assert(sizeof(HardwareVertex) == 88);
    const rhi::VertexLayout layout = MakeBatchVertexLayout();
    REQUIRE(layout.bindings.size() == 1);
    CHECK(layout.bindings[0].binding == 0);
    CHECK(layout.bindings[0].stride == 88);
    CHECK_FALSE(layout.bindings[0].per_instance);
    // sizes {4, 4, 2, 2, 2, 1, 4, 3} floats, offsets are their running sum
    const u32 offsets[8] = {0, 16, 32, 40, 48, 56, 60, 76};
    const rhi::Format formats[8] = {rhi::Format::Rgba32Float, rhi::Format::Rgba32Float, rhi::Format::Rg32Float,
                                    rhi::Format::Rg32Float,   rhi::Format::Rg32Float,   rhi::Format::R32Float,
                                    rhi::Format::Rgba32Float, rhi::Format::Rgb32Float};
    REQUIRE(layout.attributes.size() == 8);
    for (u32 i = 0; i < 8; ++i) {
        CHECK(layout.attributes[i].location == i);
        CHECK(layout.attributes[i].binding == 0);
        CHECK(layout.attributes[i].offset == offsets[i]);
        CHECK(layout.attributes[i].format == formats[i]);
    }
}

TEST_CASE("batch/Clear keeps the storage: a second draw of the same size allocates nothing") {
    TriangleBatch batch;
    for (int i = 0; i < 40; ++i) {
        batch.AddTriangle(Q(1, 0, 0, 0), Q(1, 0, 0, 0), Q(1, 0, 0, 0));
    }
    const HardwareVertex* first = batch.Data();
    batch.Clear();
    CHECK(batch.Empty());
    for (int i = 0; i < 40; ++i) {
        batch.AddTriangle(Q(1, 0, 0, 0), Q(1, 0, 0, 0), Q(1, 0, 0, 0));
    }
    CHECK(batch.Data() == first);
    CHECK(batch.TriangleCount() == 40);
}

TEST_CASE("batch/the trivial vertex stage is SPIR-V in two variants") {
    const std::span<const u32> clip = TrivialVertexShader(true);
    const std::span<const u32> noclip = TrivialVertexShader(false);
    REQUIRE(clip.size() > 5);
    REQUIRE(noclip.size() > 5);
    CHECK(clip[0] == 0x07230203u);
    CHECK(noclip[0] == 0x07230203u);
    CHECK(clip.size() != noclip.size()); // the clip variant writes gl_ClipDistance
}

// ================================================================================================
// The batch drawn through the trivial vertex stage (a device)
// ================================================================================================

namespace {

using rhi::Backend;
using rhi::BindingSet;
using rhi::BindingSetDesc;
using rhi::BindingType;
using rhi::Buffer;
using rhi::BufferDesc;
using rhi::BufferImageRegion;
using rhi::BufferUsage;
using rhi::Format;
using rhi::GraphicsPipelineDesc;
using rhi::Heap;
using rhi::Image;
using rhi::ImageDesc;
using rhi::LoadOp;
using rhi::Pipeline;
using rhi::Recorder;
using rhi::Rect;
using rhi::Result;
using rhi::Ring;
using rhi::ShaderCode;
using rhi::TargetPassDesc;
using rhi::TimelineValue;
using rhi::VertexBufferBinding;

constexpr u32 kSize = 8;

template <size_t N>
ShaderCode Spv(const uint32_t (&words)[N]) {
    ShaderCode code;
    code.spirv = std::span<const u32>(words, N);
    return code;
}

std::shared_ptr<Buffer> MakeBuffer(Backend& backend, u64 size, Heap heap, BufferUsage usage, const char* name) {
    BufferDesc desc;
    desc.size = size;
    desc.heap = heap;
    desc.usage = usage;
    desc.name = name;
    Result<std::shared_ptr<Buffer>> buffer = backend.CreateBuffer(desc);
    REQUIRE_MESSAGE(buffer.ok(), buffer.error());
    return *buffer;
}

// Everything a draw needs, alive for the whole case (freed buffers whose memory is reused are a
// SYNC-HAZARD under the validation layer: nothing here is released before the case ends).
struct Rig {
    std::shared_ptr<Backend> backend;
    std::unique_ptr<Ring> ring;
    std::shared_ptr<Buffer> pica_uniforms;   // binding 0 (the translated stage's; the trivial stage ignores it)
    std::shared_ptr<Buffer> vertex_uniforms; // binding 1
    std::shared_ptr<BindingSet> set;
    std::shared_ptr<Image> target;
    std::vector<std::shared_ptr<Buffer>> readbacks;
    std::vector<std::shared_ptr<Pipeline>> pipelines;
    std::vector<u64> offsets; // the ring offset of every batch drawn

    static BindingSetDesc SetDesc() {
        BindingSetDesc desc;
        desc.slots = {{0, BindingType::UniformBuffer}, {1, BindingType::UniformBuffer}};
        return desc;
    }

    bool Init(u64 ring_size = 64 * 1024) {
        backend = test::RequireBackend();
        if (!backend) {
            return false;
        }
        Result<std::unique_ptr<Ring>> r = backend->CreateRing(ring_size, BufferUsage::Vertex, "batch ring");
        REQUIRE_MESSAGE(r.ok(), r.error());
        ring = std::move(*r);
        pica_uniforms = MakeBuffer(*backend, 2048, Heap::Upload, BufferUsage::Uniform, "pica uniforms");
        vertex_uniforms = MakeBuffer(*backend, 256, Heap::Upload, BufferUsage::Uniform, "vertex uniforms");
        std::memset(pica_uniforms->Mapped().data(), 0, 2048);
        pica_uniforms->Flush(0, 2048);
        SetVertexUniforms(false, {0, 0, 0, 0});
        Result<std::shared_ptr<BindingSet>> s = backend->CreateBindingSet(SetDesc());
        REQUIRE_MESSAGE(s.ok(), s.error());
        set = *s;
        REQUIRE(set->SetUniformBuffer(0, *pica_uniforms, 0, 1616).ok());
        REQUIRE(set->SetUniformBuffer(1, *vertex_uniforms, 0, 32).ok());
        ImageDesc image;
        image.format = Format::Rgba8Unorm;
        image.width = kSize;
        image.height = kSize;
        image.name = "batch target";
        Result<std::shared_ptr<Image>> t = backend->CreateImage(image);
        REQUIRE_MESSAGE(t.ok(), t.error());
        target = *t;
        return true;
    }

    // VertexUniformData: enable_clip1, flip_viewport, 2 words of padding, clip_coef.
    void SetVertexUniforms(bool enable_clip1, std::array<float, 4> clip_coef, bool flip_viewport = false) {
        u32 words[8] = {enable_clip1 ? 1u : 0u, flip_viewport ? 1u : 0u, 0, 0, 0, 0, 0, 0};
        std::memcpy(words + 4, clip_coef.data(), sizeof(clip_coef));
        std::memcpy(vertex_uniforms->Mapped().data(), words, sizeof(words));
        vertex_uniforms->Flush(0, sizeof(words));
    }

    std::shared_ptr<Pipeline> MakePipeline(bool clip_planes, ShaderCode fragment) {
        GraphicsPipelineDesc desc;
        desc.vertex.spirv = TrivialVertexShader(clip_planes);
        desc.fragment = fragment;
        desc.vertex_layout = MakeBatchVertexLayout();
        desc.topology = rhi::Topology::TriangleList;
        desc.color_format = Format::Rgba8Unorm;
        desc.sets = {SetDesc()};
        desc.name = "trivial vertex stage";
        Result<std::shared_ptr<Pipeline>> p = backend->CreateGraphicsPipeline(desc);
        REQUIRE_MESSAGE(p.ok(), p.error());
        pipelines.push_back(*p);
        return *p;
    }

    // Draws the batch with `pipeline` into the target (cleared to opaque black) and returns the 8x8 RGBA8 pixels.
    std::vector<u8> Draw(const Pipeline& pipeline, const TriangleBatch& batch, bool read = true) {
        Recorder rec = backend->BeginRecording();
        Result<StreamedBatch> streamed = UploadBatch(*ring, batch);
        REQUIRE_MESSAGE(streamed.ok(), streamed.error());
        offsets.push_back(streamed->offset);
        TargetPassDesc pass;
        pass.color = target.get();
        pass.color_load = LoadOp::Clear;
        pass.clear_color = {0, 0, 0, 1};
        pass.area = Rect{0, 0, kSize, kSize};
        rec.BeginTargetPass(pass);
        rec.SetPipeline(pipeline);
        rec.Bind(*set, 0);
        const VertexBufferBinding binding{ring->GetBuffer().get(), streamed->offset};
        rec.SetVertexBuffers(0, std::span<const VertexBufferBinding>(&binding, 1));
        rec.Draw(streamed->vertex_count);
        backend->Submit(std::move(rec));
        return read ? ReadBack() : std::vector<u8>{};
    }

    std::vector<u8> ReadBack() {
        const u64 size = u64(kSize) * kSize * 4;
        readbacks.push_back(MakeBuffer(*backend, size, Heap::Readback, BufferUsage::Transfer, "readback"));
        Buffer& buffer = *readbacks.back();
        Recorder rec = backend->BeginRecording();
        BufferImageRegion region;
        region.image_rect = Rect{0, 0, kSize, kSize};
        rec.CopyImageToBuffer(*target, buffer, std::span<const BufferImageRegion>(&region, 1));
        const TimelineValue value = backend->Submit(std::move(rec));
        std::vector<u8> bytes(size);
        Result<void> read = backend->ReadBuffer(buffer, 0, bytes, value);
        REQUIRE_MESSAGE(read.ok(), read.error());
        return bytes;
    }
};

std::array<u8, 4> Rgba(int r, int g, int b, int a) {
    return {static_cast<u8>(r), static_cast<u8>(g), static_cast<u8>(b), static_cast<u8>(a)};
}

std::array<u8, 4> Px(const std::vector<u8>& pixels, u32 x, u32 y) {
    const size_t at = (size_t(y) * kSize + x) * 4;
    return {pixels[at], pixels[at + 1], pixels[at + 2], pixels[at + 3]};
}

// An output vertex at the NDC position (x, y), z = -0.5 (the PICA's z is in [-w, 0]; the trivial stage
// negates it), w = 1, with the given attributes.
OutputVertex At(float x, float y) {
    OutputVertex v{};
    v.pos[0] = x;
    v.pos[1] = y;
    v.pos[2] = -0.5f;
    v.pos[3] = 1.0f;
    return v;
}

// The attributes of the seven-varying test: all vertices alike, so the interpolated value is the value.
// 0.2, 0.4, 0.6, 0.8 convert to 51, 102, 153, 204 in an RGBA8 UNORM target (0.2 * 255 = 51, ...).
OutputVertex Attributes(OutputVertex v) {
    v.color[0] = 0.2f; v.color[1] = 0.4f; v.color[2] = 0.6f; v.color[3] = 0.8f;
    v.tc0[0] = 0.4f;   v.tc0[1] = 0.8f;
    v.tc1[0] = 0.6f;   v.tc1[1] = 0.2f;
    v.tc2[0] = 0.8f;   v.tc2[1] = 0.6f;
    v.tc0_w = 0.6f;
    v.quat[0] = 0.8f;  v.quat[1] = 0.6f; v.quat[2] = 0.4f; v.quat[3] = 0.2f;
    v.view[0] = 0.2f;  v.view[1] = 0.8f; v.view[2] = 0.4f;
    return v;
}

// The triangle every case draws: pixel coordinates (0, 0), (4.5, 0), (0, 4.5) of the 8x8 target, which are
// NDC (-1, -1), (0.125, -1), (-1, 0.125) (x_ndc = -1 + px / 4, y_ndc = -1 + py / 4, y down). The pixel
// centre (i + 0.5, j + 0.5) is inside iff (i + 0.5) + (j + 0.5) < 4.5, i.e. i + j <= 3; no centre is on an
// edge (the hypotenuse is x + y = 4.5).
TriangleBatch CornerTriangle(const OutputVertex& a, const OutputVertex& b, const OutputVertex& c) {
    TriangleBatch batch;
    batch.AddTriangle(a, b, c);
    return batch;
}

void CheckCorner(const std::vector<u8>& pixels, std::array<u8, 4> inside) {
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            const bool in = x + y <= 3;
            const std::array<u8, 4> expected = in ? inside : Rgba(0, 0, 0, 255);
            CHECK_MESSAGE(Px(pixels, x, y) == expected, "pixel " << x << "," << y);
        }
    }
}

OutputVertex Corner(int i) {
    const float x[3] = {-1.0f, 0.125f, -1.0f};
    const float y[3] = {-1.0f, -1.0f, 0.125f};
    return Attributes(At(x[i], y[i]));
}

} // namespace

TEST_CASE("draw/the seven varyings reach the fragment stage, each at its location") {
    Rig rig;
    if (!rig.Init()) return;
    const TriangleBatch batch = CornerTriangle(Corner(0), Corner(1), Corner(2));
    struct Case {
        const char* name;
        ShaderCode fragment;
        std::array<u8, 4> pixel;
    };
    const Case cases[7] = {
        {"primary_color (0.2, 0.4, 0.6, 0.8)", Spv(spv_show_primary_color), {51, 102, 153, 204}},
        {"texcoord0 (0.4, 0.8)", Spv(spv_show_texcoord0), {102, 204, 0, 255}},
        {"texcoord1 (0.6, 0.2)", Spv(spv_show_texcoord1), {153, 51, 0, 255}},
        {"texcoord2 (0.8, 0.6)", Spv(spv_show_texcoord2), {204, 153, 0, 255}},
        {"texcoord0_w 0.6", Spv(spv_show_texcoord0_w), {153, 0, 0, 255}},
        {"normquat (0.8, 0.6, 0.4, 0.2)", Spv(spv_show_normquat), {204, 153, 102, 51}},
        {"view (0.2, 0.8, 0.4)", Spv(spv_show_view), {51, 204, 102, 255}},
    };
    for (const bool clip : {false, true}) {
        for (const Case& c : cases) {
            INFO(c.name << (clip ? " with the clip-plane variant" : " without clip planes"));
            const std::shared_ptr<Pipeline> pipeline = rig.MakePipeline(clip, c.fragment);
            CheckCorner(rig.Draw(*pipeline, batch), c.pixel);
        }
    }
}

TEST_CASE("draw/the quaternion of a flagged vertex arrives negated") {
    Rig rig;
    if (!rig.Init()) return;
    const std::shared_ptr<Pipeline> pipeline = rig.MakePipeline(false, Spv(spv_show_normquat));
    // v0 = (0.2, 0.4, 0.6, 0.8); v1 = v2 = its opposite: both flagged (dot = -1.2 < 0) and negated back, so
    // the three vertices carry (0.2, 0.4, 0.6, 0.8) and every covered pixel is (51, 102, 153, 204). Without the
    // fix-up the interpolation would pass through (0.2.. -> -0.2..): the clamped pixels would differ.
    {
        OutputVertex a = Corner(0), b = Corner(1), c = Corner(2);
        a.quat[0] = 0.2f; a.quat[1] = 0.4f; a.quat[2] = 0.6f; a.quat[3] = 0.8f;
        for (OutputVertex* v : {&b, &c}) {
            v->quat[0] = -0.2f; v->quat[1] = -0.4f; v->quat[2] = -0.6f; v->quat[3] = -0.8f;
        }
        CheckCorner(rig.Draw(*pipeline, CornerTriangle(a, b, c)), {51, 102, 153, 204});
    }
    // only vertex 2 opposite: v1 = v0 is not flagged, v2 is: still uniform
    {
        OutputVertex a = Corner(0), b = Corner(1), c = Corner(2);
        a.quat[0] = 0.2f; a.quat[1] = 0.4f; a.quat[2] = 0.6f; a.quat[3] = 0.8f;
        b.quat[0] = 0.2f; b.quat[1] = 0.4f; b.quat[2] = 0.6f; b.quat[3] = 0.8f;
        c.quat[0] = -0.2f; c.quat[1] = -0.4f; c.quat[2] = -0.6f; c.quat[3] = -0.8f;
        CheckCorner(rig.Draw(*pipeline, CornerTriangle(a, b, c)), {51, 102, 153, 204});
    }
    // no vertex opposite: nothing is negated (the shown value is the vertices' own)
    {
        OutputVertex a = Corner(0), b = Corner(1), c = Corner(2);
        CheckCorner(rig.Draw(*pipeline, CornerTriangle(a, b, c)), {204, 153, 102, 51});
    }
}

TEST_CASE("draw/the clip distances of the clip-plane variant cut the triangle at the plane") {
    Rig rig;
    if (!rig.Init()) return;
    // A larger triangle: pixels (0,0), (7.5,0), (0,7.5) = NDC (-1,-1), (0.875,-1), (-1,0.875); inside iff
    // i + j + 1 < 7.5, i.e. i + j <= 6. With enable_clip1 and clip_coef = (1, 0, 0, 0) the second clip
    // distance is x_ndc, so only x_ndc >= 0 (columns 4 to 7) is kept.
    OutputVertex a = Attributes(At(-1.0f, -1.0f)), b = Attributes(At(0.875f, -1.0f)), c = Attributes(At(-1.0f, 0.875f));
    const TriangleBatch batch = CornerTriangle(a, b, c);
    const std::shared_ptr<Pipeline> clip = rig.MakePipeline(true, Spv(spv_show_primary_color));
    const std::shared_ptr<Pipeline> noclip = rig.MakePipeline(false, Spv(spv_show_primary_color));
    const std::array<u8, 4> color{51, 102, 153, 204};

    rig.SetVertexUniforms(false, {1, 0, 0, 0});
    {
        const std::vector<u8> pixels = rig.Draw(*clip, batch); // plane off: the second distance is 0
        for (u32 y = 0; y < kSize; ++y) {
            for (u32 x = 0; x < kSize; ++x) {
                CHECK_MESSAGE(Px(pixels, x, y) == (x + y <= 6 ? color : Rgba(0, 0, 0, 255)),
                              "plane off, pixel " << x << "," << y);
            }
        }
    }
    rig.SetVertexUniforms(true, {1, 0, 0, 0});
    {
        const std::vector<u8> pixels = rig.Draw(*clip, batch);
        for (u32 y = 0; y < kSize; ++y) {
            for (u32 x = 0; x < kSize; ++x) {
                const bool in = x >= 4 && x + y <= 6;
                CHECK_MESSAGE(Px(pixels, x, y) == (in ? color : Rgba(0, 0, 0, 255)),
                              "plane on, pixel " << x << "," << y);
            }
        }
    }
    {
        // the variant without clip planes ignores the uniform
        const std::vector<u8> pixels = rig.Draw(*noclip, batch);
        CHECK(Px(pixels, 0, 0) == color);
        CHECK(Px(pixels, 6, 0) == color);
        CHECK(Px(pixels, 7, 0) == Rgba(0, 0, 0, 255));
    }
    rig.SetVertexUniforms(false, {0, 0, 0, 0});
}

TEST_CASE("draw/two triangles of a batch, and the assembler's strip and fan through the whole chain") {
    Rig rig;
    if (!rig.Init()) return;
    const std::shared_ptr<Pipeline> pipeline = rig.MakePipeline(false, Spv(spv_show_primary_color));
    // Fan of the quad (-1,-1) (1,-1) (1,1) (-1,1) around vertex 0 with positions in NDC; colours differ per
    // triangle through the vertices' colour (all three vertices of a triangle alike is not possible in a fan,
    // so the colour is the same for all vertices: the whole quad is that colour, and nothing is left black).
    PrimitiveAssembler fan(Topology::Fan);
    OutputStream s;
    const float xs[4] = {-1, 1, 1, -1}, ys[4] = {-1, -1, 1, 1};
    for (int i = 0; i < 4; ++i) {
        s.vertices.push_back(Attributes(At(xs[i], ys[i])));
    }
    TriangleBatch batch;
    fan.SubmitStream(s, [&](const OutputVertex& a, const OutputVertex& b, const OutputVertex& c) { batch.AddTriangle(a, b, c); });
    REQUIRE(batch.TriangleCount() == 2);
    const std::vector<u8> pixels = rig.Draw(*pipeline, batch);
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            CHECK_MESSAGE(Px(pixels, x, y) == Rgba(51, 102, 153, 204), "pixel " << x << "," << y);
        }
    }
}

TEST_CASE("draw/a batch of no triangles makes no ring allocation and nothing to draw") {
    Rig rig;
    if (!rig.Init(4096)) return;
    TriangleBatch one = CornerTriangle(Corner(0), Corner(1), Corner(2)); // 3 vertices of 88 bytes: 264 bytes
    Recorder rec = rig.backend->BeginRecording();
    rhi::Result<StreamedBatch> first = UploadBatch(*rig.ring, one);
    REQUIRE_MESSAGE(first.ok(), first.error());
    CHECK(first->offset == 0);
    CHECK(first->vertex_count == 3);
    TriangleBatch none;
    rhi::Result<StreamedBatch> empty = UploadBatch(*rig.ring, none);
    REQUIRE_MESSAGE(empty.ok(), empty.error());
    CHECK(empty->vertex_count == 0);
    // the next allocation starts after the first (264 rounded up to the 16-byte alignment = 272): the empty
    // batch took nothing
    rhi::Result<StreamedBatch> second = UploadBatch(*rig.ring, one);
    REQUIRE_MESSAGE(second.ok(), second.error());
    CHECK(second->offset == 272);
    rig.backend->Submit(std::move(rec));
    rig.backend->WaitIdle();
}

TEST_CASE("draw/many draws over a small ring: the space is reused once its submission has passed") {
    // 1024-byte ring, 272 bytes per batch (264 at 16-byte alignment): three fit, the fourth wraps. Twelve draws,
    // each its own recorder and submission, each with another colour, every one read back.
    Rig rig;
    if (!rig.Init(1024)) return;
    const std::shared_ptr<Pipeline> pipeline = rig.MakePipeline(false, Spv(spv_show_primary_color));
    for (int i = 0; i < 12; ++i) {
        OutputVertex v[3] = {Corner(0), Corner(1), Corner(2)};
        const float level = 0.2f * static_cast<float>(i % 5 + 1); // 0.2 .. 1.0 -> 51 .. 255
        for (OutputVertex& x : v) {
            x.color[0] = level; x.color[1] = 0.0f; x.color[2] = 1.0f - level; x.color[3] = 1.0f;
        }
        const std::vector<u8> pixels = rig.Draw(*pipeline, CornerTriangle(v[0], v[1], v[2]));
        const u8 red = static_cast<u8>(51 * (i % 5 + 1));
        const u8 blue = static_cast<u8>(255 - red);
        CHECK_MESSAGE(Px(pixels, 0, 0) == Rgba(red, 0, blue, 255), "draw " << i);
        CHECK_MESSAGE(Px(pixels, 7, 7) == Rgba(0, 0, 0, 255), "draw " << i);
    }
    // The wrap, made certain. A fresh ring of 1024 bytes; one recorder takes three batches (offsets 0, 272 and
    // 544: nothing is retired while the recorder is open, and each Map aligns the head to 16), the recorder is
    // submitted without a wait, then a fourth batch is mapped in another recorder. The head is 808, aligned 816,
    // and 816 + 264 > 1024, so the allocation cannot be at the end and restarts at 0, whatever the GPU's speed:
    // if the first three spans have retired by then the ring is empty and starts at 0 (RingAllocator::TryMap);
    // if not, the end is given up, the head goes to 0 and the oldest (stamped, submitted) span is waited for,
    // which frees 0 too. It never fails: the first recorder is submitted.
    Rig wrap;
    if (!wrap.Init(1024)) return;
    const std::shared_ptr<Pipeline> wrap_pipeline = wrap.MakePipeline(false, Spv(spv_show_primary_color));
    {
        Recorder rec = wrap.backend->BeginRecording();
        for (int i = 0; i < 3; ++i) {
            rhi::Result<StreamedBatch> r = UploadBatch(*wrap.ring, CornerTriangle(Corner(0), Corner(1), Corner(2)));
            REQUIRE_MESSAGE(r.ok(), r.error());
            wrap.offsets.push_back(r->offset);
        }
        wrap.backend->Submit(std::move(rec));
    }
    OutputVertex v[3] = {Corner(0), Corner(1), Corner(2)};
    for (OutputVertex& x : v) { x.color[0] = 1.0f; x.color[1] = 0.0f; x.color[2] = 0.0f; x.color[3] = 1.0f; }
    const std::vector<u8> pixels = wrap.Draw(*wrap_pipeline, CornerTriangle(v[0], v[1], v[2]));
    REQUIRE(wrap.offsets.size() == 4);
    CHECK(wrap.offsets[0] == 0);
    CHECK(wrap.offsets[1] == 272);
    CHECK(wrap.offsets[2] == 544);
    CHECK(wrap.offsets[3] == 0);
    CHECK(wrap.offsets[3] < wrap.offsets[2]);
    CHECK(Px(pixels, 0, 0) == Rgba(255, 0, 0, 255));
    CHECK(Px(pixels, 7, 7) == Rgba(0, 0, 0, 255));
}

TEST_CASE("draw/flip_viewport mirrors the triangle vertically") {
    Rig rig;
    if (!rig.Init()) return;
    const std::shared_ptr<Pipeline> pipeline = rig.MakePipeline(false, Spv(spv_show_primary_color));
    rig.SetVertexUniforms(false, {0, 0, 0, 0}, true);
    // y is negated: the corner triangle sits at the bottom-left, pixel (x, y) is inside iff x + (7 - y) <= 3
    const std::vector<u8> pixels = rig.Draw(*pipeline, CornerTriangle(Corner(0), Corner(1), Corner(2)));
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            CHECK_MESSAGE(Px(pixels, x, y) == (x + (7 - y) <= 3 ? Rgba(51, 102, 153, 204) : Rgba(0, 0, 0, 255)),
                          "pixel " << x << "," << y);
        }
    }
    rig.SetVertexUniforms(false, {0, 0, 0, 0});
}

TEST_CASE("draw/a ring smaller than the batch fails in UploadBatch and writes nothing") {
    Rig rig;
    if (!rig.Init(256)) return; // 264 bytes do not fit
    Recorder rec = rig.backend->BeginRecording();
    rhi::Result<StreamedBatch> r = UploadBatch(*rig.ring, CornerTriangle(Corner(0), Corner(1), Corner(2)));
    CHECK_FALSE(r.ok());
    CHECK_FALSE(r.error().empty());
    rig.backend->Submit(std::move(rec));
    rig.backend->WaitIdle();
}
