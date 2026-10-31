// Derived from Azahar src/video_core/shader/shader_interpreter.cpp, src/video_core/pica_types.h,
// src/video_core/pica/shader_unit.cpp, shader_unit.h, output_vertex.cpp, regs_shader.h, regs_rasterizer.h,
// regs_pipeline.h, pica_core.cpp and src/video_core/shader/generator/glsl_shader_gen.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; each of those files says "Licensed under
// GPLv2 or any later version"): the CPU interpreter below (namespace oracle) is a port of RunInterpreter
// with the float rules of Pica::Float's operator*, ShaderUnit::LoadInput / WriteOutput and the OutputVertex
// semantic fill; the register positions the tests write are those of ShaderRegs, RasterizerRegs and
// PipelineRegs, and the command list writer follows the loop of PicaCore::ProcessCmdList.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (shader_interpreter.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (pica_types.h)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (shader_unit.cpp, shader_unit.h,
//   pica_core.cpp, glsl_shader_gen.cpp)
// Copyright 2023 Citra Emulator Project (output_vertex.cpp)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_shader.h, regs_pipeline.h)
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (regs_rasterizer.h)
//
// mh4n_gpu_shader (N2-06): the PICA vertex shader as SPIR-V.
//
// The oracle is a CPU port of Azahar's shader interpreter (namespace oracle, below). Every case builds
// a synthetic program (hand-written with the assembler in namespace asmb: no game data), translates it,
// runs the SPIR-V through the backend core on whatever Vulkan device the machine has (lavapipe in the
// cloud) for 64 vertices, and compares every output attribute with the interpreter's: equal when both
// are NaN, or the same infinity, or |a - b| <= 2^-16 * max(1, |a|, |b|). The translated shader runs with
// the test hook of vertex_shader.h (`capture_outputs`), which writes each vertex's raw output attributes
// and the seven varyings to a storage buffer. A test that needs a device skips with a message without
// one, and fails under MH4N_REQUIRE_VULKAN=1.
//
// The dump check: with MH4N_SHADER_CAPTURES naming .mgpu capture files (or directories that hold them,
// several entries separated by the platform's path-list separator) the last case replays each capture
// through the PICA processor (N2-04) and, at every draw, runs the draw's vertex program on the first 64
// vertices with the oracle and the translated shader. It prints counts only, never buffer content.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "gpu/pica/command_processor.h"
#include "gpu/pica/recording_backend.h"
#include "gpu/shader/glsl_decompiler.h"
#include "gpu/shader/pica_isa.h"
#include "gpu/shader/vertex_shader.h"
#include "gpu/vertex/vertex_loader.h"
#include "gpu_capture/capture_host.h"
#include "gpu_capture/mgpu.h"
#include "gpu_capture/player.h"
#include "mh4n/gpu.h"
#include "mh4n/gpu_debug.h"
#include "mh4n/options.h"
#include "mh4n/registry.h"
#include "support/vulkan_device.h"

#if defined(_WIN32)
#include <process.h>
#define MH4N_GETPID _getpid
#else
#include <unistd.h>
#define MH4N_GETPID getpid
#endif

// `using namespace mh4n` would make Result ambiguous (the 3DS result code of mh4n/types.h): name what is used.
using mh4n::s32;
using mh4n::u16;
using mh4n::u32;
using mh4n::u64;
using mh4n::u8;
namespace test = mh4n::test;
namespace rhi = mh4n::gpu::rhi;
namespace pica = mh4n::gpu::pica;
namespace shader = mh4n::gpu::shader;
namespace vertex = mh4n::gpu::vertex;
namespace isa = mh4n::gpu::shader::isa;
namespace fs = std::filesystem;
using mh4n::gpu_capture::CaptureHost;

namespace {

[[maybe_unused]] constexpr float kInf = std::numeric_limits<float>::infinity();
[[maybe_unused]] constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

// ================================================================================================
// The assembler: synthetic PICA programs
// ================================================================================================

namespace asmb {

// Source and destination register numbers.
[[maybe_unused]] constexpr u32 V(u32 n) { return n; }        // input v0-v15
[[maybe_unused]] constexpr u32 R(u32 n) { return 0x10 + n; } // temporary r0-r15 (a source, or a destination)
[[maybe_unused]] constexpr u32 C(u32 n) { return 0x20 + n; } // float uniform c0-c95
[[maybe_unused]] constexpr u32 O(u32 n) { return n; }        // output o0-o15 (a destination)

u32 Sel(const char* s) { // "xyzw" -> the 8-bit selector, component 0 in the highest 2 bits
    u32 v = 0;
    for (int c = 0; c < 4; ++c) {
        const u32 lane = static_cast<u32>(std::string("xyzw").find(s[c]));
        v |= lane << (2 * (3 - c));
    }
    return v;
}

// An operand descriptor: destination mask ("xyzw" with '_' for a masked lane), per source a swizzle
// and a negate flag.
struct Desc {
    const char* mask = "xyzw";
    const char* s1 = "xyzw";
    bool n1 = false;
    const char* s2 = "xyzw";
    bool n2 = false;
    const char* s3 = "xyzw";
    bool n3 = false;
    u32 Word() const {
        u32 w = 0;
        for (int i = 0; i < 4; ++i) {
            if (mask[i] != '_') {
                w |= 0x8u >> i;
            }
        }
        w |= (n1 ? 1u : 0u) << 4;
        w |= Sel(s1) << 5;
        w |= (n2 ? 1u : 0u) << 13;
        w |= Sel(s2) << 14;
        w |= (n3 ? 1u : 0u) << 22;
        w |= Sel(s3) << 23;
        return w;
    }
};

enum Cc : u32 { Or = 0, And = 1, JustX = 2, JustY = 3 };
struct Cond {
    Cc op = JustX;
    bool refx = true;
    bool refy = true;
    u32 Bits() const { return (static_cast<u32>(op) << 22) | ((refy ? 1u : 0u) << 24) | ((refx ? 1u : 0u) << 25); }
};

[[maybe_unused]] constexpr u32 kAdd = 0x00, kDp3 = 0x01, kDp4 = 0x02, kDph = 0x03, kDst = 0x04, kEx2 = 0x05, kLg2 = 0x06,
              kLit = 0x07, kMul = 0x08, kSge = 0x09, kSlt = 0x0A, kFlr = 0x0B, kMax = 0x0C, kMin = 0x0D,
              kRcp = 0x0E, kRsq = 0x0F, kMova = 0x12, kMov = 0x13, kDphi = 0x18, kDsti = 0x19, kSgei = 0x1A,
              kSlti = 0x1B, kBreak = 0x20, kNop = 0x21, kEnd = 0x22, kBreakc = 0x23, kCall = 0x24,
              kCallc = 0x25, kCallu = 0x26, kIfu = 0x27, kIfc = 0x28, kLoop = 0x29, kEmit = 0x2A,
              kSetemit = 0x2B, kJmpc = 0x2C, kJmpu = 0x2D, kCmp = 0x2E, kMadi = 0x30, kMad = 0x38;
enum CmpOp : u32 { Eq = 0, Ne = 1, Lt = 2, Le = 3, Gt = 4, Ge = 5 };

class Asm {
public:
    std::vector<u32> code;
    std::vector<u32> descs;

    u32 D(const Desc& d) { return D(d.Word()); }
    u32 D(u32 word) {
        for (size_t i = 0; i < descs.size(); ++i) {
            if (descs[i] == word) {
                return static_cast<u32>(i);
            }
        }
        descs.push_back(word);
        return static_cast<u32>(descs.size() - 1);
    }
    u32 Pc() const { return static_cast<u32>(code.size()); }
    Asm& Raw(u32 word) {
        code.push_back(word);
        return *this;
    }

    // dest = op(src1, src2); `inverted` for the "i" opcodes (the second source has the wide field).
    Asm& Op(u32 op, u32 dest, u32 src1, u32 src2, const Desc& d = {}, u32 addr = 0) {
        const u32 id = D(d);
        REQUIRE(id < 128);
        const bool inverted = op == kDphi || op == kDsti || op == kSgei || op == kSlti;
        // field widths: one source has 7 bits (it may be a uniform), the other 5
        const bool fits = inverted ? (src1 < 32 && src2 < 128) : (src1 < 128 && src2 < 32);
        REQUIRE_MESSAGE(fits, "an operand does not fit its field");
        REQUIRE(dest < 32);
        u32 w = (op << 26) | (dest << 21) | (addr << 19) | id;
        w |= inverted ? ((src1 & 0x1F) << 14) | ((src2 & 0x7F) << 7) : ((src1 & 0x7F) << 12) | ((src2 & 0x1F) << 7);
        return Raw(w);
    }
    Asm& Op1(u32 op, u32 dest, u32 src1, const Desc& d = {}, u32 addr = 0) { return Op(op, dest, src1, 0, d, addr); }
    Asm& Cmp(CmpOp x, CmpOp y, u32 src1, u32 src2, const Desc& d = {}, u32 addr = 0) {
        const u32 id = D(d);
        REQUIRE(id < 128);
        const bool fits = src1 < 128 && src2 < 32;
        REQUIRE_MESSAGE(fits, "an operand does not fit its field");
        return Raw((kCmp << 26) | (x << 24) | (y << 21) | (addr << 19) | ((src1 & 0x7F) << 12) | ((src2 & 0x1F) << 7) | id);
    }
    // dest = src1 * src2 + src3; the operand descriptor id has 5 bits.
    Asm& Mad(u32 dest, u32 src1, u32 src2, u32 src3, const Desc& d = {}, u32 addr = 0) {
        const u32 id = D(d);
        REQUIRE(id < 32);
        const bool fits = src1 < 32 && src2 < 128 && src3 < 32 && dest < 32;
        REQUIRE_MESSAGE(fits, "an operand does not fit its field");
        return Raw((kMad << 26) | (dest << 24) | (addr << 22) | ((src1 & 0x1F) << 17) | ((src2 & 0x7F) << 10) |
                   ((src3 & 0x1F) << 5) | id);
    }
    Asm& Madi(u32 dest, u32 src1, u32 src2, u32 src3, const Desc& d = {}, u32 addr = 0) {
        const u32 id = D(d);
        REQUIRE(id < 32);
        const bool fits = src1 < 32 && src2 < 32 && src3 < 128 && dest < 32;
        REQUIRE_MESSAGE(fits, "an operand does not fit its field");
        return Raw((kMadi << 26) | (dest << 24) | (addr << 22) | ((src1 & 0x1F) << 17) | ((src2 & 0x1F) << 12) |
                   ((src3 & 0x7F) << 5) | id);
    }

    // ---- flow control ----
    static u32 Flow(u32 op, u32 dest, u32 num, u32 extra) { return (op << 26) | (dest << 10) | num | extra; }
    Asm& End() { return Raw(kEnd << 26); }
    Asm& Nop() { return Raw(kNop << 26); }
    Asm& Break() { return Raw(kBreak << 26); }
    Asm& Breakc(Cond c) { return Raw(Flow(kBreakc, 0, 0, c.Bits())); }
    Asm& Emit() { return Raw(kEmit << 26); }
    Asm& Setemit(u32 vertex_id, bool prim, bool winding) {
        return Raw((kSetemit << 26) | ((winding ? 1u : 0u) << 22) | ((prim ? 1u : 0u) << 23) | (vertex_id << 24));
    }
    Asm& Jmpc(Cond c, u32 dest) { return Raw(Flow(kJmpc, dest, 0, c.Bits())); }
    Asm& Jmpu(u32 bool_id, u32 dest, bool jump_when_false = false) {
        return Raw(Flow(kJmpu, dest, jump_when_false ? 1 : 0, bool_id << 22));
    }

    // Structured forms that patch their own offsets.
    struct Open {
        u32 at;
        u32 else_at = 0;
    };
    std::vector<Open> stack;
    Asm& BeginIfu(u32 bool_id) {
        stack.push_back({Pc()});
        return Raw(Flow(kIfu, 0, 0, bool_id << 22));
    }
    Asm& BeginIfc(Cond c) {
        stack.push_back({Pc()});
        return Raw(Flow(kIfc, 0, 0, c.Bits()));
    }
    Asm& Else() {
        stack.back().else_at = Pc();
        return *this;
    }
    Asm& EndIf() {
        const Open o = stack.back();
        stack.pop_back();
        u32& w = code[o.at];
        if (o.else_at == 0) { // no else: the else address is the end, the else length 0
            w = (w & ~(0xFFFu << 10)) | (Pc() << 10);
        } else {
            w = (w & ~((0xFFFu << 10) | 0xFF)) | (o.else_at << 10) | (Pc() - o.else_at);
        }
        return *this;
    }
    Asm& BeginLoop(u32 int_id) {
        stack.push_back({Pc()});
        return Raw(Flow(kLoop, 0, 0, int_id << 22));
    }
    Asm& EndLoop() {
        const Open o = stack.back();
        stack.pop_back();
        // dest_offset is the last instruction of the body
        code[o.at] = (code[o.at] & ~(0xFFFu << 10)) | ((Pc() - 1) << 10);
        return *this;
    }

    // Subroutines are placed after the main code; a call names one.
    struct Site {
        u32 at;
        std::string name;
    };
    std::vector<Site> sites;
    std::map<std::string, std::pair<u32, u32>> subs; // name -> begin, length
    std::string open_sub;
    Asm& Call(const std::string& name) {
        sites.push_back({Pc(), name});
        return Raw(Flow(kCall, 0, 0, 0));
    }
    Asm& Callc(Cond c, const std::string& name) {
        sites.push_back({Pc(), name});
        return Raw(Flow(kCallc, 0, 0, c.Bits()));
    }
    Asm& Callu(u32 bool_id, const std::string& name) {
        sites.push_back({Pc(), name});
        return Raw(Flow(kCallu, 0, 0, bool_id << 22));
    }
    Asm& Sub(const std::string& name) {
        open_sub = name;
        subs[name] = {Pc(), 0};
        return *this;
    }
    Asm& EndSub() {
        subs[open_sub].second = Pc() - subs[open_sub].first;
        return *this;
    }
    // Resolves the calls.
    Asm& Done() {
        for (const Site& s : sites) {
            const auto [begin, length] = subs.at(s.name);
            code[s.at] = (code[s.at] & ~((0xFFFu << 10) | 0xFF)) | (begin << 10) | length;
        }
        sites.clear();
        return *this;
    }

    void Install(pica::ShaderSetup& setup) const {
        for (u32 i = 0; i < code.size(); ++i) {
            setup.UpdateProgramCode(i, code[i]);
        }
        for (u32 i = 0; i < descs.size(); ++i) {
            setup.UpdateSwizzleData(i, descs[i]);
        }
        // a program whose last word is 0 would not extend the loaded size
        setup.biggest_program_size = std::max<u32>(setup.biggest_program_size, static_cast<u32>(code.size()));
        setup.biggest_swizzle_size = std::max<u32>(setup.biggest_swizzle_size, static_cast<u32>(descs.size()));
    }
};

} // namespace asmb
using asmb::Asm;
using asmb::Desc;

// ================================================================================================
// The oracle: a CPU port of Azahar's shader interpreter
// ================================================================================================

namespace oracle {

// Pica::Float<16, 7>::operator*: 0 instead of NaN when a factor is infinite and the other zero.
float Mul(float a, float b) {
    float result = a * b;
    if (std::isnan(result) && !std::isnan(a) && !std::isnan(b)) {
        result = 0.f;
    }
    return result;
}

struct Vec4 {
    float c[4] = {0, 0, 0, 0};
};

struct Uniforms {
    Vec4 f[96];
    bool b[16] = {};
    u8 i[4][4] = {};
};

struct Memory {
    std::array<u32, 4096> code{};
    std::array<u32, 4096> swizzle{};
};

// boost::circular_buffer with a capacity: a push at capacity drops the oldest.
template <typename T, size_t N>
struct Ring {
    std::deque<T> items;
    void push_back(const T& v) {
        if (items.size() == N) {
            items.pop_front();
        }
        items.push_back(v);
    }
    bool empty() const { return items.empty(); }
    T& back() { return items.back(); }
    void pop_back() { items.pop_back(); }
    size_t size() const { return items.size(); }
};

// ShaderUnit. `reset` gives the state the translated shader starts from (temporaries and output
// registers (0, 0, 0, 1)): Azahar's unit keeps the previous vertex's values.
struct Unit {
    s32 address_registers[3] = {};
    bool conditional_code[2] = {};
    Vec4 input[16];
    Vec4 temporary[16];
    Vec4 output[2][16];
    bool output_bank = false;
    u32 emits = 0; // EMIT / SETEMIT executed (a vertex unit has no emitter: they do nothing)

    Unit() {
        for (auto& t : temporary) {
            t.c[3] = 1.f;
        }
        for (auto& bank : output) {
            for (auto& o : bank) {
                o.c[3] = 1.f;
            }
        }
    }
};

enum class Status { Ended, NotTerminating, Unhandled };

struct IfEl {
    u32 else_address, end_address;
};
struct CallEl {
    u32 end_address, return_address;
};
struct LoopEl {
    u32 entry_address, end_address;
    u8 loop_downcounter, address_increment, previous_aL;
};

// RunInterpreter. `budget` bounds the number of instructions (the interpreter has none).
Status Run(const Memory& mem, const Uniforms& uniforms, Unit& state, u32 entry_point, u64 budget = 1u << 22) {
    Ring<IfEl, 8> if_stack;
    Ring<CallEl, 4> call_stack;
    Ring<LoopEl, 4> loop_stack;
    u32 program_counter = entry_point;
    Status status = Status::Ended;

    const auto do_if = [&](u32 hex, bool condition) {
        const isa::Instruction instr{hex};
        if (condition) {
            if_stack.push_back({instr.dest_offset(), instr.dest_offset() + instr.num_instructions()});
        } else {
            program_counter = instr.dest_offset() - 1;
        }
    };
    const auto do_call = [&](u32 hex) {
        const isa::Instruction instr{hex};
        call_stack.push_back({instr.dest_offset() + instr.num_instructions(), program_counter + 1});
        program_counter = instr.dest_offset() - 1;
    };
    const auto do_loop = [&](u32 hex, const u8* loop_param) {
        const isa::Instruction instr{hex};
        const u8 previous_aL = static_cast<u8>(state.address_registers[2]);
        loop_stack.push_back({program_counter + 1, instr.dest_offset() + 1, loop_param[0], loop_param[2], previous_aL});
        state.address_registers[2] = loop_param[1];
    };
    const auto evaluate_condition = [&state](const isa::Instruction& instr) {
        const bool result_x = instr.refx() == state.conditional_code[0];
        const bool result_y = instr.refy() == state.conditional_code[1];
        switch (instr.condition_op()) {
        case isa::ConditionOp::Or:
            return result_x || result_y;
        case isa::ConditionOp::And:
            return result_x && result_y;
        case isa::ConditionOp::JustX:
            return result_x;
        case isa::ConditionOp::JustY:
            return result_y;
        }
        return false;
    };

    static const float zeros[4] = {0, 0, 0, 0};
    static const float ones[4] = {1, 1, 1, 1};

    bool should_stop = false;
    u64 executed = 0;
    while (!should_stop) {
        if (++executed > budget) {
            return Status::NotTerminating;
        }
        bool is_break = false;
        const u32 old_program_counter = program_counter;

        // The last instruction of the buffer is always an END.
        isa::Instruction instr{};
        if (program_counter < 4095) {
            instr.hex = mem.code[program_counter];
        } else {
            instr.hex = static_cast<u32>(isa::Opcode::END) << 26;
        }

        const isa::Swizzle swizzle{mem.swizzle[instr.operand_desc_id()]};

        const auto lookup = [&](isa::SourceRegister source_reg, u32 address_register_index) -> const float* {
            int index = static_cast<int>(source_reg.index());
            switch (source_reg.kind()) {
            case isa::SourceKind::Input:
                return state.input[index].c;
            case isa::SourceKind::Temporary:
                return state.temporary[index].c;
            case isa::SourceKind::FloatUniform:
                if (address_register_index != 0) {
                    int offset = state.address_registers[address_register_index - 1];
                    if (offset < -128 || offset > 127) {
                        offset = 0;
                    }
                    index = (index + offset) & 0x7F;
                    if (index >= 96) {
                        return ones;
                    }
                }
                return uniforms.f[index].c;
            }
            return zeros;
        };
        float scratch[4];
        const auto dest_of = [&](isa::DestRegister d) -> float* {
            if (d.value < 0x10) {
                return state.output[state.output_bank][d.index()].c;
            }
            if (d.value < 0x20) {
                return state.temporary[d.index()].c;
            }
            return scratch;
        };

        const isa::OpInfo& info = instr.info();
        switch (info.format) {
        case isa::Format::Arithmetic: {
            const bool is_inverted = info.src_inversed;
            const float* src1_ = lookup(instr.src1(is_inverted), !is_inverted * instr.address_register_index());
            const float* src2_ = lookup(instr.src2(is_inverted), is_inverted * instr.address_register_index());
            float src1[4], src2[4];
            for (u32 i = 0; i < 4; ++i) {
                src1[i] = src1_[swizzle.selector(1, i)];
                src2[i] = src2_[swizzle.selector(2, i)];
                if (swizzle.negate(1)) {
                    src1[i] = -src1[i];
                }
                if (swizzle.negate(2)) {
                    src2[i] = -src2[i];
                }
            }
            float* dest = dest_of(instr.dest());
            switch (instr.Effective()) {
            case isa::Opcode::ADD:
                for (int i = 0; i < 4; ++i) {
                    if (swizzle.DestEnabled(i)) dest[i] = src1[i] + src2[i];
                }
                break;
            case isa::Opcode::MUL:
                for (int i = 0; i < 4; ++i) {
                    if (swizzle.DestEnabled(i)) dest[i] = Mul(src1[i], src2[i]);
                }
                break;
            case isa::Opcode::FLR:
                for (int i = 0; i < 4; ++i) {
                    if (swizzle.DestEnabled(i)) dest[i] = std::floor(src1[i]);
                }
                break;
            case isa::Opcode::MAX:
                for (int i = 0; i < 4; ++i) {
                    // NOTE: Exact form required to match NaN semantics to hardware:
                    //   max(0, NaN) -> NaN, max(NaN, 0) -> 0
                    if (swizzle.DestEnabled(i)) dest[i] = (src1[i] > src2[i]) ? src1[i] : src2[i];
                }
                break;
            case isa::Opcode::MIN:
                for (int i = 0; i < 4; ++i) {
                    if (swizzle.DestEnabled(i)) dest[i] = (src1[i] < src2[i]) ? src1[i] : src2[i];
                }
                break;
            case isa::Opcode::DP3:
            case isa::Opcode::DP4:
            case isa::Opcode::DPH:
            case isa::Opcode::DPHI: {
                const isa::Opcode opcode = instr.Effective();
                if (opcode == isa::Opcode::DPH || opcode == isa::Opcode::DPHI) {
                    src1[3] = 1.f;
                }
                const int num_components = (opcode == isa::Opcode::DP3) ? 3 : 4;
                float dot = 0.f; // std::inner_product(src1, src1 + n, src2, f24::Zero())
                for (int i = 0; i < num_components; ++i) {
                    dot = dot + Mul(src1[i], src2[i]);
                }
                for (int i = 0; i < 4; ++i) {
                    if (swizzle.DestEnabled(i)) dest[i] = dot;
                }
                break;
            }
            case isa::Opcode::DST:
            case isa::Opcode::DSTI: { // vs_3_0; Azahar does not implement it (see glsl_decompiler.cpp)
                const float r[4] = {1.f, Mul(src1[1], src2[1]), src1[2], src2[3]};
                for (int i = 0; i < 4; ++i) {
                    if (swizzle.DestEnabled(i)) dest[i] = r[i];
                }
                break;
            }
            case isa::Opcode::LIT: { // vs_3_0 / ARB_vertex_program; Azahar does not implement it
                const float power = std::min(std::max(src1[3], -127.9961f), 127.9961f);
                const float spec = (src1[0] > 0.f && src1[1] > 0.f) ? std::pow(src1[1], power) : 0.f;
                const float r[4] = {1.f, std::max(src1[0], 0.f), spec, 1.f};
                for (int i = 0; i < 4; ++i) {
                    if (swizzle.DestEnabled(i)) dest[i] = r[i];
                }
                break;
            }
            case isa::Opcode::RCP: {
                const float res = 1.0f / src1[0];
                for (int i = 0; i < 4; ++i) {
                    if (swizzle.DestEnabled(i)) dest[i] = res;
                }
                break;
            }
            case isa::Opcode::RSQ: {
                const float res = 1.0f / std::sqrt(src1[0]);
                for (int i = 0; i < 4; ++i) {
                    if (swizzle.DestEnabled(i)) dest[i] = res;
                }
                break;
            }
            case isa::Opcode::MOVA:
                for (int i = 0; i < 2; ++i) {
                    if (swizzle.DestEnabled(i)) {
                        // TODO: Figure out how the rounding is done on hardware
                        state.address_registers[i] = static_cast<s32>(src1[i]);
                    }
                }
                break;
            case isa::Opcode::MOV:
                for (int i = 0; i < 4; ++i) {
                    if (swizzle.DestEnabled(i)) dest[i] = src1[i];
                }
                break;
            case isa::Opcode::SGE:
            case isa::Opcode::SGEI:
                for (int i = 0; i < 4; ++i) {
                    if (swizzle.DestEnabled(i)) dest[i] = (src1[i] >= src2[i]) ? 1.f : 0.f;
                }
                break;
            case isa::Opcode::SLT:
            case isa::Opcode::SLTI:
                for (int i = 0; i < 4; ++i) {
                    if (swizzle.DestEnabled(i)) dest[i] = (src1[i] < src2[i]) ? 1.f : 0.f;
                }
                break;
            case isa::Opcode::CMP:
                for (int i = 0; i < 2; ++i) {
                    const isa::CompareOp op = (i == 0) ? instr.compare_x() : instr.compare_y();
                    switch (op) {
                    case isa::CompareOp::Equal:
                        state.conditional_code[i] = (src1[i] == src2[i]);
                        break;
                    case isa::CompareOp::NotEqual:
                        state.conditional_code[i] = (src1[i] != src2[i]);
                        break;
                    case isa::CompareOp::LessThan:
                        state.conditional_code[i] = (src1[i] < src2[i]);
                        break;
                    case isa::CompareOp::LessEqual:
                        state.conditional_code[i] = (src1[i] <= src2[i]);
                        break;
                    case isa::CompareOp::GreaterThan:
                        state.conditional_code[i] = (src1[i] > src2[i]);
                        break;
                    case isa::CompareOp::GreaterEqual:
                        state.conditional_code[i] = (src1[i] >= src2[i]);
                        break;
                    default: // unknown compare mode: the condition code is not changed
                        break;
                    }
                }
                break;
            case isa::Opcode::EX2: {
                const float res = std::exp2(src1[0]);
                for (int i = 0; i < 4; ++i) {
                    if (swizzle.DestEnabled(i)) dest[i] = res;
                }
                break;
            }
            case isa::Opcode::LG2: {
                const float res = std::log2(src1[0]);
                for (int i = 0; i < 4; ++i) {
                    if (swizzle.DestEnabled(i)) dest[i] = res;
                }
                break;
            }
            default:
                status = Status::Unhandled;
                break;
            }
            break;
        }

        case isa::Format::MultiplyAdd: {
            if (instr.Effective() == isa::Opcode::MAD || instr.Effective() == isa::Opcode::MADI) {
                const isa::Swizzle mad_swizzle{mem.swizzle[instr.mad_operand_desc_id()]};
                const bool is_inverted = instr.Effective() == isa::Opcode::MADI;
                const float* src1_ = lookup(instr.mad_src1(is_inverted), 0);
                const float* src2_ = lookup(instr.mad_src2(is_inverted), !is_inverted * instr.mad_address_register_index());
                const float* src3_ = lookup(instr.mad_src3(is_inverted), is_inverted * instr.mad_address_register_index());
                float src1[4], src2[4], src3[4];
                for (u32 i = 0; i < 4; ++i) {
                    src1[i] = src1_[mad_swizzle.selector(1, i)];
                    src2[i] = src2_[mad_swizzle.selector(2, i)];
                    src3[i] = src3_[mad_swizzle.selector(3, i)];
                    if (mad_swizzle.negate(1)) src1[i] = -src1[i];
                    if (mad_swizzle.negate(2)) src2[i] = -src2[i];
                    if (mad_swizzle.negate(3)) src3[i] = -src3[i];
                }
                float* dest = dest_of(instr.mad_dest());
                for (int i = 0; i < 4; ++i) {
                    if (mad_swizzle.DestEnabled(i)) dest[i] = Mul(src1[i], src2[i]) + src3[i];
                }
            }
            break;
        }

        default: {
            switch (static_cast<isa::Opcode>(instr.opcode())) {
            case isa::Opcode::END:
                should_stop = true;
                break;
            case isa::Opcode::JMPC:
                if (evaluate_condition(instr)) {
                    program_counter = instr.dest_offset() - 1;
                }
                break;
            case isa::Opcode::JMPU:
                if (uniforms.b[instr.bool_uniform_id()] == !(instr.num_instructions() & 1)) {
                    program_counter = instr.dest_offset() - 1;
                }
                break;
            case isa::Opcode::CALL:
                do_call(instr.hex);
                break;
            case isa::Opcode::CALLU:
                if (uniforms.b[instr.bool_uniform_id()]) {
                    do_call(instr.hex);
                }
                break;
            case isa::Opcode::CALLC:
                if (evaluate_condition(instr)) {
                    do_call(instr.hex);
                }
                break;
            case isa::Opcode::NOP:
                break;
            case isa::Opcode::IFU:
                do_if(instr.hex, uniforms.b[instr.bool_uniform_id()]);
                break;
            case isa::Opcode::IFC:
                do_if(instr.hex, evaluate_condition(instr));
                break;
            case isa::Opcode::LOOP: {
                const u8* loop_param = uniforms.i[instr.int_uniform_id()];
                state.address_registers[2] = loop_param[1];
                do_loop(instr.hex, loop_param);
                break;
            }
            case isa::Opcode::BREAK:
                is_break = true;
                break;
            case isa::Opcode::BREAKC:
                if (evaluate_condition(instr)) {
                    is_break = true;
                }
                break;
            case isa::Opcode::EMIT:
            case isa::Opcode::SETEMIT:
                // Azahar asserts "execute EMIT on VS" with no emitter; a vertex unit here does nothing,
                // as the translated shader does.
                ++state.emits;
                break;
            default:
                status = Status::Unhandled;
                break;
            }
            break;
        }
        }

        ++program_counter;

        // Stacks are checked in the order CALL -> IF -> LOOP. The CALL stack can be popped multiple
        // times per instruction. A JMP at the end of a scope is never taken, this is why we compare
        // against old_program_counter + 1 here.
        u32 next_program_counter = old_program_counter + 1;
        for (u32 i = 0; i < 4; i++) {
            if (call_stack.empty() || call_stack.back().end_address != next_program_counter) {
                break;
            }
            // Hardware bug: when popping four CALL scopes at once, the last one doesn't update the
            // program counter
            if (i < 3) {
                program_counter = call_stack.back().return_address;
                next_program_counter = program_counter;
            }
            call_stack.pop_back();
        }

        if (!if_stack.empty() && if_stack.back().else_address == old_program_counter + 1) {
            program_counter = if_stack.back().end_address;
            if_stack.pop_back();
        }

        if (!loop_stack.empty() && (loop_stack.back().end_address == old_program_counter + 1 || is_break)) {
            auto& loop = loop_stack.back();
            state.address_registers[2] += loop.address_increment;
            if (!is_break && loop.loop_downcounter--) {
                program_counter = loop.entry_address;
            } else {
                program_counter = loop.end_address;
                // Only restore previous value if there is a surrounding LOOP scope.
                if (loop_stack.size() > 1) {
                    state.address_registers[2] = loop.previous_aL;
                }
                loop_stack.pop_back();
            }
        }
    }
    return status;
}

} // namespace oracle

// ================================================================================================
// Comparison, inputs, and the device
// ================================================================================================

// The comparison of the WP: equal when both are NaN, or the same infinity, or |a - b| <= 2^-16 * max(1, |a|, |b|).
bool Near(float a, float b) {
    if (std::isnan(a) && std::isnan(b)) {
        return true;
    }
    if (a == b) {
        return true; // includes the same infinity
    }
    if (std::isnan(a) || std::isnan(b) || std::isinf(a) || std::isinf(b)) {
        return false;
    }
    const float scale = std::max({1.0f, std::fabs(a), std::fabs(b)});
    return std::fabs(a - b) <= scale / 65536.0f;
}

// Deterministic pseudo-random numbers.
struct Rng {
    u64 s;
    explicit Rng(u64 seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    u32 Next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<u32>(s >> 16);
    }
    float Uniform(float lo, float hi) { return lo + (hi - lo) * (static_cast<float>(Next() & 0xFFFFFF) / 16777215.0f); }
};

// A vertex stream of float4 registers v0..v(regs-1) for `vertices` vertices.
struct FloatInputs {
    u32 regs = 0;
    u32 vertices = 0;
    std::vector<float> data; // [vertex][reg][lane]
    float& At(u32 v, u32 r, u32 l) { return data[(static_cast<size_t>(v) * regs + r) * 4 + l]; }
};

FloatInputs RandomInputs(u32 regs, u32 vertices, u64 seed, float lo = -4.f, float hi = 4.f) {
    FloatInputs in;
    in.regs = regs;
    in.vertices = vertices;
    in.data.resize(static_cast<size_t>(regs) * vertices * 4);
    Rng rng(seed);
    for (float& f : in.data) {
        f = rng.Uniform(lo, hi);
    }
    return in;
}

// The vertex loader's description (vertex_loader.h) of such a stream: binding 0 holds the registers
// the stream names, the last binding is the constant buffer the other registers read.
vertex::LoadedDraw MakeDraw(const FloatInputs& in) {
    vertex::LoadedDraw d;
    vertex::VertexBuffer main;
    main.stride = in.regs * 16;
    main.vertex_count = in.vertices;
    main.data.resize(in.data.size() * 4 + 16, 0);
    std::memcpy(main.data.data(), in.data.data(), in.data.size() * 4);
    vertex::VertexBuffer constant;
    constant.stride = 16;
    constant.per_instance = true;
    constant.vertex_count = 1;
    constant.data.resize(32, 0);
    const float one[4] = {0, 0, 0, 1};
    std::memcpy(constant.data.data(), one, 16);
    d.buffers.push_back(std::move(main));
    d.buffers.push_back(std::move(constant));
    for (u32 r = 0; r < 16; ++r) {
        vertex::VertexAttribute& a = d.attributes[r];
        a.location = r;
        a.format = vertex::VertexFormat::R32G32B32A32_FLOAT;
        a.components = 4;
        if (r < in.regs) {
            a.binding = 0;
            a.offset = r * 16;
        } else {
            a.binding = 1;
            a.offset = 0;
        }
    }
    d.count = in.vertices;
    return d;
}

// ShaderUnit::LoadInput on the loader's description: the float4 a vertex shader sees in each register.
float ReadComponent(const u8* p, vertex::ComponentKind kind, u32 lane) {
    switch (kind) {
    case vertex::ComponentKind::S8: {
        int8_t v;
        std::memcpy(&v, p + lane, 1);
        return static_cast<float>(v);
    }
    case vertex::ComponentKind::U8:
        return static_cast<float>(p[lane]);
    case vertex::ComponentKind::S16: {
        int16_t v;
        std::memcpy(&v, p + 2 * lane, 2);
        return static_cast<float>(v);
    }
    case vertex::ComponentKind::F32: {
        float v;
        std::memcpy(&v, p + 4 * lane, 4);
        return v;
    }
    }
    return 0.f;
}

void FetchInput(const vertex::LoadedDraw& d, u32 vertex_id, oracle::Vec4* regs) {
    for (u32 r = 0; r < 16; ++r) {
        const vertex::VertexAttribute& a = d.attributes[r];
        const vertex::VertexBuffer& b = d.buffers[a.binding];
        const u32 v = b.per_instance ? 0 : vertex_id;
        const u8* p = b.data.data() + static_cast<size_t>(v) * b.stride + a.offset;
        const vertex::FormatInfo info = vertex::GetFormatInfo(a.format);
        const float defaults[4] = {0, 0, 0, 1};
        for (u32 l = 0; l < 4; ++l) {
            regs[r].c[l] = l < info.lanes ? ReadComponent(p, info.kind, l) : defaults[l];
        }
        if (a.alias_w) {
            regs[r].c[3] = 1.0f; // Azahar's interpreter: lane 3 of a 3-component attribute is One
        }
    }
}

rhi::Format ToRhiFormat(vertex::VertexFormat f) {
    using VF = vertex::VertexFormat;
    switch (f) {
    case VF::R8_SINT: return rhi::Format::R8Sint;
    case VF::R8G8_SINT: return rhi::Format::Rg8Sint;
    case VF::R8G8B8A8_SINT: return rhi::Format::Rgba8Sint;
    case VF::R8_UINT: return rhi::Format::R8Uint;
    case VF::R8G8_UINT: return rhi::Format::Rg8Uint;
    case VF::R8G8B8A8_UINT: return rhi::Format::Rgba8Uint;
    case VF::R16_SINT: return rhi::Format::R16Sint;
    case VF::R16G16_SINT: return rhi::Format::Rg16Sint;
    case VF::R16G16B16A16_SINT: return rhi::Format::Rgba16Sint;
    case VF::R32_FLOAT: return rhi::Format::R32Float;
    case VF::R32G32_FLOAT: return rhi::Format::Rg32Float;
    case VF::R32G32B32_FLOAT: return rhi::Format::Rgb32Float;
    case VF::R32G32B32A32_FLOAT: return rhi::Format::Rgba32Float;
    }
    return rhi::Format::Undefined;
}

// ---- the pipeline state around a program --------------------------------------------------------

struct Draw {
    pica::ShaderSetup setup;
    std::unique_ptr<pica::PicaState> state = std::make_unique<pica::PicaState>();
    vertex::LoadedDraw loaded;
    bool flip_viewport = false;
};

// Registers of the vertex shader unit and OUTMAP (the positions are ShaderRegs, RasterizerRegs).
void SetShaderRegs(pica::PicaState& state, u32 main_offset, u32 output_mask) {
    state.regs[pica::reg::kVs + pica::reg::kShaderMainOffset] = main_offset;
    state.regs[pica::reg::kVs + pica::reg::kShaderOutputMask] = output_mask;
}
// Maps the output attributes' lanes to semantics (31 = invalid).
[[maybe_unused]] void SetOutmap(pica::PicaState& state, const std::vector<std::array<u32, 4>>& semantics) {
    state.regs[shader::reg::kVsOutputTotal] = static_cast<u32>(semantics.size());
    for (u32 a = 0; a < semantics.size(); ++a) {
        state.regs[shader::reg::kVsOutputAttributes + a] =
            semantics[a][0] | (semantics[a][1] << 8) | (semantics[a][2] << 16) | (semantics[a][3] << 24);
    }
}

// The usual map: o0 = position, o1 = quaternion, o2 = colour, o3 = (tc0 u, v, tc1 u, v),
// o4 = (tc0 w, -, view x, y), o5 = (view z, -, tc2 u, v)
[[maybe_unused]] std::vector<std::array<u32, 4>> StandardOutmap() {
    namespace s = shader::semantic;
    return {{s::kPositionX, s::kPositionY, s::kPositionZ, s::kPositionW},
            {s::kQuaternionX, s::kQuaternionY, s::kQuaternionZ, s::kQuaternionW},
            {s::kColorR, s::kColorG, s::kColorB, s::kColorA},
            {s::kTexcoord0U, s::kTexcoord0V, s::kTexcoord1U, s::kTexcoord1V},
            {s::kTexcoord0W, s::kInvalid, s::kViewX, s::kViewY},
            {s::kViewZ, s::kInvalid, s::kTexcoord2U, s::kTexcoord2V}};
}

oracle::Uniforms OracleUniforms(const pica::ShaderSetup& setup) {
    oracle::Uniforms u;
    for (u32 i = 0; i < 96; ++i) {
        for (u32 c = 0; c < 4; ++c) {
            u.f[i].c[c] = setup.uniform_f[i].c[c];
        }
    }
    for (u32 i = 0; i < 16; ++i) {
        u.b[i] = setup.uniform_b[i];
    }
    for (u32 i = 0; i < 4; ++i) {
        for (u32 c = 0; c < 4; ++c) {
            u.i[i][c] = setup.uniform_i[i][c];
        }
    }
    return u;
}

oracle::Memory OracleMemory(const pica::ShaderSetup& setup) {
    oracle::Memory m;
    m.code = setup.program_code;
    m.swizzle = setup.swizzle_data;
    return m;
}

// ---- the device ---------------------------------------------------------------------------------

rhi::BindingSetDesc TestSetDesc() {
    rhi::BindingSetDesc set;
    set.slots = {{shader::kBindingPicaUniforms, rhi::BindingType::UniformBuffer},
                 {shader::kBindingVertexData, rhi::BindingType::UniformBuffer},
                 {shader::kBindingCapture, rhi::BindingType::StorageBuffer}};
    return set;
}

struct Rig {
    std::shared_ptr<rhi::Backend> backend;
    std::map<std::string, std::shared_ptr<rhi::Pipeline>> pipelines;
    // Under the validation layer the buffers of a run are kept: the layer's synchronisation tracking does
    // not see the host wait on the timeline, so memory reused for another buffer reads as a hazard.
    std::vector<std::shared_ptr<void>> kept;

    Rig() : backend(test::RequireBackend()) {}
    bool ok() const { return backend != nullptr; }

    std::shared_ptr<rhi::Buffer> Make(u64 size, rhi::Heap heap, rhi::BufferUsage usage, const char* name) {
        rhi::BufferDesc desc;
        desc.size = std::max<u64>(size, 16);
        desc.heap = heap;
        desc.usage = usage;
        desc.name = name;
        rhi::Result<std::shared_ptr<rhi::Buffer>> buffer = backend->CreateBuffer(desc);
        REQUIRE_MESSAGE(buffer.ok(), buffer.error());
        return *buffer;
    }

    static std::string LayoutKey(const shader::TranslatedVertexShader& t, const vertex::LoadedDraw& d) {
        // keyed by the SPIR-V itself (an address can be reused by the next shader)
        u64 h = 1469598103934665603ull;
        for (u32 w : t.spirv) {
            h = (h ^ w) * 1099511628211ull;
        }
        std::string key = std::to_string(h) + "/" + std::to_string(t.spirv.size()) + ":";
        for (const vertex::VertexBuffer& b : d.buffers) {
            key += "b" + std::to_string(b.stride) + (b.per_instance ? "i" : "v");
        }
        for (const vertex::VertexAttribute& a : d.attributes) {
            key += "a" + std::to_string(a.binding) + "." + std::to_string(a.offset) + "." +
                   std::to_string(static_cast<int>(a.format));
        }
        return key;
    }

    // The pipeline for a translated shader and the description of its inputs: a vertex stage and
    // nothing else (no fragment shader, point list) in a pass with no attachments.
    std::shared_ptr<rhi::Pipeline> PipelineFor(const std::shared_ptr<const shader::TranslatedVertexShader>& t,
                                               const vertex::LoadedDraw& d, std::string* error = nullptr) {
        const std::string key = LayoutKey(*t, d);
        if (auto it = pipelines.find(key); it != pipelines.end()) {
            return it->second;
        }
        rhi::GraphicsPipelineDesc desc;
        desc.vertex.spirv = t->spirv;
        for (u32 i = 0; i < d.buffers.size(); ++i) {
            desc.vertex_layout.bindings.push_back({i, d.buffers[i].stride, d.buffers[i].per_instance});
        }
        for (const vertex::VertexAttribute& a : d.attributes) {
            desc.vertex_layout.attributes.push_back({a.location, a.binding, ToRhiFormat(a.format), a.offset});
        }
        desc.topology = rhi::Topology::PointList;
        desc.sets = {TestSetDesc()};
        desc.name = "vertex shader test";
        rhi::Result<std::shared_ptr<rhi::Pipeline>> pipeline = backend->CreateGraphicsPipeline(desc);
        if (!pipeline.ok()) {
            if (error != nullptr) {
                *error = pipeline.error();
            }
            return nullptr;
        }
        pipelines[key] = *pipeline;
        return *pipeline;
    }

    // Runs the shader for the first `count` vertices (or indices) of the draw and returns the capture
    // buffer: kCaptureVec4PerVertex float4 per vertex id, for the ids below `slots`.
    std::vector<float> Run(const std::shared_ptr<const shader::TranslatedVertexShader>& t, const vertex::LoadedDraw& d,
                           const pica::ShaderSetup& setup, const pica::PicaState& state, u32 count, u32 slots,
                           bool indexed = false, std::string* error = nullptr, bool flip = false) {
        std::shared_ptr<rhi::Pipeline> pipeline = PipelineFor(t, d, error);
        if (!pipeline) {
            return {};
        }
        const u64 capture_bytes = static_cast<u64>(slots) * shader::kCaptureVec4PerVertex * 16;
        auto uniforms = Make(sizeof(shader::PicaUniformData), rhi::Heap::Upload, rhi::BufferUsage::Uniform, "pica uniforms");
        auto vdata = Make(sizeof(shader::VertexUniformData), rhi::Heap::Upload, rhi::BufferUsage::Uniform, "vertex data");
        // host-visible, so that the shader writes where the host reads and nothing is copied (a copy through a
        // staging buffer the backend frees would read as a hazard when the layer sees its memory reused)
        auto capture = Make(capture_bytes, rhi::Heap::Readback, rhi::BufferUsage::Storage, "capture");
        const shader::PicaUniformData pu = shader::MakePicaUniformData(setup);
        const shader::VertexUniformData vu = shader::MakeVertexUniformData(state, flip);
        std::memcpy(uniforms->Mapped().data(), &pu, sizeof(pu));
        uniforms->Flush(0, sizeof(pu));
        std::memcpy(vdata->Mapped().data(), &vu, sizeof(vu));
        vdata->Flush(0, sizeof(vu));
        std::memset(capture->Mapped().data(), 0, capture_bytes);
        capture->Flush(0, capture_bytes);

        std::vector<std::shared_ptr<rhi::Buffer>> vbuffers;
        std::vector<rhi::VertexBufferBinding> bindings;
        for (const vertex::VertexBuffer& b : d.buffers) {
            auto vb = Make(b.data.size(), rhi::Heap::Upload, rhi::BufferUsage::Vertex, "vertices");
            std::memcpy(vb->Mapped().data(), b.data.data(), b.data.size());
            vb->Flush(0, b.data.size());
            bindings.push_back({vb.get(), 0});
            vbuffers.push_back(std::move(vb));
        }
        std::shared_ptr<rhi::Buffer> ib;
        if (indexed) {
            ib = Make(d.index_data.size(), rhi::Heap::Upload, rhi::BufferUsage::Index, "indices");
            std::memcpy(ib->Mapped().data(), d.index_data.data(), d.index_data.size());
            ib->Flush(0, d.index_data.size());
        }

        rhi::Result<std::shared_ptr<rhi::BindingSet>> set = backend->CreateBindingSet(TestSetDesc());
        REQUIRE_MESSAGE(set.ok(), set.error());
        REQUIRE((*set)->SetUniformBuffer(shader::kBindingPicaUniforms, *uniforms, 0, sizeof(pu)).ok());
        REQUIRE((*set)->SetUniformBuffer(shader::kBindingVertexData, *vdata, 0, sizeof(vu)).ok());
        REQUIRE((*set)->SetStorageBuffer(shader::kBindingCapture, *capture, 0, capture_bytes).ok());

        rhi::Recorder rec = backend->BeginRecording();
        rec.BeginPass({8, 8});
        rec.SetPipeline(*pipeline);
        rec.Bind(**set, 0);
        rec.SetVertexBuffers(0, bindings);
        if (indexed) {
            const rhi::IndexType type = d.index_type == vertex::IndexType::U8    ? rhi::IndexType::U8
                                        : d.index_type == vertex::IndexType::U16 ? rhi::IndexType::U16
                                                                                  : rhi::IndexType::U32;
            rec.SetIndexBuffer(*ib, 0, type);
            rec.DrawIndexed(count, 1, 0, d.base_vertex);
        } else {
            rec.Draw(count);
        }
        rec.EndPass();
        const rhi::TimelineValue value = backend->Submit(std::move(rec));
        std::vector<float> out(static_cast<size_t>(slots) * shader::kCaptureVec4PerVertex * 4);
        rhi::Result<void> read = backend->ReadBuffer(
            *capture, 0, std::span<u8>(reinterpret_cast<u8*>(out.data()), out.size() * 4), value);
        REQUIRE_MESSAGE(read.ok(), read.error());
        if (test::ValidationEnabled()) {
            for (auto& b : vbuffers) kept.push_back(b);
            kept.insert(kept.end(), {uniforms, vdata, capture, *set});
            if (ib) kept.push_back(ib);
        }
        return out;
    }
};

// ---- translation helpers -----------------------------------------------------------------------------

std::shared_ptr<const shader::TranslatedVertexShader> Translate(Draw& d, bool clip = false, bool geometry_stage = false,
                                                                  std::string* error = nullptr) {
    shader::ConfigOptions opt;
    opt.capture_outputs = true;
    opt.use_clip_planes = clip;
    opt.geometry_stage = geometry_stage;
    d.state->vs = d.setup;
    const shader::VertexShaderConfig config = shader::MakeVertexShaderConfig(*d.state, &d.loaded, opt);
    shader::TranslateResult r = shader::TranslateVertexShader(d.setup, config);
    if (error != nullptr) {
        *error = r.error;
    }
    return r.shader;
}

#if defined(_WIN32)
#define MH4N_POPEN _popen
#define MH4N_PCLOSE _pclose
#else
#define MH4N_POPEN popen
#define MH4N_PCLOSE pclose
#endif

enum class SpirvVal { Valid, Invalid, Unavailable };

// spirv-val (Vulkan 1.2 environment) on the module.
SpirvVal ValidateSpirv(const std::vector<u32>& words, std::string& output) {
    static std::atomic<unsigned> counter{0};
    const fs::path path = fs::temp_directory_path() / ("mh4n_gpu_shader_test_" + std::to_string(MH4N_GETPID()) + "_" +
                                                       std::to_string(counter++) + ".spv");
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(words.data()), static_cast<std::streamsize>(words.size() * 4));
    }
    const std::string command = "spirv-val --target-env vulkan1.2 \"" + path.string() + "\" 2>&1";
    FILE* pipe = MH4N_POPEN(command.c_str(), "r");
    output.clear();
    if (pipe == nullptr) {
        fs::remove(path);
        return SpirvVal::Unavailable;
    }
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    const int status = MH4N_PCLOSE(pipe);
    fs::remove(path);
    if (status != 0 && (output.find("not found") != std::string::npos || output.find("not recognized") != std::string::npos)) {
        return SpirvVal::Unavailable;
    }
    return status == 0 ? SpirvVal::Valid : SpirvVal::Invalid;
}

void CheckSpirvValid(const std::vector<u32>& words, const char* what) {
    std::string output;
    switch (ValidateSpirv(words, output)) {
    case SpirvVal::Valid:
        break;
    case SpirvVal::Invalid:
        FAIL_CHECK(what << ": spirv-val: " << output);
        break;
    case SpirvVal::Unavailable:
        WARN_MESSAGE(false, "spirv-val is not available: the SPIR-V of " << what << " was not validated");
        break;
    }
}

} // namespace

// ================================================================================================
// Running a case against the oracle
// ================================================================================================

namespace {

struct Varyings {
    float pos[4], clip[2], color[4], tc0[2], tc1[2], tc2[2], tc0w, quat[4], view[3];
};

// What the vertex stage must write for the vertex whose output registers are `u`, worked out here
// from the registers (not from the module's maps). A semantic no output attribute maps is 1.0.
Varyings ExpectedVaryings(const oracle::Unit& u, const pica::PicaState& state, bool flip) {
    const u32 mask = state.regs[pica::reg::kVs + pica::reg::kShaderOutputMask] & 0xFFFF;
    std::vector<oracle::Vec4> attrs;
    for (u32 r = 0; r < 16; ++r) {
        if ((mask >> r) & 1u) {
            attrs.push_back(u.output[u.output_bank][r]);
        }
    }
    float slots[32];
    std::fill(std::begin(slots), std::end(slots), 1.0f);
    const u32 total = state.regs[shader::reg::kVsOutputTotal] & 7;
    for (u32 a = 0; a < total; ++a) {
        const u32 word = state.regs[shader::reg::kVsOutputAttributes + a];
        for (u32 c = 0; c < 4; ++c) {
            const u32 s = (word >> (8 * c)) & 0x1F;
            slots[s] = a < attrs.size() ? attrs[a].c[c] : 1.0f;
        }
    }
    Varyings v{};
    float x = slots[0], y = slots[1], z = slots[2], w = slots[3];
    const float ndc_z = z / w;
    if (ndc_z > 0.f && ndc_z < 0.000001f) {
        z = 0.f;
    }
    if (ndc_z < -1.f && ndc_z > -1.00001f) {
        z = -w;
    }
    if (flip) {
        y = -y;
    }
    v.pos[0] = x;
    v.pos[1] = y;
    v.pos[2] = -z;
    v.pos[3] = w;
    v.clip[0] = -z;
    const bool clip1 = (state.regs[shader::reg::kClipEnable] & 1u) != 0;
    v.clip[1] = 0.f;
    if (clip1) {
        float dot = 0.f;
        const float p[4] = {x, y, z, w};
        for (u32 c = 0; c < 4; ++c) {
            dot += pica::Float24ToFloat(state.regs[shader::reg::kClipCoef + c] & 0xFFFFFFu) * p[c];
        }
        v.clip[1] = dot;
    }
    for (u32 c = 0; c < 4; ++c) {
        const float a = std::fabs(slots[8 + c]);
        v.color[c] = a < 1.0f ? a : 1.0f;
        v.quat[c] = slots[4 + c];
    }
    v.tc0[0] = slots[12];
    v.tc0[1] = slots[13];
    v.tc1[0] = slots[14];
    v.tc1[1] = slots[15];
    v.tc0w = slots[16];
    v.view[0] = slots[18];
    v.view[1] = slots[19];
    v.view[2] = slots[20];
    v.tc2[0] = slots[22];
    v.tc2[1] = slots[23];
    return v;
}

struct Report {
    u64 vertices = 0;
    u64 values = 0;
    u64 mismatches = 0;
    bool not_terminating = false;
    bool unhandled = false;
    bool redact = false; // the dump path: no value of a shader output in any text
    std::string first;

    void Note(const std::string& what) {
        ++mismatches;
        if (first.empty()) {
            first = what;
        }
    }
};

void CompareFloat(Report& r, float gpu, float cpu, const std::string& what) {
    ++r.values;
    if (!Near(gpu, cpu)) {
        r.Note(r.redact ? what + " differs" : what + ": gpu " + std::to_string(gpu) + " cpu " + std::to_string(cpu));
    }
}

// Compares the capture of the GPU with the oracle for the vertex ids in `ids`. `varyings`: also the
// seven varyings and the position.
Report CompareWithOracle(const pica::ShaderSetup& setup, const pica::PicaState& state, const vertex::LoadedDraw& loaded,
                         bool flip, const shader::TranslatedVertexShader& t, const std::vector<float>& cap,
                         const std::vector<u32>& ids, bool varyings, bool clip = false, bool redact = false) {
    Report report;
    report.redact = redact;
    const oracle::Memory mem = OracleMemory(setup);
    const oracle::Uniforms uniforms = OracleUniforms(setup);
    const u32 mask = state.regs[pica::reg::kVs + pica::reg::kShaderOutputMask] & 0xFFFF;
    const u32 entry = state.regs[pica::reg::kVs + pica::reg::kShaderMainOffset] & 0xFFFF;
    for (u32 id : ids) {
        oracle::Unit u;
        FetchInput(loaded, id, u.input);
        const oracle::Status status = oracle::Run(mem, uniforms, u, entry);
        report.not_terminating |= status == oracle::Status::NotTerminating;
        report.unhandled |= status == oracle::Status::Unhandled;
        ++report.vertices;
        const float* g = &cap[static_cast<size_t>(id) * shader::kCaptureVec4PerVertex * 4];
        for (u32 r = 0; r < 16; ++r) {
            if (!((mask >> r) & 1u)) {
                continue;
            }
            const u32 attr = t.output_map[r];
            for (u32 c = 0; c < 4; ++c) {
                CompareFloat(report, g[attr * 4 + c], u.output[u.output_bank][r].c[c],
                             "vertex " + std::to_string(id) + " o" + std::to_string(r) + "." + "xyzw"[c]);
            }
        }
        // attributes past the output count stay zero
        for (u32 a = t.num_outputs; a < 16; ++a) {
            for (u32 c = 0; c < 4; ++c) {
                if (g[a * 4 + c] != 0.f) {
                    report.Note("vertex " + std::to_string(id) + ": attribute " + std::to_string(a) + " is not zero");
                }
            }
        }
        if (varyings) {
            const Varyings e = ExpectedVaryings(u, state, flip);
            const auto cmp = [&](u32 slot, u32 lane, float want, const char* name) {
                CompareFloat(report, g[slot * 4 + lane], want, "vertex " + std::to_string(id) + " " + name);
            };
            for (u32 c = 0; c < 4; ++c) {
                cmp(shader::kCapturePosition, c, e.pos[c], "position");
                cmp(shader::kCapturePrimaryColor, c, e.color[c], "primary_color");
                cmp(shader::kCaptureNormquat, c, e.quat[c], "normquat");
            }
            cmp(shader::kCaptureTexcoord01, 0, e.tc0[0], "texcoord0.x");
            cmp(shader::kCaptureTexcoord01, 1, e.tc0[1], "texcoord0.y");
            cmp(shader::kCaptureTexcoord01, 2, e.tc1[0], "texcoord1.x");
            cmp(shader::kCaptureTexcoord01, 3, e.tc1[1], "texcoord1.y");
            cmp(shader::kCaptureTexcoord2W, 0, e.tc2[0], "texcoord2.x");
            cmp(shader::kCaptureTexcoord2W, 1, e.tc2[1], "texcoord2.y");
            cmp(shader::kCaptureTexcoord2W, 2, e.tc0w, "texcoord0_w");
            for (u32 c = 0; c < 3; ++c) {
                cmp(shader::kCaptureView, c, e.view[c], "view");
            }
            if (clip) {
                cmp(shader::kCaptureClip, 0, e.clip[0], "clip0");
                cmp(shader::kCaptureClip, 1, e.clip[1], "clip1");
            }
        }
    }
    return report;
}

Report CompareWithOracle(const Draw& d, const shader::TranslatedVertexShader& t, const std::vector<float>& cap,
                         const std::vector<u32>& ids, bool varyings, bool clip = false) {
    return CompareWithOracle(d.setup, *d.state, d.loaded, d.flip_viewport, t, cap, ids, varyings, clip);
}

std::vector<u32> Iota(u32 n) {
    std::vector<u32> v(n);
    for (u32 i = 0; i < n; ++i) {
        v[i] = i;
    }
    return v;
}

// A program with its inputs, ready to translate.
Draw MakeProgram(const Asm& a, const FloatInputs& in, u32 output_mask = 0xFFFF, u32 main_offset = 0) {
    Draw d;
    a.Install(d.setup);
    d.loaded = MakeDraw(in);
    SetShaderRegs(*d.state, main_offset, output_mask);
    return d;
}

void SetUniform(Draw& d, u32 index, float x, float y, float z, float w) {
    d.setup.uniform_f[index] = pica::Vec4f{{x, y, z, w}};
}
void SetRandomUniforms(Draw& d, u32 count, u64 seed, float lo = -3.f, float hi = 3.f) {
    Rng rng(seed);
    for (u32 i = 0; i < count; ++i) {
        SetUniform(d, i, rng.Uniform(lo, hi), rng.Uniform(lo, hi), rng.Uniform(lo, hi), rng.Uniform(lo, hi));
    }
}

// Translates (the program must translate), validates the SPIR-V, runs the first `n` vertices on the
// device and compares with the oracle. Returns the report; the checks are made by the caller.
struct Outcome {
    std::shared_ptr<const shader::TranslatedVertexShader> shader;
    Report report;
    std::vector<float> capture;
    std::string error;
};

Outcome RunCase(Rig& rig, Draw& d, u32 n, bool varyings = false, bool clip = false, bool validate = true) {
    Outcome out;
    out.shader = Translate(d, clip, false, &out.error);
    if (!out.shader) {
        return out;
    }
    if (validate) {
        CheckSpirvValid(out.shader->spirv, "a test program");
    }
    out.capture = rig.Run(out.shader, d.loaded, d.setup, *d.state, n, n, false, &out.error, d.flip_viewport);
    if (out.capture.empty()) {
        return out;
    }
    out.report = CompareWithOracle(d, *out.shader, out.capture, Iota(n), varyings, clip);
    return out;
}

#define CHECK_OUTCOME(outcome)                                                                                \
    do {                                                                                                      \
        REQUIRE_MESSAGE((outcome).shader != nullptr, (outcome).error);                                        \
        REQUIRE_MESSAGE(!(outcome).capture.empty(), (outcome).error);                                         \
        CHECK_MESSAGE((outcome).report.mismatches == 0,                                                       \
                      (outcome).report.mismatches << " of " << (outcome).report.values                        \
                                                  << " values differ; first: " << (outcome).report.first);    \
        CHECK_FALSE((outcome).report.unhandled);                                                              \
        CHECK_FALSE((outcome).report.not_terminating);                                                        \
    } while (0)

} // namespace

// ================================================================================================
// Instructions
// ================================================================================================

TEST_CASE("every arithmetic instruction equals the interpreter (swizzles, negation, destination masks)") {
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    Asm a;
    a.Op(kAdd, O(0), V(0), V(1));
    a.Op(kAdd, O(1), V(0), V(1), {.mask = "x_zw", .s1 = "wzyx", .n1 = true, .s2 = "yyxz"});
    a.Op(kMul, O(2), C(0), V(0), {.mask = "xyz_", .s1 = "zzxy", .s2 = "wxyz", .n2 = true});
    a.Mad(O(3), V(0), V(1), V(2));
    a.Mad(O(4), V(0), C(3), V(1), {.mask = "_y_w", .s1 = "zyxw", .n1 = true, .s2 = "wzyx", .s3 = "xxyy", .n3 = true});
    a.Op(kDp3, O(5), V(0), V(1));
    a.Op(kDp4, O(6), V(0), V(1), {.mask = "x_zw", .s1 = "wzyx", .s2 = "yyzz", .n2 = true});
    a.Op(kDph, O(7), V(0), V(1), {.mask = "xyzw", .s1 = "xyzw", .n1 = true});
    a.Op(kDphi, O(8), V(2), C(2), {.s2 = "wzyx"});
    a.Op(kMax, O(9), V(0), V(1), {.s1 = "yxwz", .s2 = "zzzz"});
    a.Op(kMin, O(10), V(0), V(1), {.mask = "xy_w", .n1 = true});
    a.Op(kSlt, O(11), V(0), V(1));
    a.Op(kSge, O(12), V(0), V(1), {.s1 = "xxyy"});
    a.Op(kSlti, O(13), V(0), C(4), {.s2 = "zzzz"});
    a.Op(kSgei, O(14), V(1), C(4), {.mask = "x_z_"});
    a.Op1(kFlr, O(15), V(0), {.s1 = "wxyz"});
    a.End();
    Draw d = MakeProgram(a, RandomInputs(3, 64, 1));
    SetRandomUniforms(d, 8, 2);
    d.setup.uniform_f[4] = pica::Vec4f{{0.5f, -0.25f, 1.0f, 0.0f}};
    Outcome o = RunCase(rig, d, 64);
    CHECK_OUTCOME(o);
    CHECK(o.report.vertices == 64);
}

TEST_CASE("the mad family and the 5-bit operand descriptors of mad") {
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    Asm a;
    a.Mad(O(0), V(0), V(1), V(2));
    a.Madi(O(1), V(0), V(1), C(1));
    a.Madi(O(2), V(2), R(0), C(2), {.mask = "xyz_", .n1 = true, .s2 = "wzyx"});
    a.Op1(kMov, R(0), V(1));
    a.Mad(O(3), R(0), C(0), R(0), {.s3 = "yzwx"});
    a.Madi(O(4), R(0), V(0), C(5), {.n3 = true});
    a.End();
    Draw d = MakeProgram(a, RandomInputs(3, 64, 3), 0x1F);
    SetRandomUniforms(d, 8, 4);
    Outcome o = RunCase(rig, d, 64);
    CHECK_OUTCOME(o);
}

TEST_CASE("the scalar instructions, mov, flr and the compare-and-set family") {
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    Asm a;
    a.Op1(kRcp, O(0), V(0), {.s1 = "yyyy"});
    a.Op1(kRcp, O(1), V(0), {.mask = "x_z_", .n1 = true});
    a.Op1(kRsq, O(2), V(1));
    a.Op1(kRsq, O(3), V(1), {.mask = "_yzw", .s1 = "wxyz"});
    a.Op1(kEx2, O(4), V(2));
    a.Op1(kEx2, O(5), V(2), {.mask = "xy__", .s1 = "zwxy", .n1 = true});
    a.Op1(kLg2, O(6), V(1));
    a.Op1(kLg2, O(7), V(1), {.mask = "__zw", .s1 = "yxzw"});
    a.Op1(kMov, O(8), V(0), {.mask = "xyzw", .s1 = "wzyx"});
    a.Op1(kMov, O(9), V(0), {.mask = "x__w", .n1 = true});
    a.Op1(kFlr, O(10), V(0), {.n1 = true});
    // temporaries: r0 = v0 * c0; o11 = r0 + v1
    a.Op(kMul, R(0), C(0), V(0));
    a.Op(kAdd, O(11), R(0), V(1));
    a.End();
    // v0 away from 0 (rcp), v1 positive (rsq, lg2), v2 in a range exp2 stays finite
    FloatInputs in = RandomInputs(3, 64, 5);
    Rng rng(6);
    for (u32 v = 0; v < 64; ++v) {
        for (u32 l = 0; l < 4; ++l) {
            float x = rng.Uniform(0.3f, 4.f);
            in.At(v, 0, l) = (rng.Next() & 1) ? x : -x;
            in.At(v, 1, l) = rng.Uniform(0.05f, 9.f);
            in.At(v, 2, l) = rng.Uniform(-6.f, 6.f);
        }
    }
    Draw d = MakeProgram(a, in, 0xFFF);
    SetRandomUniforms(d, 4, 7);
    Outcome o = RunCase(rig, d, 64);
    CHECK_OUTCOME(o);
}

// The last bit of RCP and RSQ. The interpreter computes 1.0f / x and 1.0f / sqrtf(x), each step rounded to
// nearest. A device's own reciprocal and inverse square root are within a unit in the last place of the
// true value but not always the rounded one (the RX 6700 XT's differ from the CPU's in a sizeable share of
// the inputs), and a program that subtracts nearly equal numbers afterwards (one plus the z of a unit
// vector, with z near -1) makes 1 ulp of difference more than the 2^-16 of the comparison: the dump check
// of N2-06 met exactly that. So the translation rounds as the CPU does, on any device.
TEST_CASE("RCP and RSQ round as the interpreter does, to the last bit") {
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    Asm a;
    // o0 = rsq of each lane of v0, o1 = rcp of each lane of v0
    const char* lanes[4] = {"xxxx", "yyyy", "zzzz", "wwww"};
    const char* masks[4] = {"x___", "_y__", "__z_", "___w"};
    for (u32 l = 0; l < 4; ++l) {
        a.Op1(kRsq, O(0), V(0), {.mask = masks[l], .s1 = lanes[l]});
        a.Op1(kRcp, O(1), V(0), {.mask = masks[l], .s1 = lanes[l]});
    }
    a.End();
    constexpr u32 kVertices = 4096;
    FloatInputs in = RandomInputs(1, kVertices, 41);
    Rng rng(42);
    for (u32 v = 0; v < kVertices; ++v) {
        for (u32 l = 0; l < 4; ++l) {
            // log-uniform magnitudes over 2^-40 .. 2^40 with random mantissas, any sign
            const u32 exponent = 127 - 40 + rng.Next() % 81;
            const u32 bits = (exponent << 23) | (rng.Next() & 0x7FFFFF) | ((rng.Next() & 1) ? 0x80000000u : 0u);
            std::memcpy(&in.At(v, 0, l), &bits, 4);
        }
    }
    // the values that are not ordinary: zeros, infinities, NaN (rsq of a negative is NaN, and so is the
    // interpreter's 1.0f / sqrtf)
    // and the ends of the range where the translation corrects the device's value (2^-80 .. 2^80, with the
    // exact powers of two at and next to both ends) and the plain device operation just outside it
    const float special[] = {0.f,        -0.f,         kInf,        -kInf,        kNaN,         1.f,
                             -1.f,       4.f,          0.25f,       3.f,          1e-5f,        1e5f,
                             0x1p80f,    0x1p79f,      0x1p-80f,    0x1p-79f,     -0x1p80f,     -0x1p-80f,
                             1.2e24f,    8.27e-25f,    1.21e24f,    8.26e-25f,    0x1p81f,      0x1p-81f,
                             0x1p100f,   0x1p-100f,    1e30f,       1e-30f,       -0x1p100f,    -0x1p-100f};
    for (u32 i = 0; i < sizeof(special) / sizeof(special[0]); ++i) {
        in.At(i, 0, 0) = special[i];
        in.At(i, 0, 1) = special[(i + 1) % (sizeof(special) / sizeof(special[0]))];
    }
    Draw d = MakeProgram(a, in, 0x3);
    Outcome o = RunCase(rig, d, kVertices);
    REQUIRE_MESSAGE(o.shader != nullptr, o.error);
    REQUIRE_MESSAGE(!o.capture.empty(), o.error);
    const oracle::Memory mem = OracleMemory(d.setup);
    const oracle::Uniforms uniforms = OracleUniforms(d.setup);
    u64 different = 0, compared = 0, different_rsq = 0;
    std::string first;
    for (u32 id = 0; id < kVertices; ++id) {
        oracle::Unit u;
        FetchInput(d.loaded, id, u.input);
        oracle::Run(mem, uniforms, u, 0);
        const float* g = &o.capture[static_cast<size_t>(id) * shader::kCaptureVec4PerVertex * 4];
        for (u32 r = 0; r < 2; ++r) {
            for (u32 c = 0; c < 4; ++c) {
                const float want = u.output[u.output_bank][r].c[c];
                const float got = g[o.shader->output_map[r] * 4 + c];
                ++compared;
                u32 wb, gb;
                std::memcpy(&wb, &want, 4);
                std::memcpy(&gb, &got, 4);
                const bool same = wb == gb || (std::isnan(want) && std::isnan(got));
                if (!same) {
                    ++different;
                    different_rsq += r == 0 ? 1 : 0;
                    if (first.empty()) {
                        first = std::string(r == 0 ? "rsq" : "rcp") + " of vertex " + std::to_string(id) + " lane " +
                                std::to_string(c) + " differs in the last bits";
                    }
                }
            }
        }
    }
    CHECK_MESSAGE(different == 0, different << " of " << compared << " results differ from the interpreter's (" << different_rsq
                                              << " of them rsq); first: " << first);
}

// The shape of the program the dump check of N2-06 failed on, with synthetic numbers: a vector is
// normalized (dp3, rsq, mul), one is added to its z, and the root of half of that scales the vector. With z
// near -1 the sum is small, so the last bit of the first rsq is amplified by about 1 / (1 + z) in the
// result: the result agrees with the interpreter's to 2^-16 only if both round the same way.
TEST_CASE("a normalization followed by the root of 1 + z (z near -1) agrees with the interpreter") {
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    Asm a;
    a.Op(kDp3, R(0), V(0), V(0), {.mask = "x___"});
    a.Op1(kRsq, R(0), R(0), {.mask = "x___", .s1 = "xxxx"});
    a.Op(kMul, R(1), V(0), R(0), {.s2 = "xxxx"});
    a.Op(kAdd, R(2), C(0), R(1), {.s2 = "zzzz"});
    a.Op(kMul, R(2), C(1), R(2));
    a.Op1(kRsq, R(3), R(2), {.s1 = "xxxx"});
    a.Op(kMul, O(0), R(1), R(3));
    a.End();
    constexpr u32 kVertices = 4096;
    FloatInputs in = RandomInputs(1, kVertices, 51);
    Rng rng(52);
    for (u32 v = 0; v < kVertices; ++v) {
        // a vector of length 2^(-8..16) whose z is within 0.001 .. 0.05 of -1 (as a unit vector)
        const double tilt = rng.Uniform(0.0447f, 0.3162f); // sqrt(1 - z^2) for z from -0.999 to -0.95
        const double angle = rng.Uniform(0.f, 6.2831853f);
        const double scale = std::ldexp(1.0, static_cast<int>(rng.Next() % 25) - 8);
        in.At(v, 0, 0) = static_cast<float>(tilt * std::cos(angle) * scale);
        in.At(v, 0, 1) = static_cast<float>(tilt * std::sin(angle) * scale);
        in.At(v, 0, 2) = static_cast<float>(-std::sqrt(1.0 - tilt * tilt) * scale);
        in.At(v, 0, 3) = 1.f;
    }
    Draw d = MakeProgram(a, in, 0x1);
    SetUniform(d, 0, 1.f, 1.f, 1.f, 1.f);
    SetUniform(d, 1, 0.5f, 0.5f, 0.5f, 0.5f);
    Outcome o = RunCase(rig, d, kVertices);
    CHECK_OUTCOME(o);
}

// ---- RCP and RSQ in integers: the same algorithm on the CPU, and designed inputs on the device ------------
//
// pica_rcp and pica_rsq (glsl_decompiler.cpp) compute the rounded 1.0f / x and 1.0f / sqrtf(x) with exact
// integer arithmetic (umulExtended, usubBorrow, shifts and comparisons) after float estimates that only
// have to be roughly right. rcp_mirror is that code on the CPU, line by line: the same constants, the same
// order of operations, the same loop counts, umulExtended and usubBorrow as the 64-bit operations they are.
// It checks the mathematics (the proofs in the comment of the translator) without any device, and it takes a
// Noise that moves the start values and the float estimates the way an imprecise device would, to show that
// the result does not depend on them. If the GLSL is changed, change this with it.
namespace rcp_mirror {

struct Noise {
    int start = 0; // the start value of Q (RCP) or C (RSQ) is moved by this many units
    int ulps = 0;  // every float division and square root that makes an estimate is moved by this many ulp
};

float Nudge(float v, int ulps) {
    for (; ulps > 0; --ulps) v = std::nextafter(v, kInf);
    for (; ulps < 0; ++ulps) v = std::nextafter(v, -kInf);
    return v;
}

void UMulExtended(u32 x, u32 y, u32& msb, u32& lsb) {
    const u64 p = static_cast<u64>(x) * y;
    msb = static_cast<u32>(p >> 32);
    lsb = static_cast<u32>(p);
}

u32 USubBorrow(u32 x, u32 y, u32& borrow) {
    borrow = x < y ? 1u : 0u;
    return x - y;
}

float Magnitude(u32 dh, u32 dl) { return static_cast<float>(dh) * 4294967296.0f + static_cast<float>(dl); }

u32 ClampU(u32 v, u32 lo, u32 hi) { return std::min(std::max(v, lo), hi); }

u32 Start(float estimate, int moved, u32 lo, u32 hi) {
    const std::int64_t s = static_cast<std::int64_t>(std::floor(estimate + 0.5f)) + moved;
    return ClampU(static_cast<u32>(std::clamp<std::int64_t>(s, 0, 0x7FFFFFFF)), lo, hi);
}

void RcpResidual(u32 q, u32 m, bool& low, u32& dh, u32& dl) {
    u32 ph, pl;
    UMulExtended(q, m, ph, pl);
    low = ph < 0x8000u || (ph == 0x8000u && pl == 0u);
    if (low) {
        u32 borrow;
        dl = USubBorrow(0u, pl, borrow);
        dh = 0x8000u - ph - borrow;
    } else {
        dl = pl;
        dh = ph - 0x8000u;
    }
}

// `repairs` receives the largest number of +-1 steps that one of the exact loops took.
u32 RcpSignificand(u32 m, const Noise& n, int* repairs) {
    const float mf = static_cast<float>(m);
    u32 q = Start(Nudge(140737488355328.0f / mf, n.ulps), n.start, 0x800000u, 0x1000000u);
    for (int i = 0; i < 2; ++i) {
        bool low;
        u32 dh, dl;
        RcpResidual(q, m, low, dh, dl);
        const u32 jump = static_cast<u32>(std::floor(Nudge(Magnitude(dh, dl) / mf, n.ulps) + 0.5f));
        q = ClampU(low ? q + jump : q - std::min(jump, q), 0x800000u, 0x1000000u);
    }
    int steps = 0;
    for (int i = 0; i < 3; ++i) {
        bool low;
        u32 dh, dl;
        RcpResidual(q, m, low, dh, dl);
        const u32 twice_high = (dh << 1) | (dl >> 31);
        const u32 twice_low = dl << 1;
        if (twice_high != 0u || twice_low > m) {
            q = low ? q + 1u : q - 1u;
            ++steps;
        }
    }
    if (repairs != nullptr) *repairs = std::max(*repairs, steps);
    return q;
}

float RcpNormal(u32 m, int e, const Noise& n, int* repairs) {
    const u32 bits = (static_cast<u32>(126 - e) << 23) + (RcpSignificand(m, n, repairs) - 0x800000u);
    return std::bit_cast<float>(bits);
}

void SqrtResidual(u32 c, u32 nh, u32 nl, bool& pos, u32& dh, u32& dl) {
    u32 ph, pl;
    UMulExtended(c, c, ph, pl);
    pos = nh > ph || (nh == ph && nl >= pl);
    u32 borrow;
    if (pos) {
        dl = USubBorrow(nl, pl, borrow);
        dh = nh - ph - borrow;
    } else {
        dl = USubBorrow(pl, nl, borrow);
        dh = ph - nh - borrow;
    }
}

u32 SqrtSignificand(u32 m, bool odd, const Noise& n, int* repairs) {
    const u32 nh = odd ? (m >> 8) : (m >> 9);
    const u32 nl = odd ? (m << 24) : (m << 23);
    const float nf = static_cast<float>(m) * (odd ? 16777216.0f : 8388608.0f);
    u32 c = Start(std::floor(Nudge(std::sqrt(nf), n.ulps)), n.start, 0x800000u, 0xFFFFFFu);
    for (int i = 0; i < 3; ++i) {
        bool pos;
        u32 dh, dl;
        SqrtResidual(c, nh, nl, pos, dh, dl);
        const u32 jump = static_cast<u32>(std::floor(Nudge(Magnitude(dh, dl) / (2.0f * static_cast<float>(c)), n.ulps) + 0.5f));
        c = ClampU(pos ? c + jump : c - std::min(jump, c), 0x800000u, 0xFFFFFFu);
    }
    int steps = 0;
    for (int i = 0; i < 3; ++i) {
        bool pos;
        u32 dh, dl;
        SqrtResidual(c, nh, nl, pos, dh, dl);
        if (pos) {
            if (dh != 0u || dl > c) {
                c += 1u;
                ++steps;
            }
        } else if (dh != 0u || dl >= c) {
            c -= 1u;
            ++steps;
        }
    }
    if (repairs != nullptr) *repairs = std::max(*repairs, steps);
    return c;
}

float PicaRcp(float x, const Noise& n, int* repairs) {
    const u32 bits = std::bit_cast<u32>(x) & 0x7FFFFFFFu;
    if (bits >= 0x00800000u && bits < 0x7E800000u) {
        const float q = RcpNormal((bits & 0x7FFFFFu) | 0x800000u, static_cast<int>(bits >> 23) - 127, n, repairs);
        return x < 0.0f ? -q : q;
    }
    return 1.0f / x;
}

float PicaRsq(float x, const Noise& n, int* repairs) {
    const u32 bits = std::bit_cast<u32>(x);
    if (bits >= 0x00800000u && bits < 0x7F800000u) {
        const int e = static_cast<int>(bits >> 23) - 127;
        const u32 root = SqrtSignificand((bits & 0x7FFFFFu) | 0x800000u, (e & 1) != 0, n, repairs);
        return RcpNormal(root, e >> 1, n, repairs);
    }
    return 1.0f / std::sqrt(x);
}

} // namespace rcp_mirror

namespace {

float FloatFrom(u32 sign, u32 field, u32 fraction) {
    return std::bit_cast<float>((sign << 31) | (field << 23) | (fraction & 0x7FFFFFu));
}

bool SameBits(float a, float b) {
    return std::bit_cast<u32>(a) == std::bit_cast<u32>(b) || (std::isnan(a) && std::isnan(b));
}

// The significands m (as the fraction m - 2^23) for which the fractional part of 2^47 / m is nearest to one
// half, nearest first: the reciprocals that are the hardest to round.
std::vector<u32> NearTieReciprocals(size_t count) {
    std::vector<std::pair<u32, u32>> keyed;
    keyed.reserve(1u << 23);
    for (u32 m = 0x800001u; m < 0x1000000u; ++m) {
        const u64 twice = 2 * ((1ull << 47) % m);
        keyed.emplace_back(static_cast<u32>(twice > m ? twice - m : m - twice), m);
    }
    std::partial_sort(keyed.begin(), keyed.begin() + static_cast<std::ptrdiff_t>(count), keyed.end());
    std::vector<u32> out;
    for (size_t i = 0; i < count; ++i) out.push_back(keyed[i].second - 0x800000u);
    return out;
}

// The significands m for which sqrt(m * 2^23) (even exponent) or sqrt(m * 2^24) (odd) is nearest to a half
// integer, nearest first: the square roots that are the hardest to round.
std::vector<u32> NearHalfRoots(bool odd, size_t count) {
    std::vector<std::pair<u64, u32>> keyed;
    keyed.reserve(1u << 23);
    for (u32 m = 0x800000u; m < 0x1000000u; ++m) {
        const u64 n = static_cast<u64>(m) << (odd ? 24 : 23);
        u64 f = static_cast<u64>(std::sqrt(static_cast<double>(n)));
        while (f * f > n) --f;
        while ((f + 1) * (f + 1) <= n) ++f;
        const u64 r = n - f * f; // 0 .. 2f; the root rounds down while r <= f and up when r > f
        keyed.emplace_back(r > f ? r - f - 1 : f - r, m);
    }
    std::partial_sort(keyed.begin(), keyed.begin() + static_cast<std::ptrdiff_t>(count), keyed.end());
    std::vector<u32> out;
    for (size_t i = 0; i < count; ++i) out.push_back(keyed[i].second - 0x800000u);
    return out;
}

} // namespace

TEST_CASE("RCP and RSQ in integers (the CPU mirror of the shader's algorithm) equal 1.0f / x and 1.0f / sqrtf(x)") {
    using namespace rcp_mirror;
    u64 compared = 0, different = 0;
    int max_repairs = 0;
    std::string first;
    const auto check = [&](float x, const Noise& n) {
        int repairs = 0;
        const float rcp = PicaRcp(x, n, &repairs);
        const float rsq = PicaRsq(x, n, &repairs);
        max_repairs = std::max(max_repairs, repairs);
        compared += 2;
        if (!SameBits(rcp, 1.0f / x)) {
            ++different;
            if (first.empty()) first = "rcp of bits " + std::to_string(std::bit_cast<u32>(x));
        }
        if (!SameBits(rsq, 1.0f / std::sqrt(x))) {
            ++different;
            if (first.empty()) first = "rsq of bits " + std::to_string(std::bit_cast<u32>(x));
        }
    };
    const Noise exact;

    // every exponent, the powers of two and their neighbours, the extreme and the middle significands; the
    // exponent fields 0 and 255 (zero, denormals, infinity, NaN) take the plain path
    for (u32 field = 0; field < 256; ++field) {
        for (const u32 fraction : {0x000000u, 0x000001u, 0x000002u, 0x400000u, 0x3FFFFFu, 0x400001u, 0x7FFFFEu, 0x7FFFFFu,
                                   0x555555u, 0x2AAAAAu, 0x6A09E6u, 0x35042Fu}) {
            check(FloatFrom(0, field, fraction), exact);
            check(FloatFrom(1, field, fraction), exact);
        }
    }
    // the powers of two at every exponent of the range, spelled out: 1 / 2^e is exact, 1 / sqrt(2^e) is exact
    // for even e and rounded for odd e
    for (int e = -126; e <= 127; ++e) {
        check(std::ldexp(1.0f, e), exact);
    }
    // 200000 random significands at random exponents (both signs; a negative operand of RSQ is NaN)
    Rng rng(7001);
    for (u32 i = 0; i < 200000; ++i) {
        check(FloatFrom(rng.Next() & 1, 1 + rng.Next() % 254, rng.Next()), exact);
    }
    // the two ends of the range of each operation, many significands
    for (const u32 field : {1u, 2u, 3u, 125u, 126u, 127u, 128u, 251u, 252u, 253u, 254u}) {
        for (u32 i = 0; i < 20000; ++i) {
            check(FloatFrom(0, field, rng.Next()), exact);
        }
    }
    CHECK_MESSAGE(different == 0, different << " of " << compared << " results differ from the plain operations; first: " << first);
    CHECK_MESSAGE(max_repairs <= 1, "the exact loops took " << max_repairs << " steps (three are available)");

    // the same with the start values and the estimates off by what an imprecise device could be: the result
    // must not change, only the number of repair steps may (the third step of each loop is slack)
    const Noise noises[] = {{1, 0},  {-1, 0},  {0, 1},     {0, -1},     {3, 2},       {-7, -2},    {100, 3},
                            {-100, -3}, {4096, 0}, {-4096, 3}, {65536, -3}, {-65536, 2}, {1 << 20, 3}, {-(1 << 20), -3}};
    max_repairs = 0;
    for (const Noise& n : noises) {
        for (u32 i = 0; i < 4000; ++i) {
            check(FloatFrom(0, 1 + rng.Next() % 254, rng.Next()), n);
        }
        check(1.0f, n);
        check(2.0f, n);
        check(FloatFrom(0, 127, 0x7FFFFF), n);
    }
    CHECK_MESSAGE(different == 0, different << " of " << compared << " results differ from the plain operations; first: " << first);
    CHECK_MESSAGE(max_repairs <= 2, "the exact loops took " << max_repairs << " steps (three are available)");
}

// A program that gives the reciprocal and the inverse square root of every lane of v0 (the layout of the
// case above), run for `values` (four to a vertex, the rest of the last vertex 1.0) and compared bit by bit
// with the interpreter's.
namespace {

struct RcpRsqCounts {
    u64 compared = 0;
    u64 different = 0;
    u64 different_rsq = 0;
    std::string first;
};

RcpRsqCounts RunRcpRsq(Rig& rig, const std::vector<float>& values) {
    using namespace asmb;
    Asm a;
    const char* lanes[4] = {"xxxx", "yyyy", "zzzz", "wwww"};
    const char* masks[4] = {"x___", "_y__", "__z_", "___w"};
    for (u32 l = 0; l < 4; ++l) {
        a.Op1(kRsq, O(0), V(0), {.mask = masks[l], .s1 = lanes[l]});
        a.Op1(kRcp, O(1), V(0), {.mask = masks[l], .s1 = lanes[l]});
    }
    a.End();
    const u32 vertices = static_cast<u32>((values.size() + 3) / 4);
    FloatInputs in = RandomInputs(1, vertices, 61);
    for (u32 v = 0; v < vertices; ++v) {
        for (u32 l = 0; l < 4; ++l) {
            const size_t i = static_cast<size_t>(v) * 4 + l;
            in.At(v, 0, l) = i < values.size() ? values[i] : 1.0f;
        }
    }
    Draw d = MakeProgram(a, in, 0x3);
    Outcome o = RunCase(rig, d, vertices);
    REQUIRE_MESSAGE(o.shader != nullptr, o.error);
    REQUIRE_MESSAGE(!o.capture.empty(), o.error);
    const oracle::Memory mem = OracleMemory(d.setup);
    const oracle::Uniforms uniforms = OracleUniforms(d.setup);
    RcpRsqCounts counts;
    for (u32 id = 0; id < vertices; ++id) {
        oracle::Unit u;
        FetchInput(d.loaded, id, u.input);
        oracle::Run(mem, uniforms, u, 0);
        const float* g = &o.capture[static_cast<size_t>(id) * shader::kCaptureVec4PerVertex * 4];
        for (u32 r = 0; r < 2; ++r) {
            for (u32 c = 0; c < 4; ++c) {
                const float want = u.output[u.output_bank][r].c[c];
                const float got = g[o.shader->output_map[r] * 4 + c];
                ++counts.compared;
                if (!SameBits(want, got)) {
                    ++counts.different;
                    counts.different_rsq += r == 0 ? 1 : 0;
                    if (counts.first.empty()) {
                        char text[160];
                        std::snprintf(text, sizeof(text), "%s of %a (lane %u of vertex %u): gpu %a, cpu %a", r == 0 ? "rsq" : "rcp",
                                      static_cast<double>(in.At(id, 0, c)), c, id, static_cast<double>(got), static_cast<double>(want));
                        counts.first = text;
                    }
                }
            }
        }
    }
    return counts;
}

} // namespace

// Inputs designed to hit every correction of the integer algorithm: the powers of two and the values one ulp
// either side (where the reciprocal or the root changes exponent and the start can be on either side),
// the extreme significands, the significands whose reciprocal is nearest to a tie, and those whose square
// root is nearest to a half integer (both parities of the exponent), then those squares whose root is one
// of the hard reciprocals, and random values to fill.
TEST_CASE("RCP and RSQ on the device agree bit for bit on the hard cases of rounding") {
    Rig rig;
    if (!rig.ok()) return;
    constexpr size_t kValues = 16384;
    std::vector<float> values;
    Rng rng(71);
    // fields 2 .. 252: the neighbours of a power of two stay normal and the reciprocal is normal
    for (u32 field = 2; field <= 252; ++field) {
        const float p = FloatFrom(0, field, 0);
        values.push_back(p);
        values.push_back(std::nextafter(p, kInf));
        values.push_back(std::nextafter(p, 0.f));
        values.push_back(-p);
        for (const u32 fraction : {0x000001u, 0x400000u, 0x7FFFFEu, 0x7FFFFFu}) {
            values.push_back(FloatFrom(field & 1, field, fraction));
        }
    }
    const std::vector<u32> ties = NearTieReciprocals(1200);
    for (size_t i = 0; i < ties.size(); ++i) {
        // a random exponent; every third negative (the sign is applied to the result by the translation)
        const u32 field = 2 + rng.Next() % 251;
        values.push_back(FloatFrom(i % 3 == 0 ? 1 : 0, field, ties[i]));
    }
    for (size_t i = 0; i < ties.size(); ++i) {
        // x = r * r where r is a hard reciprocal: its rounded root is r or a neighbour of r
        const float r = FloatFrom(0, 127 - 30 + rng.Next() % 61, ties[i]);
        const float x = r * r;
        values.push_back(x);
        values.push_back(std::nextafter(x, kInf));
        values.push_back(std::nextafter(x, 0.f));
    }
    for (const bool odd : {false, true}) {
        const std::vector<u32> halves = NearHalfRoots(odd, 700);
        for (const u32 fraction : halves) {
            // an exponent of the wanted parity (e = field - 127, so an odd e has an even field)
            const u32 field = (odd ? 66u : 67u) + (rng.Next() % 61) * 2;
            values.push_back(FloatFrom(0, field, fraction));
        }
    }
    while (values.size() < kValues) {
        values.push_back(FloatFrom(rng.Next() & 1, 2 + rng.Next() % 251, rng.Next()));
    }
    REQUIRE(values.size() <= kValues);
    const RcpRsqCounts counts = RunRcpRsq(rig, values);
    CHECK_MESSAGE(counts.different == 0, counts.different << " of " << counts.compared << " results differ from the interpreter's ("
                                                          << counts.different_rsq << " of them rsq); first: " << counts.first);
}

TEST_CASE("DST and LIT (vs_3_0 definitions; Azahar implements neither) are translated and flagged") {
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    Asm a;
    a.Op(kDst, O(0), V(0), V(1));
    a.Op(kDst, O(1), V(0), V(1), {.mask = "x_zw", .s1 = "wzyx", .n2 = true});
    a.Op(kDsti, O(2), V(0), C(0), {.s2 = "zwxy"});
    a.Op1(kLit, O(3), V(2));
    a.Op1(kLit, O(4), V(2), {.mask = "_yz_", .s1 = "yxwz"});
    a.End();
    FloatInputs in = RandomInputs(3, 64, 8);
    for (u32 v = 0; v < 64; ++v) { // v2.xy mostly positive, v2.w a power within range
        in.At(v, 2, 0) = (v % 5 == 0) ? -0.5f : in.At(v, 2, 0) + 4.5f;
        in.At(v, 2, 1) = (v % 7 == 0) ? -0.5f : in.At(v, 2, 1) + 4.5f;
        in.At(v, 2, 3) = in.At(v, 2, 3) * 8.f;
    }
    Draw d = MakeProgram(a, in, 0x1F);
    SetRandomUniforms(d, 2, 9);
    Outcome o = RunCase(rig, d, 64);
    CHECK_OUTCOME(o);
    CHECK(o.shader->uses_unverified_instructions);
}

TEST_CASE("relative addressing with a0.x, a0.y and aL, and the out-of-range rules") {
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    Asm a;
    // mova a0.xy from v3.xy (integers, some out of range of the 96 uniforms)
    a.Op1(kMova, 0, V(3), {.mask = "xy__"});
    a.Op1(kMov, O(0), C(5), {}, 1); // c[5 + a0.x]
    a.Op1(kMov, O(1), C(5), {}, 2); // c[5 + a0.y]
    a.Op(kAdd, O(2), C(7), V(0), {.s1 = "wzyx"}, 1);
    a.Op(kAdd, O(3), C(9), V(1), {}, 0); // plain
    a.Op(kMul, O(4), C(2), V(0), {}, 2); // second source relative
    a.Op(kDphi, O(5), V(0), C(3), {}, 1); // the inverted form: the address register is on the second source
    a.Mad(O(6), V(0), C(4), V(1), {}, 1);
    a.Madi(O(7), V(0), V(1), C(6), {}, 2);
    a.Op1(kMova, 0, V(3), {.mask = "x___", .s1 = "zzzz"});
    a.Op1(kMov, O(8), C(0), {}, 1);
    a.End();
    FloatInputs in = RandomInputs(4, 64, 10);
    Rng rng(11);
    for (u32 v = 0; v < 64; ++v) {
        for (u32 l = 0; l < 4; ++l) {
            in.At(v, 3, l) = static_cast<float>(static_cast<int>(rng.Next() % 160) - 40) + (rng.Next() % 2 ? 0.7f : 0.f);
        }
    }
    // some vertices with offsets outside the signed byte (those read c[index + 0])
    in.At(3, 3, 0) = 200.f;
    in.At(4, 3, 1) = -200.f;
    in.At(5, 3, 0) = 127.f;
    in.At(6, 3, 0) = -128.f;
    Draw d = MakeProgram(a, in, 0x1FF);
    for (u32 i = 0; i < 96; ++i) {
        SetUniform(d, i, 0.01f * i, 1.0f + i, -2.0f * i, 0.5f);
    }
    Outcome o = RunCase(rig, d, 64);
    CHECK_OUTCOME(o);
}

TEST_CASE("CMP with every operator pair, the condition code in IFC / CALLC / JMPC / BREAKC conditions") {
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    // every pair of operators (36 programs would be too slow: one program runs them in turn and writes
    // the condition code as 1.0 / 0.0 to an output register each time)
    Asm a;
    u32 out = 0;
    for (u32 x = 0; x < 6; ++x) {
        for (u32 y = 0; y < 6 && out < 16; ++y) {
            if ((x + y) % 2 != 0 && x != y) {
                continue; // 18 pairs of 36 keeps the program small; the other half is the next case
            }
            a.Cmp(static_cast<CmpOp>(x), static_cast<CmpOp>(y), V(0), V(1), {.s1 = "xyzw", .s2 = "yxwz"});
            // o = ifc cc.x ? 1 : 0, then the same for y
            a.Op1(kMov, R(0), C(0));
            a.BeginIfc({.op = JustX, .refx = true});
            a.Op1(kMov, R(0), C(1), {.mask = "x___"});
            a.EndIf();
            a.BeginIfc({.op = JustY, .refy = true});
            a.Op1(kMov, R(0), C(1), {.mask = "_y__"});
            a.EndIf();
            a.Op1(kMov, O(out++), R(0));
        }
    }
    a.End();
    // v0 and v1 share many exact values so that Eq / Ne / Le / Ge see equal operands
    FloatInputs in = RandomInputs(2, 64, 12);
    for (u32 v = 0; v < 64; ++v) {
        for (u32 l = 0; l < 4; ++l) {
            if (v % 3 == 0) in.At(v, 1, l) = in.At(v, 0, l == 0 || l == 2 ? l + 1 : l - 1);
            if (v % 4 == 0) in.At(v, 1, l) = in.At(v, 0, l);
        }
    }
    Draw d = MakeProgram(a, in);
    SetUniform(d, 0, 0, 0, 0, 0);
    SetUniform(d, 1, 1, 1, 1, 1);
    Outcome o = RunCase(rig, d, 64);
    CHECK_OUTCOME(o);
}

// ================================================================================================
// The float rules: special values
// ================================================================================================

namespace {

const float kSpecial[] = {0.f, -0.f, 1.f, -1.f, kInf, -kInf, kNaN, 3.5f, -2.25f, 1e30f, -1e30f, 1e-30f};
constexpr u32 kNumSpecial = sizeof(kSpecial) / sizeof(kSpecial[0]);

float At(const std::vector<float>& cap, u32 vertex, u32 slot, u32 lane) {
    return cap[(static_cast<size_t>(vertex) * shader::kCaptureVec4PerVertex + slot) * 4 + lane];
}

} // namespace

TEST_CASE("special values: 0 x inf, NaN operands, MAX and MIN with NaN, against the interpreter and by value") {
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    Asm a;
    a.Op(kMul, O(0), V(0), V(1));
    a.Op(kDp3, O(1), V(0), V(1));
    a.Op(kDp4, O(2), V(0), V(1));
    a.Op(kDph, O(3), V(0), V(1));
    a.Mad(O(4), V(0), V(1), V(2));
    a.Op(kMax, O(5), V(0), V(1));
    a.Op(kMin, O(6), V(0), V(1));
    a.Op(kAdd, O(7), V(0), V(1));
    a.Op(kSlt, O(8), V(0), V(1));
    a.Op(kSge, O(9), V(0), V(1));
    a.Op1(kRcp, O(10), V(0));
    a.Op1(kRsq, O(11), V(0));
    a.Op1(kEx2, O(12), V(0));
    a.Op1(kLg2, O(13), V(0));
    a.Op1(kFlr, O(14), V(0));
    a.Op(kMul, O(15), V(0), V(1), {.s1 = "xyxy", .n1 = true, .s2 = "wzwz"});
    a.End();

    // 144 vertices of every pair of specials in every lane, then 144 with a special chosen per lane.
    const u32 pairs = kNumSpecial * kNumSpecial;
    FloatInputs in;
    in.regs = 3;
    in.vertices = 2 * pairs;
    in.data.assign(static_cast<size_t>(in.regs) * in.vertices * 4, 0.f);
    Rng rng(13);
    for (u32 v = 0; v < in.vertices; ++v) {
        for (u32 l = 0; l < 4; ++l) {
            if (v < pairs) {
                in.At(v, 0, l) = kSpecial[v / kNumSpecial];
                in.At(v, 1, l) = kSpecial[v % kNumSpecial];
                in.At(v, 2, l) = kSpecial[(v + 5 * l) % kNumSpecial];
            } else {
                in.At(v, 0, l) = kSpecial[rng.Next() % kNumSpecial];
                in.At(v, 1, l) = kSpecial[rng.Next() % kNumSpecial];
                in.At(v, 2, l) = kSpecial[rng.Next() % kNumSpecial];
            }
        }
    }
    Draw d = MakeProgram(a, in);
    Outcome o = RunCase(rig, d, in.vertices);
    CHECK_OUTCOME(o);
    REQUIRE(!o.capture.empty());

    // By value (not through the oracle): lane-uniform pairs, vertex = 12 * ia + ib.
    const auto vid = [](u32 ia, u32 ib) { return ia * kNumSpecial + ib; };
    // indexes into kSpecial: 0 = 0, 4 = +inf, 5 = -inf, 6 = NaN, 2 = 1, 3 = -1
    for (u32 inf_index : {4u, 5u}) {
        for (u32 zero_index : {0u, 1u}) {
            for (u32 order = 0; order < 2; ++order) {
                const u32 v = order ? vid(zero_index, inf_index) : vid(inf_index, zero_index);
                const float mul = At(o.capture, v, 0, 0);
                INFO("0 x inf, vertex " << v);
                CHECK(mul == 0.f);
                CHECK_FALSE(std::signbit(mul)); // +0, not -0
                CHECK(At(o.capture, v, 15, 0) == 0.f); // the negated, swizzled form
                CHECK(At(o.capture, v, 1, 0) == 0.f); // dp3: three products of +0
                CHECK(At(o.capture, v, 2, 0) == 0.f);
                CHECK_FALSE(std::isnan(At(o.capture, v, 3, 0)));
            }
        }
    }
    for (u32 nan_pair : {vid(6, 2), vid(2, 6), vid(6, 0), vid(0, 6), vid(6, 4), vid(4, 6), vid(6, 6)}) {
        CHECK(std::isnan(At(o.capture, nan_pair, 0, 0)));
        CHECK(std::isnan(At(o.capture, nan_pair, 1, 0)));
        CHECK(std::isnan(At(o.capture, nan_pair, 2, 0)));
        CHECK(std::isnan(At(o.capture, nan_pair, 3, 0)));
    }
    // MAX / MIN: `src1 > src2 ? src1 : src2`, `src1 < src2 ? src1 : src2`
    CHECK(At(o.capture, vid(0, 6), 5, 0) != At(o.capture, vid(0, 6), 5, 0)); // max(0, NaN) -> NaN
    CHECK(At(o.capture, vid(6, 0), 5, 0) == 0.f);                          // max(NaN, 0) -> 0
    CHECK(std::isnan(At(o.capture, vid(2, 6), 6, 0)));                      // min(1, NaN) -> NaN
    CHECK(At(o.capture, vid(6, 2), 6, 0) == 1.f);                          // min(NaN, 1) -> 1
    CHECK(At(o.capture, vid(2, 3), 5, 0) == 1.f);                          // max(1, -1)
    CHECK(At(o.capture, vid(2, 3), 6, 0) == -1.f);                         // min(1, -1)
    CHECK(At(o.capture, vid(2, 2), 5, 0) == 1.f);
    // comparisons with NaN are false
    CHECK(At(o.capture, vid(6, 2), 8, 0) == 0.f);
    CHECK(At(o.capture, vid(2, 6), 9, 0) == 0.f);
}

// ================================================================================================
// Flow control
// ================================================================================================

namespace {

// Runs the translated shader of `o` again, with the uniforms `d` holds now.
Report Rerun(Rig& rig, Draw& d, const Outcome& o, u32 n, bool varyings = false) {
    std::string error;
    const std::vector<float> capture = rig.Run(o.shader, d.loaded, d.setup, *d.state, n, n, false, &error, d.flip_viewport);
    REQUIRE_MESSAGE(!capture.empty(), error);
    return CompareWithOracle(d, *o.shader, capture, Iota(n), varyings);
}

void SetBools(Draw& d, u32 bits) {
    for (u32 i = 0; i < 16; ++i) {
        d.setup.uniform_b[i] = (bits >> i) & 1u;
    }
}

} // namespace

TEST_CASE("IFU and IFC: then, else, nested, every condition operator") {
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    Asm a;
    a.Cmp(Lt, Ge, V(0), V(1)); // per-vertex condition code
    a.BeginIfu(0);
    a.Op1(kMov, O(0), C(1));
    a.Else();
    a.Op1(kMov, O(0), C(2));
    a.EndIf();
    a.BeginIfu(1); // no else
    a.Op1(kMov, O(1), C(3));
    a.EndIf();
    a.Op1(kMov, O(2), C(4));
    // nested: an ifu in the then of an ifc, an ifc in the else of an ifu
    a.BeginIfc({.op = JustX, .refx = true});
    a.BeginIfu(2);
    a.Op1(kMov, O(2), C(5));
    a.Else();
    a.Op1(kMov, O(2), C(6));
    a.EndIf();
    a.Op(kAdd, O(3), C(7), V(0));
    a.Else();
    a.BeginIfu(2);
    a.Op(kAdd, O(3), C(8), V(0));
    a.Else();
    a.BeginIfc({.op = JustY, .refy = false});
    a.Op(kAdd, O(3), C(9), V(1));
    a.Else();
    a.Op(kAdd, O(3), C(10), V(1));
    a.EndIf();
    a.EndIf();
    a.EndIf();
    // all 16 (operator, refx, refy) conditions
    u32 out = 4;
    for (Cc op : {Or, And, JustX, JustY}) {
        for (bool refx : {false, true}) {
            for (bool refy : {false, true}) {
                if (out >= 16) continue;
                a.BeginIfc({.op = op, .refx = refx, .refy = refy});
                a.Op1(kMov, O(out), C(11));
                a.Else();
                a.Op1(kMov, O(out), C(12));
                a.EndIf();
                ++out;
            }
        }
    }
    a.End();
    FloatInputs in = RandomInputs(2, 64, 14);
    for (u32 v = 0; v < 64; ++v) { // equal and ordered pairs, so that every cc combination appears
        if (v % 4 == 1) in.At(v, 1, 0) = in.At(v, 0, 0);
        if (v % 4 == 2) in.At(v, 1, 1) = in.At(v, 0, 1);
        if (v % 4 == 3) {
            in.At(v, 1, 0) = in.At(v, 0, 0);
            in.At(v, 1, 1) = in.At(v, 0, 1);
        }
    }
    Draw d = MakeProgram(a, in);
    SetRandomUniforms(d, 16, 15);
    SetBools(d, 0);
    Outcome o = RunCase(rig, d, 64);
    CHECK_OUTCOME(o);
    for (u32 bits = 1; bits < 8; ++bits) {
        SetBools(d, bits);
        const Report r = Rerun(rig, d, o, 64);
        CHECK_MESSAGE(r.mismatches == 0, "bools " << bits << ": " << r.first);
    }
}

TEST_CASE("IFC with all four operators and conditions") {
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    Asm a;
    u32 out = 0;
    for (u32 pair = 0; pair < 2; ++pair) {
        a.Cmp(pair == 0 ? Lt : Eq, pair == 0 ? Gt : Ne, V(0), V(1));
        for (Cc op : {Or, And, JustX, JustY}) {
            for (bool refx : {false, true}) {
                for (bool refy : {false, true}) {
                    if (out >= 16) continue;
                    if ((op == JustX && refy != refx) || (op == JustY && refx != refy)) continue; // the unused ref
                    a.BeginIfc({.op = op, .refx = refx, .refy = refy});
                    a.Op1(kMov, O(out), C(0));
                    a.Else();
                    a.Op1(kMov, O(out), C(1));
                    a.EndIf();
                    ++out;
                }
            }
        }
    }
    a.End();
    FloatInputs in = RandomInputs(2, 64, 16);
    for (u32 v = 0; v < 64; ++v) {
        if (v % 4 == 1) in.At(v, 1, 0) = in.At(v, 0, 0);
        if (v % 4 == 2) in.At(v, 1, 1) = in.At(v, 0, 1);
        if (v % 4 == 3) {
            in.At(v, 1, 0) = in.At(v, 0, 0);
            in.At(v, 1, 1) = in.At(v, 0, 1);
        }
    }
    Draw d = MakeProgram(a, in);
    SetUniform(d, 0, 1, 2, 3, 4);
    SetUniform(d, 1, -1, -2, -3, -4);
    Outcome o = RunCase(rig, d, 64);
    CHECK_OUTCOME(o);
}

TEST_CASE("CALL, CALLC and CALLU, nested, with subroutines that end and that return") {
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    Asm a;
    a.Cmp(Gt, Lt, V(0), V(1));
    a.Op1(kMov, O(0), C(0));
    a.Call("plain");
    a.Callc({.op = JustX, .refx = true}, "ccx");
    a.Callc({.op = And, .refx = false, .refy = true}, "ccand");
    a.Callu(0, "byb0");
    a.Callu(1, "byb1");
    a.Call("outer");
    a.Nop();
    a.BeginIfu(2);
    a.Call("ender"); // a subroutine that ends: the code after is skipped when b2 is set
    a.Nop();         // (a CALL as the last instruction of an IF is never taken: see the interpreter)
    a.EndIf();
    a.Op1(kMov, O(9), C(9));
    a.End();
    a.Sub("plain");
    a.Op(kAdd, O(1), C(1), V(0));
    a.EndSub();
    a.Sub("ccx");
    a.Op(kMul, O(2), C(2), V(0));
    a.EndSub();
    a.Sub("ccand");
    a.Op(kMul, O(3), C(3), V(1));
    a.EndSub();
    a.Sub("byb0");
    a.Op(kAdd, O(4), C(4), V(1));
    a.EndSub();
    a.Sub("byb1");
    a.Op(kAdd, O(5), C(5), V(1));
    a.EndSub();
    a.Sub("outer");
    a.Op(kAdd, O(6), C(6), V(0));
    a.Call("inner");
    a.Op(kAdd, O(7), C(7), V(0));
    a.EndSub();
    a.Sub("inner");
    a.Op(kMul, O(8), C(8), V(1));
    a.Callc({.op = JustY, .refy = true}, "ccx");
    a.EndSub();
    a.Sub("ender");
    a.Op1(kMov, O(10), C(10));
    a.End();
    a.EndSub();
    a.Done();
    FloatInputs in = RandomInputs(2, 64, 17);
    for (u32 v = 0; v < 64; ++v) {
        if (v % 4 == 1) in.At(v, 1, 0) = in.At(v, 0, 0);
        if (v % 4 == 2) in.At(v, 1, 1) = in.At(v, 0, 1);
    }
    Draw d = MakeProgram(a, in, 0x7FF);
    SetRandomUniforms(d, 12, 18);
    SetBools(d, 0);
    Outcome o = RunCase(rig, d, 64);
    CHECK_OUTCOME(o);
    for (u32 bits = 1; bits < 8; ++bits) {
        SetBools(d, bits);
        const Report r = Rerun(rig, d, o, 64);
        CHECK_MESSAGE(r.mismatches == 0, "bools " << bits << ": " << r.first);
    }
}

TEST_CASE("LOOP: the address register aL, nesting and its restore, uniforms of every shape") {
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    Asm a;
    a.Op1(kMov, R(0), C(0));
    a.Op1(kMov, R(1), C(0));
    a.Op1(kMov, R(2), C(0));
    a.Op1(kMov, R(3), C(0));
    a.BeginLoop(0); // a single loop, relative addressing by aL
    a.Op(kAdd, R(0), C(10), R(0), {}, 3);
    a.EndLoop();
    a.Op1(kMov, O(0), R(0));
    a.BeginLoop(1); // nested
    a.Op(kAdd, R(1), C(20), R(1), {}, 3);
    a.BeginLoop(2);
    a.Op(kAdd, R(2), C(40), R(2), {}, 3);
    a.EndLoop();
    a.Op(kAdd, R(3), C(20), R(3), {}, 3); // aL after the inner loop: the interpreter gives back the inner loop's y
    a.EndLoop();
    a.Op1(kMov, O(1), R(1));
    a.Op1(kMov, O(2), R(2));
    a.Op1(kMov, O(3), R(3));
    a.Op1(kMov, O(4), C(30), {}, 3); // aL after the loops
    a.Op(kMul, O(5), C(50), V(0), {}, 3);
    a.End();
    FloatInputs in = RandomInputs(1, 8, 19);
    Draw d = MakeProgram(a, in, 0x3F);
    for (u32 i = 0; i < 96; ++i) {
        SetUniform(d, i, 1.0f + i, 0.5f * i, -0.25f * i, 100.f - i);
    }
    d.setup.uniform_i[0] = {3, 2, 2, 0};
    d.setup.uniform_i[1] = {1, 1, 3, 0};
    d.setup.uniform_i[2] = {2, 4, 1, 0};
    Outcome o = RunCase(rig, d, 8);
    CHECK_OUTCOME(o);
    const std::array<u8, 4> shapes[2][5] = {
        {{0, 0, 0, 0}, {0, 5, 7, 0}, {1, 250, 3, 0}, {4, 0, 255, 0}, {2, 1, 0, 0}},
        {{5, 1, 1, 0}, {0, 0, 0, 0}, {3, 2, 4, 0}, {1, 1, 1, 0}, {2, 3, 2, 0}},
    };
    for (const auto& shape : shapes) {
        for (u32 pick = 0; pick < 5; ++pick) {
            d.setup.uniform_i[0] = shape[pick];
            d.setup.uniform_i[1] = shape[(pick + 1) % 5];
            d.setup.uniform_i[2] = shape[(pick + 2) % 5];
            const Report r = Rerun(rig, d, o, 8);
            CHECK_MESSAGE(r.mismatches == 0, "ints " << pick << ": " << r.first);
        }
    }
}

TEST_CASE("BREAKC and BREAK in loops: directly, in an IFC, and in a called subroutine") {
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    Asm a;
    a.Op1(kMov, R(0), C(0));
    a.Op1(kMov, R(1), C(0));
    a.Op1(kMov, R(2), C(0));
    a.Op1(kMov, R(3), C(0));
    // loop 1: count until the counter reaches the limit (c2.x), breaking in the loop body
    a.BeginLoop(0);
    a.Op(kAdd, R(0), C(1), R(0));
    a.Cmp(Le, Le, C(2), R(0));
    a.Breakc({.op = JustX, .refx = true});
    a.Op(kAdd, R(1), C(3), R(1), {}, 3);
    a.EndLoop();
    a.Op1(kMov, O(0), R(0));
    a.Op1(kMov, O(1), R(1));
    a.Op1(kMov, O(2), C(4), {}, 3); // aL after a break: the increment was added
    // loop 2: the break is inside an IFC; the vertex decides
    a.BeginLoop(1);
    a.Op(kAdd, R(2), C(1), R(2));
    a.Cmp(Gt, Gt, R(2), V(0), {.s2 = "xxxx"});
    a.BeginIfc({.op = JustX, .refx = true});
    a.Break();
    a.EndIf();
    a.Op(kAdd, R(3), C(5), R(3));
    a.EndLoop();
    a.Op1(kMov, O(3), R(2));
    a.Op1(kMov, O(4), R(3));
    // loop 3: the break is in a subroutine the body calls, inside a nested loop
    a.Op1(kMov, R(4), C(0));
    a.Op1(kMov, R(5), C(0));
    a.BeginLoop(2);
    a.Op(kAdd, R(4), C(1), R(4));
    a.BeginLoop(0);
    a.Op(kAdd, R(5), C(1), R(5));
    a.Call("maybe_break");
    a.Op(kAdd, R(6), C(1), R(6));
    a.EndLoop();
    a.Op1(kMov, O(7), C(4), {}, 3); // aL: restored to the outer loop's
    a.EndLoop();
    a.Op1(kMov, O(5), R(4));
    a.Op1(kMov, O(6), R(5));
    a.Op1(kMov, O(8), R(6));
    a.Break(); // outside any loop it does nothing
    a.End();
    a.Sub("maybe_break");
    a.Cmp(Le, Le, C(6), R(5));
    a.Breakc({.op = JustX, .refx = true});
    a.EndSub();
    a.Done();
    FloatInputs in = RandomInputs(1, 16, 20, 0.f, 6.f);
    Draw d = MakeProgram(a, in, 0x1FF);
    for (u32 i = 0; i < 96; ++i) {
        SetUniform(d, i, 1.0f + 0.1f * i, 2.f, 3.f, 4.f);
    }
    SetUniform(d, 0, 0, 0, 0, 0);
    SetUniform(d, 1, 1, 1, 1, 1);
    SetUniform(d, 2, 4, 4, 4, 4);
    SetUniform(d, 6, 3, 3, 3, 3);
    d.setup.uniform_i[0] = {9, 1, 1, 0};
    d.setup.uniform_i[1] = {9, 0, 2, 0};
    d.setup.uniform_i[2] = {3, 0, 1, 0};
    Outcome o = RunCase(rig, d, 16);
    CHECK_OUTCOME(o);
    for (u32 limit : {1u, 3u, 5u, 12u}) {
        SetUniform(d, 2, static_cast<float>(limit), 0, 0, 0);
        SetUniform(d, 6, static_cast<float>(limit / 2), 0, 0, 0);
        const Report r = Rerun(rig, d, o, 16);
        CHECK_MESSAGE(r.mismatches == 0, "limit " << limit << ": " << r.first);
    }
}

TEST_CASE("JMPC and JMPU: forward, backward loops, jumps over a region, NOP and END") {
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    Asm a;
    a.Op1(kMov, O(0), C(0));
    a.Jmpu(0, 4); // pc 1: skip pc 2 and 3 when b0 is set
    a.Op1(kMov, O(0), C(1));
    a.Nop();
    a.Op1(kMov, O(1), C(2)); // pc 4
    a.Jmpu(1, 7, true); // pc 5: jump when b1 is clear
    a.Op1(kMov, O(1), C(3));
    a.Nop(); // pc 7
    a.Cmp(Lt, Lt, V(0), V(1)); // pc 8
    a.Jmpc({.op = JustX, .refx = false}, 11); // jump when !cc.x
    a.Op1(kMov, O(2), C(4));
    a.Nop(); // pc 11
    // a counting loop with a backward JMPC: r0.x += 1 until r0.x >= 5 (c5.x)
    a.Op1(kMov, R(0), C(0)); // pc 12
    a.Op(kAdd, R(0), C(6), R(0), {.mask = "x___"}); // pc 13
    a.Cmp(Gt, Gt, C(5), R(0));
    a.Jmpc({.op = JustX, .refx = true}, 13);
    a.Op1(kMov, O(3), R(0));
    a.BeginIfc({.op = JustY, .refy = true}); // an END inside an IFC
    a.Op1(kMov, O(4), C(7));
    a.End();
    a.EndIf();
    a.Op1(kMov, O(4), C(8));
    a.End();
    FloatInputs in = RandomInputs(2, 64, 21);
    for (u32 v = 0; v < 64; ++v) {
        if (v % 3 == 1) in.At(v, 1, 1) = in.At(v, 0, 1);
    }
    Draw d = MakeProgram(a, in, 0x1F);
    SetRandomUniforms(d, 10, 22);
    SetUniform(d, 5, 5.f, 0, 0, 0);
    SetUniform(d, 6, 1.f, 0, 0, 0);
    SetBools(d, 0);
    Outcome o = RunCase(rig, d, 64);
    CHECK_OUTCOME(o);
    CHECK(o.shader->notes.size() == 1); // the backward jump is noted
    for (u32 bits = 1; bits < 4; ++bits) {
        SetBools(d, bits);
        const Report r = Rerun(rig, d, o, 64);
        CHECK_MESSAGE(r.mismatches == 0, "bools " << bits << ": " << r.first);
    }
}

TEST_CASE("a subroutine whose paths all end but one breaks out of its loop first: the code after the loop runs") {
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    // (1) the loop body: add; IFU b0 { BREAK; END } ELSE { END }
    const auto body = [](Asm& a) {
        a.Op(kAdd, R(0), C(1), R(0));
        a.BeginIfu(0);
        a.Break();
        a.End();
        a.Else();
        a.End();
        a.EndIf();
    };
    Asm one;
    one.Op1(kMov, R(0), C(0));
    one.BeginLoop(0);
    body(one);
    one.EndLoop();
    one.Op1(kMov, O(0), R(0));
    one.Op1(kMov, O(1), C(2));
    one.End();
    // (2) the mirror: END inside a called subroutine that has its own loop with that body
    Asm two;
    two.Op1(kMov, R(0), C(0));
    two.Call("s");
    two.Op1(kMov, O(2), C(3)); // never reached: the subroutine ends the program
    two.End();
    two.Sub("s");
    two.BeginLoop(0);
    body(two);
    two.EndLoop();
    two.Op1(kMov, O(0), R(0));
    two.Op1(kMov, O(1), C(2));
    two.End();
    two.EndSub();
    two.Done();
    // (3) the loop body calls a subroutine that has the BREAK and the ENDs
    Asm three;
    three.Op1(kMov, R(0), C(0));
    three.BeginLoop(0);
    three.Call("t");
    three.Nop();
    three.EndLoop();
    three.Op1(kMov, O(0), R(0));
    three.Op1(kMov, O(1), C(2));
    three.End();
    three.Sub("t");
    body(three);
    three.EndSub();
    three.Done();

    const Asm* programs[] = {&one, &two, &three};
    for (u32 p = 0; p < 3; ++p) {
        Draw d = MakeProgram(*programs[p], RandomInputs(1, 8, 60 + p), 0x7);
        SetRandomUniforms(d, 8, 61 + p, 0.5f, 2.f);
        d.setup.uniform_i[0] = {3, 0, 1, 0};
        for (u32 b0 = 0; b0 < 2; ++b0) {
            d.setup.uniform_b[0] = b0 != 0;
            Outcome o = RunCase(rig, d, 8);
            INFO("program " << p << ", b0 = " << b0);
            CHECK_OUTCOME(o);
            if (b0 != 0) { // the break happened, the code after the loop wrote o1
                REQUIRE(!o.capture.empty());
                CHECK(At(o.capture, 0, 1, 0) == d.setup.uniform_f[2].c[0]);
            }
        }
    }
}

TEST_CASE("known difference: a CALL that is the last instruction of an IF is never taken by the interpreter, the translation takes it") {
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    Asm a;
    a.Op1(kMov, O(0), C(0));
    a.BeginIfu(0);
    a.Call("s"); // the IF's end overrides the program counter of the CALL in the interpreter (shader_interpreter.cpp)
    a.EndIf();
    a.End();
    a.Sub("s");
    a.Op1(kMov, O(0), C(1));
    a.EndSub();
    a.Done();
    Draw d = MakeProgram(a, RandomInputs(1, 4, 62), 0x1);
    SetUniform(d, 0, 1, 2, 3, 4);
    SetUniform(d, 1, 5, 6, 7, 8);
    d.setup.uniform_b[0] = true;
    Outcome o = RunCase(rig, d, 4);
    REQUIRE(o.shader != nullptr);
    REQUIRE(!o.capture.empty());
    // pinned as it is: the generated code calls the subroutine, the interpreter skips it. A program that
    // relies on either is not a valid program (the hardware's behaviour is not known); this documents the gap.
    CHECK(At(o.capture, 0, 0, 0) == 5.f);
    CHECK(o.report.mismatches > 0);
    CHECK(o.shader->differs_from_interpreter); // the translation says so
    oracle::Unit u;
    oracle::Run(OracleMemory(d.setup), OracleUniforms(d.setup), u, 0);
    CHECK(u.output[0][0].c[0] == 1.f);
}

TEST_CASE("known difference: a loop whose body ends with an if/else does not iterate after the then-branch in the interpreter, it does in the translation") {
    // shader_interpreter.cpp, after each instruction: the call-stack pop is tested against
    // old_program_counter + 1, then the if-stack (`else_address == old_program_counter + 1`: the then-branch's
    // last instruction) sets the program counter to the IF's end_address, which is the loop's end_address, and
    // only then the loop-stack is tested with `end_address == old_program_counter + 1`: that is the else
    // address, not the loop's end, so the loop does not iterate and the stale loop entry stays. The else
    // branch ends exactly at the loop's end and iterates. Azahar's x64 JIT (Compile_LOOP, structural) and its
    // GLSL decompiler iterate in both cases, and so does this translation. The oracle in this file is the same
    // code as Azahar's interpreter here (a faithful port), so it shows the interpreter's behaviour.
    // If either side changes, this case fails: re-read As built (N2-06) before changing the expectation.
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    // r0 = c0, r1 = r2 = 0; LOOP i0 (4 iterations) { r0 += c1; IFU b1 { r1 += c2 } ELSE { r2 += c3 } }
    Asm plain;
    plain.Op1(kMov, R(0), C(0));
    plain.BeginLoop(0);
    plain.Op(kAdd, R(0), C(1), R(0));
    plain.BeginIfu(1);
    plain.Op(kAdd, R(1), C(2), R(1));
    plain.Else();
    plain.Op(kAdd, R(2), C(3), R(2));
    plain.EndIf();
    plain.EndLoop();
    plain.Op1(kMov, O(0), R(0));
    plain.Op1(kMov, O(1), R(1));
    plain.Op1(kMov, O(2), R(2));
    plain.End();
    // the nested form: the inner loop's body ends with the if/else and the inner loop ends the outer body
    Asm nested;
    nested.Op1(kMov, R(0), C(0));
    nested.BeginLoop(1); // outer: 2 iterations
    nested.BeginLoop(0); // inner: 4 iterations
    nested.Op(kAdd, R(0), C(1), R(0));
    nested.BeginIfu(1);
    nested.Op(kAdd, R(1), C(2), R(1));
    nested.Else();
    nested.Op(kAdd, R(2), C(3), R(2));
    nested.EndIf();
    nested.EndLoop();
    nested.EndLoop();
    nested.Op1(kMov, O(0), R(0));
    nested.Op1(kMov, O(1), R(1));
    nested.Op1(kMov, O(2), R(2));
    nested.End();

    // What the interpreter does, worked out from its code (the iterations of the innermost add, r0 += c1):
    //   plain loop, b1 clear:   4 (the else branch ends the loop: it iterates)         = the translation
    //   plain loop, b1 set:     1 (the then-branch jumps over the loop-end test)
    //   nested loops, b1 clear: 4 (the inner loop ends on the outer loop's last instruction and only one
    //                              loop is popped or repeated per instruction: the outer never iterates)
    //   nested loops, b1 set:   1
    // The translation iterates 4 (plain) and 2 x 4 = 8 (nested) times in every case.
    const float interpreter_iterations[2][2] = {{4.f, 1.f}, {4.f, 1.f}}; // [shape][b1]
    for (int shape = 0; shape < 2; ++shape) {
        for (int b1 = 0; b1 < 2; ++b1) {
            Draw d = MakeProgram(shape == 0 ? plain : nested, RandomInputs(1, 8, 70), 0x7);
            SetUniform(d, 0, 100, 0, 0, 0);
            SetUniform(d, 1, 1, 0, 0, 0);
            SetUniform(d, 2, 10, 0, 0, 0);
            SetUniform(d, 3, 1000, 0, 0, 0);
            d.setup.uniform_i[0] = {3, 0, 0, 0};
            d.setup.uniform_i[1] = {1, 0, 0, 0};
            d.setup.uniform_b[1] = b1 != 0;
            const float translated = shape == 0 ? 4.f : 8.f;
            const float interpreted = interpreter_iterations[shape][b1];
            INFO("shape " << shape << " (0 plain loop, 1 nested loops), b1 " << b1);
            Outcome o = RunCase(rig, d, 8);
            REQUIRE(o.shader != nullptr);
            REQUIRE(!o.capture.empty());
            CHECK(o.shader->differs_from_interpreter); // detected from the shape, not from the values
            // the translation iterates as the structured reading says ...
            CHECK(At(o.capture, 0, 0, 0) == 100.f + translated);
            CHECK(At(o.capture, 0, b1 != 0 ? 1 : 2, 0) == translated * (b1 != 0 ? 10.f : 1000.f));
            // ... the interpreter as its code does
            oracle::Unit u;
            oracle::Run(OracleMemory(d.setup), OracleUniforms(d.setup), u, 0);
            CHECK(u.output[0][0].c[0] == 100.f + interpreted);
            CHECK(u.output[0][b1 != 0 ? 1 : 2].c[0] == interpreted * (b1 != 0 ? 10.f : 1000.f));
            // so the comparison with the oracle sees a difference exactly where the counts differ
            CHECK((o.report.mismatches > 0) == (translated != interpreted));
        }
    }
    // the ordinary loop of the other cases is not flagged
    Asm ordinary;
    ordinary.Op1(kMov, R(0), C(0));
    ordinary.BeginLoop(0);
    ordinary.Op(kAdd, R(0), C(1), R(0));
    ordinary.BeginIfu(1);
    ordinary.Op(kAdd, R(1), C(2), R(1));
    ordinary.Else();
    ordinary.Op(kAdd, R(2), C(3), R(2));
    ordinary.EndIf();
    ordinary.Nop(); // something after the if/else: the shape is gone
    ordinary.EndLoop();
    ordinary.Op1(kMov, O(0), R(0));
    ordinary.End();
    Draw d = MakeProgram(ordinary, RandomInputs(1, 4, 71), 0x1);
    SetUniform(d, 1, 1, 0, 0, 0);
    d.setup.uniform_i[0] = {3, 0, 0, 0};
    d.setup.uniform_b[1] = true;
    Outcome o = RunCase(rig, d, 4);
    CHECK_OUTCOME(o);
    CHECK_FALSE(o.shader->differs_from_interpreter);
}

namespace {

// One shape of the family in which the interpreter and the structured readings disagree: the program, and
// what each side must compute for o0.x from the two bools (worked out from the interpreter's code and from the
// structure; the constants are c1 = 1, c2 = 10, c3 = 100, c4 = 1000).
struct Shape {
    const char* name;
    Asm program;
    float (*structured)(bool b0, bool b1);
    float (*interpreter)(bool b0, bool b1);
    bool flagged;
};

Asm AddTo(Asm& a, u32 c) {
    a.Op(asmb::kAdd, asmb::R(0), asmb::C(c), asmb::R(0));
    return a;
}

void Finish(Asm& a) {
    a.Op1(asmb::kMov, asmb::O(0), asmb::R(0));
    a.End();
}

} // namespace

TEST_CASE("known differences, flagged: an if at the end of a loop, call or outer then-branch; and shapes that must stay unflagged") {
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    std::vector<Shape> shapes;
    {   // F2: LOOP { r0 += c1; IFU b1 { r0 += c2 } }: an if without else ends the loop body
        Asm a;
        a.Op1(kMov, R(0), C(0));
        a.BeginLoop(0);
        AddTo(a, 1);
        a.BeginIfu(1);
        AddTo(a, 2);
        a.EndIf();
        a.EndLoop();
        Finish(a);
        shapes.push_back({"F2 loop body ends with an if without else", a,
                          [](bool, bool b1) { return 4.f * (b1 ? 11.f : 1.f); },
                          // the false IF jumps to the loop's end, where the loop test (against the IF's address + 1) does not fire
                          [](bool, bool b1) { return b1 ? 44.f : 1.f; }, true});
    }
    {   // F8: call s; s = { r0 += c1; IFU b1 { r0 += c2 } }
        Asm a;
        a.Op1(kMov, R(0), C(0));
        a.Call("s");
        Finish(a);
        a.Sub("s");
        AddTo(a, 1);
        a.BeginIfu(1);
        AddTo(a, 2);
        a.EndIf();
        a.EndSub();
        a.Done();
        shapes.push_back({"F8 call body ends with an if without else", a,
                          [](bool, bool b1) { return b1 ? 11.f : 1.f; },
                          // the call never returns: the program runs on into the empty words after the subroutine and the
                          // mov to o0 is never reached (o0.x keeps its initial 0)
                          [](bool, bool) { return 0.f; }, true});
    }
    {   // F3: IFU b0 { IFU b1 { r0 += c1 } ELSE { r0 += c2 } } ELSE { r0 += c3 }
        Asm a;
        a.Op1(kMov, R(0), C(0));
        a.BeginIfu(0);
        a.BeginIfu(1);
        AddTo(a, 1);
        a.Else();
        AddTo(a, 2);
        a.EndIf();
        a.Else();
        AddTo(a, 3);
        a.EndIf();
        Finish(a);
        shapes.push_back({"F3 if/else at the tail of an outer then-branch", a,
                          [](bool b0, bool b1) { return b0 ? (b1 ? 1.f : 10.f) : 100.f; },
                          // b0 and b1: the inner pop leaves the outer entry, the outer else branch runs too
                          [](bool b0, bool b1) { return b0 ? (b1 ? 101.f : 10.f) : 100.f; }, true});
    }
    {   // F4: IFU b0 { r0 += c1; IFU b1 { r0 += c2 } } ELSE { r0 += c3 }
        Asm a;
        a.Op1(kMov, R(0), C(0));
        a.BeginIfu(0);
        AddTo(a, 1);
        a.BeginIfu(1);
        AddTo(a, 2);
        a.EndIf();
        a.Else();
        AddTo(a, 3);
        a.EndIf();
        Finish(a);
        shapes.push_back({"F4 if without else at the tail of an outer then-branch", a,
                          [](bool b0, bool b1) { return b0 ? (b1 ? 11.f : 1.f) : 100.f; },
                          [](bool b0, bool b1) { return b0 ? (b1 ? 111.f : 101.f) : 100.f; }, true});
    }
    {   // F1: LOOP { r0 += c1; IFU b1 { r0 += c2 } ELSE { r0 += c3 }; r0 += c4 }: the if/else is in the middle
        Asm a;
        a.Op1(kMov, R(0), C(0));
        a.BeginLoop(0);
        AddTo(a, 1);
        a.BeginIfu(1);
        AddTo(a, 2);
        a.Else();
        AddTo(a, 3);
        a.EndIf();
        AddTo(a, 4);
        a.EndLoop();
        Finish(a);
        shapes.push_back({"F1 if/else in the middle of a loop body", a,
                          [](bool, bool b1) { return 4.f * (1.f + (b1 ? 10.f : 100.f) + 1000.f); },
                          [](bool, bool b1) { return 4.f * (1.f + (b1 ? 10.f : 100.f) + 1000.f); }, false});
    }
    {   // F9: IFU b0 { r0 += c1 } ELSE { r0 += c2; IFU b1 { r0 += c3 } ELSE { r0 += c4 } }: the tail of an outer ELSE
        Asm a;
        a.Op1(kMov, R(0), C(0));
        a.BeginIfu(0);
        AddTo(a, 1);
        a.Else();
        AddTo(a, 2);
        a.BeginIfu(1);
        AddTo(a, 3);
        a.Else();
        AddTo(a, 4);
        a.EndIf();
        a.EndIf();
        Finish(a);
        auto f = [](bool b0, bool b1) { return b0 ? 1.f : 10.f + (b1 ? 100.f : 1000.f); };
        shapes.push_back({"F9 if/else at the tail of an outer else-branch", a, f, f, false});
    }
    {   // F10: LOOP { r0 += c1; IFU b1 { r0 += c2 }; r0 += c3 }: an if without else in the middle
        Asm a;
        a.Op1(kMov, R(0), C(0));
        a.BeginLoop(0);
        AddTo(a, 1);
        a.BeginIfu(1);
        AddTo(a, 2);
        a.EndIf();
        AddTo(a, 3);
        a.EndLoop();
        Finish(a);
        auto f = [](bool, bool b1) { return 4.f * (1.f + (b1 ? 10.f : 0.f) + 100.f); };
        shapes.push_back({"F10 if without else in the middle of a loop body", a, f, f, false});
    }
    {   // F12: LOOP outer (2) { LOOP inner (4) { r0 += c1 }; r0 += c2 }: the inner loop is not the last statement
        Asm a;
        a.Op1(kMov, R(0), C(0));
        a.BeginLoop(1);
        a.BeginLoop(0);
        AddTo(a, 1);
        a.EndLoop();
        AddTo(a, 2);
        a.EndLoop();
        Finish(a);
        auto f = [](bool, bool) { return 2.f * (4.f + 10.f); };
        shapes.push_back({"F12 nested loops, the inner not last", a, f, f, false});
    }

    for (Shape& shape : shapes) {
        for (int bits = 0; bits < 4; ++bits) {
            const bool b0 = (bits & 1) != 0, b1 = (bits & 2) != 0;
            Draw d = MakeProgram(shape.program, RandomInputs(1, 8, 80), 0x1);
            SetUniform(d, 0, 0, 0, 0, 0);
            SetUniform(d, 1, 1, 0, 0, 0);
            SetUniform(d, 2, 10, 0, 0, 0);
            SetUniform(d, 3, 100, 0, 0, 0);
            SetUniform(d, 4, 1000, 0, 0, 0);
            d.setup.uniform_i[0] = {3, 0, 0, 0}; // 4 iterations
            d.setup.uniform_i[1] = {1, 0, 0, 0}; // 2 iterations
            d.setup.uniform_b[0] = b0;
            d.setup.uniform_b[1] = b1;
            INFO(shape.name << ", b0 " << b0 << ", b1 " << b1);
            Outcome o = RunCase(rig, d, 8);
            REQUIRE(o.shader != nullptr);
            REQUIRE(!o.capture.empty());
            CHECK(o.shader->differs_from_interpreter == shape.flagged);
            const float structured = shape.structured(b0, b1);
            const float interpreted = shape.interpreter(b0, b1);
            CHECK(At(o.capture, 0, 0, 0) == structured); // the translation: the structured reading
            oracle::Unit u;
            oracle::Run(OracleMemory(d.setup), OracleUniforms(d.setup), u, 0);
            CHECK(u.output[0][0].c[0] == interpreted);   // the interpreter: its own code
            CHECK((o.report.mismatches > 0) == (structured != interpreted));
            if (!shape.flagged) {
                CHECK(o.report.mismatches == 0); // the over-flagging guard: unflagged shapes agree
            }
        }
    }
}

// ================================================================================================
// The cache
// ================================================================================================

TEST_CASE("the cache: a hit does not compile; another program, swizzle, output map, conversion or option is another key") {
    using namespace asmb;
    Asm a;
    a.Op(kAdd, O(0), V(0), V(1));
    a.Op1(kMov, O(1), V(2), {.mask = "xy__"});
    a.Nop();
    a.End();
    Draw d = MakeProgram(a, RandomInputs(3, 4, 40), 0x3);
    SetOutmap(*d.state, StandardOutmap());
    shader::ConfigOptions opt;
    shader::VertexShaderCache cache;
    const auto key = [&] {
        d.state->vs = d.setup;
        return shader::MakeVertexShaderConfig(*d.state, &d.loaded, opt);
    };
    const shader::VertexShaderConfig base = key();
    shader::TranslateResult first = cache.Get(d.setup, base);
    REQUIRE_MESSAGE(first.ok(), first.error);
    CHECK(cache.stats().lookups == 1);
    CHECK(cache.stats().compiles == 1);
    CHECK(cache.stats().hits == 0);
    CHECK(cache.size() == 1);
    // the same key built again from the same state: a hit, the same object
    shader::TranslateResult again = cache.Get(d.setup, key());
    CHECK(again.shader == first.shader);
    CHECK(cache.stats().hits == 1);
    CHECK(cache.stats().compiles == 1);
    CHECK(key() == base);
    CHECK(key().Hash() == base.Hash());
    // another state with the same program, swizzle, output map and conversions (a different object
    // built independently) is the same key
    Draw twin = MakeProgram(a, RandomInputs(3, 7, 41), 0x3);
    SetOutmap(*twin.state, StandardOutmap());
    twin.state->vs = twin.setup;
    CHECK(shader::MakeVertexShaderConfig(*twin.state, &twin.loaded, opt) == base);
    // uniforms and the vertices are not part of the key
    SetRandomUniforms(d, 8, 42);
    d.setup.uniform_b[3] = true;
    d.setup.uniform_i[1] = {1, 2, 3, 4};
    CHECK(key() == base);
    CHECK(cache.Get(d.setup, key()).shader == first.shader);
    CHECK(cache.stats().compiles == 1);

    u64 expected_compiles = 1;
    const auto differs = [&](const char* what, const auto& change, const auto& restore) {
        change();
        const shader::VertexShaderConfig k = key();
        CHECK_MESSAGE(!(k == base), what);
        shader::TranslateResult r = cache.Get(d.setup, k);
        CHECK_MESSAGE(r.ok(), what << ": " << r.error);
        CHECK_MESSAGE(r.shader != first.shader, what);
        ++expected_compiles;
        CHECK_MESSAGE(cache.stats().compiles == expected_compiles, what);
        restore();
        CHECK_MESSAGE(key() == base, what);
        CHECK(cache.Get(d.setup, key()).shader == first.shader);
    };
    const u32 nop_word = d.setup.program_code[2];
    differs("a program word", [&] { d.setup.UpdateProgramCode(2, nop_word | 1u); },
            [&] { d.setup.UpdateProgramCode(2, nop_word); });
    const u32 swz = d.setup.swizzle_data[0];
    differs("a swizzle word", [&] { d.setup.UpdateSwizzleData(0, swz ^ 0x10u); },
            [&] { d.setup.UpdateSwizzleData(0, swz); });
    differs("the output mask", [&] { SetShaderRegs(*d.state, 0, 0x7); }, [&] { SetShaderRegs(*d.state, 0, 0x3); });
    differs("the entry point", [&] { SetShaderRegs(*d.state, 2, 0x3); }, [&] { SetShaderRegs(*d.state, 0, 0x3); });
    const u32 outmap0 = d.state->regs[shader::reg::kVsOutputAttributes];
    differs("an OUTMAP register", [&] { d.state->regs[shader::reg::kVsOutputAttributes] = outmap0 ^ 0x01; },
            [&] { d.state->regs[shader::reg::kVsOutputAttributes] = outmap0; });
    differs("the OUTMAP count", [&] { d.state->regs[shader::reg::kVsOutputTotal] = 5; },
            [&] { d.state->regs[shader::reg::kVsOutputTotal] = 6; });
    differs("an input's conversion", [&] { d.loaded.attributes[1].conversion = vertex::Conversion::FromSint; },
            [&] { d.loaded.attributes[1].conversion = vertex::Conversion::None; });
    differs("an input's conversion (unsigned)", [&] { d.loaded.attributes[1].conversion = vertex::Conversion::FromUint; },
            [&] { d.loaded.attributes[1].conversion = vertex::Conversion::None; });
    differs("an input's w alias", [&] { d.loaded.attributes[5].alias_w = true; },
            [&] { d.loaded.attributes[5].alias_w = false; });
    differs("the clip-plane option", [&] { opt.use_clip_planes = !opt.use_clip_planes; },
            [&] { opt.use_clip_planes = !opt.use_clip_planes; });
    differs("the geometry-stage option", [&] { opt.geometry_stage = true; }, [&] { opt.geometry_stage = false; });
    differs("the test hook", [&] { opt.capture_outputs = true; }, [&] { opt.capture_outputs = false; });
    CHECK(cache.size() == expected_compiles);
    CHECK(cache.stats().failures == 0);
    cache.Clear();
    CHECK(cache.size() == 0);
}

TEST_CASE("the cache remembers a program that is not translated") {
    using namespace asmb;
    Asm a;
    a.Jmpu(0, 3000); // outside the loaded program
    a.End();
    Draw d = MakeProgram(a, RandomInputs(1, 2, 43), 0x1);
    d.state->vs = d.setup;
    const shader::VertexShaderConfig cfg = shader::MakeVertexShaderConfig(*d.state, &d.loaded);
    shader::VertexShaderCache cache;
    shader::TranslateResult r1 = cache.Get(d.setup, cfg);
    CHECK_FALSE(r1.ok());
    CHECK(r1.error.find("loaded") != std::string::npos);
    shader::TranslateResult r2 = cache.Get(d.setup, cfg);
    CHECK_FALSE(r2.ok());
    CHECK(r2.error == r1.error);
    CHECK(cache.stats().compiles == 1);
    CHECK(cache.stats().failures == 1);
    CHECK(cache.stats().hits == 1);
}

TEST_CASE("the cache from several threads: one key, one result") {
    using namespace asmb;
    Asm a;
    a.Op(kMul, O(0), V(0), V(1));
    a.End();
    Draw d = MakeProgram(a, RandomInputs(2, 2, 44), 0x1);
    d.state->vs = d.setup;
    const shader::VertexShaderConfig cfg = shader::MakeVertexShaderConfig(*d.state, &d.loaded);
    shader::VertexShaderCache cache;
    std::vector<std::shared_ptr<const shader::TranslatedVertexShader>> got(4);
    std::vector<std::thread> threads;
    for (size_t i = 0; i < got.size(); ++i) {
        threads.emplace_back([&, i] { got[i] = cache.Get(d.setup, cfg).shader; });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    for (const auto& g : got) {
        REQUIRE(g != nullptr);
        CHECK(g == got[0]); // a second translation of the key is dropped
    }
    CHECK(cache.size() == 1);
    CHECK(cache.stats().lookups == 4);
}

// ================================================================================================
// Programs that are not programs
// ================================================================================================

namespace {

shader::TranslateResult TranslateRaw(const Asm& a, u32 main_offset = 0) {
    Draw d;
    a.Install(d.setup);
    SetShaderRegs(*d.state, main_offset, 0x1);
    d.state->vs = d.setup;
    d.loaded = MakeDraw(RandomInputs(1, 2, 1));
    const shader::VertexShaderConfig cfg = shader::MakeVertexShaderConfig(*d.state, &d.loaded);
    return shader::TranslateVertexShader(d.setup, cfg);
}

} // namespace

TEST_CASE("malformed programs are rejected with a message, never run") {
    using namespace asmb;
    {   // a jump outside the loaded code
        Asm a;
        a.Jmpu(0, 3000);
        a.End();
        const auto r = TranslateRaw(a);
        CHECK_FALSE(r.ok());
        CHECK(r.error.find("loaded") != std::string::npos);
    }
    {   // a call outside the loaded code
        Asm a;
        a.Raw(Asm::Flow(kCall, 1000, 4, 0));
        a.End();
        const auto r = TranslateRaw(a);
        CHECK_FALSE(r.ok());
        CHECK(r.error.find("loaded") != std::string::npos);
    }
    {   // no END: the code runs off the loaded words
        Asm a;
        a.Op1(kMov, O(0), V(0));
        a.Op1(kMov, O(0), V(0));
        const auto r = TranslateRaw(a);
        CHECK_FALSE(r.ok());
        CHECK(r.error.find("loaded") != std::string::npos);
    }
    {   // recursion
        Asm a;
        a.Call("loop");
        a.End();
        a.Sub("loop");
        a.Nop();
        a.Call("loop");
        a.Nop();
        a.EndSub();
        a.Done();
        const auto r = TranslateRaw(a);
        CHECK_FALSE(r.ok());
        CHECK(r.error.find("Recursive") != std::string::npos);
    }
    {   // an instruction that is not one
        Asm a;
        a.Raw(0x10u << 26);
        a.End();
        const auto r = TranslateRaw(a);
        CHECK_FALSE(r.ok());
        CHECK(r.error.find("Unhandled instruction") != std::string::npos);
    }
    {   // an empty program, and an entry point outside the memory
        Asm a;
        const auto r = TranslateRaw(a);
        CHECK_FALSE(r.ok());
        Asm b;
        b.End();
        const auto far = TranslateRaw(b, 5000);
        CHECK_FALSE(far.ok());
        CHECK(far.error.find("entry point") != std::string::npos);
        const auto beyond = TranslateRaw(b, 100); // inside the memory, outside the loaded words
        CHECK_FALSE(beyond.ok());
    }
    {   // a forever-true backward jump is accepted, bounded, and says so
        Asm a;
        a.Op1(kMov, O(0), C(0));
        a.Jmpu(0, 0);
        a.End();
        const auto r = TranslateRaw(a);
        REQUIRE_MESSAGE(r.ok(), r.error);
        REQUIRE(!r.shader->notes.empty());
        CHECK(r.shader->notes[0].find("backwards") != std::string::npos);
        CHECK(r.shader->glsl.find("jmp_guard < 65536u") != std::string::npos);
    }
    {   // a program of thousands of instructions is translated or refused, in bounded time
        Asm a;
        for (u32 i = 0; i < 3000; ++i) {
            a.Op(kAdd, R(i % 16), V(i % 4), V((i + 1) % 4));
        }
        a.Op1(kMov, O(0), R(0));
        a.End();
        const auto start = std::chrono::steady_clock::now();
        const auto r = TranslateRaw(a);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        CHECK(seconds < 60.0);
        MESSAGE("3000 instructions: " << (r.ok() ? "translated" : "refused: " + r.error) << " in " << seconds << " s");
    }
}

TEST_CASE("random words never crash the translator, hang it or give GLSL that does not compile") {
    using namespace asmb;
    Rng rng(45);
    u32 accepted = 0, rejected = 0, compile_failures = 0, compiled = 0;
    std::string first_failure;
    const auto start = std::chrono::steady_clock::now();
    for (u32 p = 0; p < 500; ++p) {
        Asm a;
        const u32 length = 4 + rng.Next() % 40;
        for (u32 i = 0; i + 1 < length; ++i) {
            u32 word = rng.Next();
            const u32 kind = rng.Next() % 10;
            if (kind < 5) { // an arithmetic or mad instruction, any fields
                static const u32 kOps[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0x12, 0x13, 0x18, 0x19,
                                           0x1A, 0x1B, 0x2E, 0x30, 0x38};
                word = (word & 0x03FFFFFFu) | (kOps[rng.Next() % 25] << 26);
            } else { // flow control
                static const u32 kFlow[] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D};
                const u32 op = kFlow[rng.Next() % 14];
                const u32 dest = rng.Next() % (length + 3);
                const u32 num = rng.Next() % 6;
                word = (op << 26) | (dest << 10) | num | (word & (0xFu << 22));
            }
            a.Raw(word);
        }
        a.End();
        Draw d;
        a.Install(d.setup);
        for (u32 i = 0; i < 128; ++i) {
            d.setup.UpdateSwizzleData(i, rng.Next());
        }
        SetShaderRegs(*d.state, rng.Next() % 8 == 0 ? rng.Next() % length : 0, 0xFFFF);
        d.state->vs = d.setup;
        d.loaded = MakeDraw(RandomInputs(4, 2, 1));
        const shader::VertexShaderConfig cfg = shader::MakeVertexShaderConfig(*d.state, &d.loaded);
        shader::TranslateResult g = shader::GenerateVertexGlsl(d.setup, cfg);
        if (!g.ok()) {
            ++rejected;
            CHECK_FALSE(g.error.empty());
            continue;
        }
        ++accepted;
        { // compile every accepted program
            std::vector<u32> spirv;
            std::string error;
            ++compiled;
            if (!shader::CompileGlslToSpirv(g.shader->glsl, spirv, error)) {
                ++compile_failures;
                if (first_failure.empty()) {
                    first_failure = error.substr(0, 300);
                }
            }
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    MESSAGE("random programs: " << accepted << " translated, " << rejected << " rejected, " << compiled
                                << " compiled, " << compile_failures << " did not compile, " << seconds << " s");
    CHECK(accepted > 0);
    CHECK(rejected > 0);
    CHECK_MESSAGE(compile_failures == 0, first_failure);
}

TEST_CASE("EMIT and SETEMIT in a vertex program do nothing (as Azahar's decompiler and a vertex unit)") {
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    Asm a;
    a.Op(kAdd, O(0), V(0), V(1));
    a.Setemit(1, true, false);
    a.Emit();
    a.Op(kMul, O(1), V(0), V(1));
    a.Setemit(2, false, true);
    a.End();
    Draw d = MakeProgram(a, RandomInputs(2, 16, 50), 0x3);
    Outcome o = RunCase(rig, d, 16);
    CHECK_OUTCOME(o);
    REQUIRE(o.shader != nullptr);
    REQUIRE(!o.shader->notes.empty());
    CHECK(o.shader->notes[0].find("EMIT") != std::string::npos);
}

TEST_CASE("a backward jump that never ends ends the shader on the device") {
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    Asm a;
    a.Op1(kMov, O(0), C(0));
    a.Jmpu(0, 0); // b0 is set: forever
    a.Op1(kMov, O(1), C(0));
    a.End();
    Draw d = MakeProgram(a, RandomInputs(1, 8, 46), 0x3);
    SetUniform(d, 0, 7, 8, 9, 10);
    d.setup.uniform_b[0] = true;
    std::string error;
    auto t = Translate(d, false, false, &error);
    REQUIRE_MESSAGE(t != nullptr, error);
    CheckSpirvValid(t->spirv, "the endless loop");
    const auto start = std::chrono::steady_clock::now();
    const std::vector<float> cap = rig.Run(t, d.loaded, d.setup, *d.state, 8, 8, false, &error);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    REQUIRE_MESSAGE(!cap.empty(), error);
    MESSAGE("the endless loop ran 8 vertices in " << seconds << " s");
    CHECK(seconds < 120.0);
    for (u32 v = 0; v < 8; ++v) {
        CHECK(At(cap, v, 0, 0) == 7.f); // the first instruction ran, the one after the jump never did
        CHECK(At(cap, v, 1, 0) == 0.f);
    }
    // the interpreter has no bound: the oracle gives up (its instruction budget)
    oracle::Unit u;
    CHECK(oracle::Run(OracleMemory(d.setup), OracleUniforms(d.setup), u, 0, 100000) == oracle::Status::NotTerminating);
    // with the bool clear the same program ends normally
    d.setup.uniform_b[0] = false;
    const std::vector<float> cap2 = rig.Run(t, d.loaded, d.setup, *d.state, 8, 8, false, &error);
    REQUIRE(!cap2.empty());
    CHECK(At(cap2, 0, 1, 0) == 7.f);
}

// ================================================================================================
// From the PICA's registers: a command list, the processor, the vertex loader, the translation
// ================================================================================================

namespace {

constexpr u32 kFcram = 0x20000000;
constexpr u32 kBase = kFcram + 0x100000; // the vertex arrays' base address (a multiple of 16)
constexpr u32 kListAt = kFcram + 0x1000;

// A command list: [value, header, ...] commands (pica_core.cpp's loop), mask 0xF.
class CmdList {
public:
    CmdList& Write(u32 id, u32 value) {
        if (words_.size() % 2 != 0) {
            words_.push_back(0xDEADBEEF);
        }
        words_.push_back(value);
        words_.push_back(id | (0xFu << 16));
        return *this;
    }
    CmdList& Vs(u32 rel, u32 value) { return Write(0x2B0 + rel, value); }
    std::vector<u8> Bytes() const {
        std::vector<u32> w = words_;
        if (w.size() % 2 != 0) {
            w.push_back(0);
        }
        std::vector<u8> b(w.size() * 4);
        std::memcpy(b.data(), w.data(), b.size());
        return b;
    }

    // The vertex shader unit's program, swizzle table, float uniforms (float32 format), bools and ints.
    CmdList& Program(const Asm& a) {
        Vs(0x1B, 0);
        for (u32 w : a.code) {
            Vs(0x1C, w);
        }
        Vs(0x25, 0);
        for (u32 w : a.descs) {
            Vs(0x26, w);
        }
        return *this;
    }
    CmdList& FloatUniform(u32 index, float x, float y, float z, float w) {
        Vs(0x10, (1u << 31) | index);
        for (float f : {w, z, y, x}) { // float32 mode: the first word is w
            u32 bits;
            std::memcpy(&bits, &f, 4);
            Vs(0x11, bits);
        }
        return *this;
    }

private:
    std::vector<u32> words_;
};

// The vertex layout of the e2e program: attribute 0 = FLOAT x4, attribute 1 = UBYTE x3, attribute 2 =
// SHORT x2, all in one loader of 24 bytes per vertex.
void SetupAttributes(CmdList& list, u32 base = kBase) {
    list.Write(0x200, (base >> 4) << 1);
    list.Write(0x201, (3u | (3u << 2)) | ((1u | (2u << 2)) << 4) | ((2u | (1u << 2)) << 8));
    list.Write(0x202, 2u << 28); // 3 attributes, none default
    list.Write(0x203, 0);
    list.Write(0x204, 0u | (1u << 4) | (2u << 8));
    list.Write(0x205, (24u << 16) | (3u << 28));
    list.Vs(9, 2);                                   // the last input attribute
    list.Vs(0xB, 0u | (1u << 4) | (2u << 8));        // attribute n -> register n
}

void SetupOutputs(CmdList& list, u32 mask, const std::vector<std::array<u32, 4>>& outmap) {
    list.Vs(0x0A, 0); // main offset
    list.Vs(0x0D, mask);
    list.Write(shader::reg::kVsOutputTotal, static_cast<u32>(outmap.size()));
    for (u32 a = 0; a < outmap.size(); ++a) {
        list.Write(shader::reg::kVsOutputAttributes + a,
                   outmap[a][0] | (outmap[a][1] << 8) | (outmap[a][2] << 16) | (outmap[a][3] << 24));
    }
}

// 24 bytes a vertex: float4, ubyte3 (+1 pad), short2 (+0), as the PICA loader lays them out.
std::vector<u8> VertexBytes(u32 count, u64 seed) {
    std::vector<u8> bytes(static_cast<size_t>(count) * 24, 0);
    Rng rng(seed);
    for (u32 v = 0; v < count; ++v) {
        u8* p = bytes.data() + static_cast<size_t>(v) * 24;
        for (u32 c = 0; c < 4; ++c) {
            const float f = rng.Uniform(-3.f, 3.f);
            std::memcpy(p + 4 * c, &f, 4);
        }
        for (u32 c = 0; c < 3; ++c) {
            p[16 + c] = static_cast<u8>(rng.Next());
        }
        p[19] = 0x7F; // the byte after the three is not data
        for (u32 c = 0; c < 2; ++c) {
            const int16_t s = static_cast<int16_t>(rng.Next());
            std::memcpy(p + 20 + 2 * c, &s, 2);
        }
    }
    return bytes;
}

Asm EndToEndProgram() {
    using namespace asmb;
    Asm a;
    a.Op1(kMov, O(0), V(0));                                   // float4 as it is
    a.Op1(kMov, O(1), V(1));                                   // ubyte3: w = 1.0
    a.Op1(kMov, O(2), V(2));                                   // short2: (x, y, 0, 1)
    a.Op(kMul, O(3), C(0), V(0));                              // a uniform
    a.Mad(O(4), V(0), C(1), V(1));
    a.BeginIfu(0);                                             // a bool
    a.Op(kAdd, O(5), V(0), V(1));
    a.Else();
    a.Op(kAdd, O(5), V(1), V(2));
    a.EndIf();
    a.Op1(kMov, R(0), V(2));
    a.BeginLoop(0);                                            // an integer vector
    a.Op(kAdd, R(0), C(2), R(0), {}, 3);
    a.EndLoop();
    a.Op1(kMov, O(6), R(0));
    a.End();
    return a;
}

void SetupEndToEnd(CmdList& list, const Asm& program, u32 vertices, bool bool0, float scale, u32 base = kBase) {
    SetupAttributes(list, base);
    list.Program(program);
    list.FloatUniform(0, scale, scale * 2, -scale, 0.5f);
    list.FloatUniform(1, 1.f, 0.f, 2.f, 1.f);
    list.FloatUniform(2, 0.25f, 0.5f, 0.75f, 1.f);
    list.FloatUniform(3, 0.f, 1.f, 2.f, 3.f);
    list.FloatUniform(4, 10.f, 20.f, 30.f, 40.f);
    list.FloatUniform(5, 100.f, 200.f, 300.f, 400.f);
    list.Vs(0x00, bool0 ? 1u : 0u);                 // bool uniforms
    list.Vs(0x01, 2u | (1u << 8) | (1u << 16));     // int uniform 0: x = 2, y = 1, z = 1
    SetupOutputs(list, 0x7F, StandardOutmap());
    list.Write(0x25E, 0);                           // list topology
    list.Write(0x228, vertices);
    list.Write(0x22A, 0);
    list.Write(0x22E, 1);                           // draw arrays
}

} // namespace

TEST_CASE("from PICA registers: the processor's state, the vertex loader's description, the translated shader") {
    Rig rig;
    if (!rig.ok()) return;
    CaptureHost host;
    constexpr u32 kVertices = 20;
    const std::vector<u8> vertices = VertexBytes(kVertices, 47);
    REQUIRE(host.WriteBoth(kBase, vertices.data(), static_cast<u32>(vertices.size())));
    CmdList list;
    SetupEndToEnd(list, EndToEndProgram(), kVertices, true, 1.5f);
    const std::vector<u8> bytes = list.Bytes();
    REQUIRE(host.WriteBoth(kListAt, bytes.data(), static_cast<u32>(bytes.size())));

    pica::RecordingBackend recorder;
    pica::CommandProcessor processor(host, recorder);
    processor.ProcessCmdList(kListAt, static_cast<u32>(bytes.size()));
    REQUIRE(recorder.draws.size() == 1);
    const pica::RecordedDraw& recorded = recorder.draws[0];
    const pica::PicaState& state = *recorded.state;
    CHECK(state.vs.biggest_program_size > 10);
    CHECK(state.vs.uniform_b[0]);
    CHECK(state.vs.uniform_i[0][0] == 2);

    vertex::VertexLoader loader{vertex::VertexCaps{}};
    const vertex::LoadedDraw ld = loader.Load(host, recorded.call);
    REQUIRE(ld.status == vertex::LoadStatus::Ok);
    CHECK(ld.attributes[0].conversion == vertex::Conversion::None);
    CHECK(ld.attributes[1].conversion == vertex::Conversion::FromUint);
    CHECK(ld.attributes[1].alias_w);
    CHECK(ld.attributes[2].conversion == vertex::Conversion::FromSint);
    CHECK_FALSE(ld.attributes[2].alias_w);

    shader::ConfigOptions opt;
    opt.capture_outputs = true;
    opt.use_clip_planes = false;
    const shader::VertexShaderConfig cfg = shader::MakeVertexShaderConfig(state, &ld, opt);
    CHECK(cfg.output_mask == 0x7F);
    CHECK(cfg.outmap_total == 6);
    CHECK(cfg.main_offset == 0);
    CHECK(cfg.inputs[1].conversion == vertex::Conversion::FromUint);
    CHECK(cfg.inputs[1].alias_w);
    shader::VertexShaderCache cache;
    shader::TranslateResult t = cache.Get(state.vs, cfg);
    REQUIRE_MESSAGE(t.ok(), t.error);
    CheckSpirvValid(t.shader->spirv, "the end-to-end shader");
    std::string error;
    const std::vector<float> cap = rig.Run(t.shader, ld, state.vs, state, kVertices, kVertices, false, &error);
    REQUIRE_MESSAGE(!cap.empty(), error);

    // the oracle, on the loader's description
    const Report report = CompareWithOracle(state.vs, state, ld, false, *t.shader, cap, Iota(kVertices), true);
    CHECK_MESSAGE(report.mismatches == 0, report.first);
    CHECK(report.values > 0);
    // and by value from the bytes in memory
    for (u32 v = 0; v < kVertices; ++v) {
        const u8* p = vertices.data() + static_cast<size_t>(v) * 24;
        float f[4];
        std::memcpy(f, p, 16);
        for (u32 c = 0; c < 4; ++c) {
            CHECK(At(cap, v, 0, c) == f[c]);             // o0 = v0
        }
        for (u32 c = 0; c < 3; ++c) {
            CHECK(At(cap, v, 1, c) == static_cast<float>(p[16 + c])); // o1 = ubyte3, not normalised
        }
        CHECK(At(cap, v, 1, 3) == 1.0f);                 // w of the aliased attribute
        int16_t s[2];
        std::memcpy(s, p + 20, 4);
        CHECK(At(cap, v, 2, 0) == static_cast<float>(s[0]));
        CHECK(At(cap, v, 2, 1) == static_cast<float>(s[1]));
        CHECK(At(cap, v, 2, 2) == 0.0f);
        CHECK(At(cap, v, 2, 3) == 1.0f);
        CHECK(Near(At(cap, v, 3, 0), 1.5f * f[0]));      // o3 = c0 * v0
        CHECK(Near(At(cap, v, 3, 1), 3.0f * f[1]));
        // the loop (x = 2, y = 1, z = 1): r0 = v2 + c[aL + 2] for aL = 1, 2, 3 = c3.x + c4.x + c5.x = 0 + 10 + 100
        CHECK(Near(At(cap, v, 6, 0), static_cast<float>(s[0]) + 110.f));
        CHECK(Near(At(cap, v, 6, 1), static_cast<float>(s[1]) + 1.f + 20.f + 200.f));
    }
}

// ================================================================================================
// The dump check: capture replay through the PICA processor and the translation
// ================================================================================================

namespace {

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
            const u32 size = mh4n::gpu_capture::InitBlockSize(kind);
            Put32(payload, kind);
            Put32(payload, size);
            payload.resize(payload.size() + size, 0);
        }
        Record(mh4n::gpu_capture::kInit, payload);
    }
    void Mem(u32 addr, const std::vector<u8>& data) {
        std::vector<u8> payload;
        Put32(payload, addr);
        Put32(payload, static_cast<u32>(data.size()));
        payload.insert(payload.end(), data.begin(), data.end());
        Record(mh4n::gpu_capture::kMem, payload);
    }
    void SubmitCmdList(u32 vaddr, u32 size) {
        std::vector<u8> payload;
        for (u32 w : {1u, vaddr, size, 0u, 0u, 0u, 0u, 0u}) {
            Put32(payload, w);
        }
        Record(mh4n::gpu_capture::kGx, payload);
    }
    void NoRegisterWrites() {
        std::vector<u8> payload;
        Put32(payload, 0);
        Record(mh4n::gpu_capture::kPicaReg, payload);
    }
    void Frame(u32 n) {
        frame = n;
        std::vector<u8> payload;
        Put32(payload, n);
        Record(mh4n::gpu_capture::kFrame, payload);
    }
    std::string Save(const std::string& name) const {
        const auto path = fs::temp_directory_path() /
                          ("mh4n_gpu_shader_test_" + std::to_string(MH4N_GETPID()) + "_" + name + ".mgpu");
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
        return path.string();
    }
};

// Two programs and three draws: the end-to-end program with the bool set and clear and another
// uniform, then a second program (no loop), a draw of 70 vertices (only the first 64 are checked).
std::string SyntheticCapture(const std::string& name) {
    using namespace asmb;
    CmdList list;
    SetupEndToEnd(list, EndToEndProgram(), 70, true, 1.5f);
    SetupEndToEnd(list, EndToEndProgram(), 12, false, -2.0f);
    // a second program: different code, same registers
    Asm b;
    b.Op(kDp3, O(0), V(0), V(1));
    b.Op1(kMov, O(1), V(1));
    b.Op(kMax, O(2), V(0), V(2));
    b.Op1(kRcp, O(3), V(0));
    b.Op1(kMov, O(4), V(0));
    b.Op1(kMov, O(5), V(0));
    b.Op1(kMov, O(6), V(0));
    b.End();
    SetupEndToEnd(list, b, 9, false, 1.0f);
    const std::vector<u8> bytes = list.Bytes();
    CaptureFile capture;
    capture.Init();
    capture.Mem(kListAt, bytes);
    capture.Mem(kBase, VertexBytes(70, 48));
    capture.SubmitCmdList(0x14001000, static_cast<u32>(bytes.size()));
    capture.NoRegisterWrites();
    capture.Frame(1);
    return capture.Save(name);
}

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

constexpr u32 kFirstVertices = 64;

// The counts of one dump check. Nothing in it, and no text built from it, holds a value of the game: the
// problems are named by draw number, vertex, register, lane, category and word offset only.
struct Totals {
    size_t captures = 0;
    u64 draws_seen = 0;
    u64 no_vertices = 0;            // the draw has no vertices (legitimate; its program must be run by another draw)
    u64 loader_errors = 0;          // the loader could not read the draw's vertices (cannot happen in a replay)
    u64 program_runs = 0;           // draws whose program was run on the device and the oracle
    u64 vertices = 0;
    u64 mismatches = 0;
    u64 translation_failures = 0;
    u64 oracle_problems = 0;        // the oracle did not end, or met an instruction it does not know
    u64 pipeline_failures = 0;      // the device refused the pipeline
    std::set<std::pair<u64, u64>> distinct;      // (program hash, swizzle hash) of EVERY draw, whatever happened to it
    std::set<std::pair<u64, u64>> checked;       // ... run at least once
    std::set<std::pair<u64, u64>> unverified;    // ... that use DST, DSTI or LIT
    std::set<std::pair<u64, u64>> differing;     // ... with a shape the interpreter runs differently
    u64 mismatching_draws_in_differing = 0;      // draws with mismatches in such a program
    std::string first_problem;
};

// Printed after a failed dump check (no data).
constexpr const char* kQuirkHint =
    "differs_from_interpreter is a hint: a mismatch in a program with a loop or call body that ends in an if "
    "(with or without else), an if at the tail of an outer then-branch, two loops that end together, or a call "
    "or jump as the last instruction of a scope may be the interpreter's quirk (read it first as that), a "
    "mismatch in any other program is a translator bug until shown otherwise; see As built (N2-06)";

// The pass of the dump check: every program seen was run, and nothing went wrong on the way.
bool DumpPassed(const Totals& t) {
    return t.program_runs > 0 && t.vertices > 0 && t.mismatches == 0 && t.translation_failures == 0 && t.loader_errors == 0 &&
           t.pipeline_failures == 0 && t.oracle_problems == 0 &&
           t.checked.size() == t.distinct.size();
}

// Why it did not pass, in counts and the first problem (never content).
std::string DumpSummary(const Totals& t) {
    return "programs seen " + std::to_string(t.distinct.size()) + ", checked " + std::to_string(t.checked.size()) +
           ", draws without vertices " + std::to_string(t.no_vertices) + ", loader errors " +
           std::to_string(t.loader_errors) + ", mismatches " + std::to_string(t.mismatches) +
           ", translation failures " + std::to_string(t.translation_failures) + ", pipeline failures " +
           std::to_string(t.pipeline_failures) + ", oracle problems " + std::to_string(t.oracle_problems) +
           (t.first_problem.empty() ? "" : "; first problem: " + t.first_problem);
}

// The state of the replay: the device, the cache and the totals.
struct DumpState {
    Rig* rig = nullptr;
    shader::VertexShaderCache cache;
    vertex::VertexLoader loader{vertex::VertexCaps{}};
    Totals totals;
    bool perturb_oracle = false; // a test hook: the oracle sees another uniform, so every draw mismatches

    void Reset(Rig* r) {
        rig = r;
        cache.Clear();
        totals = Totals{};
        perturb_oracle = false;
    }
};
DumpState g_dump;
int g_capture_index = 0; // the place of the capture being replayed in the list

// The first n vertices of a loaded draw as a plain stream: row k holds vertex ids[k] of every per-vertex
// buffer (the shader is run on the same bytes, in draw order, so no slot cap is needed). False when an
// id is outside the loaded vertices.
bool CompactDraw(const vertex::LoadedDraw& ld, const std::vector<u32>& ids, vertex::LoadedDraw& out) {
    out = ld;
    out.index_type = vertex::IndexType::None;
    out.index_data.clear();
    out.base_vertex = 0;
    out.first_vertex = 0;
    out.count = static_cast<u32>(ids.size());
    for (vertex::VertexBuffer& b : out.buffers) {
        if (b.per_instance) {
            continue;
        }
        const vertex::VertexBuffer& source = ld.buffers[&b - out.buffers.data()];
        b.data.assign(static_cast<size_t>(b.stride) * ids.size() + 16, 0);
        for (size_t k = 0; k < ids.size(); ++k) {
            if (ids[k] >= source.vertex_count || (static_cast<size_t>(ids[k]) + 1) * b.stride > source.data.size()) {
                return false;
            }
            std::memcpy(b.data.data() + k * b.stride, source.data.data() + static_cast<size_t>(ids[k]) * b.stride, b.stride);
        }
        b.vertex_count = static_cast<u32>(ids.size());
    }
    return true;
}

const char* LoadStatusName(vertex::LoadStatus status) {
    switch (status) {
    case vertex::LoadStatus::Ok: return "Ok";
    case vertex::LoadStatus::NoVertices: return "NoVertices";
    case vertex::LoadStatus::Unmapped: return "Unmapped";
    case vertex::LoadStatus::TooLarge: return "TooLarge";
    }
    return "Unknown";
}

void Problem(Totals& t, const std::string& text) {
    if (t.first_problem.empty()) {
        t.first_problem = text;
    }
}

void CheckDraw(mh4n::gpu::Host& host, const pica::DrawCall& draw) {
    Totals& t = g_dump.totals;
    ++t.draws_seen;
    const std::string where = "draw " + std::to_string(draw.sequence) + ": ";
    const pica::PicaState& state = *draw.state;
    shader::ConfigOptions opt;
    opt.capture_outputs = true;
    opt.use_clip_planes = false;
    // the program is counted whatever happens to the draw: a program none of whose draws loads is not checked
    const shader::VertexShaderConfig plain = shader::MakeVertexShaderConfig(state, nullptr, opt);
    const std::pair<u64, u64> key{plain.program_hash, plain.swizzle_hash};
    t.distinct.insert(key);

    const vertex::LoadedDraw ld = g_dump.loader.Load(host, draw);
    if (ld.status == vertex::LoadStatus::NoVertices || (ld.status == vertex::LoadStatus::Ok && ld.count == 0)) {
        ++t.no_vertices;
        return;
    }
    if (ld.status != vertex::LoadStatus::Ok) {
        ++t.loader_errors;
        Problem(t, where + "the vertex loader returned " + LoadStatusName(ld.status));
        return;
    }
    const shader::VertexShaderConfig cfg = shader::MakeVertexShaderConfig(state, &ld, opt);
    shader::TranslateResult translated = g_dump.cache.Get(state.vs, cfg);
    if (!translated.ok()) {
        ++t.translation_failures;
        Problem(t, where + "translation refused: " + translated.error_category +
                       (translated.error_word >= 0 ? " at word " + std::to_string(translated.error_word) : ""));
        return;
    }
    if (translated.shader->uses_unverified_instructions) {
        t.unverified.insert(key);
    }
    if (translated.shader->differs_from_interpreter) {
        t.differing.insert(key);
    }
    const u32 n = std::min(ld.count, kFirstVertices);
    const bool indexed = ld.index_type != vertex::IndexType::None;
    std::vector<u32> ids(n);
    bool readable = true;
    for (u32 i = 0; i < n; ++i) {
        int64_t id = i;
        if (indexed) {
            const size_t width = ld.index_type == vertex::IndexType::U8 ? 1 : ld.index_type == vertex::IndexType::U16 ? 2 : 4;
            u32 index = 0;
            std::memcpy(&index, ld.index_data.data() + i * width, width);
            id = static_cast<int64_t>(index) + ld.base_vertex;
        }
        readable = readable && id >= 0;
        ids[i] = static_cast<u32>(id);
    }
    vertex::LoadedDraw stream;
    if (!readable || !CompactDraw(ld, ids, stream)) {
        ++t.loader_errors;
        Problem(t, where + "an index names a vertex the loader did not load");
        return;
    }
    std::string error;
    const std::vector<float> cap = g_dump.rig->Run(translated.shader, stream, state.vs, state, n, n, false, &error);
    if (cap.empty()) {
        ++t.pipeline_failures;
        Problem(t, where + "the device refused the pipeline");
        return;
    }
    pica::ShaderSetup oracle_setup = state.vs;
    if (g_dump.perturb_oracle) {
        oracle_setup.uniform_f[0].c[0] += 1.5f;
    }
    const Report report = CompareWithOracle(oracle_setup, state, stream, false, *translated.shader, cap, Iota(n), false,
                                            false, /*redact=*/true);
    ++t.program_runs;
    t.checked.insert(key);
    t.vertices += report.vertices;
    t.mismatches += report.mismatches;
    if (report.mismatches != 0 && translated.shader->differs_from_interpreter) {
        ++t.mismatching_draws_in_differing;
    }
    if (report.not_terminating || report.unhandled) {
        ++t.oracle_problems;
        Problem(t, where + "the oracle did not end or met an instruction it does not know");
    }
    if (report.mismatches != 0) {
        // the program is named by its hashes and the capture by its place in the list (never by content)
        Problem(t, where + report.first + " (capture " + std::to_string(g_capture_index) + " of the list, program hash " +
                       std::to_string(key.first) + ", swizzle hash " + std::to_string(key.second) + ")");
    }
}

class CheckingBackend final : public pica::Backend {
public:
    explicit CheckingBackend(mh4n::gpu::Host& host) : host_(host) {}
    void Draw(const pica::DrawCall& draw) override { CheckDraw(host_, draw); }

private:
    mh4n::gpu::Host& host_;
};

class ShaderCheckGpu final : public mh4n::gpu::Gpu, public mh4n::gpu::GpuDebug {
public:
    explicit ShaderCheckGpu(mh4n::gpu::Host& host) : host_(host), backend_(host), processor_(host, backend_) {}

    void Execute(const mh4n::gpu::GxCommand& command) override {
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
    void SetBufferSwap(u32, const mh4n::gpu::FrameBufferInfo&) override {}
    void SetColorFill(u32) override {}
    void FlushRegion(u32, u32) override {}
    void InvalidateRegion(u32, u32) override {}
    void VBlank(u32) override {}
    void LoadInitState(const mh4n::gpu::InitState& init) override { processor_.LoadInitState(init); }

private:
    mh4n::gpu::Host& host_;
    CheckingBackend backend_;
    pica::CommandProcessor processor_;
};

} // namespace

MH4N_REGISTER(mh4n::gpu::GpuFactory, "shader-check",
              [](mh4n::gpu::Host& host, const mh4n::Options&) -> std::unique_ptr<mh4n::gpu::Gpu> {
                  return std::make_unique<ShaderCheckGpu>(host);
              });

namespace {

mh4n::gpu_capture::PlayerResult Replay(const std::string& capture) {
    const mh4n::Options options = mh4n::ParseOptions(
        "capture-player", std::vector<std::string>{"--gpu", "shader-check", "--judge", "none", capture});
    REQUIRE_MESSAGE(options.ok(), options.error());
    mh4n::gpu_capture::PlayerConfig config;
    std::string error;
    REQUIRE_MESSAGE(mh4n::gpu_capture::ConfigFromOptions(options, config, &error), error);
    return mh4n::gpu_capture::RunPlayer(config, options);
}

// Replays every capture the list names once, checking every draw's vertex program on the device
// against the oracle, and prints counts (never any content) when `print` is set. nullopt: no device.
std::optional<Totals> DumpCheck(const char* value, bool print, bool perturb_oracle = false) {
    Rig rig;
    if (!rig.ok()) {
        return std::nullopt;
    }
    const std::vector<std::string> paths = CapturePathsFromEnvironment(value);
    REQUIRE_MESSAGE(!paths.empty(), "the capture list names no capture");
    g_dump.Reset(&rig);
    g_dump.perturb_oracle = perturb_oracle;
    g_dump.totals.captures = paths.size();
    for (const std::string& path : paths) {
        g_capture_index = static_cast<int>(&path - paths.data());
        const mh4n::gpu_capture::PlayerResult result = Replay(path);
        CHECK_MESSAGE(result.error.empty(), "a capture could not be replayed: " << result.error);
    }
    Totals t = g_dump.totals;
    g_dump.rig = nullptr;
    if (print) {
        std::printf("shader dump check: %zu captures; draws seen %llu, distinct programs %zu, programs checked %zu, "
                    "vertices compared %llu, mismatches %llu, translation failures %llu\n",
                    t.captures, static_cast<unsigned long long>(t.draws_seen), t.distinct.size(), t.checked.size(),
                    static_cast<unsigned long long>(t.vertices), static_cast<unsigned long long>(t.mismatches),
                    static_cast<unsigned long long>(t.translation_failures));
        std::printf("shader dump check: program runs %llu, draws without vertices %llu, loader errors %llu, "
                    "programs with DST/DSTI/LIT %zu, programs with an interpreter-differing shape %zu (draws with mismatches in them %llu), "
                    "device refused a pipeline %llu, oracle problems %llu\n",
                    static_cast<unsigned long long>(t.program_runs), static_cast<unsigned long long>(t.no_vertices),
                    static_cast<unsigned long long>(t.loader_errors), t.unverified.size(), t.differing.size(),
                    static_cast<unsigned long long>(t.mismatching_draws_in_differing),
                    static_cast<unsigned long long>(t.pipeline_failures), static_cast<unsigned long long>(t.oracle_problems));
        if (!DumpPassed(t)) {
            std::printf("shader dump check: NOT PASSED: %s; hint: %s\n", DumpSummary(t).c_str(), kQuirkHint);
        }
        std::fflush(stdout);
    }
    return t;
}

} // namespace

TEST_CASE("the replay path: a synthetic capture goes through the processor, the loader, the translation and the device") {
    const std::string path = SyntheticCapture("replay");
    const auto totals = DumpCheck(path.c_str(), true); // prints the line the real check prints
    fs::remove(path);
    if (!totals) return;
    CHECK(totals->captures == 1);
    CHECK(totals->draws_seen == 3);
    CHECK(totals->distinct.size() == 2);
    CHECK(totals->checked.size() == 2);
    CHECK(totals->program_runs == 3);
    CHECK(totals->vertices == 64 + 12 + 9);
    CHECK_MESSAGE(totals->mismatches == 0, totals->first_problem);
    CHECK(totals->translation_failures == 0);
    CHECK(totals->pipeline_failures == 0);
    CHECK(totals->oracle_problems == 0);
    CHECK(totals->loader_errors == 0);
    CHECK(totals->no_vertices == 0);
    CHECK(totals->unverified.empty());
    CHECK_MESSAGE(DumpPassed(*totals), DumpSummary(*totals));
}

TEST_CASE("the capture list of the dump check: a directory, a file, and the counts") {
    const fs::path dir = fs::temp_directory_path() / ("mh4n_gpu_shader_test_dir_" + std::to_string(MH4N_GETPID()));
    fs::create_directories(dir / "sub");
    const std::string made = SyntheticCapture("listed");
    fs::rename(made, dir / "sub" / "one.mgpu");
    std::ofstream(dir / "readme.txt") << "not a capture";
    const auto totals = DumpCheck(dir.string().c_str(), false);
    if (totals) {
        CHECK(totals->captures == 1);
        CHECK(totals->draws_seen == 3);
        CHECK(totals->checked.size() == 2);
        CHECK(totals->mismatches == 0);
    }
    const std::vector<std::string> listed = CapturePathsFromEnvironment(
        (dir.string() + std::string(1,
#if defined(_WIN32)
                                    ';'
#else
                                    ':'
#endif
                                    ) + (dir / "sub" / "one.mgpu").string()).c_str());
    CHECK(listed.size() == 2);
    fs::remove_all(dir);
}

TEST_CASE("a program the translation refuses is a counted failure of the dump check") {
    using namespace asmb;
    CmdList list;
    Asm bad;
    bad.Jmpu(0, 3000);
    bad.End();
    SetupEndToEnd(list, bad, 4, false, 1.0f);
    const std::vector<u8> bytes = list.Bytes();
    CaptureFile capture;
    capture.Init();
    capture.Mem(kListAt, bytes);
    capture.Mem(kBase, VertexBytes(4, 49));
    capture.SubmitCmdList(0x14001000, static_cast<u32>(bytes.size()));
    capture.NoRegisterWrites();
    capture.Frame(1);
    const std::string path = capture.Save("refused");
    const auto totals = DumpCheck(path.c_str(), false);
    fs::remove(path);
    if (!totals) return;
    CHECK(totals->draws_seen == 1);
    CHECK(totals->translation_failures == 1);
    CHECK(totals->checked.empty());
    CHECK(totals->distinct.size() == 1);
    CHECK_FALSE(DumpPassed(*totals));
}


namespace {

// A capture of the end-to-end program A (a good draw) and the program B of the replay capture, whose
// draws are given by the caller; `bad_base` puts B's vertices outside the guest memory.
std::string TwoProgramCapture(const std::string& name, const std::vector<std::pair<u32, u32>>& b_draws) {
    using namespace asmb;
    CmdList list;
    SetupEndToEnd(list, EndToEndProgram(), 10, true, 1.5f);
    Asm b;
    b.Op(kDp3, O(0), V(0), V(1));
    b.Op1(kMov, O(1), V(1));
    b.End();
    for (const auto& [vertices, base] : b_draws) {
        SetupEndToEnd(list, b, vertices, false, 1.0f, base);
    }
    const std::vector<u8> bytes = list.Bytes();
    CaptureFile capture;
    capture.Init();
    capture.Mem(kListAt, bytes);
    capture.Mem(kBase, VertexBytes(70, 48));
    capture.SubmitCmdList(0x14001000, static_cast<u32>(bytes.size()));
    capture.NoRegisterWrites();
    capture.Frame(1);
    return capture.Save(name);
}

// Scans a text that the dump path prints for what must never be in it: a float literal, a hex word,
// an exponent, nan or inf.
bool HasValueLeak(const std::string& text) {
    static const std::regex float_literal("[0-9]+\\.[0-9]+|[0-9][eE][+-]?[0-9]|0x[0-9a-fA-F]|\\b(nan|inf)\\b",
                                          std::regex::icase);
    return std::regex_search(text, float_literal);
}

} // namespace

TEST_CASE("a program none of whose draws loads is not checked: the dump check fails and names the category") {
    const std::string path = TwoProgramCapture("unmapped", {{8, 0x70000000}}); // B's vertices are outside the memory
    const auto totals = DumpCheck(path.c_str(), false);
    fs::remove(path);
    if (!totals) return;
    CHECK(totals->draws_seen == 2);
    CHECK(totals->distinct.size() == 2);
    CHECK(totals->checked.size() == 1);
    CHECK(totals->loader_errors == 1);
    CHECK(totals->program_runs == 1);
    CHECK_FALSE(DumpPassed(*totals));
    CHECK(totals->first_problem.find("draw 1") != std::string::npos);
    CHECK(totals->first_problem.find("Unmapped") != std::string::npos);
    CHECK_FALSE(HasValueLeak(DumpSummary(*totals)));
}

TEST_CASE("a mismatch in a program with a shape the interpreter runs differently is counted as such") {
    using namespace asmb;
    Asm a; // LOOP { r0 += c1; IFU b0 { r1 += c2 } ELSE { r2 += c3 } }: the then-branch ends the loop body
    a.Op1(kMov, R(0), V(0));
    a.BeginLoop(0);
    a.Op(kAdd, R(0), C(1), R(0));
    a.BeginIfu(0);
    a.Op(kAdd, R(1), C(2), R(1));
    a.Else();
    a.Op(kAdd, R(2), C(3), R(2));
    a.EndIf();
    a.EndLoop();
    a.Op1(kMov, O(0), R(0));
    a.Op1(kMov, O(1), R(1));
    a.End();
    CmdList list;
    SetupEndToEnd(list, a, 6, true, 1.0f);
    const std::vector<u8> bytes = list.Bytes();
    CaptureFile capture;
    capture.Init();
    capture.Mem(kListAt, bytes);
    capture.Mem(kBase, VertexBytes(6, 51));
    capture.SubmitCmdList(0x14001000, static_cast<u32>(bytes.size()));
    capture.NoRegisterWrites();
    capture.Frame(1);
    const std::string path = capture.Save("quirk");
    const auto totals = DumpCheck(path.c_str(), true);
    fs::remove(path);
    if (!totals) return;
    CHECK(totals->mismatches > 0);
    CHECK_FALSE(DumpPassed(*totals));
    CHECK(totals->differing.size() == 1);
    CHECK(totals->mismatching_draws_in_differing == 1);
    CHECK_FALSE(HasValueLeak(kQuirkHint));
    CHECK(std::string(kQuirkHint).find("As built (N2-06)") != std::string::npos);
}

TEST_CASE("one draw of a program that another draw runs can still fail to load: the check fails") {
    const std::string path = TwoProgramCapture("oneunmapped", {{6, kBase}, {8, 0x70000000}});
    const auto totals = DumpCheck(path.c_str(), false);
    fs::remove(path);
    if (!totals) return;
    CHECK(totals->distinct.size() == 2);
    CHECK(totals->checked.size() == 2); // every program was run by a draw
    CHECK(totals->loader_errors == 1);
    CHECK_FALSE(DumpPassed(*totals));
    CHECK(totals->first_problem.find("Unmapped") != std::string::npos);
}

TEST_CASE("a draw without vertices is counted; its program must still be run by another draw") {
    {   // B is only drawn with no vertices: seen 2, checked 1
        const std::string path = TwoProgramCapture("novertices", {{0, kBase}});
        const auto totals = DumpCheck(path.c_str(), false);
        fs::remove(path);
        if (!totals) return;
        CHECK(totals->no_vertices == 1);
        CHECK(totals->loader_errors == 0);
        CHECK(totals->distinct.size() == 2);
        CHECK(totals->checked.size() == 1);
        CHECK_FALSE(DumpPassed(*totals));
    }
    {   // ... and drawn again with vertices: it passes
        const std::string path = TwoProgramCapture("novertices2", {{0, kBase}, {6, kBase}});
        const auto totals = DumpCheck(path.c_str(), false);
        fs::remove(path);
        if (!totals) return;
        CHECK(totals->no_vertices == 1);
        CHECK(totals->checked.size() == 2);
        CHECK_MESSAGE(DumpPassed(*totals), DumpSummary(*totals));
    }
}

TEST_CASE("the dump path reports without any value of the game: no float, no instruction word, no GLSL") {
    // a mismatch (the oracle is shown another uniform) on a good draw
    {
        const std::string path = SyntheticCapture("leak_mismatch");
        const auto totals = DumpCheck(path.c_str(), false, /*perturb_oracle=*/true);
        fs::remove(path);
        if (!totals) return;
        CHECK(totals->mismatches > 0);
        CHECK_FALSE(DumpPassed(*totals));
        CHECK_FALSE(totals->first_problem.empty());
        CHECK(totals->first_problem.find("differs") != std::string::npos);
        CHECK_FALSE(HasValueLeak(DumpSummary(*totals)));
        CHECK_FALSE(HasValueLeak(totals->first_problem));
    }
    // a program the translation refuses for an instruction it does not know (the message of the
    // translator names the word, the dump path only its offset)
    {
        using namespace asmb;
        CmdList list;
        Asm bad;
        bad.Op1(kMov, O(0), V(0));
        bad.Raw(0x10u << 26 | 0x2345u); // an unknown opcode with fields in it
        bad.End();
        SetupEndToEnd(list, bad, 4, false, 1.0f);
        const std::vector<u8> bytes = list.Bytes();
        CaptureFile capture;
        capture.Init();
        capture.Mem(kListAt, bytes);
        capture.Mem(kBase, VertexBytes(4, 49));
        capture.SubmitCmdList(0x14001000, static_cast<u32>(bytes.size()));
        capture.NoRegisterWrites();
        capture.Frame(1);
        const std::string path = capture.Save("leak_refused");
        const auto totals = DumpCheck(path.c_str(), false);
        fs::remove(path);
        if (!totals) return;
        CHECK(totals->translation_failures == 1);
        CHECK(totals->first_problem.find("unhandled-instruction at word 1") != std::string::npos);
        CHECK_FALSE(HasValueLeak(totals->first_problem));
        CHECK_FALSE(HasValueLeak(DumpSummary(*totals)));
    }
    // the scanner itself sees what it must
    CHECK(HasValueLeak("vertex 3 o2.x: gpu 0.500000 cpu 1.000000"));
    CHECK(HasValueLeak("Unhandled instruction: 0x40000000"));
    CHECK(HasValueLeak("value 1e+30"));
    CHECK(HasValueLeak("gpu nan"));
    CHECK_FALSE(HasValueLeak("draw 1: vertex 3 o12.x differs; translation refused: recursion at word 4"));
}

// ================================================================================================
// The dump check
// ================================================================================================

TEST_CASE("(with the dump) every vertex program of the captured draws passes the oracle") {
    const char* value = std::getenv("MH4N_SHADER_CAPTURES");
    if (value == nullptr || *value == '\0') {
        WARN_MESSAGE(false, "skipped: set MH4N_SHADER_CAPTURES to .mgpu capture files or directories "
                            "(separated by the platform's path-list separator) to run the dump check");
        return;
    }
    const auto totals = DumpCheck(value, true);
    if (!totals) return;
    CHECK_MESSAGE(DumpPassed(*totals), DumpSummary(*totals));
}

// ================================================================================================
// The inputs: the conversions of the integer-formatted attributes
// ================================================================================================

namespace {

// An attribute of the hand-made vertex description and the float4 the shader must see for it.
struct AttrSpec {
    vertex::VertexFormat format;
    u32 components;
    vertex::Conversion conversion;
    bool alias_w;
};

} // namespace

TEST_CASE("integer attributes are converted to float in the shader, and an aliased w is 1.0") {
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    using VF = vertex::VertexFormat;
    using Cv = vertex::Conversion;
    const AttrSpec specs[] = {
        {VF::R8G8B8A8_SINT, 4, Cv::FromSint, false},   // v0
        {VF::R8G8B8A8_UINT, 3, Cv::FromUint, true},    // v1: 3 bytes read as 4, w is not data
        {VF::R16G16B16A16_SINT, 3, Cv::FromSint, true},// v2
        {VF::R8G8_SINT, 2, Cv::FromSint, false},       // v3: z, w default to 0, 1
        {VF::R32G32B32_FLOAT, 3, Cv::None, false},     // v4
        {VF::R32_FLOAT, 1, Cv::None, false},           // v5
        {VF::R16_SINT, 1, Cv::FromSint, false},        // v6
        {VF::R8_UINT, 1, Cv::FromUint, false},         // v7
        {VF::R8G8_UINT, 2, Cv::FromUint, false},       // v8
        {VF::R16G16_SINT, 2, Cv::FromSint, false},     // v9
        {VF::R8_SINT, 1, Cv::FromSint, false},         // v10
        {VF::R8G8B8A8_UINT, 4, Cv::FromUint, false},   // v11
        {VF::R32G32_FLOAT, 2, Cv::None, false},        // v12
    };
    constexpr u32 kAttrs = sizeof(specs) / sizeof(specs[0]);
    constexpr u32 kVertices = 40;
    // layout: every attribute at a multiple of 4 bytes in one interleaved buffer
    u32 offsets[kAttrs];
    u32 stride = 0;
    for (u32 i = 0; i < kAttrs; ++i) {
        offsets[i] = stride;
        stride += (vertex::GetFormatInfo(specs[i].format).size_bytes + 3) & ~3u;
    }
    vertex::LoadedDraw ld;
    vertex::VertexBuffer main;
    main.stride = stride;
    main.vertex_count = kVertices;
    main.data.assign(static_cast<size_t>(stride) * kVertices + 16, 0);
    vertex::VertexBuffer constant;
    constant.stride = 16;
    constant.per_instance = true;
    constant.vertex_count = 1;
    constant.data.assign(32, 0);
    const float one[4] = {0, 0, 0, 1};
    std::memcpy(constant.data.data(), one, 16);
    ld.buffers = {main, constant};
    for (u32 r = 0; r < 16; ++r) {
        vertex::VertexAttribute& a = ld.attributes[r];
        a.location = r;
        if (r < kAttrs) {
            a.binding = 0;
            a.offset = offsets[r];
            a.format = specs[r].format;
            a.components = specs[r].components;
            a.conversion = specs[r].conversion;
            a.alias_w = specs[r].alias_w;
        } else {
            a.binding = 1;
            a.format = VF::R32G32B32A32_FLOAT;
            a.components = 4;
        }
    }
    // values, written as the PICA's attribute data and expected as floats (worked out here)
    std::vector<std::array<std::array<float, 4>, kAttrs>> expected(kVertices);
    Rng rng(23);
    for (u32 v = 0; v < kVertices; ++v) {
        u8* base = ld.buffers[0].data.data() + static_cast<size_t>(v) * stride;
        for (u32 r = 0; r < kAttrs; ++r) {
            const vertex::FormatInfo info = vertex::GetFormatInfo(specs[r].format);
            float want[4] = {0, 0, 0, 1};
            for (u32 l = 0; l < info.lanes; ++l) {
                float value = 0;
                switch (info.kind) {
                case vertex::ComponentKind::S8: {
                    const int8_t x = static_cast<int8_t>(rng.Next());
                    std::memcpy(base + offsets[r] + l, &x, 1);
                    value = x;
                    break;
                }
                case vertex::ComponentKind::U8: {
                    const u8 x = static_cast<u8>(rng.Next());
                    base[offsets[r] + l] = x;
                    value = x;
                    break;
                }
                case vertex::ComponentKind::S16: {
                    const int16_t x = static_cast<int16_t>(rng.Next());
                    std::memcpy(base + offsets[r] + 2 * l, &x, 2);
                    value = x;
                    break;
                }
                case vertex::ComponentKind::F32: {
                    const float x = rng.Uniform(-100.f, 100.f);
                    std::memcpy(base + offsets[r] + 4 * l, &x, 4);
                    value = x;
                    break;
                }
                }
                want[l] = value;
            }
            if (specs[r].alias_w) {
                want[3] = 1.0f; // the lane read from the bytes that follow is replaced
            }
            for (u32 l = 0; l < 4; ++l) {
                expected[v][r][l] = want[l];
            }
        }
    }
    Asm a;
    for (u32 r = 0; r < kAttrs; ++r) {
        a.Op1(kMov, r < 16 ? O(r) : 0, V(r));
    }
    a.End();
    Draw d;
    a.Install(d.setup);
    d.loaded = ld;
    SetShaderRegs(*d.state, 0, 0x1FFF);
    Outcome o = RunCase(rig, d, kVertices);
    CHECK_OUTCOME(o);
    REQUIRE(!o.capture.empty());
    // by value: no oracle in this comparison
    u32 wrong = 0;
    for (u32 v = 0; v < kVertices; ++v) {
        for (u32 r = 0; r < kAttrs; ++r) {
            for (u32 l = 0; l < 4; ++l) {
                if (At(o.capture, v, r, l) != expected[v][r][l]) {
                    ++wrong;
                }
            }
        }
    }
    CHECK(wrong == 0);
    // the SPIR-V declares the input of each class: a vec4, ivec4 or uvec4 per register
    CHECK(o.shader->glsl.find("in ivec4 vs_in_typed_reg0;") != std::string::npos);
    CHECK(o.shader->glsl.find("in uvec4 vs_in_typed_reg1;") != std::string::npos);
    CHECK(o.shader->glsl.find("in vec4 vs_in_typed_reg4;") != std::string::npos);
    CHECK(o.shader->glsl.find("vs_in_reg1.w = 1.0;") != std::string::npos);
    CHECK(o.shader->glsl.find("vs_in_reg0.w = 1.0;") == std::string::npos);
    CHECK(o.shader->used_inputs == (1u << kAttrs) - 1);
}

// ================================================================================================
// The outputs: the output map, the seven varyings, the position
// ================================================================================================

TEST_CASE("the seven varyings, the position and the output map (every output register in use)") {
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    Asm a;
    for (u32 i = 0; i < 6; ++i) {
        a.Op1(kMov, O(i), V(i));
    }
    a.End();
    // values in and out of [0, 1] for the colour, z at the sanitised bounds, w both signs
    FloatInputs in = RandomInputs(6, 64, 24, -2.f, 2.f);
    for (u32 v = 0; v < 64; ++v) {
        in.At(v, 0, 3) = (v % 7 == 0) ? -in.At(v, 0, 3) : std::fabs(in.At(v, 0, 3)) + 0.5f; // w
        if (v % 5 == 0) in.At(v, 0, 2) = in.At(v, 0, 3) * 0.0000005f; // 0 < z/w < 1e-6
        if (v % 5 == 1) in.At(v, 0, 2) = -in.At(v, 0, 3) * 1.000005f; // -1.00001 < z/w < -1
    }
    in.At(2, 2, 1) = kNaN; // a NaN colour channel saturates to 1.0 (c < 1 ? c : 1)
    in.At(3, 2, 0) = -kInf;
    Draw d = MakeProgram(a, in, 0x3F);
    SetOutmap(*d.state, StandardOutmap());
    d.state->regs[shader::reg::kClipEnable] = 1;
    d.state->regs[shader::reg::kClipCoef + 0] = 0x3F8000; // float24 1.0 ... (sign 0, exponent 0x3F, mantissa 0)
    d.state->regs[shader::reg::kClipCoef + 1] = 0x000000;
    d.state->regs[shader::reg::kClipCoef + 3] = 0x3F8000;
    for (bool flip : {false, true}) {
        d.flip_viewport = flip;
        Outcome o = RunCase(rig, d, 64, true, true);
        CHECK_OUTCOME(o);
        CHECK(o.shader->num_outputs == 6);
        for (u32 r = 0; r < 6; ++r) {
            CHECK(o.shader->output_map[r] == r);
        }
        CHECK(o.shader->output_map[6] == 16);
        CHECK(o.shader->semantic_map[shader::semantic::kColorG].attribute == 2);
        CHECK(o.shader->semantic_map[shader::semantic::kColorG].component == 1);
        CHECK(o.shader->semantic_map[shader::semantic::kTexcoord2V].attribute == 5);
        CHECK(o.shader->semantic_map[17].attribute == 16);
        CHECK(o.report.values > 0);
        // the NaN colour channel of vertex 2 is 1.0
        REQUIRE(!o.capture.empty());
        CHECK(At(o.capture, 2, shader::kCapturePrimaryColor, 1) == 1.0f);
        CHECK(At(o.capture, 3, shader::kCapturePrimaryColor, 0) == 1.0f); // |-inf| saturates
    }
}

TEST_CASE("a sparse output mask and an output map that moves the semantics around") {
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    namespace s = shader::semantic;
    Asm a;
    a.Op1(kMov, O(1), V(0));   // attribute 0
    a.Op1(kMov, O(4), V(1));   // attribute 1
    a.Op1(kMov, O(5), V(2));   // attribute 2
    a.Op1(kMov, O(12), V(3));  // attribute 3
    a.Op1(kMov, O(7), V(0));   // not in the mask: dropped
    a.End();
    FloatInputs in = RandomInputs(4, 64, 25, -1.5f, 1.5f);
    for (u32 v = 0; v < 64; ++v) {
        in.At(v, 1, 3) = std::fabs(in.At(v, 1, 3)) + 0.25f;
    }
    const u32 mask = (1u << 1) | (1u << 4) | (1u << 5) | (1u << 12);
    Draw d = MakeProgram(a, in, mask);
    // attribute 1 = position, attribute 0 = colour (r, g, b, a) swapped with the quaternion, attribute 3 = view + tc0_w
    SetOutmap(*d.state, {{s::kColorA, s::kColorB, s::kColorG, s::kColorR},
                         {s::kPositionX, s::kPositionY, s::kPositionZ, s::kPositionW},
                         {s::kQuaternionW, s::kQuaternionZ, s::kInvalid, s::kTexcoord1U},
                         {s::kViewX, s::kViewY, s::kViewZ, s::kTexcoord0W}});
    Outcome o = RunCase(rig, d, 64, true);
    CHECK_OUTCOME(o);
    CHECK(o.shader->num_outputs == 4);
    CHECK(o.shader->output_map[1] == 0);
    CHECK(o.shader->output_map[4] == 1);
    CHECK(o.shader->output_map[5] == 2);
    CHECK(o.shader->output_map[12] == 3);
    CHECK(o.shader->output_map[7] == 16);
    CHECK(o.shader->semantic_map[s::kPositionW].attribute == 1);
    CHECK(o.shader->semantic_map[s::kPositionW].component == 3);
    CHECK(o.shader->semantic_map[s::kTexcoord0W].attribute == 3);
    // the instruction that wrote o7 changed nothing: attribute outputs beyond 4 are zero (checked by the comparison)
}

TEST_CASE("a semantic no output attribute maps is 1.0 (not 0), as Azahar and the hardware") {
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    namespace s = shader::semantic;
    Asm a;
    a.Op1(kMov, O(0), V(0));
    a.Op1(kMov, O(1), V(1));
    a.End();
    FloatInputs in = RandomInputs(2, 8, 26, 0.1f, 0.9f);
    Draw d = MakeProgram(a, in, 0x3);
    // only the position (attribute 0) and the first texture coordinates (attribute 1) are mapped
    SetOutmap(*d.state, {{s::kPositionX, s::kPositionY, s::kPositionZ, s::kPositionW},
                         {s::kTexcoord0U, s::kTexcoord0V, s::kInvalid, s::kInvalid}});
    Outcome o = RunCase(rig, d, 8, true);
    CHECK_OUTCOME(o);
    REQUIRE(!o.capture.empty());
    for (u32 v = 0; v < 8; ++v) {
        CHECK(At(o.capture, v, shader::kCapturePrimaryColor, 0) == 1.0f);
        CHECK(At(o.capture, v, shader::kCapturePrimaryColor, 3) == 1.0f);
        CHECK(At(o.capture, v, shader::kCaptureNormquat, 2) == 1.0f);
        CHECK(At(o.capture, v, shader::kCaptureView, 1) == 1.0f);
        CHECK(At(o.capture, v, shader::kCaptureTexcoord2W, 2) == 1.0f); // texcoord0_w
        CHECK(At(o.capture, v, shader::kCaptureTexcoord01, 2) == 1.0f); // texcoord1.x
        CHECK(At(o.capture, v, shader::kCaptureTexcoord01, 0) == in.data[v * 8 + 4]); // texcoord0.x = v1.x
    }
    // no output attribute at all: everything is 1.0 and the position is (1, 1, -1, 1)
    SetOutmap(*d.state, {});
    Outcome none = RunCase(rig, d, 8, true);
    CHECK_OUTCOME(none);
    CHECK(At(none.capture, 0, shader::kCapturePosition, 2) == -1.0f);
    CHECK(At(none.capture, 0, shader::kCapturePosition, 3) == 1.0f);
}

TEST_CASE("the geometry-stage form writes the output attributes at locations 0..n-1 and nothing else") {
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    Asm a;
    a.Op(kAdd, O(0), V(0), V(1));
    a.Op(kMul, O(2), V(0), V(1));
    a.Op1(kMov, O(3), V(1));
    a.End();
    Draw d = MakeProgram(a, RandomInputs(2, 32, 27), 0xD);
    std::string error;
    auto t = Translate(d, false, true, &error);
    REQUIRE_MESSAGE(t != nullptr, error);
    CHECK(t->glsl.find("layout(location = 0) out vec4 vs_out_attr0;") != std::string::npos);
    CHECK(t->glsl.find("layout(location = 2) out vec4 vs_out_attr2;") != std::string::npos);
    CHECK(t->glsl.find("out vec4 primary_color") == std::string::npos);
    CheckSpirvValid(t->spirv, "the geometry-stage form");
    std::vector<float> cap = rig.Run(t, d.loaded, d.setup, *d.state, 32, 32, false, &error);
    REQUIRE_MESSAGE(!cap.empty(), error);
    const Report r = CompareWithOracle(d, *t, cap, Iota(32), false);
    CHECK_MESSAGE(r.mismatches == 0, r.first);
    CHECK(r.values > 0);
}

TEST_CASE("clip planes are optional, and the SPIR-V validates with them and without") {
    using namespace asmb;
    Asm a;
    a.Op1(kMov, O(0), V(0));
    a.End();
    Draw d = MakeProgram(a, RandomInputs(1, 4, 28), 0x1);
    SetOutmap(*d.state, {{0, 1, 2, 3}});
    for (bool clip : {false, true}) {
        std::string error;
        auto t = Translate(d, clip, false, &error);
        REQUIRE_MESSAGE(t != nullptr, error);
        CHECK((t->glsl.find("gl_ClipDistance") != std::string::npos) == clip);
        CheckSpirvValid(t->spirv, clip ? "with clip planes" : "without clip planes");
        bool capability = false; // OpCapability (word count 2, opcode 17) ClipDistance (32)
        for (size_t i = 5; i + 1 < t->spirv.size(); ++i) {
            capability = capability || (t->spirv[i] == 0x00020011u && t->spirv[i + 1] == 32u);
        }
        CHECK(capability == clip);
    }
}

// ================================================================================================
// Random straight-line programs (every instruction, random swizzles, masks and negations)
// ================================================================================================

namespace {

Desc RandomDesc(Rng& rng) {
    static const char* const kSwz[] = {"xyzw", "wzyx", "xxxx", "yyyy", "zzzz", "wwww", "yxwz", "zwxy", "xyxy", "wzwz", "yzwx", "xzyw"};
    static const char* const kMask[] = {"xyzw", "x___", "_y__", "__z_", "___w", "xy__", "__zw", "x_z_", "_y_w", "xyz_", "_yzw", "x__w"};
    Desc d;
    d.mask = kMask[rng.Next() % 12];
    d.s1 = kSwz[rng.Next() % 12];
    d.s2 = kSwz[rng.Next() % 12];
    d.s3 = kSwz[rng.Next() % 12];
    d.n1 = rng.Next() % 3 == 0;
    d.n2 = rng.Next() % 3 == 0;
    d.n3 = rng.Next() % 3 == 0;
    return d;
}

} // namespace

TEST_CASE("random programs: every arithmetic instruction with random swizzles, masks and negations") {
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    Rng rng(29);
    const u32 two_source[] = {kAdd, kMul, kDp3, kDp4, kDph, kMax, kMin, kSlt, kSge, kDst};
    const u32 inverted[] = {kDphi, kDsti, kSgei, kSlti};
    const u32 one_source[] = {kFlr, kRcp, kRsq, kEx2, kLg2, kMov, kLit};
    u64 values = 0;
    double compile_ms = 0;
    for (u32 program = 0; program < 10; ++program) {
        Asm a;
        for (u32 out = 0; out < 16; ++out) {
            const Desc d = RandomDesc(rng);
            const u32 pick = rng.Next() % 20;
            const u32 any_src = (rng.Next() % 2) ? C(rng.Next() % 8) : V(rng.Next() % 4); // 7 bits
            const u32 narrow_src = V(rng.Next() % 4);                                      // 5 bits
            const u32 addr = (rng.Next() % 4 == 0) ? 1 + rng.Next() % 2 : 0;               // a0.x / a0.y on a uniform
            if (pick < 8) {
                a.Op(two_source[rng.Next() % 10], O(out), any_src, narrow_src, d, any_src >= C(0) ? addr : 0);
            } else if (pick < 12) {
                a.Op(inverted[rng.Next() % 4], O(out), narrow_src, any_src, d, any_src >= C(0) ? addr : 0);
            } else if (pick < 16) {
                a.Op1(one_source[rng.Next() % 7], O(out), any_src, d, any_src >= C(0) ? addr : 0);
            } else if (pick < 18) {
                a.Mad(O(out), narrow_src, any_src, V(rng.Next() % 4), d, any_src >= C(0) ? addr : 0);
            } else {
                a.Madi(O(out), narrow_src, V(rng.Next() % 4), any_src, d, any_src >= C(0) ? addr : 0);
            }
        }
        a.End();
        // the address registers are set by the first instructions: a program that uses a0 needs them
        Asm prog;
        prog.Op1(kMova, 0, V(3), {.mask = "xy__", .s1 = "xyzw"});
        for (u32 w : a.code) {
            prog.Raw(w);
        }
        prog.descs = a.descs;
        // the MOVA's descriptor must be in the table: rebuild through D()
        const u32 mova_desc = prog.D(Desc{.mask = "xy__", .s1 = "xyzw"});
        prog.code[0] = (kMova << 26) | (V(3) << 12) | mova_desc;
        FloatInputs in = RandomInputs(4, 64, 100 + program, -2.f, 2.f);
        for (u32 v = 0; v < 64; ++v) { // v3.xy: small integers for the address registers
            in.At(v, 3, 0) = static_cast<float>(static_cast<int>(rng.Next() % 12) - 3);
            in.At(v, 3, 1) = static_cast<float>(static_cast<int>(rng.Next() % 12) - 3);
        }
        Draw d = MakeProgram(prog, in);
        SetRandomUniforms(d, 24, 200 + program, -2.f, 2.f);
        Outcome o = RunCase(rig, d, 64);
        INFO("random program " << program);
        CHECK_OUTCOME(o);
        values += o.report.values;
        compile_ms += o.shader->compile_ms;
    }
    CHECK(values > 0);
    MESSAGE("glslang: " << compile_ms / 10 << " ms per random program of 17 instructions (GLSL to SPIR-V, RelWithDebInfo)");
}

TEST_CASE("the comparison sees a difference (a changed uniform, a changed input)") {
    Rig rig;
    if (!rig.ok()) return;
    using namespace asmb;
    Asm a;
    a.Op(kMul, O(0), C(0), V(0));
    a.Op(kAdd, O(1), V(0), V(1));
    a.End();
    Draw d = MakeProgram(a, RandomInputs(2, 16, 30), 0x3);
    SetRandomUniforms(d, 1, 31);
    Outcome o = RunCase(rig, d, 16);
    CHECK_OUTCOME(o);
    // the same capture, the oracle with another uniform
    d.setup.uniform_f[0].c[2] += 0.5f;
    Report r = CompareWithOracle(d, *o.shader, o.capture, Iota(16), false);
    CHECK(r.mismatches > 0);
    d.setup.uniform_f[0].c[2] -= 0.5f;
    // and with another input
    d.loaded.buffers[0].data[11] ^= 0x01; // the exponent of v0.z of vertex 0
    r = CompareWithOracle(d, *o.shader, o.capture, Iota(16), false);
    CHECK(r.mismatches > 0);
    // the tolerance: 2^-16 * max(1, |a|, |b|), NaN equals NaN, infinities are equal to themselves only
    CHECK(Near(1.0f, 1.0f + 1.0f / 100000.0f));
    CHECK_FALSE(Near(1.0f, 1.0f + 1.0f / 30000.0f));
    CHECK(Near(1000.0f, 1000.0f + 1000.0f / 100000.0f));
    CHECK_FALSE(Near(1000.0f, 1000.0f + 1000.0f / 30000.0f));
    CHECK(Near(kNaN, -kNaN));
    CHECK(Near(kInf, kInf));
    CHECK_FALSE(Near(kInf, -kInf));
    CHECK_FALSE(Near(kInf, 1e38f));
    CHECK_FALSE(Near(kNaN, 0.f));
}

// ================================================================================================
// The encoding, the uniform blocks
// ================================================================================================

TEST_CASE("the instruction encoding: the assembler and the decoder agree on every field") {
    using namespace asmb;
    Asm a;
    // arithmetic: dest r5 (0x15), src1 c70 (0x66 = 70 + 32 ... 7 bits), src2 v9, relative a0.y, descriptor 3
    a.D(Desc{}.Word());
    a.D(Desc{.mask = "x___"}.Word());
    a.D(Desc{.mask = "_y__"}.Word());
    a.D(Desc{.mask = "__z_", .n1 = true}.Word());
    a.Op(kMul, R(5), 0x66, V(9), {.mask = "__z_", .n1 = true}, 2);
    const isa::Instruction mul{a.code[0]};
    CHECK(mul.Effective() == isa::Opcode::MUL);
    CHECK(mul.info().format == isa::Format::Arithmetic);
    CHECK_FALSE(mul.info().src_inversed);
    CHECK(mul.dest().value == R(5));
    CHECK(mul.dest().is_temporary());
    CHECK(mul.dest().index() == 5);
    CHECK(mul.src1(false).value == 0x66);
    CHECK(mul.src1(false).kind() == isa::SourceKind::FloatUniform);
    CHECK(mul.src1(false).index() == 0x46);
    CHECK(mul.src2(false).value == V(9));
    CHECK(mul.src2(false).kind() == isa::SourceKind::Input);
    CHECK(mul.address_register_index() == 2);
    CHECK(mul.operand_desc_id() == 3);
    const isa::Swizzle sw{a.descs[3]};
    CHECK_FALSE(sw.DestEnabled(0));
    CHECK(sw.DestEnabled(2));
    CHECK(sw.negate(1));
    CHECK_FALSE(sw.negate(2));
    for (u32 c = 0; c < 4; ++c) {
        CHECK(sw.selector(1, c) == c);
    }
    // the inverted form
    a.Op(kSlti, O(3), V(7), C(40), {}, 1);
    const isa::Instruction slti{a.code[1]};
    CHECK(slti.info().src_inversed);
    CHECK(slti.src1(true).value == V(7));
    CHECK(slti.src2(true).value == C(40));
    CHECK(slti.address_register_index() == 1);
    // mad
    a.Mad(O(2), V(3), C(33), V(4), {}, 3);
    const isa::Instruction mad{a.code[2]};
    CHECK(mad.Effective() == isa::Opcode::MAD);
    CHECK(mad.mad_dest().value == O(2));
    CHECK(mad.mad_src1(false).value == V(3));
    CHECK(mad.mad_src2(false).value == C(33));
    CHECK(mad.mad_src3(false).value == V(4));
    CHECK(mad.mad_address_register_index() == 3);
    a.Madi(O(2), V(3), V(5), C(34), {}, 2);
    const isa::Instruction madi{a.code[3]};
    CHECK(madi.Effective() == isa::Opcode::MADI);
    CHECK(madi.mad_src2(true).value == V(5));
    CHECK(madi.mad_src3(true).value == C(34));
    // compare and flow control
    a.Cmp(Gt, Le, V(1), V(2));
    const isa::Instruction cmp{a.code[4]};
    CHECK(cmp.Effective() == isa::Opcode::CMP);
    CHECK(cmp.compare_x() == isa::CompareOp::GreaterThan);
    CHECK(cmp.compare_y() == isa::CompareOp::LessEqual);
    a.Raw(Asm::Flow(kIfc, 321, 17, Cond{.op = And, .refx = true, .refy = false}.Bits()));
    const isa::Instruction ifc{a.code[5]};
    CHECK(ifc.Effective() == isa::Opcode::IFC);
    CHECK(ifc.dest_offset() == 321);
    CHECK(ifc.num_instructions() == 17);
    CHECK(ifc.condition_op() == isa::ConditionOp::And);
    CHECK(ifc.refx());
    CHECK_FALSE(ifc.refy());
    a.Raw(Asm::Flow(kLoop, 99, 0, 3u << 22));
    const isa::Instruction loop{a.code[6]};
    CHECK(loop.Effective() == isa::Opcode::LOOP);
    CHECK(loop.int_uniform_id() == 3);
    CHECK(loop.dest_offset() == 99);
    a.Raw(Asm::Flow(kJmpu, 7, 1, 13u << 22));
    const isa::Instruction jmpu{a.code[7]};
    CHECK(jmpu.bool_uniform_id() == 13);
    CHECK(jmpu.num_instructions() == 1);
    // the table
    CHECK(std::string(isa::GetOpInfo(0x00).name) == "add");
    CHECK(std::string(isa::GetOpInfo(0x2F).name) == "cmp");
    CHECK(std::string(isa::GetOpInfo(0x33).name) == "madi");
    CHECK(std::string(isa::GetOpInfo(0x3F).name) == "mad");
    CHECK(isa::GetOpInfo(0x10).format == isa::Format::Unknown);
    CHECK(isa::GetOpInfo(0x22).format == isa::Format::Trivial);
}

TEST_CASE("the uniform blocks are std140: bool bits, integer vectors, float vectors, clip state") {
    static_assert(offsetof(shader::PicaUniformData, b) == 0);
    static_assert(offsetof(shader::PicaUniformData, i) == 16);
    static_assert(offsetof(shader::PicaUniformData, f) == 80);
    static_assert(offsetof(shader::VertexUniformData, enable_clip1) == 0);
    static_assert(offsetof(shader::VertexUniformData, flip_viewport) == 4);
    static_assert(offsetof(shader::VertexUniformData, clip_coef) == 16);
    pica::ShaderSetup setup;
    setup.uniform_b[0] = true;
    setup.uniform_b[3] = true;
    setup.uniform_b[15] = true;
    setup.uniform_i[2] = {1, 2, 3, 255};
    setup.uniform_f[95] = pica::Vec4f{{1.5f, -2.5f, 3.5f, kInf}};
    const shader::PicaUniformData u = shader::MakePicaUniformData(setup);
    CHECK(u.b == ((1u << 0) | (1u << 3) | (1u << 15)));
    CHECK(u.i[2][0] == 1);
    CHECK(u.i[2][1] == 2);
    CHECK(u.i[2][2] == 3);
    CHECK(u.i[2][3] == 255);
    CHECK(u.f[95][0] == 1.5f);
    CHECK(u.f[95][1] == -2.5f);
    CHECK(u.f[95][3] == kInf);
    CHECK(u.f[0][0] == 0.f);

    pica::PicaState state;
    state.regs[shader::reg::kClipEnable] = 1;
    // float24: sign 0, exponent 0x7F (bias 63 -> 2^64?) use the decoder for the expectation
    state.regs[shader::reg::kClipCoef + 0] = 0x3F8000;
    state.regs[shader::reg::kClipCoef + 2] = 0xBF8000;
    const shader::VertexUniformData vu = shader::MakeVertexUniformData(state, true);
    CHECK(vu.enable_clip1 == 1);
    CHECK(vu.flip_viewport == 1);
    CHECK(vu.clip_coef[0] == pica::Float24ToFloat(0x3F8000));
    CHECK(vu.clip_coef[2] == pica::Float24ToFloat(0xBF8000));
    CHECK(vu.clip_coef[2] == -vu.clip_coef[0]);
    CHECK(vu.clip_coef[1] == 0.f);
    state.regs[shader::reg::kClipEnable] = 0;
    CHECK(shader::MakeVertexUniformData(state, false).enable_clip1 == 0);
    CHECK(shader::MakeVertexUniformData(state, false).flip_viewport == 0);
}
