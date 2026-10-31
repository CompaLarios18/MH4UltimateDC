// Derived from Azahar externals/nihstro/include/nihstro/shader_bytecode.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (the file belongs to the submodule externals/nihstro that
// Azahar pins at f4d8659decbfe5d234f04134b5002b82dc515a44; it is NOT GPL: its licence is the BSD
// 3-clause licence of externals/nihstro/license.txt, reproduced below because it asks for that),
// src/video_core/pica/pica_core.cpp, regs_pipeline.h, regs_shader.h, regs_rasterizer.h,
// shader_unit.cpp, geometry_pipeline.cpp and src/video_core/shader/shader_interpreter.cpp
// (GPL-2.0-or-later; each says "Licensed under GPLv2 or any later version") in the same commit: the
// instruction encoder below places the fields where shader_bytecode.h puts them, the register
// positions and bit fields the tests write are those of PipelineRegs, ShaderRegs and RasterizerRegs,
// and the expected values encode the quirks of shader_interpreter.cpp (the stacks, aL, the
// four-scope return) and of geometry_pipeline.cpp (the asserts, b15).
// Copyright 2014 Tony Wasserka (shader_bytecode.h)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (shader_interpreter.cpp)
// Copyright 2017-2024 Citra Emulator Project / Azahar Emulator Project (geometry_pipeline.cpp)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (pica_core.cpp, shader_unit.cpp)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_pipeline.h, regs_shader.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (regs_rasterizer.h)
//
//   Copyright 2014 Tony Wasserka
//   All rights reserved.
//
//   Redistribution and use in source and binary forms, with or without modification, are permitted
//   provided that the following conditions are met:
//       * Redistributions of source code must retain the above copyright notice, this list of
//         conditions and the following disclaimer.
//       * Redistributions in binary form must reproduce the above copyright notice, this list of
//         conditions and the following disclaimer in the documentation and/or other materials
//         provided with the distribution.
//       * Neither the name of the owner nor the names of its contributors may be used to endorse or
//         promote products derived from this software without specific prior written permission.
//   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
//   IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
//   AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
//   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
//   CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
//   SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
//   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
//   OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
//   POSSIBILITY OF SUCH DAMAGE.
//
// mh4n_gpu_cpuvertex (N2-36): the CPU shader runner.
//
// Every expected value is worked out by hand from the PICA's instruction semantics and Azahar's
// interpreter rules (the comments show the arithmetic), never by calling the code under test and
// never from the translator's test oracle (gpu_shader_test.cpp). The programs are assembled with
// the small encoder below and run through the runner on a PicaState built either field by field
// or through the command processor of N2-04 (command lists written to a capture host's memory).
//
// ---- The dump check (the last test case; "pending (dump)" until it is run on the maintainer's captures)
//
// With MH4N_CPUVERTEX_CAPTURES naming .mgpu capture files (or directories that hold them, several
// entries separated by the platform's path-list separator: ':' and ';' on Windows) the last test case
// replays each capture, in argument order, through the capture player and the PICA command
// processor, and at every draw calls NeedsCpuPath and, for the draws it selects, the runner:
//
//     MH4N_CPUVERTEX_CAPTURES=<files or dirs> ctest --test-dir build/native -R '^gpu_cpuvertex_test$' -V
//
// NeedsCpuPath gets assembler_empty = true at every draw: there is no primitive assembler yet
// (N2-37 and N2-08 pass its real IsEmpty()). So the "refused" count only holds the draws whose
// List/Shader vertex count is not a multiple of 3, and is a lower bound. The run is the full one
// (RunMode::Full). It prints counts and times only, never a value, an instruction word, a path
// or a text of the game: one line per capture, then a total line:
//
//   cpuvertex capture <n>: frames <F>, draws <D>, selected <S> (geometry shader <G>, immediate <I>,
//     refused <R>), geometry-shader draws selected <GS>, vertices in <VI>, vertices out <VO>,
//     cpu_path_unhandled <U>, load failed <L>, not run to the end <E>, runner cpu ms/frame p50 <x> p95 <y>
//   cpuvertex total: the same fields over every capture (the frame percentiles over all frames)
//
// The frame boundary is the capture format's: a FRAME record is written once per game frame at its
// start (mgpu-format.md), and the player tells the Gpu by asking it for the frame's counters
// (GpuDebug::TakeFrameCounters). So frame k is the span from its FRAME record to the next one (the
// last one to the end of the capture), and the percentiles are over those spans; the draws before
// the first FRAME record are the tail of a frame that began before the capture: they count in the
// totals, not in the percentiles. Each span's time is the sum of the wall-clock time of the Run
// calls in it (steady_clock), frames without CPU-path draws count as 0. The percentile is the
// nearest-rank one. "Geometry-shader draws selected" counts draws with use_gs set once per draw
// trigger, as the census does (docs/research/pica-census.md): an Immediate draw counts only when it
// is the first vertex of a batch (geometry_reconfigured), the census's "one per immediate-mode batch".
// "Vertices in" are the vertices submitted to the vertex unit (cpu_path_vertices), "out" the
// OutputVertex entries produced.
//
// MH4N_CPUVERTEX_EXPECT_GS=140,668,2560,542 (comma separated, one number per capture in argument
// order): when set the case REQUIRE-s that each capture's geometry-shader draws selected equals the
// listed number. The case fails if cpu_path_unhandled is not 0, if a draw fails to load, or if any
// selected draw does not run to its end. Without MH4N_CPUVERTEX_CAPTURES it skips with a message.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "gpu/cpuvertex/cpu_vertex.h"
#include "gpu/pica/command_processor.h"
#include "gpu/pica/recording_backend.h"
#include "gpu/shader/pica_isa.h"
#include "gpu_capture/capture_host.h"
#include "gpu_capture/mgpu.h"
#include "gpu_capture/player.h"
#include "mh4n/gpu.h"
#include "mh4n/gpu_debug.h"
#include "mh4n/log.h"
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
using namespace mh4n::gpu::cpuvertex;
using mh4n::gpu_capture::CaptureHost;
namespace pica = mh4n::gpu::pica;
namespace fs = std::filesystem;

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();
const float kNaN = std::numeric_limits<float>::quiet_NaN();

constexpr u32 kFcram = 0x20000000;
constexpr u32 kBase = kFcram + 0x100000; // the vertex arrays' base address (a multiple of 16)

// Registers (PipelineRegs, ShaderRegs: Vs 0x2B0, Gs 0x280; RasterizerRegs).
constexpr u32 kVs = 0x2B0;
constexpr u32 kGs = 0x280;
constexpr u32 kRegUseGs = 0x229;
constexpr u32 kRegTopology = 0x25E;
constexpr u32 kRegOutTotal = 0x4F;
constexpr u32 kRegOutMap = 0x50;

u32 WithField(u32 word, u32 first, u32 count, u32 value) {
    const u32 mask = ((1u << count) - 1u) << first;
    return (word & ~mask) | ((value << first) & mask);
}

bool Same(float a, float b) { // equal, or both NaN; +0 and -0 differ
    if (std::isnan(a) || std::isnan(b)) {
        return std::isnan(a) && std::isnan(b);
    }
    return a == b && std::signbit(a) == std::signbit(b);
}

class NoMemoryHost final : public gpu::Host {
public:
    u8* PhysicalPointer(u32, u32) override { return nullptr; }
    std::optional<u32> VirtualToPhysical(u32) override { return std::nullopt; }
    void SignalInterrupt(gpu::Interrupt, u64) override {}
    void Post(std::function<void()> work) override { work(); }
};

// ---- an encoder for shader instructions (shader_bytecode.h's field positions) --------------------
//
//  common (arithmetic, CMP): opcode 26-31, dest 21-25 (CMP: op y 21-23, op x 24-26), address register
//    index 19-20, src1 12-18 (7 bits; "inverted" forms: 14-18, 5 bits), src2 7-11 (5 bits; inverted
//    7-13, 7 bits), operand descriptor 0-6
//  flow control: opcode, refx 25, refy 24, condition op / bool id / int id from bit 22, dest offset
//    10-21, number of instructions 0-7
//  multiply-add: opcode 26-31 (MAD 0x38-0x3F, MADI 0x30-0x37), dest 24-28, address index 22-23,
//    src1 17-21, src2 10-16 (5 bits at 12 inverted), src3 5-9 (7 bits at 5-11 inverted), desc 0-4
//  SETEMIT: vertex id 24-25, prim_emit 23, winding 22

enum : u32 {
    ADD = 0x00, DP3 = 0x01, DP4 = 0x02, DPH = 0x03, DST = 0x04, EX2 = 0x05, LG2 = 0x06, LIT = 0x07,
    MUL = 0x08, SGE = 0x09, SLT = 0x0A, FLR = 0x0B, MAX = 0x0C, MIN = 0x0D, RCP = 0x0E, RSQ = 0x0F,
    MOVA = 0x12, MOV = 0x13, DPHI = 0x18, DSTI = 0x19, SGEI = 0x1A, SLTI = 0x1B,
    BREAK = 0x20, NOP = 0x21, END = 0x22, BREAKC = 0x23, CALL = 0x24, CALLC = 0x25, CALLU = 0x26,
    IFU = 0x27, IFC = 0x28, LOOP = 0x29, EMIT = 0x2A, SETEMIT = 0x2B, JMPC = 0x2C, JMPU = 0x2D,
    CMP = 0x2E,
};
enum : u32 { CMP_EQ = 0, CMP_NE = 1, CMP_LT = 2, CMP_LE = 3, CMP_GT = 4, CMP_GE = 5 };
enum : u32 { COND_OR = 0, COND_AND = 1, COND_X = 2, COND_Y = 3 };

constexpr u32 V(u32 n) { return n; }        // input v0-v15
constexpr u32 R(u32 n) { return 0x10 + n; } // source temporary r0-r15
constexpr u32 C(u32 n) { return 0x20 + n; } // float uniform c0-c95
constexpr u32 O(u32 n) { return n; }        // destination output o0-o15
constexpr u32 T(u32 n) { return 0x10 + n; } // destination temporary r0-r15

enum : u32 { A0X = 1, A0Y = 2, AL = 3 };

u32 A(u32 op, u32 dest, u32 s1, u32 s2, u32 desc, u32 idx = 0) {
    return (op << 26) | (dest << 21) | (idx << 19) | (s1 << 12) | (s2 << 7) | desc;
}
u32 Ai(u32 op, u32 dest, u32 s1, u32 s2, u32 desc, u32 idx = 0) { // DPHI, DSTI, SGEI, SLTI
    return (op << 26) | (dest << 21) | (idx << 19) | (s1 << 14) | (s2 << 7) | desc;
}
u32 Cmp(u32 opx, u32 opy, u32 s1, u32 s2, u32 desc) {
    return (CMP << 26) | (opx << 24) | (opy << 21) | (s1 << 12) | (s2 << 7) | desc;
}
u32 Mad(u32 dest, u32 s1, u32 s2, u32 s3, u32 desc, u32 idx = 0) {
    return (0x38u << 26) | (dest << 24) | (idx << 22) | (s1 << 17) | (s2 << 10) | (s3 << 5) | desc;
}
u32 Madi(u32 dest, u32 s1, u32 s2, u32 s3, u32 desc, u32 idx = 0) {
    return (0x30u << 26) | (dest << 24) | (idx << 22) | (s1 << 17) | (s2 << 12) | (s3 << 5) | desc;
}
u32 Flow(u32 op, u32 dest, u32 num, u32 field22 = 0, u32 refx = 0, u32 refy = 0) {
    return (op << 26) | (refx << 25) | (refy << 24) | (field22 << 22) | (dest << 10) | num;
}
u32 SetEmit(u32 vertex_id, u32 prim_emit, u32 winding) {
    return (SETEMIT << 26) | (vertex_id << 24) | (prim_emit << 23) | (winding << 22);
}
constexpr u32 kEnd = END << 26;
constexpr u32 kNop = NOP << 26;
constexpr u32 kEmit = EMIT << 26;

// An operand descriptor: write mask (bit 3 = x), per source a negate flag and four lane selectors.
struct Desc {
    u32 mask = 0xF;
    bool n1 = false;
    std::string s1 = "xyzw";
    bool n2 = false;
    std::string s2 = "xyzw";
    bool n3 = false;
    std::string s3 = "xyzw";

    u32 Word() const {
        auto lane = [](char c) { return static_cast<u32>(c == 'x' ? 0 : c == 'y' ? 1 : c == 'z' ? 2 : 3); };
        u32 w = mask;
        w |= static_cast<u32>(n1) << 4;
        w |= static_cast<u32>(n2) << 13;
        w |= static_cast<u32>(n3) << 22;
        for (u32 c = 0; c < 4; ++c) {
            w |= lane(s1[c]) << (11 - 2 * c);
            w |= lane(s2[c]) << (20 - 2 * c);
            w |= lane(s3[c]) << (29 - 2 * c);
        }
        return w;
    }
};
u32 Mask(const char* lanes) { // "xz" -> 0b1010
    u32 m = 0;
    for (const char* p = lanes; *p; ++p) {
        m |= 0x8u >> (*p == 'x' ? 0 : *p == 'y' ? 1 : *p == 'z' ? 2 : 3);
    }
    return m;
}

// The program of a shader unit being written.
struct Program {
    std::vector<u32> code;
    std::vector<u32> descs;

    u32 D(const Desc& d) { // the id of a descriptor
        const u32 w = d.Word();
        for (u32 i = 0; i < descs.size(); ++i) {
            if (descs[i] == w) {
                return i;
            }
        }
        descs.push_back(w);
        return static_cast<u32>(descs.size() - 1);
    }
    u32 Full() { return D(Desc{}); }
    u32 Out(const char* lanes) {
        Desc d;
        d.mask = Mask(lanes);
        return D(d);
    }
    u32 At() const { return static_cast<u32>(code.size()); }
    Program& operator<<(u32 word) {
        code.push_back(word);
        return *this;
    }
};

// ---- a pica state to run a vertex shader on ------------------------------------------------------------

// The four output registers o0-o3 map to these OutputVertex slots, so a test reads all 16 values:
// o0 -> position (0-3), o1 -> quaternion (4-7), o2 -> texcoord 0 and 1 (12-15), o3 -> tc0_w and the
// view (16, 18, 19, 20). None of them is the colour (which is made absolute and saturated).
void MapFourOutputs(pica::PicaState& s) {
    s.regs[kRegOutTotal] = 4;
    s.regs[kRegOutMap + 0] = 0u | (1u << 8) | (2u << 16) | (3u << 24);
    s.regs[kRegOutMap + 1] = 4u | (5u << 8) | (6u << 16) | (7u << 24);
    s.regs[kRegOutMap + 2] = 12u | (13u << 8) | (14u << 16) | (15u << 24);
    s.regs[kRegOutMap + 3] = 16u | (18u << 8) | (19u << 16) | (20u << 24);
}

std::array<float, 16> Outs(const OutputVertex& v) {
    return {v.pos[0], v.pos[1], v.pos[2], v.pos[3], v.quat[0], v.quat[1], v.quat[2], v.quat[3],
            v.tc0[0], v.tc0[1], v.tc1[0], v.tc1[1], v.tc0_w, v.view[0], v.view[1], v.view[2]};
}

using F16 = std::array<float, 16>;

struct Machine {
    std::unique_ptr<pica::PicaState> st = std::make_unique<pica::PicaState>();
    NoMemoryHost host;
    CpuVertexRunner runner;
    Program p;

    Machine() {
        pica::PicaState& s = *st;
        s.regs[kVs + 0x09] = 7;            // eight inputs
        s.regs[kVs + 0x0B] = 0x76543210u;  // attribute n feeds input register n
        s.regs[kVs + 0x0C] = 0xFEDCBA98u;
        s.regs[kVs + 0x0D] = 0xF;          // o0-o3 are the shader's outputs
        MapFourOutputs(s);
    }

    u32 Full() { return p.Full(); }
    u32 D(const Desc& d) { return p.D(d); }

    void Input(u32 i, float x, float y, float z, float w) {
        st->immediate.input_vertex[i] = {{x, y, z, w}};
    }
    void Uniform(u32 i, float x, float y, float z, float w) {
        st->vs.uniform_f[i] = {{x, y, z, w}};
    }
    void Bool(u32 i, bool v) { st->vs.uniform_b[i] = v; }
    void Int(u32 i, u8 x, u8 y, u8 z, u8 w) { st->vs.uniform_i[i] = {x, y, z, w}; }

    void Load() {
        std::copy(p.code.begin(), p.code.end(), st->vs.program_code.begin());
        std::copy(p.descs.begin(), p.descs.end(), st->vs.swizzle_data.begin());
    }
    pica::DrawCall Call() {
        pica::DrawCall d;
        d.kind = pica::DrawKind::Immediate;
        d.num_vertices = 1;
        d.geometry_reconfigured = true;
        d.state = st.get();
        return d;
    }
    RunStatus RunRaw(RunMode mode = RunMode::Full) {
        Load();
        return runner.Run(host, Call(), mode);
    }
    // Runs the program on the immediate vertex and returns o0-o3.
    F16 Run() {
        const RunStatus status = RunRaw();
        REQUIRE_MESSAGE(status == RunStatus::Ok, runner.last_error());
        REQUIRE(runner.output().vertices.size() == 1);
        return Outs(runner.output().vertices[0]);
    }
};

void CheckLanes(const F16& got, const F16& want) {
    for (size_t i = 0; i < 16; ++i) {
        CAPTURE(i);
        CAPTURE(got[i]);
        CAPTURE(want[i]);
        CHECK(Same(got[i], want[i]));
    }
}

const F16 kZero16{};

} // namespace

// ================================================================================================
// Arithmetic
// ================================================================================================

TEST_CASE("ADD, MUL, MOV: swizzles, negation, the write mask and a uniform source") {
    Machine m;
    m.Input(0, 1, 2, 3, 4);
    m.Input(1, 10, 20, 30, 40);
    m.Uniform(5, 7, 8, 9, 10);
    Desc neg_swz;
    neg_swz.mask = Mask("xz");
    neg_swz.n1 = true;   // -v0
    neg_swz.s2 = "wzyx"; // v1.wzyx = (40, 30, 20, 10) ... lane z of it is v1.y = 20
    m.p << A(ADD, O(0), V(0), V(1), m.Full()) << A(MUL, O(1), V(0), V(1), m.Full())
        << A(ADD, O(2), V(0), V(1), m.D(neg_swz)) << A(MOV, O(3), C(5), 0, m.Full()) << kEnd;
    // o0 = (1+10, 2+20, 3+30, 4+40); o1 = (1*10, 2*20, 3*30, 4*40);
    // o2.x = -1 + 40 = 39, o2.z = -3 + 20 = 17, y and w are not written (0); o3 = c5.
    CheckLanes(m.Run(), {11, 22, 33, 44, 10, 40, 90, 160, 39, 0, 17, 0, 7, 8, 9, 10});
}

TEST_CASE("MUL: 0 x infinity is +0, a NaN factor stays NaN, an infinity that meets a number stays") {
    Machine m;
    m.Input(0, 0, -0.0f, kInf, kNaN);
    m.Input(1, kInf, kInf, 0, 2);
    m.Input(2, kInf, -kInf, 3, 0);
    m.Input(3, -2, 2, 0.5f, 1);
    m.p << A(MUL, O(0), V(0), V(1), m.Full()) << A(MUL, O(1), V(2), V(3), m.Full()) << kEnd;
    // o0: 0*inf = +0, -0*inf = +0 (both would be NaN in IEEE), inf*0 = +0, NaN*2 = NaN.
    // o1: inf*-2 = -inf, -inf*2 = -inf, 3*0.5 = 1.5, 0*1 = 0.
    F16 want = kZero16;
    want[0] = 0.0f; want[1] = 0.0f; want[2] = 0.0f; want[3] = kNaN;
    want[4] = -kInf; want[5] = -kInf; want[6] = 1.5f; want[7] = 0.0f;
    CheckLanes(m.Run(), want);
}

TEST_CASE("ADD has no such rule: infinity plus minus infinity is NaN") {
    Machine m;
    m.Input(0, kInf, 1, kNaN, 5);
    m.Input(1, -kInf, 2, 1, -5);
    m.p << A(ADD, O(0), V(0), V(1), m.Full()) << kEnd;
    F16 want = kZero16;
    want[0] = kNaN; want[1] = 3; want[2] = kNaN; want[3] = 0;
    CheckLanes(m.Run(), want);
}

TEST_CASE("DP3, DP4, DPH, DPHI and the sum order of a dot product") {
    Machine m;
    m.Input(0, 1, 2, 3, 4);
    m.Input(1, 5, 6, 7, 8);
    m.Uniform(0, 5, 6, 7, 8);
    m.p << A(DP3, O(0), V(0), V(1), m.Full()) << A(DP4, O(1), V(0), V(1), m.Full())
        << A(DPH, O(2), V(0), V(1), m.Full()) << Ai(DPHI, O(3), V(0), C(0), m.Full()) << kEnd;
    // dp3 = 5 + 12 + 21 = 38; dp4 = 38 + 32 = 70; dph replaces v0.w by 1: 38 + 8 = 46; dphi (src1
    // v0 in the 5-bit field, src2 c0 in the 7-bit one) is the same as dph: 46.
    CheckLanes(m.Run(), {38, 38, 38, 38, 70, 70, 70, 70, 46, 46, 46, 46, 46, 46, 46, 46});
}

TEST_CASE("dot products: 0 x infinity is +0 inside the sum, NaN stays, the sum starts from +0") {
    Machine m;
    m.Input(0, 0, 1, 0, 0);
    m.Input(1, kInf, 2, 5, 5);
    m.Input(2, -1, -1, -1, 0);
    m.Input(3, 0, 0, 0, 0);
    m.Input(4, kNaN, 1, 1, 1);
    m.Input(5, 1, 1, 1, 1);
    m.p << A(DP4, O(0), V(0), V(1), m.Full()) << A(DP3, O(1), V(0), V(1), m.Full())
        << A(DP3, O(2), V(2), V(3), m.Full()) << A(DP4, O(3), V(4), V(5), m.Full()) << kEnd;
    // dp4 = 0*inf (= +0) + 1*2 + 0*5 + 0*5 = 2; dp3 = 0 + 2 + 0 = 2 (no NaN either way);
    // dp3 of (-1,-1,-1) and (0,0,0): the products are -0, the sum starts from +0: +0 + -0 = +0,
    // so the result is +0, not -0; dp4 with a NaN term is NaN.
    CheckLanes(m.Run(), {2, 2, 2, 2, 2, 2, 2, 2, 0, 0, 0, 0, kNaN, kNaN, kNaN, kNaN});
}

TEST_CASE("MAD and MADI: a multiply-add, the 0 x infinity rule, a negated third source") {
    Machine m;
    m.Input(0, 1, 2, 3, 4);
    m.Input(1, 5, 6, 7, 8);
    m.Input(2, 1, 1, 1, 1);
    m.Input(4, 0, kNaN, 2, kInf);
    m.Input(5, kInf, 1, 3, 0);
    m.Input(6, 3, 1, 1, 1);
    m.Uniform(0, 1, 1, 1, 1);
    Desc neg3;
    neg3.n3 = true;
    // MAD takes a 5-bit descriptor id: all the descriptors used here have ids below 32.
    m.p << Mad(O(0), V(0), V(1), V(2), m.Full()) << Madi(O(1), V(0), V(1), C(0), m.Full())
        << Mad(O(2), V(0), V(1), V(2), m.D(neg3)) << Mad(O(3), V(4), V(5), V(6), m.Full()) << kEnd;
    // o0 = v0*v1 + v2 = (6, 13, 22, 33); o1 (MADI: src2 5 bits, src3 the uniform) = the same;
    // o2 = v0*v1 - v2 = (4, 11, 20, 31);
    // o3 = (0*inf = +0) + 3 = 3, NaN*1 + 1 = NaN, 2*3 + 1 = 7, inf*0 = +0, + 1 = 1.
    CheckLanes(m.Run(), {6, 13, 22, 33, 6, 13, 22, 33, 4, 11, 20, 31, 3, kNaN, 7, 1});
}

TEST_CASE("MAX and MIN order as the hardware: max = src1 > src2 ? src1 : src2, with a NaN") {
    Machine m;
    m.Input(0, 1, kNaN, 0, kNaN);
    m.Input(1, 2, 0, kNaN, kNaN);
    m.p << A(MAX, O(0), V(0), V(1), m.Full()) << A(MIN, O(1), V(0), V(1), m.Full()) << kEnd;
    // max: 1>2 no -> 2; NaN>0 no -> src2 = 0; 0>NaN no -> src2 = NaN; NaN>NaN no -> NaN
    // min: 1<2 -> 1; NaN<0 no -> 0; 0<NaN no -> NaN; NaN<NaN no -> NaN
    F16 want = kZero16;
    want[0] = 2; want[1] = 0; want[2] = kNaN; want[3] = kNaN;
    want[4] = 1; want[5] = 0; want[6] = kNaN; want[7] = kNaN;
    CheckLanes(m.Run(), want);
}

TEST_CASE("FLR, SLT, SGE, SLTI, SGEI") {
    Machine m;
    m.Input(0, 1.5f, -1.5f, 2.0f, -0.2f);
    m.Input(1, 1, 2, 3, 4);
    m.Input(2, 2, 2, 2, 2);
    m.Uniform(0, 2, 2, 2, 2);
    m.p << A(FLR, O(0), V(0), 0, m.Full()) << A(SLT, O(1), V(1), V(2), m.Full())
        << A(SGE, O(2), V(1), V(2), m.Full()) << Ai(SLTI, O(3), V(1), C(0), m.Full()) << kEnd;
    // floor: (1, -2, 2, -1); slt (1,2,3,4) < 2: (1, 0, 0, 0); sge: (0, 1, 1, 1); slti the same as slt.
    CheckLanes(m.Run(), {1, -2, 2, -1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0});
    Machine n;
    n.Input(1, 1, 2, 3, 4);
    n.Uniform(0, 2, 2, 2, 2);
    n.p << Ai(SGEI, O(0), V(1), C(0), n.Full()) << kEnd;
    CheckLanes(n.Run(), {0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
}

TEST_CASE("RCP, RSQ, EX2, LG2 read lane x of the (swizzled) source and write all enabled lanes") {
    Machine m;
    m.Input(0, 4, 3, 8, 0);
    Desc yyyy;
    yyyy.s1 = "yyyy";
    Desc zzzz;
    zzzz.s1 = "zzzz";
    Desc wwww_xw;
    wwww_xw.s1 = "wwww";
    wwww_xw.mask = Mask("xw");
    m.p << A(RCP, O(0), V(0), 0, m.Full()) << A(RSQ, O(1), V(0), 0, m.Full())
        << A(EX2, O(2), V(0), 0, m.D(yyyy)) << A(LG2, O(3), V(0), 0, m.D(zzzz)) << kEnd;
    // 1/4 = 0.25; 1/sqrt(4) = 0.5; 2^3 = 8; log2(8) = 3
    CheckLanes(m.Run(), {0.25f, 0.25f, 0.25f, 0.25f, 0.5f, 0.5f, 0.5f, 0.5f, 8, 8, 8, 8, 3, 3, 3, 3});
    Machine n;
    n.Input(0, 4, 3, 8, 0);
    n.p << A(RCP, O(0), V(0), 0, n.D(wwww_xw)) << kEnd;
    // 1/0 = +infinity, written to x and w only
    CheckLanes(n.Run(), {kInf, 0, 0, kInf, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
}

TEST_CASE("MOVA and relative addressing: a0.x, a0.y, an offset out of the 8-bit range, an index past c95") {
    Machine m;
    m.Input(2, 3.7f, -2.2f, 200, 10);
    m.Uniform(8, 5, 6, 7, 8);
    m.Uniform(10, 9, 9, 9, 9);
    m.Uniform(13, 1, 2, 3, 4);
    Desc xy;
    xy.mask = Mask("xy");
    Desc x_zzzz;
    x_zzzz.mask = Mask("x");
    x_zzzz.s1 = "zzzz";
    Desc x_wwww;
    x_wwww.mask = Mask("x");
    x_wwww.s1 = "wwww";
    // mova a0.xy, v2: a0.x = 3 (truncated), a0.y = -2.
    m.p << A(MOVA, 0, V(2), 0, m.D(xy)) << A(MOV, O(0), C(10), 0, m.Full(), A0X)
        << A(MOV, O(1), C(10), 0, m.Full(), A0Y)
        // mova a0.x, v2.zzzz = 200: outside -128..127, so the offset is 0 -> c10
        << A(MOVA, 0, V(2), 0, m.D(x_zzzz)) << A(MOV, O(2), C(10), 0, m.Full(), A0X)
        // mova a0.x, v2.wwww = 10: c90 + 10 = 100 >= 96 -> all ones
        << A(MOVA, 0, V(2), 0, m.D(x_wwww)) << A(MOV, O(3), C(90), 0, m.Full(), A0X) << kEnd;
    // o0 = c(10+3) = c13; o1 = c(10-2) = c8; o2 = c10; o3 = (1, 1, 1, 1)
    CheckLanes(m.Run(), {1, 2, 3, 4, 5, 6, 7, 8, 9, 9, 9, 9, 1, 1, 1, 1});
}

TEST_CASE("relative addressing of the second source of MAD, and of the uniform of the inverted forms") {
    Machine m;
    m.Input(0, 1, 2, 3, 4);
    m.Input(1, 3, 0, 0, 0);
    m.Input(2, 1, 1, 1, 1);
    m.Uniform(13, 1, 2, 3, 4);
    Desc x;
    x.mask = Mask("x");
    m.p << A(MOVA, 0, V(1), 0, m.D(x))
        // mad o0, v0, c10[a0.x], v2 = v0 * c13 + 1 = (2, 5, 10, 17)
        << Mad(O(0), V(0), C(10), V(2), m.Full(), A0X)
        // madi o1, v0, v2, c10[a0.x]: MADI applies the register to src3: v0*v2 + c13 = (2, 4, 6, 8)
        << Madi(O(1), V(0), V(2), C(10), m.Full(), A0X)
        // dphi with src2 = c10[a0.x]: (1,2,3,1) . (1,2,3,4) = 1 + 4 + 9 + 4 = 18
        << Ai(DPHI, O(2), V(0), C(10), m.Full(), A0X) << kEnd;
    CheckLanes(m.Run(), {2, 5, 10, 17, 2, 4, 6, 8, 18, 18, 18, 18, 0, 0, 0, 0});
}

TEST_CASE("an output colour is made absolute and saturated; slots no attribute maps are 1") {
    Machine m;
    m.st->regs[kRegOutTotal] = 1;
    // attribute 0: x -> colour r (8), y -> colour g (9), z -> colour b (10), w -> position x (0)
    m.st->regs[kRegOutMap] = 8u | (9u << 8) | (10u << 16) | (0u << 24);
    m.st->regs[kVs + 0x0D] = 1;
    m.Input(0, -0.5f, 3, 0.25f, 7);
    m.p << A(MOV, O(0), V(0), 0, m.Full()) << kEnd;
    REQUIRE(m.RunRaw() == RunStatus::Ok);
    const OutputVertex& v = m.runner.output().vertices.at(0);
    CHECK(v.color[0] == 0.5f);  // |-0.5|
    CHECK(v.color[1] == 1.0f);  // |3| saturated
    CHECK(v.color[2] == 0.25f);
    CHECK(v.color[3] == 1.0f);  // not mapped: 1
    CHECK(v.pos[0] == 7);
    CHECK(v.pos[1] == 1);
    CHECK(v.pos[2] == 1);
    CHECK(v.pos[3] == 1);
    CHECK(v.tc0_w == 1);
}

TEST_CASE("a program with no END stops at its last word; the zero program does nothing") {
    Machine m;
    // All 4096 words are 0: an ADD with descriptor 0, whose write mask is empty. Word 4095 is
    // treated as END (Azahar's fix for shaders without an END).
    CheckLanes(m.Run(), kZero16);
    CHECK(m.runner.counters().unhandled == 0);
}

// ================================================================================================
// Flow control
// ================================================================================================

namespace {

// c0 = (1,2,3,4) and c1 = (5,6,7,8) are the markers the flow tests write to o0.
void Markers(Machine& m) {
    m.Uniform(0, 1, 2, 3, 4);
    m.Uniform(1, 5, 6, 7, 8);
}

} // namespace

TEST_CASE("IFU: the then part, the else part, and the code after it") {
    for (bool b : {true, false}) {
        CAPTURE(b);
        Machine m;
        Markers(m);
        m.Bool(3, b);
        m.p << Flow(IFU, 3, 1, 3)               // 0: ifu b3 { 1, 2 } else { 3 }
            << A(MOV, O(0), C(0), 0, m.Full())  // 1
            << A(MOV, O(1), C(0), 0, m.Full())  // 2
            << A(MOV, O(0), C(1), 0, m.Full())  // 3
            << kEnd;                            // 4
        // true: o0 = o1 = c0; false: only the else part: o0 = c1, o1 untouched (0)
        if (b) {
            CheckLanes(m.Run(), {1, 2, 3, 4, 1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0});
        } else {
            CheckLanes(m.Run(), {5, 6, 7, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
        }
    }
}

TEST_CASE("CMP and IFC: the six compare operations on x and on y, with a NaN") {
    // v0 = (1, 2), v1 = (2, 2): x compares 1 with 2, y compares 2 with 2.
    const bool want_x[6] = {false, true, true, true, false, false}; // ==, !=, <, <=, >, >=
    const bool want_y[6] = {true, false, false, true, false, true};
    auto cond_true = [](u32 opx, u32 opy, u32 cond_op, u32 refx, u32 refy, float v0x, float v1x) {
        Machine m;
        Markers(m);
        m.Input(0, v0x, 2, 0, 0);
        m.Input(1, v1x, 2, 0, 0);
        m.p << Cmp(opx, opy, V(0), V(1), m.Full())             // 0
            << Flow(IFC, 3, 1, cond_op, refx, refy)            // 1: ifc { 2 } else { 3 }
            << A(MOV, O(0), C(0), 0, m.Full())                 // 2
            << A(MOV, O(0), C(1), 0, m.Full())                 // 3
            << kEnd;                                           // 4
        return m.Run()[0] == 1.0f;
    };
    for (u32 op = 0; op < 6; ++op) {
        CAPTURE(op);
        // JustX with refx = 1 is cc.x; JustY with refy = 1 is cc.y
        CHECK(cond_true(op, op, COND_X, 1, 0, 1, 2) == want_x[op]);
        CHECK(cond_true(op, op, COND_Y, 0, 1, 1, 2) == want_y[op]);
    }
    // A NaN is equal to nothing and different from everything: NaN == 1 false, NaN != 1 true, and
    // NaN < 1, <=, >, >= are all false.
    CHECK_FALSE(cond_true(CMP_EQ, CMP_EQ, COND_X, 1, 0, kNaN, 1));
    CHECK(cond_true(CMP_NE, CMP_EQ, COND_X, 1, 0, kNaN, 1));
    for (u32 op : {CMP_LT, CMP_LE, CMP_GT, CMP_GE}) {
        CHECK_FALSE(cond_true(op, CMP_EQ, COND_X, 1, 0, kNaN, 1));
    }
}

TEST_CASE("IFC: the condition operators Or, And, JustX, JustY on the condition code (true, false)") {
    // v0 = (1, 2), v1 = (1, 3): cmp == gives cc = (true, false). A condition tests
    // (refx == cc.x) and (refy == cc.y) and combines them.
    struct Row {
        u32 refx, refy;
        bool or_, and_, x, y;
    };
    // (1,1): rx = true, ry = false   (0,0): rx = false, ry = true
    // (1,0): rx = true, ry = true    (0,1): rx = false, ry = false
    const Row rows[] = {
        {1, 1, true, false, true, false},
        {0, 0, true, false, false, true},
        {1, 0, true, true, true, true},
        {0, 1, false, false, false, false},
    };
    for (const Row& row : rows) {
        for (u32 op = 0; op < 4; ++op) {
            const bool want[4] = {row.or_, row.and_, row.x, row.y};
            CAPTURE(row.refx);
            CAPTURE(row.refy);
            CAPTURE(op);
            Machine m;
            Markers(m);
            m.Input(0, 1, 2, 0, 0);
            m.Input(1, 1, 3, 0, 0);
            m.p << Cmp(CMP_EQ, CMP_EQ, V(0), V(1), m.Full()) << Flow(IFC, 3, 1, op, row.refx, row.refy)
                << A(MOV, O(0), C(0), 0, m.Full()) << A(MOV, O(0), C(1), 0, m.Full()) << kEnd;
            CHECK((m.Run()[0] == 1.0f) == want[op]);
        }
    }
}

TEST_CASE("LOOP: count + 1 iterations, aL starts at y and grows by z after each iteration") {
    Machine m;
    Markers(m);
    m.Int(0, 2, 10, 3, 0);
    m.Uniform(30, 1, 0, 0, 0);
    m.Uniform(33, 10, 0, 0, 0);
    m.Uniform(36, 100, 0, 0, 0);
    m.p << Flow(LOOP, 2, 0, 0)                          // 0: loop i0, body 1..2
        << A(ADD, T(0), C(0), R(0), m.Full())           // 1: r0 += c0
        << A(ADD, T(1), C(20), R(1), m.Full(), AL)      // 2: r1 += c20[aL]
        << A(MOV, O(0), R(0), 0, m.Full())              // 3
        << A(MOV, O(1), R(1), 0, m.Full())              // 4
        << kEnd;
    // three iterations: r0 = 3 * c0 = (3, 6, 9, 12); aL = 10, 13, 16 read c30, c33, c36:
    // r1.x = 1 + 10 + 100 = 111
    CheckLanes(m.Run(), {3, 6, 9, 12, 111, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
}

TEST_CASE("LOOP nested: after the inner loop aL is the inner loop's start value (Azahar's quirk)") {
    // Azahar assigns aL before it records the "previous aL", so the value restored when an inner
    // loop ends (with an outer one on the stack) is the inner loop's own y, not the outer's aL.
    Machine m;
    m.Int(0, 1, 5, 1, 0);   // outer: 2 iterations, aL = 5, +1
    m.Int(1, 0, 20, 0, 0);  // inner: 1 iteration, aL = 20, +0
    m.Uniform(0, 1, 0, 0, 0);
    m.Uniform(60, 1000, 0, 0, 0);
    m.p << Flow(LOOP, 5, 0, 0)                          // 0: outer, body 1..5
        << Flow(LOOP, 2, 0, 1)                          // 1: inner, body 2
        << A(ADD, T(0), C(0), R(0), m.Full())           // 2: r0 += c0
        << A(ADD, T(1), C(40), R(1), m.Full(), AL)      // 3: r1 += c40[aL]
        << kNop                                         // 4
        << kNop                                         // 5: the outer loop's last instruction
        << A(MOV, O(0), R(0), 0, m.Full())              // 6
        << A(MOV, O(1), R(1), 0, m.Full())              // 7
        << kEnd;
    // Trace: aL = 5; inner: aL = 20, r0 += 1, inner ends: aL = 20 (+0), restored to 20; r1 += c60;
    // the outer's last instruction: aL = 21, second iteration; the inner loop again: aL = 20 ...
    // r0 = 2 (two runs of the inner body), r1 = 2 * c60 = 2000 (aL was 20 both times).
    CheckLanes(m.Run(), {2, 0, 0, 0, 2000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
}

TEST_CASE("BREAK leaves the loop at once; BREAKC only when the condition holds") {
    {
        Machine m;
        Markers(m);
        m.Int(0, 4, 0, 0, 0);
        m.p << Flow(LOOP, 2, 0, 0)                      // 0: body 1..2
            << A(ADD, T(0), C(0), R(0), m.Full())       // 1
            << (BREAK << 26)                            // 2
            << A(MOV, O(0), R(0), 0, m.Full())          // 3
            << kEnd;
        CheckLanes(m.Run(), {1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}); // one iteration
    }
    {
        Machine m;
        m.Uniform(0, 1, 0, 0, 0);
        m.Uniform(1, 3, 0, 0, 0);
        m.Int(0, 4, 0, 1, 0); // five iterations unless broken
        m.p << Flow(LOOP, 3, 0, 0)                                       // 0: body 1..3
            << A(ADD, T(0), C(0), R(0), m.Full())                        // 1: r0 += (1, 0, 0, 0)
            << Cmp(CMP_LE, CMP_EQ, C(1), R(0), m.Full())                 // 2: cc.x = 3 <= r0.x
            << Flow(BREAKC, 0, 0, COND_X, 1, 0)                          // 3: break when cc.x
            << A(MOV, O(0), R(0), 0, m.Full())                           // 4
            << kEnd;
        // r0.x = 1, 2, 3: at 3 the condition holds and the loop is left
        CheckLanes(m.Run(), {3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    }
}

TEST_CASE("CALL, CALLC, CALLU: the function runs, then execution returns after the call") {
    auto build = [](Machine& m, u32 call_word) {
        Markers(m);
        m.p << call_word                                // 0
            << A(MOV, O(1), R(0), 0, m.Full())          // 1: after the call
            << kEnd                                     // 2
            << kNop                                     // 3
            << A(ADD, T(0), C(0), R(0), m.Full())       // 4: the function (two instructions)
            << A(ADD, T(0), C(0), R(0), m.Full());      // 5
    };
    {
        Machine m;
        build(m, Flow(CALL, 4, 2));
        CheckLanes(m.Run(), {0, 0, 0, 0, 2, 4, 6, 8, 0, 0, 0, 0, 0, 0, 0, 0}); // r0 = 2 * c0
    }
    for (bool taken : {true, false}) {
        CAPTURE(taken);
        Machine m;
        m.Bool(1, taken);
        build(m, Flow(CALLU, 4, 2, 1));
        const F16 want = taken ? F16{0, 0, 0, 0, 2, 4, 6, 8, 0, 0, 0, 0, 0, 0, 0, 0} : kZero16;
        CheckLanes(m.Run(), want);
    }
    for (bool taken : {true, false}) {
        CAPTURE(taken);
        // cmp sets cc.x = (v0.x == v1.x); the call is made when cc.x is true (refx = 1, JustX)
        Machine m;
        m.Input(0, 1, 0, 0, 0);
        m.Input(1, taken ? 1.0f : 2.0f, 0, 0, 0);
        Markers(m);
        m.p << Cmp(CMP_EQ, CMP_EQ, V(0), V(1), m.Full())        // 0
            << Flow(CALLC, 5, 2, COND_X, 1, 0)                  // 1
            << A(MOV, O(1), R(0), 0, m.Full())                  // 2
            << kEnd                                             // 3
            << kNop                                             // 4
            << A(ADD, T(0), C(0), R(0), m.Full())               // 5
            << A(ADD, T(0), C(0), R(0), m.Full());              // 6
        const F16 want = taken ? F16{0, 0, 0, 0, 2, 4, 6, 8, 0, 0, 0, 0, 0, 0, 0, 0} : kZero16;
        CheckLanes(m.Run(), want);
    }
}

TEST_CASE("JMPU and JMPC") {
    // jmpu jumps when the bool equals !(num_instructions & 1): num 0 jumps on true, num 1 on false
    for (u32 num : {0u, 1u}) {
        for (bool b : {true, false}) {
            CAPTURE(num);
            CAPTURE(b);
            Machine m;
            Markers(m);
            m.Bool(0, b);
            m.p << Flow(JMPU, 3, num, 0)                 // 0
                << A(MOV, O(0), C(0), 0, m.Full())       // 1
                << kEnd                                  // 2
                << A(MOV, O(0), C(1), 0, m.Full())       // 3
                << kEnd;                                 // 4
            const bool jumps = (b == (num == 0));
            CheckLanes(m.Run(), jumps ? F16{5, 6, 7, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
                                      : F16{1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
        }
    }
    // jmpc: cmp == gives cc = (true, false); JustX (refx 1) jumps, JustY (refy 1) does not
    for (u32 cond : {COND_X, COND_Y}) {
        CAPTURE(cond);
        Machine m;
        Markers(m);
        m.Input(0, 1, 2, 0, 0);
        m.Input(1, 1, 3, 0, 0);
        m.p << Cmp(CMP_EQ, CMP_EQ, V(0), V(1), m.Full())                   // 0
            << Flow(JMPC, 4, 0, cond, cond == COND_X ? 1 : 0, cond == COND_Y ? 1 : 0)  // 1
            << A(MOV, O(0), C(0), 0, m.Full())                             // 2
            << kEnd                                                        // 3
            << A(MOV, O(0), C(1), 0, m.Full())                             // 4
            << kEnd;                                                       // 5
        CheckLanes(m.Run(), cond == COND_X ? F16{5, 6, 7, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
                                           : F16{1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    }
}

TEST_CASE("an IF inside a CALL inside a LOOP: the three stacks work together") {
    for (bool b : {true, false}) {
        CAPTURE(b);
        Machine m;
        Markers(m);
        m.Bool(2, b);
        m.Int(0, 1, 0, 0, 0); // two iterations
        m.p << Flow(LOOP, 2, 0, 0)                          // 0: loop, body 1..2
            << Flow(CALL, 7, 4)                             // 1: call f (7..10)
            << kNop                                         // 2
            << A(MOV, O(0), R(0), 0, m.Full())              // 3
            << A(MOV, O(1), R(1), 0, m.Full())              // 4
            << kEnd                                         // 5
            << kNop                                         // 6
            << Flow(IFU, 9, 1, 2)                           // 7: f: ifu b2 { 8 } else { 9 }
            << A(ADD, T(0), C(0), R(0), m.Full())           // 8: r0 += c0
            << A(ADD, T(0), C(1), R(0), m.Full())           // 9: (else) r0 += c1
            << A(ADD, T(1), C(0), R(1), m.Full());          // 10: r1 += c0
        // Each of the two iterations calls f once. r1 += c0 every time: (2, 4, 6, 8). r0 += c0 when
        // b2 is true: (2, 4, 6, 8); else r0 += c1: 2 * (5, 6, 7, 8) = (10, 12, 14, 16).
        const F16 want = b ? F16{2, 4, 6, 8, 2, 4, 6, 8, 0, 0, 0, 0, 0, 0, 0, 0}
                           : F16{10, 12, 14, 16, 2, 4, 6, 8, 0, 0, 0, 0, 0, 0, 0, 0};
        CheckLanes(m.Run(), want);
    }
}

TEST_CASE("the last instruction of a LOOP body that is a CALL is never called (the loop end wins)") {
    // Azahar checks the loop end after every instruction and lets it overwrite the program counter
    // the call just set ("they overwrite any previous program counter updates").
    Machine m;
    Markers(m);
    m.Int(0, 1, 0, 0, 0);
    m.p << Flow(LOOP, 1, 0, 0)                          // 0: body: 1 only
        << Flow(CALL, 5, 1)                             // 1
        << A(MOV, O(0), R(0), 0, m.Full())              // 2
        << kEnd                                         // 3
        << kNop                                         // 4
        << A(ADD, T(0), C(0), R(0), m.Full());          // 5: f
    CheckLanes(m.Run(), kZero16);
}

// ================================================================================================
// What the runner refuses
// ================================================================================================

TEST_CASE("DST, DSTI, LIT, an unknown opcode, a bad compare mode and a vertex shader that emits are unhandled") {
    struct Case {
        const char* name;
        u32 word;
    };
    Machine probe;
    const u32 full = probe.Full();
    const Case cases[] = {
        {"dst", A(DST, O(0), V(0), V(1), full)},
        {"dsti", Ai(DSTI, O(0), V(0), C(0), full)},
        {"lit", A(LIT, O(0), V(0), 0, full)},
        {"opcode 0x14 does not exist", 0x14u << 26},
        {"cmp with compare mode 6", Cmp(6, CMP_EQ, V(0), V(1), full)},
        {"cmp with compare mode 7 on y", Cmp(CMP_EQ, 7, V(0), V(1), full)},
        {"emit on the vertex shader", kEmit},
        {"setemit on the vertex shader", SetEmit(0, 1, 0)},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        Machine m;
        m.p << c.word << kEnd;
        m.p.Full();
        CHECK(m.RunRaw() == RunStatus::Unhandled);
        CHECK(m.runner.counters().unhandled == 1);
        CHECK(m.runner.counters().draws == 1);
        CHECK(m.runner.output().vertices.empty());
        CHECK_FALSE(m.runner.last_error().empty());
    }
}

TEST_CASE("a shader that never ends and an entry point outside the program are unhandled") {
    {
        Machine m;
        m.Bool(0, true);
        m.p << Flow(JMPU, 0, 0, 0); // jumps to itself for ever
        CHECK(m.RunRaw() == RunStatus::Unhandled);
        CHECK(m.runner.counters().unhandled == 1);
    }
    {
        Machine m;
        m.st->regs[kVs + 0x0A] = 5000; // main offset
        m.p << kEnd;
        CHECK(m.RunRaw() == RunStatus::Unhandled);
        CHECK(m.runner.counters().unhandled == 1);
    }
}

TEST_CASE("an unhandled program is logged once, however many draws it drops") {
    std::vector<std::string> messages;
    SetLogSink([&messages](const LogRecord& r) {
        if (r.category == "CpuVertex") {
            messages.emplace_back(r.message);
        }
    });
    Machine m;
    m.p << A(DST, O(0), V(0), V(1), m.Full()) << kEnd;
    for (int i = 0; i < 3; ++i) {
        CHECK(m.RunRaw() == RunStatus::Unhandled);
    }
    SetLogSink({});
    CHECK(m.runner.counters().unhandled == 3);
    CHECK(messages.size() == 1);
    // a different program is another message
    SetLogSink([&messages](const LogRecord& r) {
        if (r.category == "CpuVertex") {
            messages.emplace_back(r.message);
        }
    });
    Machine n;
    n.p << A(LIT, O(0), V(0), 0, n.Full()) << kEnd;
    CHECK(n.RunRaw() == RunStatus::Unhandled);
    SetLogSink({});
    CHECK(messages.size() == 2);
    // and the next draw runs: the runner is not stuck
    Machine ok;
    ok.p << A(MOV, O(0), V(0), 0, ok.Full()) << kEnd;
    ok.Input(0, 1, 2, 3, 4);
    CheckLanes(ok.Run(), {1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
}

// ================================================================================================
// NeedsCpuPath
// ================================================================================================

namespace {

pica::DrawCall MakeDraw(pica::PicaState& s, pica::DrawKind kind, u32 count, u32 topology, u32 use_gs = 0) {
    s.regs[kRegTopology] = WithField(s.regs[kRegTopology], 8, 2, topology);
    s.regs[kRegUseGs] = WithField(s.regs[kRegUseGs], 0, 2, use_gs);
    pica::DrawCall d;
    d.kind = kind;
    d.num_vertices = count;
    d.state = &s;
    return d;
}

} // namespace

TEST_CASE("NeedsCpuPath: the geometry shader, immediate mode, and what accelerate_draw refuses") {
    auto s = std::make_unique<pica::PicaState>();
    using pica::DrawKind;
    // a normal Strip / Fan / List draw is not selected, with the assembler empty
    CHECK_FALSE(NeedsCpuPath(MakeDraw(*s, DrawKind::Arrays, 6, pica::kTopologyList), true));
    CHECK_FALSE(NeedsCpuPath(MakeDraw(*s, DrawKind::Arrays, 7, pica::kTopologyStrip), true));
    CHECK_FALSE(NeedsCpuPath(MakeDraw(*s, DrawKind::Arrays, 4, pica::kTopologyFan), true));
    CHECK_FALSE(NeedsCpuPath(MakeDraw(*s, DrawKind::Indexed, 9, pica::kTopologyList), true));
    CHECK(CpuPathReason(MakeDraw(*s, DrawKind::Arrays, 6, pica::kTopologyList), true) == CpuReason::None);

    // an Arrays List draw of a multiple of 3 vertices is selected only when the assembler is not empty
    CHECK_FALSE(NeedsCpuPath(MakeDraw(*s, DrawKind::Arrays, 6, pica::kTopologyList), true));
    CHECK(NeedsCpuPath(MakeDraw(*s, DrawKind::Arrays, 6, pica::kTopologyList), false));
    CHECK(CpuPathReason(MakeDraw(*s, DrawKind::Arrays, 6, pica::kTopologyList), false) == CpuReason::Refused);
    // so is a Strip or a Fan one (the buffered vertex "leaks" into it)
    CHECK(NeedsCpuPath(MakeDraw(*s, DrawKind::Arrays, 7, pica::kTopologyStrip), false));
    CHECK(NeedsCpuPath(MakeDraw(*s, DrawKind::Indexed, 4, pica::kTopologyFan), false));

    // List and Shader topology with a count that is not a multiple of 3: refused even with an empty assembler
    CHECK(NeedsCpuPath(MakeDraw(*s, DrawKind::Arrays, 4, pica::kTopologyList), true));
    CHECK(NeedsCpuPath(MakeDraw(*s, DrawKind::Indexed, 10, pica::kTopologyList), true));
    CHECK(CpuPathReason(MakeDraw(*s, DrawKind::Arrays, 5, pica::kTopologyShader), true) == CpuReason::Refused);
    CHECK_FALSE(NeedsCpuPath(MakeDraw(*s, DrawKind::Arrays, 6, pica::kTopologyShader), true));
    // Strip and Fan have no such rule
    CHECK_FALSE(NeedsCpuPath(MakeDraw(*s, DrawKind::Arrays, 4, pica::kTopologyStrip), true));
    CHECK_FALSE(NeedsCpuPath(MakeDraw(*s, DrawKind::Arrays, 5, pica::kTopologyFan), true));

    // geometry shader draws, whatever the topology, the count and the assembler
    for (u32 topology : {0u, 1u, 2u, 3u}) {
        for (bool empty : {true, false}) {
            CHECK(CpuPathReason(MakeDraw(*s, DrawKind::Arrays, 6, topology, pica::kUseGsYes), empty) ==
                  CpuReason::GeometryShader);
            CHECK(CpuPathReason(MakeDraw(*s, DrawKind::Indexed, 5, topology, pica::kUseGsYes), empty) ==
                  CpuReason::GeometryShader);
        }
    }
    // use_gs raw values 1 and 3 are not Yes: the topology rule decides (Azahar compares with Yes)
    CHECK_FALSE(NeedsCpuPath(MakeDraw(*s, DrawKind::Arrays, 6, pica::kTopologyList, 1), true));
    CHECK_FALSE(NeedsCpuPath(MakeDraw(*s, DrawKind::Arrays, 6, pica::kTopologyList, 3), true));

    // immediate draws are always on the CPU; with use_gs the reason is the geometry shader
    for (bool empty : {true, false}) {
        CHECK(CpuPathReason(MakeDraw(*s, DrawKind::Immediate, 1, pica::kTopologyList), empty) == CpuReason::Immediate);
        CHECK(CpuPathReason(MakeDraw(*s, DrawKind::Immediate, 1, pica::kTopologyShader, pica::kUseGsYes), empty) ==
              CpuReason::GeometryShader);
    }
    CHECK(std::string(ToString(CpuReason::Refused)) == "refused by accelerate_draw");
}


TEST_CASE("the encoder places every field where the translator's independent decoder reads it") {
    // gpu/shader/pica_isa.h is a second port of the same layouts (the runner does not use it): a
    // field this file's encoder puts elsewhere than nihstro does would be read differently by it.
    namespace isa = mh4n::gpu::shader::isa;
    {
        const isa::Instruction i{A(ADD, T(5), C(3), R(2), 9, A0Y)};
        CHECK(i.dest().value == T(5));
        CHECK(i.src1(false).value == C(3));
        CHECK(i.src2(false).value == R(2));
        CHECK(i.operand_desc_id() == 9);
        CHECK(i.address_register_index() == A0Y);
    }
    {
        const isa::Instruction i{Ai(SLTI, O(7), R(4), C(60), 33, AL)};
        CHECK(i.dest().value == O(7));
        CHECK(i.src1(true).value == R(4));
        CHECK(i.src2(true).value == C(60));
        CHECK(i.operand_desc_id() == 33);
        CHECK(i.address_register_index() == AL);
    }
    {
        const isa::Instruction i{Cmp(CMP_GE, CMP_NE, C(9), R(1), 17)};
        CHECK(i.Effective() == isa::Opcode::CMP);
        CHECK(i.compare_x() == isa::CompareOp::GreaterEqual);
        CHECK(i.compare_y() == isa::CompareOp::NotEqual);
        CHECK(i.src1(false).value == C(9));
        CHECK(i.src2(false).value == R(1));
    }
    {
        const isa::Instruction i{Mad(T(6), R(3), C(40), V(7), 21, A0X)};
        CHECK(i.Effective() == isa::Opcode::MAD);
        CHECK(i.mad_dest().value == T(6));
        CHECK(i.mad_src1(false).value == R(3));
        CHECK(i.mad_src2(false).value == C(40));
        CHECK(i.mad_src3(false).value == V(7));
        CHECK(i.mad_operand_desc_id() == 21);
        CHECK(i.mad_address_register_index() == A0X);
        const isa::Instruction j{Madi(O(2), R(3), V(5), C(70), 30, AL)};
        CHECK(j.Effective() == isa::Opcode::MADI);
        CHECK(j.mad_src1(true).value == R(3));
        CHECK(j.mad_src2(true).value == V(5));
        CHECK(j.mad_src3(true).value == C(70));
    }
    {
        const isa::Instruction i{Flow(IFC, 1234, 77, COND_AND, 1, 0)};
        CHECK(i.dest_offset() == 1234);
        CHECK(i.num_instructions() == 77);
        CHECK(i.condition_op() == isa::ConditionOp::And);
        CHECK(i.refx());
        CHECK_FALSE(i.refy());
        const isa::Instruction u{Flow(IFU, 5, 1, 13)};
        CHECK(u.bool_uniform_id() == 13);
        const isa::Instruction lp{Flow(LOOP, 5, 0, 2)};
        CHECK(lp.int_uniform_id() == 2);
    }
    {
        Desc d;
        d.mask = Mask("xw");
        d.n1 = true;
        d.s1 = "wzyx";
        d.n2 = true;
        d.s2 = "yyzz";
        d.n3 = true;
        d.s3 = "xwxw";
        const isa::Swizzle sw{d.Word()};
        CHECK(sw.DestEnabled(0));
        CHECK_FALSE(sw.DestEnabled(1));
        CHECK_FALSE(sw.DestEnabled(2));
        CHECK(sw.DestEnabled(3));
        CHECK(sw.negate(1));
        CHECK(sw.negate(2));
        CHECK(sw.negate(3));
        const char* lanes[3] = {"wzyx", "yyzz", "xwxw"};
        for (u32 src = 1; src <= 3; ++src) {
            for (u32 c = 0; c < 4; ++c) {
                const char ch = lanes[src - 1][c];
                CHECK(sw.selector(src, c) == static_cast<u32>(ch == 'x' ? 0 : ch == 'y' ? 1 : ch == 'z' ? 2 : 3));
            }
        }
    }
}

// ================================================================================================
// The inputs: what the loader gives the vertex shader
// ================================================================================================

namespace {

void Le16(std::vector<u8>& b, int v) {
    b.push_back(static_cast<u8>(v & 0xFF));
    b.push_back(static_cast<u8>((v >> 8) & 0xFF));
}

// A draw on a CaptureHost: registers set field by field, the vertex shader mov o_n, v_n (n = 0-3).
struct Scene {
    CaptureHost host;
    std::unique_ptr<pica::PicaState> st = std::make_unique<pica::PicaState>();
    CpuVertexRunner runner;
    pica::DrawCall draw;
    Program vs;

    Scene() {
        pica::PicaState& s = *st;
        draw.state = st.get();
        draw.vertex_base_address = kBase;
        draw.geometry_reconfigured = true;
        s.regs[0x200] = (kBase >> 4) << 1;
        s.regs[kVs + 0x0D] = 0xF;
        MapFourOutputs(s);
        // attr0: UBYTE x3 (format 1, size-1 2); attr1: SHORT x2 (2, 1); attr2: FLOAT x2 (3, 1), a
        // default attribute; attr3: BYTE x4 (0, 3)
        s.regs[0x201] = 9u | (6u << 4) | (7u << 8) | (0xCu << 12);
        s.regs[0x202] = (3u << 28) | (1u << (16 + 2));
        // loader 0 holds attr0 (3 bytes at 0) and attr1 (aligned to 2: 4 bytes at 4): 8 bytes
        s.regs[0x203] = 0;
        s.regs[0x204] = 0u | (1u << 4);
        s.regs[0x205] = (8u << 16) | (2u << 28);
        // loader 1 holds attr3 (4 bytes) at 0x1000
        s.regs[0x206] = 0x1000;
        s.regs[0x207] = 3u;
        s.regs[0x208] = (4u << 16) | (1u << 28);
        // attribute n feeds the input register: attr0 -> 1, attr1 -> 0, attr2 -> 2, attr3 -> 3
        s.regs[kVs + 0x09] = 3;
        s.regs[kVs + 0x0B] = 1u | (0u << 4) | (2u << 8) | (3u << 12);
        s.default_attributes[2] = {{7, 8, 9, 10}};
        vs << A(MOV, O(0), V(0), 0, vs.Full()) << A(MOV, O(1), V(1), 0, vs.Full())
           << A(MOV, O(2), V(2), 0, vs.Full()) << A(MOV, O(3), V(3), 0, vs.Full()) << kEnd;
        // four vertices
        std::vector<u8> l0;
        for (const auto& v : std::vector<std::array<int, 5>>{
                 {200, 100, 50, -300, 400}, {1, 2, 3, 5, -6}, {255, 0, 128, 32767, -32768}, {10, 20, 30, 7, 8}}) {
            l0.push_back(static_cast<u8>(v[0]));
            l0.push_back(static_cast<u8>(v[1]));
            l0.push_back(static_cast<u8>(v[2]));
            l0.push_back(0);
            Le16(l0, v[3]);
            Le16(l0, v[4]);
        }
        REQUIRE(host.WriteBoth(kBase, l0.data(), static_cast<u32>(l0.size())));
        const std::vector<u8> l1 = {0xFF, 2, 0x80, 127, 1, 0xFE, 3, 0xFC, 0, 0, 0, 0, 9, 9, 9, 9};
        REQUIRE(host.WriteBoth(kBase + 0x1000, l1.data(), static_cast<u32>(l1.size())));
        std::copy(vs.code.begin(), vs.code.end(), st->vs.program_code.begin());
        std::copy(vs.descs.begin(), vs.descs.end(), st->vs.swizzle_data.begin());
    }

    void Arrays(u32 first, u32 count) {
        draw.kind = pica::DrawKind::Arrays;
        draw.vertex_offset = first;
        draw.num_vertices = count;
    }
    void Indexed(const std::vector<u32>& indices, bool u16) {
        draw.kind = pica::DrawKind::Indexed;
        draw.index_u16 = u16;
        draw.index_offset = 0x80000;
        draw.num_vertices = static_cast<u32>(indices.size());
        std::vector<u8> bytes;
        for (u32 i : indices) {
            u16 ? Le16(bytes, static_cast<int>(i)) : bytes.push_back(static_cast<u8>(i));
        }
        REQUIRE(host.WriteBoth(kBase + 0x80000, bytes.data(), static_cast<u32>(bytes.size())));
    }
    RunStatus Run(RunMode mode = RunMode::Full) {
        return runner.Run(host, draw, mode);
    }
    F16 Vertex(size_t i) const {
        return Outs(runner.output().vertices.at(i));
    }
};

const F16 kV0 = {-300, 400, 0, 1, 200, 100, 50, 1, 7, 8, 9, 10, -1, 2, -128, 127};
const F16 kV1 = {5, -6, 0, 1, 1, 2, 3, 1, 7, 8, 9, 10, 1, -2, 3, -4};
const F16 kV2 = {32767, -32768, 0, 1, 255, 0, 128, 1, 7, 8, 9, 10, 0, 0, 0, 0};
const F16 kV3 = {7, 8, 0, 1, 10, 20, 30, 1, 7, 8, 9, 10, 9, 9, 9, 9};

} // namespace

TEST_CASE("an Arrays draw: UBYTE/SHORT/BYTE are not normalised, a 3-component attribute has w = 1, lanes the "
          "attribute lacks are 0 (w 1), a default attribute is its default, the register map is applied") {
    Scene sc;
    sc.Arrays(0, 4);
    REQUIRE(sc.Run() == RunStatus::Ok);
    REQUIRE(sc.runner.output().vertices.size() == 4);
    CheckLanes(sc.Vertex(0), kV0);
    CheckLanes(sc.Vertex(1), kV1);
    CheckLanes(sc.Vertex(2), kV2);
    CheckLanes(sc.Vertex(3), kV3);
    CHECK(sc.runner.output().winding_before.empty());
    const Counters& c = sc.runner.counters();
    CHECK(c.draws == 1);
    CHECK(c.vertices == 4);
    CHECK(c.vs_invocations == 4);
    CHECK(c.vertices_out == 4);
    CHECK(c.unhandled == 0);
    // the loader was the N2-05 one, with the PICA's own layout
    CHECK(sc.runner.loader_counters().draws_loaded == 1);
}

TEST_CASE("an Arrays draw that starts at vertex 1") {
    Scene sc;
    sc.Arrays(1, 2);
    REQUIRE(sc.Run() == RunStatus::Ok);
    REQUIRE(sc.runner.output().vertices.size() == 2);
    CheckLanes(sc.Vertex(0), kV1);
    CheckLanes(sc.Vertex(1), kV2);
}

TEST_CASE("an Indexed draw: the order and repeats of the indices, the vertex cache saves the shading, "
          "8-bit and 16-bit indices") {
    for (bool u16 : {false, true}) {
        CAPTURE(u16);
        Scene sc;
        sc.Indexed({3, 2, 3, 2, 3}, u16);
        REQUIRE(sc.Run() == RunStatus::Ok);
        REQUIRE(sc.runner.output().vertices.size() == 5);
        CheckLanes(sc.Vertex(0), kV3);
        CheckLanes(sc.Vertex(1), kV2);
        CheckLanes(sc.Vertex(2), kV3);
        CheckLanes(sc.Vertex(3), kV2);
        CheckLanes(sc.Vertex(4), kV3);
        // five vertices submitted, two shaded (the cache served the other three)
        CHECK(sc.runner.counters().vertices == 5);
        CHECK(sc.runner.counters().vs_invocations == 2);
    }
    Scene sc;
    sc.Indexed({1, 0, 1}, false);
    REQUIRE(sc.Run() == RunStatus::Ok);
    CheckLanes(sc.Vertex(0), kV1);
    CheckLanes(sc.Vertex(1), kV0);
    CheckLanes(sc.Vertex(2), kV1);
}

TEST_CASE("an input register no attribute feeds keeps the unit's 0, where the loader's description has "
          "(0, 0, 0, 1) (Azahar's interpreter, N2-05's note)") {
    Scene sc;
    sc.st->regs[kVs + 0x09] = 0;      // one attribute only
    sc.st->regs[kVs + 0x0B] = 4u;     // attr0 -> register 4
    sc.vs = Program{};
    sc.vs << A(MOV, O(0), V(0), 0, sc.vs.Full()) << A(MOV, O(1), V(4), 0, sc.vs.Full()) << kEnd;
    std::copy(sc.vs.code.begin(), sc.vs.code.end(), sc.st->vs.program_code.begin());
    sc.Arrays(0, 1);
    REQUIRE(sc.Run() == RunStatus::Ok);
    CheckLanes(sc.Vertex(0), {0, 0, 0, 0, 200, 100, 50, 1, 0, 0, 0, 0, 0, 0, 0, 0});
}

TEST_CASE("the unit's temporaries and condition codes persist from vertex to vertex of a draw, and start "
          "at 0 for the next draw") {
    Scene sc;
    sc.vs = Program{};
    sc.st->vs.uniform_f[0] = {{1, 0, 0, 0}};
    // r0 += c0; o0 = r0: the n-th vertex of a draw shows n
    sc.vs << A(ADD, T(0), C(0), R(0), sc.vs.Full()) << A(MOV, O(0), R(0), 0, sc.vs.Full()) << kEnd;
    std::copy(sc.vs.code.begin(), sc.vs.code.end(), sc.st->vs.program_code.begin());
    sc.Arrays(0, 3);
    REQUIRE(sc.Run() == RunStatus::Ok);
    CHECK(sc.Vertex(0)[0] == 1);
    CHECK(sc.Vertex(1)[0] == 2);
    CHECK(sc.Vertex(2)[0] == 3);
    REQUIRE(sc.Run() == RunStatus::Ok);
    CHECK(sc.Vertex(0)[0] == 1); // a new ShaderUnit for the next draw
}

TEST_CASE("a draw whose vertices cannot be read is LoadFailed; a draw of no vertices is Ok and empty") {
    Scene sc;
    sc.draw.vertex_base_address = 0x70000000;
    sc.Arrays(0, 3);
    CHECK(sc.Run() == RunStatus::LoadFailed);
    CHECK(sc.runner.output().vertices.empty());
    CHECK(sc.runner.counters().load_failed == 1);
    CHECK(sc.runner.counters().unhandled == 0);
    CHECK_FALSE(sc.runner.last_error().empty());
    sc.draw.vertex_base_address = kBase;
    sc.Arrays(0, 0);
    CHECK(sc.Run() == RunStatus::Ok);
    CHECK(sc.runner.output().vertices.empty());
    CHECK(sc.runner.counters().draws == 2);
}

TEST_CASE("the output of one Run is replaced by the next, and the vector's storage is reused") {
    Scene sc;
    sc.Arrays(0, 4);
    REQUIRE(sc.Run() == RunStatus::Ok);
    const OutputVertex* first = sc.runner.output().vertices.data();
    sc.Arrays(0, 2);
    REQUIRE(sc.Run() == RunStatus::Ok);
    CHECK(sc.runner.output().vertices.size() == 2);
    CHECK(sc.runner.output().vertices.data() == first); // no new allocation
}

// ================================================================================================
// The geometry shader: through the command processor
// ================================================================================================

namespace {

// A command list as the command processor reads it: [value, header] pairs.
class CmdList {
public:
    CmdList& Write(u32 id, u32 value) {
        words_.push_back(value);
        words_.push_back(id | (0xFu << 16));
        return *this;
    }
    // A program, a swizzle table or uniforms of a shader unit (`base` is kVs or kGs).
    CmdList& Program(u32 base, const std::vector<u32>& code, const std::vector<u32>& descs) {
        Write(base + 0x1B, 0);
        for (u32 w : code) {
            Write(base + 0x1C, w);
        }
        Write(base + 0x25, 0);
        for (u32 w : descs) {
            Write(base + 0x26, w);
        }
        return *this;
    }
    // float32 uniforms from index `first`: four words per vector, w first (PackedAttribute, float32 mode)
    CmdList& Uniforms(u32 base, u32 first, const std::vector<std::array<float, 4>>& values) {
        Write(base + 0x10, first | (1u << 31));
        for (const auto& v : values) {
            for (int c = 3; c >= 0; --c) {
                Write(base + 0x11, std::bit_cast<u32>(v[static_cast<size_t>(c)]));
            }
        }
        return *this;
    }
    std::vector<u8> Bytes() const {
        std::vector<u32> w = words_;
        std::vector<u8> b(w.size() * 4);
        std::memcpy(b.data(), w.data(), b.size());
        return b;
    }

private:
    std::vector<u32> words_;
};

// The command processor with a capture host's memory and a recording backend.
struct Sim {
    CaptureHost host;
    pica::RecordingBackend backend;
    pica::CommandProcessor proc{host, backend};

    // Runs a command list; returns the index of the first draw it issued.
    size_t Submit(const CmdList& list) {
        const std::vector<u8> bytes = list.Bytes();
        REQUIRE(host.WriteBoth(kFcram + 0x1000, bytes.data(), static_cast<u32>(bytes.size())));
        const size_t first = backend.draws.size();
        proc.ProcessCmdList(kFcram + 0x1000, static_cast<u32>(bytes.size()));
        return first;
    }
    void Memory(u32 offset, const std::vector<u8>& bytes) {
        REQUIRE(host.WriteBoth(kBase + offset, bytes.data(), static_cast<u32>(bytes.size())));
    }
};

std::vector<u8> FloatBytes(const std::vector<std::array<float, 4>>& vectors) {
    std::vector<u8> b(vectors.size() * 16);
    std::memcpy(b.data(), vectors.data(), b.size());
    return b;
}

struct GsConfig {
    u32 mode = 0;           // 0 Point, 1 VariablePrimitive, 2 FixedPrimitive
    u32 attributes = 1;     // vertex attributes (float4 each), also the vertex shader's inputs
    u32 vs_outputs = 1;     // vertex shader outputs per vertex
    u32 gs_inputs = 1;      // Point mode: geometry shader inputs
    u32 fixed_vertices = 3; // FixedPrimitive
    u32 start_index = 0;    // FixedPrimitive: first float uniform of the buffer
    u32 main_vertices = 1;  // VariablePrimitive
    u32 gs_mask = 1;
    u32 out_attributes = 1; // the rasterizer's output attributes
};

// The registers of a draw with a geometry shader (pipeline, both shader units, rasterizer outputs),
// and the programs. The vertex shader mask is (1 << vs_outputs) - 1.
void ConfigureGs(CmdList& l, const GsConfig& c, const Program& vs, const Program& gs) {
    const bool variable = c.mode == 1;
    const bool to_uniform = c.mode != 0;
    l.Write(kRegUseGs, 2u | (variable ? 1u << 31 : 0u)); // first: the geometry unit is not a mirror
    l.Write(0x24A, c.vs_outputs - 1);
    l.Write(0x251, c.vs_outputs - 1);
    l.Write(0x252, c.mode | ((c.fixed_vertices - 1) << 8) | ((c.vs_outputs - 1) << 12) | (c.start_index << 16));
    l.Write(0x254, c.main_vertices - 1);
    l.Write(kGs + 0x09, (c.gs_inputs - 1) | (to_uniform ? 1u << 8 : 0u) | (0x08u << 24));
    l.Write(kGs + 0x0A, 0);
    l.Write(kGs + 0x0B, 0x76543210u);
    l.Write(kGs + 0x0C, 0xFEDCBA98u);
    l.Write(kGs + 0x0D, c.gs_mask);
    l.Write(kVs + 0x09, c.attributes - 1);
    l.Write(kVs + 0x0A, 0);
    l.Write(kVs + 0x0B, 0x76543210u);
    l.Write(kVs + 0x0C, 0xFEDCBA98u);
    l.Write(kVs + 0x0D, (1u << c.vs_outputs) - 1);
    // rasterizer outputs: attribute 0 -> position, 1 -> quaternion, 2 -> texcoords, 3 -> view
    l.Write(kRegOutTotal, c.out_attributes);
    l.Write(kRegOutMap + 0, 0u | (1u << 8) | (2u << 16) | (3u << 24));
    l.Write(kRegOutMap + 1, 4u | (5u << 8) | (6u << 16) | (7u << 24));
    l.Write(kRegOutMap + 2, 12u | (13u << 8) | (14u << 16) | (15u << 24));
    l.Write(kRegOutMap + 3, 16u | (18u << 8) | (19u << 16) | (20u << 24));
    l.Write(kRegTopology, 3u << 8); // Shader topology
    // the vertex arrays: `attributes` float4 attributes in one loader
    u32 formats = 0;
    u32 components = 0;
    for (u32 a = 0; a < c.attributes; ++a) {
        formats |= 0xFu << (4 * a); // FLOAT, 4 components
        components |= a << (4 * a);
    }
    l.Write(0x200, (kBase >> 4) << 1);
    l.Write(0x201, formats);
    l.Write(0x202, (c.attributes - 1) << 28);
    l.Write(0x203, 0);
    l.Write(0x204, components);
    l.Write(0x205, ((16 * c.attributes) << 16) | (c.attributes << 28));
    l.Program(kVs, vs.code, vs.descs);
    l.Program(kGs, gs.code, gs.descs);
}

// The N draws a list issued, run one after the other (the output is copied after each).
struct DrawResult {
    RunStatus status;
    std::vector<OutputVertex> vertices;
    std::vector<u32> winding;
};
DrawResult RunDraw(Sim& sim, CpuVertexRunner& runner, size_t index, RunMode mode = RunMode::Full) {
    REQUIRE(index < sim.backend.draws.size());
    DrawResult r;
    r.status = runner.Run(sim.host, sim.backend.draws[index].call, mode);
    r.vertices = runner.output().vertices;
    r.winding = runner.output().winding_before;
    return r;
}

std::array<float, 4> Pos(const OutputVertex& v) {
    return {v.pos[0], v.pos[1], v.pos[2], v.pos[3]};
}

// Programs: the vertex shader copies its attributes to its outputs.
Program PassVs(u32 n) {
    Program p;
    for (u32 i = 0; i < n; ++i) {
        p << A(MOV, O(i), V(i), 0, p.Full());
    }
    p << kEnd;
    return p;
}

} // namespace

TEST_CASE("a Point-mode geometry shader that emits a quad per point: vertices, order, winding events") {
    // c0-c3 are the corner offsets; per point the program emits the triangle (A, B, C), then the
    // triangle (D, B, C) with the winding flag set.
    Program gs;
    gs << SetEmit(0, 0, 0) << A(ADD, O(0), C(0), V(0), gs.Full()) << kEmit
       << SetEmit(1, 0, 0) << A(ADD, O(0), C(1), V(0), gs.Full()) << kEmit
       << SetEmit(2, 1, 0) << A(ADD, O(0), C(2), V(0), gs.Full()) << kEmit
       << SetEmit(0, 1, 1) << A(ADD, O(0), C(3), V(0), gs.Full()) << kEmit << kEnd;
    Sim sim;
    CmdList l;
    ConfigureGs(l, GsConfig{}, PassVs(1), gs);
    l.Uniforms(kGs, 0, {{-1, -1, 0, 0}, {1, -1, 0, 0}, {-1, 1, 0, 0}, {1, 1, 0, 0}});
    sim.Memory(0, FloatBytes({{10, 20, 5, 1}, {0, 0, 0, 1}}));
    l.Write(0x228, 2).Write(0x22A, 0).Write(0x22E, 0); // two points
    const size_t draw = sim.Submit(l);
    REQUIRE(sim.backend.draws.size() == draw + 1);
    CHECK(sim.backend.draws[draw].call.geometry_reconfigured);

    CpuVertexRunner runner;
    const DrawResult r = RunDraw(sim, runner, draw);
    REQUIRE(r.status == RunStatus::Ok);
    // the point (10, 20, 5, 1): A = +c0, B = +c1, C = +c2, D = +c3 (w: 1 + 0)
    const std::array<std::array<float, 4>, 6> p0 = {{{9, 19, 5, 1}, {11, 19, 5, 1}, {9, 21, 5, 1},
                                                     {11, 21, 5, 1}, {11, 19, 5, 1}, {9, 21, 5, 1}}};
    // the point (0, 0, 0, 1)
    const std::array<std::array<float, 4>, 6> p1 = {{{-1, -1, 0, 1}, {1, -1, 0, 1}, {-1, 1, 0, 1},
                                                     {1, 1, 0, 1}, {1, -1, 0, 1}, {-1, 1, 0, 1}}};
    REQUIRE(r.vertices.size() == 12);
    for (size_t i = 0; i < 6; ++i) {
        CAPTURE(i);
        CHECK(Pos(r.vertices[i]) == p0[i]);
        CHECK(Pos(r.vertices[6 + i]) == p1[i]);
    }
    // SetWinding before the three vertices of the second triangle of each point
    CHECK(r.winding == std::vector<u32>{3, 9});
    const Counters& c = runner.counters();
    CHECK(c.vertices == 2);
    CHECK(c.vs_invocations == 2);
    CHECK(c.gs_invocations == 2);
    CHECK(c.vertices_out == 12);
    CHECK(c.unhandled == 0);

    // N2-16's fallback: the vertex unit alone gives the two points and nothing else
    const DrawResult only = RunDraw(sim, runner, draw, RunMode::VertexOnly);
    REQUIRE(only.status == RunStatus::Ok);
    REQUIRE(only.vertices.size() == 2);
    CHECK(Pos(only.vertices[0]) == std::array<float, 4>{10, 20, 5, 1});
    CHECK(Pos(only.vertices[1]) == std::array<float, 4>{0, 0, 0, 1});
    CHECK(only.winding.empty());
    CHECK(runner.counters().gs_invocations == 2);
}

TEST_CASE("Point mode with two geometry shader inputs: a shader run per two vertices, Arrays and Indexed") {
    // inputs v0, v1; the triangle (v0, v1, v0 + v1)
    Program gs;
    gs << SetEmit(0, 0, 0) << A(MOV, O(0), V(0), 0, gs.Full()) << kEmit
       << SetEmit(1, 0, 0) << A(MOV, O(0), V(1), 0, gs.Full()) << kEmit
       << SetEmit(2, 1, 0) << A(ADD, O(0), V(0), V(1), gs.Full()) << kEmit << kEnd;
    GsConfig config;
    config.gs_inputs = 2;
    const std::vector<std::array<float, 4>> pts = {{1, 2, 3, 4}, {10, 20, 30, 40}, {100, 200, 300, 400}, {5, 6, 7, 8}};
    {
        Sim sim;
        CmdList l;
        ConfigureGs(l, config, PassVs(1), gs);
        sim.Memory(0, FloatBytes(pts));
        l.Write(0x228, 4).Write(0x22A, 0).Write(0x22E, 0);
        const size_t draw = sim.Submit(l);
        CpuVertexRunner runner;
        const DrawResult r = RunDraw(sim, runner, draw);
        REQUIRE(r.status == RunStatus::Ok);
        REQUIRE(r.vertices.size() == 6);
        const std::array<std::array<float, 4>, 6> want = {{pts[0], pts[1], {11, 22, 33, 44}, pts[2], pts[3], {105, 206, 307, 408}}};
        for (size_t i = 0; i < 6; ++i) {
            CAPTURE(i);
            CHECK(Pos(r.vertices[i]) == want[i]);
        }
        CHECK(runner.counters().gs_invocations == 2);
    }
    {
        // Indexed, with a repeated pair: the cache serves the second pair
        Sim sim;
        CmdList l;
        ConfigureGs(l, config, PassVs(1), gs);
        sim.Memory(0, FloatBytes(pts));
        sim.Memory(0x80000, {3, 2, 3, 2});
        l.Write(0x227, 0x80000).Write(0x228, 4).Write(0x22F, 0);
        const size_t draw = sim.Submit(l);
        CpuVertexRunner runner;
        const DrawResult r = RunDraw(sim, runner, draw);
        REQUIRE(r.status == RunStatus::Ok);
        REQUIRE(r.vertices.size() == 6);
        const std::array<float, 4> sum = {105, 206, 307, 408}; // pts[3] + pts[2]
        CHECK(Pos(r.vertices[0]) == pts[3]);
        CHECK(Pos(r.vertices[1]) == pts[2]);
        CHECK(Pos(r.vertices[2]) == sum);
        CHECK(Pos(r.vertices[3]) == pts[3]);
        CHECK(Pos(r.vertices[4]) == pts[2]);
        CHECK(Pos(r.vertices[5]) == sum);
        CHECK(runner.counters().vertices == 4);
        CHECK(runner.counters().vs_invocations == 2);
    }
}

TEST_CASE("FixedPrimitive mode: vertices buffer in the float uniforms from start_index on") {
    Program gs;
    gs << SetEmit(0, 0, 0) << A(MOV, O(0), C(10), 0, gs.Full()) << kEmit
       << SetEmit(1, 0, 0) << A(MOV, O(0), C(11), 0, gs.Full()) << kEmit
       << SetEmit(2, 1, 0) << A(MOV, O(0), C(12), 0, gs.Full()) << kEmit << kEnd;
    GsConfig config;
    config.mode = 2;
    config.fixed_vertices = 3;
    config.start_index = 10;
    Sim sim;
    CmdList l;
    ConfigureGs(l, config, PassVs(1), gs);
    std::vector<std::array<float, 4>> pts;
    for (int i = 0; i < 6; ++i) {
        pts.push_back({static_cast<float>(i), static_cast<float>(10 * i), 0, 1});
    }
    sim.Memory(0, FloatBytes(pts));
    l.Write(0x228, 6).Write(0x22A, 0).Write(0x22E, 0);
    const size_t draw = sim.Submit(l);
    CpuVertexRunner runner;
    const DrawResult r = RunDraw(sim, runner, draw);
    REQUIRE(r.status == RunStatus::Ok);
    REQUIRE(r.vertices.size() == 6);
    for (size_t i = 0; i < 6; ++i) {
        CAPTURE(i);
        CHECK(Pos(r.vertices[i]) == pts[i]);
    }
    CHECK(runner.counters().gs_invocations == 2);
    CHECK(r.winding.empty());
}

TEST_CASE("VariablePrimitive mode: the first index of a batch is its vertex count, then the vertices") {
    // two vertex shader outputs: o0 position, o1 texcoord; one "main" vertex receives both, the
    // others only the first. The uniforms: c0 = count, c1 = main position, c2 = main texcoord,
    // c3, c4 = the other positions. Triangle (c1, c3, c4), each with o1 = c2.
    Program gs;
    gs << SetEmit(0, 0, 0) << A(MOV, O(0), C(1), 0, gs.Full()) << A(MOV, O(1), C(2), 0, gs.Full()) << kEmit
       << SetEmit(1, 0, 0) << A(MOV, O(0), C(3), 0, gs.Full()) << A(MOV, O(1), C(2), 0, gs.Full()) << kEmit
       << SetEmit(2, 1, 0) << A(MOV, O(0), C(4), 0, gs.Full()) << A(MOV, O(1), C(2), 0, gs.Full()) << kEmit
       << kEnd;
    GsConfig config;
    config.mode = 1;
    config.attributes = 2;
    config.vs_outputs = 2;
    config.gs_mask = 3;
    config.out_attributes = 2;
    config.main_vertices = 1;
    Sim sim;
    CmdList l;
    ConfigureGs(l, config, PassVs(2), gs);
    std::vector<std::array<float, 4>> data; // vertex i: position, texcoord
    for (int i = 0; i < 6; ++i) {
        data.push_back({static_cast<float>(i), static_cast<float>(10 * i), 0, 1});
        data.push_back({static_cast<float>(100 + i), 0, 0, 0});
    }
    sim.Memory(0, FloatBytes(data));
    sim.Memory(0x80000, {3, 0, 1, 2, 3, 3, 4, 5});
    l.Write(0x227, 0x80000).Write(0x228, 8).Write(0x22F, 0);
    const size_t draw = sim.Submit(l);
    CpuVertexRunner runner;
    const DrawResult r = RunDraw(sim, runner, draw);
    REQUIRE(r.status == RunStatus::Ok);
    REQUIRE(r.vertices.size() == 6);
    const int firsts[2] = {0, 3};
    for (int batch = 0; batch < 2; ++batch) {
        for (int k = 0; k < 3; ++k) {
            CAPTURE(batch);
            CAPTURE(k);
            const OutputVertex& v = r.vertices[static_cast<size_t>(3 * batch + k)];
            const int vertex = firsts[batch] + k;
            CHECK(Pos(v) == std::array<float, 4>{static_cast<float>(vertex), static_cast<float>(10 * vertex), 0, 1});
            // o1 = the main vertex's texcoord -> the quaternion slots
            CHECK(v.quat[0] == static_cast<float>(100 + firsts[batch]));
        }
    }
    // the two counts are not vertices: six went through the vertex shader
    CHECK(runner.counters().vertices == 6);
    CHECK(runner.counters().gs_invocations == 2);

    // on an Arrays draw there is no index buffer to take the counts from
    Sim sim2;
    CmdList l2;
    ConfigureGs(l2, config, PassVs(2), gs);
    sim2.Memory(0, FloatBytes(data));
    l2.Write(0x228, 6).Write(0x22A, 0).Write(0x22E, 0);
    const size_t draw2 = sim2.Submit(l2);
    CpuVertexRunner runner2;
    CHECK(RunDraw(sim2, runner2, draw2).status == RunStatus::Unhandled);
    CHECK(runner2.counters().unhandled == 1);
}

TEST_CASE("the geometry unit keeps its two output banks between emits, invocations and draws") {
    // o1 is written once, before the first emit of the invocation (bank 0). Emit 1 uses bank 0,
    // emit 2 bank 1 (o1 never written there: 0), emit 3 bank 0 again (o1 still c0).
    Program gs;
    gs << SetEmit(0, 0, 0) << A(MOV, O(1), C(0), 0, gs.Full()) << A(MOV, O(0), C(1), 0, gs.Full()) << kEmit
       << SetEmit(1, 0, 0) << A(MOV, O(0), C(2), 0, gs.Full()) << kEmit
       << SetEmit(2, 1, 0) << A(MOV, O(0), C(3), 0, gs.Full()) << kEmit << kEnd;
    GsConfig config;
    config.gs_mask = 3;
    config.vs_outputs = 1;
    config.out_attributes = 2;
    Sim sim;
    CmdList l;
    ConfigureGs(l, config, PassVs(1), gs);
    l.Uniforms(kGs, 0, {{7, 7, 7, 7}, {1, 1, 1, 1}, {2, 2, 2, 2}, {3, 3, 3, 3}});
    sim.Memory(0, FloatBytes({{0, 0, 0, 1}, {0, 0, 0, 1}}));
    l.Write(0x228, 2).Write(0x22A, 0).Write(0x22E, 0);
    const size_t draw = sim.Submit(l);
    CpuVertexRunner runner;
    const DrawResult r = RunDraw(sim, runner, draw);
    REQUIRE(r.status == RunStatus::Ok);
    REQUIRE(r.vertices.size() == 6);
    // first invocation: (o0, o1) = (c1, c0), (c2, 0), (c3, c0)
    CHECK(r.vertices[0].pos[0] == 1);
    CHECK(r.vertices[0].quat[0] == 7);
    CHECK(r.vertices[1].pos[0] == 2);
    CHECK(r.vertices[1].quat[0] == 0);
    CHECK(r.vertices[2].pos[0] == 3);
    CHECK(r.vertices[2].quat[0] == 7);
    // second invocation starts on bank 1 (three emits switched it an odd number of times): the
    // o1 write goes to bank 1, emit 2 uses bank 0, whose o1 is still c0 from the first invocation
    CHECK(r.vertices[3].pos[0] == 1);
    CHECK(r.vertices[3].quat[0] == 7);
    CHECK(r.vertices[4].pos[0] == 2);
    CHECK(r.vertices[4].quat[0] == 7);
    CHECK(r.vertices[5].pos[0] == 3);
    CHECK(r.vertices[5].quat[0] == 7);
}

TEST_CASE("b15 is set after every geometry shader invocation and stays set until the game writes the bools") {
    // marker = 1 when b15 is set, 2 when it is not
    Program gs;
    gs << Flow(IFU, 2, 1, 15) << A(MOV, T(0), C(0), 0, gs.Full()) << A(MOV, T(0), C(1), 0, gs.Full())
       << SetEmit(0, 0, 0) << A(MOV, O(0), R(0), 0, gs.Full()) << kEmit
       << SetEmit(1, 0, 0) << A(MOV, O(0), R(0), 0, gs.Full()) << kEmit
       << SetEmit(2, 1, 0) << A(MOV, O(0), R(0), 0, gs.Full()) << kEmit << kEnd;
    Sim sim;
    CpuVertexRunner runner;
    CmdList setup;
    ConfigureGs(setup, GsConfig{}, PassVs(1), gs);
    setup.Uniforms(kGs, 0, {{1, 0, 0, 0}, {2, 0, 0, 0}});
    sim.Memory(0, FloatBytes({{0, 0, 0, 1}, {0, 0, 0, 1}}));
    auto marker = [&](const CmdList& list) {
        const size_t d = sim.Submit(list);
        std::vector<float> m;
        const DrawResult r = RunDraw(sim, runner, d);
        REQUIRE(r.status == RunStatus::Ok);
        for (size_t i = 0; i < r.vertices.size(); i += 3) {
            m.push_back(r.vertices[i].pos[0]);
        }
        return m;
    };
    auto draw_points = [](CmdList l, u32 count) {
        return l.Write(0x228, count).Write(0x22A, 0).Write(0x22E, 0);
    };
    // the game clears the bools (b15 false), then draws two points: 2 for the first, 1 for the second
    CmdList first = setup;
    first.Write(kGs + 0x00, 0);
    CHECK(marker(draw_points(first, 2)) == std::vector<float>{2, 1});
    // no bool write: b15 is still set from the last invocation
    CHECK(marker(draw_points(CmdList{}, 1)) == std::vector<float>{1});
    // the game writes the same bools again (the value is unchanged, the write counts): b15 is false
    CmdList again;
    again.Write(kGs + 0x00, 0);
    CHECK(marker(draw_points(again, 1)) == std::vector<float>{2});
    // a write between two draws the runner does not see (an accelerated draw): NoteDraw keeps it
    CmdList between;
    between.Write(kGs + 0x00, 0).Write(kRegUseGs, 0).Write(0x228, 3).Write(0x22A, 0).Write(0x22E, 0);
    const size_t plain = sim.Submit(between);
    runner.NoteDraw(sim.backend.draws[plain].call);
    CmdList gs_draw;
    gs_draw.Write(kRegUseGs, 2);
    CHECK(marker(draw_points(gs_draw, 1)) == std::vector<float>{2});
}

TEST_CASE("immediate-mode vertices: the pipeline's buffer lives between draws; reconfiguring empties it") {
    Program gs;
    gs << SetEmit(0, 0, 0) << A(MOV, O(0), V(0), 0, gs.Full()) << kEmit
       << SetEmit(1, 0, 0) << A(MOV, O(0), V(1), 0, gs.Full()) << kEmit
       << SetEmit(2, 1, 0) << A(ADD, O(0), V(0), V(1), gs.Full()) << kEmit << kEnd;
    GsConfig config;
    config.gs_inputs = 2;
    Sim sim;
    CmdList l;
    ConfigureGs(l, config, PassVs(1), gs);
    sim.Memory(0, FloatBytes({{0, 0, 0, 1}, {0, 0, 0, 1}}));
    l.Write(0x228, 0).Write(0x22A, 0).Write(0x22E, 0); // an empty draw: reconfigures the pipeline
    const size_t draw = sim.Submit(l);
    REQUIRE(sim.backend.draws[draw].state->geometry.mode == pica::GeometryMode::Point);
    auto state = std::make_unique<pica::PicaState>(*sim.backend.draws[draw].state);

    auto vertex = [&](CpuVertexRunner& runner, float x, bool reconfigured) {
        state->immediate.input_vertex[0] = {{x, 0, 0, 1}};
        pica::DrawCall d;
        d.kind = pica::DrawKind::Immediate;
        d.num_vertices = 1;
        d.geometry_reconfigured = reconfigured;
        d.state = state.get();
        const RunStatus s = runner.Run(sim.host, d);
        REQUIRE(s == RunStatus::Ok);
        std::vector<float> xs;
        for (const OutputVertex& v : runner.output().vertices) {
            xs.push_back(v.pos[0]);
        }
        return xs;
    };
    CpuVertexRunner runner;
    using X = std::vector<float>;
    CHECK(vertex(runner, 1, true) == X{});            // the first input register is filled
    CHECK(vertex(runner, 2, false) == X{1, 2, 3});    // the second: the shader runs
    CHECK(vertex(runner, 10, false) == X{});
    CHECK(vertex(runner, 20, false) == X{10, 20, 30});
    // a vertex that is only half of a pair, then a reconfiguration: the half is dropped
    CHECK(vertex(runner, 5, false) == X{});
    CHECK(vertex(runner, 6, true) == X{});
    CHECK(vertex(runner, 7, false) == X{6, 7, 13});
    CHECK(runner.counters().unhandled == 0);

    // a runner that was never configured takes the cursor and buffer of the INIT state
    CpuVertexRunner fresh;
    state->geometry.buffer_index = 1;
    state->geometry.attribute_buffer[0] = {{50, 0, 0, 1}};
    CHECK(vertex(fresh, 8, false) == X{50, 8, 58});
}

// ================================================================================================
// The dump check: the replay harness and its pieces
// ================================================================================================

namespace {

// The nearest-rank percentile (p in 0-100) of the samples; 0 for none.
double Percentile(std::vector<double> samples, double p) {
    if (samples.empty()) {
        return 0.0;
    }
    std::sort(samples.begin(), samples.end());
    size_t rank = static_cast<size_t>(std::ceil(p / 100.0 * static_cast<double>(samples.size())));
    rank = std::clamp<size_t>(rank, 1, samples.size());
    return samples[rank - 1];
}

// "140,668,2560,542" -> {140, 668, 2560, 542}. Spaces around a number are allowed. An empty text is
// "not set" (nullopt, no error); anything else that is not a comma separated list of unsigned
// numbers sets *error.
std::optional<std::vector<u64>> ParseExpectList(const char* text, std::string* error) {
    if (text == nullptr || *text == '\0') {
        return std::nullopt;
    }
    std::vector<u64> values;
    const std::string s = text;
    size_t at = 0;
    while (at <= s.size()) {
        size_t end = s.find(',', at);
        if (end == std::string::npos) {
            end = s.size();
        }
        std::string item = s.substr(at, end - at);
        at = end + 1;
        const size_t b = item.find_first_not_of(' ');
        const size_t e = item.find_last_not_of(' ');
        item = b == std::string::npos ? std::string() : item.substr(b, e - b + 1);
        if (item.empty() || item.find_first_not_of("0123456789") != std::string::npos || item.size() > 18) {
            if (error != nullptr) {
                *error = "not a comma separated list of numbers: " + s;
            }
            return std::nullopt;
        }
        values.push_back(std::stoull(item));
    }
    return values;
}

// What a capture's draws were, by the reason NeedsCpuPath gives.
struct ReasonTally {
    u64 draws = 0;     // every draw
    u64 selected = 0;  // NeedsCpuPath true
    u64 geometry = 0;  // ... by reason
    u64 immediate = 0;
    u64 refused = 0;
    u64 gs_draws = 0;  // geometry-shader draws, once per draw trigger / immediate batch (the census's count)

    void Add(const pica::DrawCall& draw, CpuReason reason) {
        ++draws;
        switch (reason) {
        case CpuReason::None:
            return;
        case CpuReason::GeometryShader:
            ++geometry;
            if (draw.kind != pica::DrawKind::Immediate || draw.geometry_reconfigured) {
                ++gs_draws;
            }
            break;
        case CpuReason::Immediate:
            ++immediate;
            break;
        case CpuReason::Refused:
            ++refused;
            break;
        }
        ++selected;
    }
    void Merge(const ReasonTally& o) {
        draws += o.draws;
        selected += o.selected;
        geometry += o.geometry;
        immediate += o.immediate;
        refused += o.refused;
        gs_draws += o.gs_draws;
    }
};

struct CaptureStats {
    ReasonTally tally;
    u64 frames = 0;
    u64 vertices_in = 0;
    u64 vertices_out = 0;
    u64 unhandled = 0;
    u64 load_failed = 0;
    u64 not_run_to_end = 0;
    std::vector<double> frame_ms; // the runner's time in each frame

    void Merge(const CaptureStats& o) {
        tally.Merge(o.tally);
        frames += o.frames;
        vertices_in += o.vertices_in;
        vertices_out += o.vertices_out;
        unhandled += o.unhandled;
        load_failed += o.load_failed;
        not_run_to_end += o.not_run_to_end;
        frame_ms.insert(frame_ms.end(), o.frame_ms.begin(), o.frame_ms.end());
    }
};

// The run in progress: a capture is replayed with one runner, and the frame spans are cut at the
// FRAME records (the player asks the Gpu for the frame's counters at each).
bool g_note_draws_default = true;

struct CheckRun {
    CpuVertexRunner runner;
    CaptureStats stats;
    bool frame_open = false;
    double span_ns = 0;
    bool note_draws = true; // the contract of cpu_vertex.h; a test turns it off to show it is needed

    void Reset() {
        runner.Reset();
        stats = CaptureStats{};
        frame_open = false;
        span_ns = 0;
        note_draws = g_note_draws_default;
    }
    void OnDraw(gpu::Host& host, const pica::DrawCall& draw) {
        const CpuReason reason = CpuPathReason(draw, /*assembler_empty=*/true);
        stats.tally.Add(draw, reason);
        if (reason == CpuReason::None) {
            // cpu_vertex.h: a draw that is not run is passed to NoteDraw, so the runner sees the
            // register writes whose dirty flags the processor clears after it.
            if (note_draws) {
                runner.NoteDraw(draw);
            }
            return;
        }
        const Counters before = runner.counters();
        const auto t0 = std::chrono::steady_clock::now();
        const RunStatus status = runner.Run(host, draw);
        const auto t1 = std::chrono::steady_clock::now();
        span_ns += static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        const Counters& after = runner.counters();
        stats.vertices_in += after.vertices - before.vertices;
        stats.vertices_out += after.vertices_out - before.vertices_out;
        stats.unhandled += after.unhandled - before.unhandled;
        stats.load_failed += after.load_failed - before.load_failed;
        if (status != RunStatus::Ok) {
            ++stats.not_run_to_end;
        }
    }
    void CloseSpan() {
        if (frame_open) {
            stats.frame_ms.push_back(span_ns / 1e6);
        }
        span_ns = 0;
    }
    void OnFrame() {
        CloseSpan();
        frame_open = true;
        ++stats.frames;
    }
    void Finish() {
        CloseSpan();
        frame_open = false;
    }
};
CheckRun g_run;

class CheckBackend final : public pica::Backend {
public:
    explicit CheckBackend(gpu::Host& host) : host_(host) {}
    void Draw(const pica::DrawCall& draw) override { g_run.OnDraw(host_, draw); }

private:
    gpu::Host& host_;
};

class CpuVertexCheckGpu final : public gpu::Gpu, public gpu::GpuDebug {
public:
    explicit CpuVertexCheckGpu(gpu::Host& host) : host_(host), backend_(host), processor_(host, backend_) {}

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
    u32 ReadReg(u32) override { return 0; }
    void SetBufferSwap(u32, const gpu::FrameBufferInfo&) override {}
    void SetColorFill(u32) override {}
    void FlushRegion(u32, u32) override {}
    void InvalidateRegion(u32, u32) override {}
    void VBlank(u32) override {}
    void LoadInitState(const gpu::InitState& init) override { processor_.LoadInitState(init); }
    // The player asks for the frame's counters at every FRAME record: the frame boundary.
    std::vector<gpu::Counter> TakeFrameCounters() override {
        g_run.OnFrame();
        return {};
    }

private:
    gpu::Host& host_;
    CheckBackend backend_;
    pica::CommandProcessor processor_;
};

} // namespace

MH4N_REGISTER(gpu::GpuFactory, "cpuvertex-check",
              [](gpu::Host& host, const Options&) -> std::unique_ptr<gpu::Gpu> {
                  return std::make_unique<CpuVertexCheckGpu>(host);
              });

namespace {

gpu_capture::PlayerResult Replay(const std::string& capture) {
    const Options options = ParseOptions(
        "capture-player", std::vector<std::string>{"--gpu", "cpuvertex-check", "--judge", "none", capture});
    // (no message with a text: an error could name the capture's path)
    REQUIRE(options.ok());
    gpu_capture::PlayerConfig config;
    std::string error;
    REQUIRE(gpu_capture::ConfigFromOptions(options, config, &error));
    return gpu_capture::RunPlayer(config, options);
}

std::vector<std::string> CapturePathsFromEnvironment(const char* value) {
#if defined(_WIN32)
    const char separator = ';';
#else
    const char separator = ':';
#endif
    std::vector<std::string> paths;
    const std::string text = value;
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

std::string FormatStats(const std::string& label, const CaptureStats& s) {
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer),
                  "cpuvertex %s: frames %llu, draws %llu, selected %llu (geometry shader %llu, immediate %llu, "
                  "refused %llu), geometry-shader draws selected %llu, vertices in %llu, vertices out %llu, "
                  "cpu_path_unhandled %llu, load failed %llu, not run to the end %llu, runner cpu ms/frame "
                  "p50 %.3f p95 %.3f",
                  label.c_str(), static_cast<unsigned long long>(s.frames),
                  static_cast<unsigned long long>(s.tally.draws), static_cast<unsigned long long>(s.tally.selected),
                  static_cast<unsigned long long>(s.tally.geometry), static_cast<unsigned long long>(s.tally.immediate),
                  static_cast<unsigned long long>(s.tally.refused), static_cast<unsigned long long>(s.tally.gs_draws),
                  static_cast<unsigned long long>(s.vertices_in), static_cast<unsigned long long>(s.vertices_out),
                  static_cast<unsigned long long>(s.unhandled), static_cast<unsigned long long>(s.load_failed),
                  static_cast<unsigned long long>(s.not_run_to_end), Percentile(s.frame_ms, 50),
                  Percentile(s.frame_ms, 95));
    return buffer;
}

// Replays every capture in order; the statistics of each, and (when `print`) their lines.
std::vector<CaptureStats> ReplayAll(const std::vector<std::string>& paths, bool print) {
    std::vector<CaptureStats> all;
    CaptureStats total;
    for (size_t i = 0; i < paths.size(); ++i) {
        g_run.Reset();
        const gpu_capture::PlayerResult result = Replay(paths[i]);
        g_run.Finish();
        // the ordinal and the player's exit status only: the player's text can name the capture's path
        CHECK_MESSAGE(result.error.empty(), "capture " << (i + 1) << " could not be replayed (player exit status "
                                                       << result.ExitStatus() << ")");
        all.push_back(g_run.stats);
        total.Merge(g_run.stats);
        if (print) {
            std::printf("%s\n", FormatStats("capture " + std::to_string(i + 1), g_run.stats).c_str());
        }
    }
    if (print) {
        std::printf("%s\n", FormatStats("total", total).c_str());
        std::fflush(stdout);
    }
    return all;
}

// ---- a synthetic capture ----------------------------------------------------------------------------------

void Put32(std::vector<u8>& b, u32 v) {
    for (int i = 0; i < 4; ++i) {
        b.push_back(static_cast<u8>(v >> (8 * i)));
    }
}

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
        std::vector<u8> none; // the PICA_REG record that follows a command list: empty
        Put32(none, 0);
        Record(gpu_capture::kPicaReg, none);
    }
    void Frame(u32 n) {
        frame = n;
        std::vector<u8> payload;
        Put32(payload, n);
        Record(gpu_capture::kFrame, payload);
    }
    std::string Save(const std::string& name) const {
        const auto path = fs::temp_directory_path() /
                          ("mh4n_gpu_cpuvertex_test_" + std::to_string(MH4N_GETPID()) + "_" + name + ".mgpu");
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
        return path.string();
    }
};

// Frame 1: a geometry-shader draw of two points (Point mode, the quad program), a plain List draw of
// 3 vertices (not selected), a List draw of 4 vertices (refused). Frame 2: a geometry-shader draw of one point.
std::string SyntheticCapture(const std::string& name) {
    Program gs;
    gs << SetEmit(0, 0, 0) << A(ADD, O(0), C(0), V(0), gs.Full()) << kEmit
       << SetEmit(1, 0, 0) << A(ADD, O(0), C(1), V(0), gs.Full()) << kEmit
       << SetEmit(2, 1, 0) << A(ADD, O(0), C(2), V(0), gs.Full()) << kEmit
       << SetEmit(0, 1, 1) << A(ADD, O(0), C(3), V(0), gs.Full()) << kEmit << kEnd;
    CmdList l1;
    ConfigureGs(l1, GsConfig{}, PassVs(1), gs);
    l1.Uniforms(kGs, 0, {{-1, -1, 0, 0}, {1, -1, 0, 0}, {-1, 1, 0, 0}, {1, 1, 0, 0}});
    l1.Write(0x228, 2).Write(0x22A, 0).Write(0x22E, 0);                         // A: 2 points, geometry shader
    l1.Write(kRegUseGs, 0).Write(kRegTopology, 0).Write(0x228, 3).Write(0x22E, 0); // B: List of 3
    l1.Write(0x228, 4).Write(0x22E, 0);                                         // C: List of 4
    CmdList l2;
    l2.Write(kRegUseGs, 2).Write(kRegTopology, 3u << 8).Write(0x228, 1).Write(0x22E, 0); // A': 1 point

    CaptureFile capture;
    capture.Init();
    capture.Mem(kBase, FloatBytes({{10, 20, 5, 1}, {0, 0, 0, 1}, {1, 1, 1, 1}, {2, 2, 2, 1}}));
    capture.Frame(1);
    capture.Mem(kFcram + 0x1000, l1.Bytes());
    capture.SubmitCmdList(0x14001000, static_cast<u32>(l1.Bytes().size()));
    capture.Frame(2);
    capture.Mem(kFcram + 0x2000, l2.Bytes());
    capture.SubmitCmdList(0x14002000, static_cast<u32>(l2.Bytes().size()));
    return capture.Save(name);
}

} // namespace

TEST_CASE("the percentile of the frame times is the nearest rank") {
    std::vector<double> ten;
    for (int i = 1; i <= 10; ++i) {
        ten.push_back(i);
    }
    CHECK(Percentile(ten, 50) == 5);   // ceil(0.5 * 10) = 5th
    CHECK(Percentile(ten, 95) == 10);  // ceil(9.5) = 10th
    CHECK(Percentile(ten, 10) == 1);
    CHECK(Percentile({7.5}, 50) == 7.5);
    CHECK(Percentile({7.5}, 95) == 7.5);
    CHECK(Percentile({}, 50) == 0);
    CHECK(Percentile({3, 1, 2}, 50) == 2); // unsorted input: ceil(1.5) = 2nd of 1, 2, 3
}

TEST_CASE("the expected geometry-shader draws are a comma separated list of numbers") {
    std::string error;
    auto v = ParseExpectList("140,668,2560,542", &error);
    REQUIRE(v.has_value());
    CHECK(*v == std::vector<u64>{140, 668, 2560, 542});
    v = ParseExpectList(" 1 , 22 ", &error);
    REQUIRE(v.has_value());
    CHECK(*v == std::vector<u64>{1, 22});
    CHECK(ParseExpectList("7", &error).value() == std::vector<u64>{7});
    CHECK_FALSE(ParseExpectList(nullptr, &error).has_value());
    CHECK_FALSE(ParseExpectList("", &error).has_value());
    for (const char* bad : {"1,,2", "a", "-1", "12x", "1,", ",1", "1.5"}) {
        error.clear();
        CAPTURE(bad);
        CHECK_FALSE(ParseExpectList(bad, &error).has_value());
        CHECK_FALSE(error.empty());
    }
}

TEST_CASE("the tally splits draws by reason and counts geometry-shader draws once per trigger or batch") {
    auto s = std::make_unique<pica::PicaState>();
    ReasonTally t;
    auto add = [&](pica::DrawKind kind, u32 count, u32 topology, u32 use_gs, bool reconfigured) {
        pica::DrawCall d = MakeDraw(*s, kind, count, topology, use_gs);
        d.geometry_reconfigured = reconfigured;
        t.Add(d, CpuPathReason(d, true));
    };
    add(pica::DrawKind::Arrays, 6, pica::kTopologyShader, pica::kUseGsYes, true);   // geometry
    add(pica::DrawKind::Indexed, 9, pica::kTopologyShader, pica::kUseGsYes, true);  // geometry
    add(pica::DrawKind::Immediate, 1, pica::kTopologyShader, pica::kUseGsYes, true);  // geometry, a batch start
    add(pica::DrawKind::Immediate, 1, pica::kTopologyShader, pica::kUseGsYes, false); // geometry, same batch
    add(pica::DrawKind::Immediate, 1, pica::kTopologyList, 0, true);                // immediate
    add(pica::DrawKind::Arrays, 4, pica::kTopologyList, 0, true);                   // refused
    add(pica::DrawKind::Arrays, 6, pica::kTopologyList, 0, true);                   // accelerated
    CHECK(t.draws == 7);
    CHECK(t.selected == 6);
    CHECK(t.geometry == 4);
    CHECK(t.immediate == 1);
    CHECK(t.refused == 1);
    CHECK(t.gs_draws == 3); // the second immediate vertex of a batch is not another draw
    CaptureStats a, b;
    a.tally = t;
    a.frame_ms = {1.0};
    b.tally = t;
    b.frame_ms = {2.0, 3.0};
    a.Merge(b);
    CHECK(a.tally.draws == 14);
    CHECK(a.frame_ms.size() == 3);
}

// ================================================================================================
// Quirks of the interpreter that Azahar's code has (mirrored on purpose)
// ================================================================================================

TEST_CASE("MOVA: an address offset above 127 or below -128 is 0; 127 is used as it is") {
    Machine m;
    m.Input(3, 127, -129, -200, 0);
    m.Uniform(9, 7, 7, 7, 7);
    m.Uniform(10, 9, 9, 9, 9);
    m.Uniform(66, 5, 5, 5, 5);
    auto lane = [&m](const char* l) {
        Desc d;
        d.mask = Mask("x");
        d.s1 = l;
        return m.D(d);
    };
    m.p << A(MOVA, 0, V(3), 0, lane("xxxx")) << A(MOV, O(0), C(10), 0, m.Full(), A0X)
        << A(MOVA, 0, V(3), 0, lane("yyyy")) << A(MOV, O(1), C(10), 0, m.Full(), A0X)
        << A(MOVA, 0, V(3), 0, lane("zzzz")) << A(MOV, O(2), C(10), 0, m.Full(), A0X) << kEnd;
    // 127: index (10 + 127) & 0x7F = 9 -> c9 (seven); -129 is below -128: offset 0 -> c10 (nine; used
    // as it is it would be (10 - 129) & 0x7F = 9); -200 likewise -> c10 (as it is: 66 -> five)
    CheckLanes(m.Run(), {7, 7, 7, 7, 9, 9, 9, 9, 9, 9, 9, 9, 0, 0, 0, 0});
}

TEST_CASE("CALL scopes that end together: three pop normally, the fourth does not restore the pc") {
    // The scopes are one-word functions [10k + 10, 10k + 11) that hold the call of the next one (the
    // return address of each is the end of the one before), so they all end on the same step.
    // "Hardware bug: when popping four CALL scopes at once, the last one doesn't update the program
    // counter" (Azahar's comment): the pc stays at the third return address.
    for (int scopes : {3, 4}) {
        CAPTURE(scopes);
        Machine m;
        Markers(m);
        m.p.code.assign(64, kNop);
        m.p.Full();
        for (int k = 0; k < scopes; ++k) {
            m.p.code[static_cast<size_t>(10 * k)] = Flow(CALL, static_cast<u32>(10 * (k + 1)), 1);
        }
        m.p.code[static_cast<size_t>(10 * scopes)] = A(ADD, T(0), C(0), R(0), m.Full());
        m.p.code[1] = A(MOV, O(1), C(0), 0, m.Full());  // where the first call returns to
        m.p.code[2] = kEnd;
        m.p.code[11] = A(MOV, O(0), C(0), 0, m.Full()); // where the second one does
        m.p.code[12] = kEnd;
        // 3 scopes: three pops update the pc, execution is back at word 1: o1 = c0.
        // 4 scopes: the fourth pop leaves the pc at the return address of the second, word 11: o0 = c0.
        const F16 want = scopes == 3 ? F16{0, 0, 0, 0, 1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0}
                                     : F16{1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        CheckLanes(m.Run(), want);
    }
}

TEST_CASE("the call stack holds four scopes: a fifth nested CALL overwrites the oldest (circular buffer)") {
    for (bool five : {true, false}) {
        CAPTURE(five);
        Machine m;
        Markers(m);
        m.p.code.assign(64, kNop);
        m.p.Full();
        auto add_r0 = [&m] { return A(ADD, T(0), C(0), R(0), m.Full()); };
        auto add_r1 = [&m] { return A(ADD, T(1), C(0), R(1), m.Full()); };
        m.p.code[0] = Flow(CALL, 10, 2);
        m.p.code[1] = A(MOV, O(0), R(0), 0, m.Full());
        m.p.code[2] = kEnd;
        m.p.code[10] = Flow(CALL, 20, 2);
        m.p.code[11] = add_r0();
        m.p.code[12] = A(MOV, O(2), C(0), 0, m.Full()); // what runs after the first function if its return is lost
        m.p.code[13] = kEnd;
        m.p.code[20] = Flow(CALL, 30, 2);
        m.p.code[21] = add_r0();
        m.p.code[30] = Flow(CALL, 40, 2);
        m.p.code[31] = add_r0();
        if (five) {
            m.p.code[40] = Flow(CALL, 50, 2);
            m.p.code[41] = add_r0();
            m.p.code[50] = add_r1();
            m.p.code[51] = add_r1();
        } else {
            m.p.code[40] = add_r0();
            m.p.code[41] = add_r0();
        }
        // five nested calls: the first one's entry is overwritten, its function ends without a return:
        //   r0 gets the adds of words 41, 31, 21 and 11 = 4 * c0, then execution runs on into word 12:
        //   o2 = c0, o0 stays 0.
        // four: all return; r0 = words 40, 41, 31, 21, 11 = 5 * c0; back at word 1: o0 = r0.
        const F16 want = five ? F16{0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 0, 0, 0, 0}
                              : F16{5, 10, 15, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        CheckLanes(m.Run(), want);
    }
}

// ================================================================================================
// The asserts of Azahar's geometry pipeline and rasterizer registers
// ================================================================================================

TEST_CASE("an inconsistent geometry pipeline set-up is Unhandled, once per assert of Azahar's Reconfigure") {
    struct Case {
        const char* name;
        u32 mode;
        std::function<void(CmdList&)> tweak;
    };
    constexpr u32 kShaderModeBits = 0x08u << 24;
    const std::vector<Case> cases = {
        {"point mode with variable_primitive", 0, [](CmdList& l) { l.Write(kRegUseGs, 2u | (1u << 31)); }},
        {"point mode with input_to_uniform", 0, [](CmdList& l) { l.Write(kGs + 0x09, kShaderModeBits | (1u << 8)); }},
        {"variable mode without variable_primitive", 1, [](CmdList& l) { l.Write(kRegUseGs, 2u); }},
        {"variable mode without input_to_uniform", 1, [](CmdList& l) { l.Write(kGs + 0x09, kShaderModeBits); }},
        {"fixed mode with variable_primitive", 2, [](CmdList& l) { l.Write(kRegUseGs, 2u | (1u << 31)); }},
        {"fixed mode without input_to_uniform", 2, [](CmdList& l) { l.Write(kGs + 0x09, kShaderModeBits); }},
        {"fixed mode with stride_minus_1 + 1 != the vertex shader outputs", 2,
         [](CmdList& l) { l.Write(0x252, 2u | (2u << 8) | (3u << 12) | (10u << 16)); }},
        {"unit 3 is not in geometry shader mode", 0, [](CmdList& l) { l.Write(kGs + 0x09, 0xA0u << 24); }},
        {"the two vertex shader output counts differ", 0, [](CmdList& l) { l.Write(0x251, 3); }},
        {"use_gs is neither No nor Yes", 0, [](CmdList& l) { l.Write(kRegUseGs, 1u); }},
        {"point mode whose inputs are not a multiple of the outputs", 0,
         [](CmdList& l) { l.Write(0x24A, 1).Write(0x251, 1); }}, // 2 outputs, 1 input
    };
    Program gs;
    gs << kEnd;
    for (const Case& c : cases) {
        CAPTURE(c.name);
        GsConfig config;
        config.mode = c.mode;
        config.start_index = 10;
        Sim sim;
        CmdList l;
        ConfigureGs(l, config, PassVs(1), gs);
        c.tweak(l);
        sim.Memory(0, FloatBytes({{1, 2, 3, 1}, {1, 2, 3, 1}, {1, 2, 3, 1}, {1, 2, 3, 1}, {1, 2, 3, 1}, {1, 2, 3, 1}}));
        l.Write(0x228, 6).Write(0x22A, 0).Write(0x22E, 0);
        const size_t draw = sim.Submit(l);
        CpuVertexRunner runner;
        const DrawResult r = RunDraw(sim, runner, draw);
        CHECK(r.status == RunStatus::Unhandled);
        CHECK(r.vertices.empty());
        CHECK(runner.counters().unhandled == 1);
    }
    // the same set-ups, consistent, run (Point mode: 6 vertices, 1 input; Fixed: 6 = 2 x 3; Variable
    // needs an index buffer, see its own case)
    for (u32 mode : {0u, 2u}) {
        CAPTURE(mode);
        GsConfig config;
        config.mode = mode;
        config.start_index = 10;
        Sim sim;
        CmdList l;
        ConfigureGs(l, config, PassVs(1), gs);
        sim.Memory(0, FloatBytes({{1, 2, 3, 1}, {1, 2, 3, 1}, {1, 2, 3, 1}, {1, 2, 3, 1}, {1, 2, 3, 1}, {1, 2, 3, 1}}));
        l.Write(0x228, 6).Write(0x22A, 0).Write(0x22E, 0);
        const size_t draw = sim.Submit(l);
        CpuVertexRunner runner;
        CHECK(RunDraw(sim, runner, draw).status == RunStatus::Ok);
    }
}

TEST_CASE("RasterizerRegs::ValidateSemantics: an output semantic of 24 to 30 refuses an Arrays/Indexed draw, "
          "not an immediate vertex") {
    {
        Scene sc;
        sc.st->regs[kRegOutMap] = 24u | (1u << 8) | (2u << 16) | (3u << 24);
        sc.Arrays(0, 1);
        CHECK(sc.Run() == RunStatus::Unhandled);
        CHECK(sc.runner.counters().unhandled == 1);
        CHECK(sc.runner.output().vertices.empty());
        // 31 (invalid: the component is not mapped) and an attribute past vs_output_total pass
        sc.st->regs[kRegOutMap] = 31u | (1u << 8) | (2u << 16) | (3u << 24);
        sc.st->regs[kRegOutMap + 5] = 27u;
        CHECK(sc.Run() == RunStatus::Ok);
    }
    {
        // DrawImmediate does not validate: the value goes to a slot past the end of OutputVertex
        Machine m;
        m.st->regs[kRegOutMap] = 24u | (1u << 8) | (2u << 16) | (3u << 24);
        m.Input(0, 1, 2, 3, 4);
        m.p << A(MOV, O(0), V(0), 0, m.Full()) << kEnd;
        CHECK(m.RunRaw() == RunStatus::Ok);
        CHECK(m.runner.output().vertices.at(0).pos[0] == 1); // the unmapped slot 0 is 1.0 (nothing maps x)
    }
}

// ================================================================================================
// NoteDraw
// ================================================================================================

namespace {

// The geometry shader emits a triangle only when b15 is clear: the number of vertices out says what
// b15 was. Frame 1: the game clears the bools, one point (3 vertices out, and b15 is set by the
// invocation). Frame 2: the game writes the same bools again, then a plain List draw the runner does
// not run (accelerated), then a geometry shader draw of one point.
std::string B15Capture(const std::string& name) {
    Program gs;
    gs << Flow(JMPU, 10, 0, 15)                                        // 0: skip everything when b15
       << SetEmit(0, 0, 0) << A(MOV, O(0), V(0), 0, gs.Full()) << kEmit  // 1-3
       << SetEmit(1, 0, 0) << A(MOV, O(0), V(0), 0, gs.Full()) << kEmit  // 4-6
       << SetEmit(2, 1, 0) << A(MOV, O(0), V(0), 0, gs.Full()) << kEmit  // 7-9
       << kEnd;                                                         // 10
    CmdList l1;
    ConfigureGs(l1, GsConfig{}, PassVs(1), gs);
    l1.Write(kGs + 0x00, 0);
    l1.Write(0x228, 1).Write(0x22A, 0).Write(0x22E, 0);
    CmdList l2;
    l2.Write(kGs + 0x00, 0);
    l2.Write(kRegUseGs, 0).Write(kRegTopology, 0).Write(0x228, 3).Write(0x22E, 0);       // accelerated
    l2.Write(kRegUseGs, 2).Write(kRegTopology, 3u << 8).Write(0x228, 1).Write(0x22E, 0); // geometry shader

    CaptureFile capture;
    capture.Init();
    capture.Mem(kBase, FloatBytes({{1, 2, 3, 1}, {4, 5, 6, 1}, {7, 8, 9, 1}}));
    capture.Frame(1);
    capture.Mem(kFcram + 0x1000, l1.Bytes());
    capture.SubmitCmdList(0x14001000, static_cast<u32>(l1.Bytes().size()));
    capture.Frame(2);
    capture.Mem(kFcram + 0x2000, l2.Bytes());
    capture.SubmitCmdList(0x14002000, static_cast<u32>(l2.Bytes().size()));
    return capture.Save(name);
}

} // namespace

TEST_CASE("the harness passes the draws it does not run to NoteDraw: a same-value rewrite of the geometry "
          "bools across an accelerated draw resets b15") {
    const std::string path = B15Capture("b15");
    {
        const std::vector<CaptureStats> all = ReplayAll({path}, false);
        REQUIRE(all.size() == 1);
        const CaptureStats& s = all[0];
        CHECK(s.tally.draws == 3);
        CHECK(s.tally.selected == 2);
        CHECK(s.tally.geometry == 2);
        CHECK(s.vertices_in == 2);
        CHECK(s.vertices_out == 6); // both points emit their triangle: b15 was clear for each
        CHECK(s.unhandled == 0);
    }
    {
        // The contract is needed: without NoteDraw the processor's cleared dirty flag hides the
        // rewrite (the value is unchanged), b15 stays set from frame 1 and the second point emits nothing.
        g_note_draws_default = false;
        const std::vector<CaptureStats> all = ReplayAll({path}, false);
        g_note_draws_default = true;
        REQUIRE(all.size() == 1);
        CHECK(all[0].vertices_out == 3);
    }
    fs::remove(path);
}

TEST_CASE("the replay harness on a synthetic capture: reasons, vertices, frames, no unhandled draw") {
    const std::string path = SyntheticCapture("replay");
    const std::vector<CaptureStats> all = ReplayAll({path}, false);
    fs::remove(path);
    REQUIRE(all.size() == 1);
    const CaptureStats& s = all[0];
    CHECK(s.frames == 2);
    CHECK(s.frame_ms.size() == 2); // one time per frame (the span of each FRAME record to the next / the end)
    CHECK(s.tally.draws == 4);
    CHECK(s.tally.selected == 3);
    CHECK(s.tally.geometry == 2);
    CHECK(s.tally.immediate == 0);
    CHECK(s.tally.refused == 1);
    CHECK(s.tally.gs_draws == 2);
    // A: 2 points in, 12 out; C: 4 vertices in and out (no geometry shader); A': 1 point in, 6 out
    CHECK(s.vertices_in == 2 + 4 + 1);
    CHECK(s.vertices_out == 12 + 4 + 6);
    CHECK(s.unhandled == 0);
    CHECK(s.load_failed == 0);
    CHECK(s.not_run_to_end == 0);
    for (double ms : s.frame_ms) {
        CHECK(ms >= 0.0);
    }
    // the line the check prints has the counts in it
    const std::string line = FormatStats("capture 1", s);
    CHECK(line.find("frames 2, draws 4, selected 3 (geometry shader 2, immediate 0, refused 1), "
                    "geometry-shader draws selected 2, vertices in 7, vertices out 22, cpu_path_unhandled 0") !=
          std::string::npos);
}

TEST_CASE("a capture that does not reach the end of a selected draw is counted") {
    // a geometry shader draw with no geometry shader program: all words 0 = an ADD with an empty write
    // mask, which ends at the last word: the draw runs. To get a draw that fails, use a program with DST.
    Program gs;
    gs << A(DST, O(0), V(0), V(1), gs.Full()) << kEnd;
    CmdList l;
    ConfigureGs(l, GsConfig{}, PassVs(1), gs);
    l.Write(0x228, 1).Write(0x22A, 0).Write(0x22E, 0);
    CaptureFile capture;
    capture.Init();
    capture.Mem(kBase, FloatBytes({{1, 2, 3, 1}}));
    capture.Frame(1);
    capture.Mem(kFcram + 0x1000, l.Bytes());
    capture.SubmitCmdList(0x14001000, static_cast<u32>(l.Bytes().size()));
    const std::string path = capture.Save("unhandled");
    const std::vector<CaptureStats> all = ReplayAll({path}, false);
    fs::remove(path);
    REQUIRE(all.size() == 1);
    CHECK(all[0].unhandled == 1);
    CHECK(all[0].not_run_to_end == 1);
}

TEST_CASE("(with the dump) every geometry-shader draw of the captures runs, and the counts are the census's") {
    const char* value = std::getenv("MH4N_CPUVERTEX_CAPTURES");
    if (value == nullptr || *value == '\0') {
        WARN_MESSAGE(false, "skipped: set MH4N_CPUVERTEX_CAPTURES to .mgpu capture files or directories "
                            "(separated by the platform's path-list separator) to run the dump check");
        return;
    }
    const std::vector<std::string> paths = CapturePathsFromEnvironment(value);
    REQUIRE_MESSAGE(!paths.empty(), "the capture list names no capture");
    std::string error;
    const std::optional<std::vector<u64>> expect = ParseExpectList(std::getenv("MH4N_CPUVERTEX_EXPECT_GS"), &error);
    if (std::getenv("MH4N_CPUVERTEX_EXPECT_GS") != nullptr && *std::getenv("MH4N_CPUVERTEX_EXPECT_GS") != '\0') {
        REQUIRE_MESSAGE(expect.has_value(), error);
        REQUIRE_MESSAGE(expect->size() == paths.size(), "MH4N_CPUVERTEX_EXPECT_GS lists " << expect->size()
                                                         << " numbers for " << paths.size() << " captures");
    }
    const std::vector<CaptureStats> all = ReplayAll(paths, true);
    REQUIRE(all.size() == paths.size());
    for (size_t i = 0; i < all.size(); ++i) {
        CAPTURE(i);
        CHECK(all[i].unhandled == 0);
        CHECK(all[i].load_failed == 0);
        CHECK(all[i].not_run_to_end == 0);
        if (expect.has_value()) {
            REQUIRE(all[i].tally.gs_draws == (*expect)[i]);
        }
    }
}
