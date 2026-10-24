// Derived from Azahar src/video_core/shader/shader_interpreter.cpp, src/video_core/pica_types.h,
// src/video_core/pica/shader_unit.cpp, shader_unit.h, output_vertex.cpp, regs_shader.h, regs_rasterizer.h
// and src/video_core/shader/generator/glsl_shader_gen.cpp at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6
// (GPL-2.0-or-later; each of those files says "Licensed under GPLv2 or any later version"): the CPU
// interpreter below (namespace oracle) is a port of RunInterpreter with the float rules of
// Pica::Float's operator*, ShaderUnit::LoadInput / WriteOutput and the OutputVertex semantic fill; the
// register positions the tests write are those of ShaderRegs, RasterizerRegs and PipelineRegs.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (shader_interpreter.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (pica_types.h)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (shader_unit.cpp, glsl_shader_gen.cpp)
// Copyright 2023-2024 Citra Emulator Project / Azahar Emulator Project (shader_unit.h)
// Copyright 2023 Citra Emulator Project (output_vertex.cpp)
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (regs_shader.h)
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
        std::string key = std::to_string(reinterpret_cast<uintptr_t>(&t)) + ":";
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
        auto capture = Make(capture_bytes, rhi::Heap::Device, rhi::BufferUsage::Storage | rhi::BufferUsage::Transfer, "capture");
        auto zeros = Make(capture_bytes, rhi::Heap::Upload, rhi::BufferUsage::Transfer, "zeros");
        const shader::PicaUniformData pu = shader::MakePicaUniformData(setup);
        const shader::VertexUniformData vu = shader::MakeVertexUniformData(state, flip);
        std::memcpy(uniforms->Mapped().data(), &pu, sizeof(pu));
        uniforms->Flush(0, sizeof(pu));
        std::memcpy(vdata->Mapped().data(), &vu, sizeof(vu));
        vdata->Flush(0, sizeof(vu));
        std::memset(zeros->Mapped().data(), 0, capture_bytes);
        zeros->Flush(0, capture_bytes);

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
        rec.CopyBuffer(*capture, 0, *zeros, 0, capture_bytes);
        rec.Barrier(rhi::Stage::Transfer, rhi::Stage::VertexShader);
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
        r.Note(what + ": gpu " + std::to_string(gpu) + " cpu " + std::to_string(cpu));
    }
}

// Compares the capture of the GPU with the oracle for the vertex ids in `ids`. `varyings`: also the
// seven varyings and the position.
Report CompareWithOracle(const Draw& d, const shader::TranslatedVertexShader& t, const std::vector<float>& cap,
                         const std::vector<u32>& ids, bool varyings, bool clip = false) {
    Report report;
    const oracle::Memory mem = OracleMemory(d.setup);
    const oracle::Uniforms uniforms = OracleUniforms(d.setup);
    const u32 mask = d.state->regs[pica::reg::kVs + pica::reg::kShaderOutputMask] & 0xFFFF;
    const u32 entry = d.state->regs[pica::reg::kVs + pica::reg::kShaderMainOffset] & 0xFFFF;
    for (u32 id : ids) {
        oracle::Unit u;
        FetchInput(d.loaded, id, u.input);
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
            const Varyings e = ExpectedVaryings(u, *d.state, d.flip_viewport);
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
