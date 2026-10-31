// Derived from Azahar externals/nihstro/include/nihstro/shader_bytecode.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (the file belongs to the submodule externals/nihstro that
// Azahar pins at f4d8659decbfe5d234f04134b5002b82dc515a44; it is NOT GPL: its licence is the BSD
// 3-clause licence of externals/nihstro/license.txt, reproduced below because it asks for that) and
// src/video_core/shader/shader_interpreter.cpp (the use of the fields: which of the two source fields
// a form uses, the "inverted" forms) in the same commit (GPL-2.0-or-later; it says "Licensed under
// GPLv2 or any later version").
// Copyright 2014 Tony Wasserka (shader_bytecode.h)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (shader_interpreter.cpp)
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
// The PICA shader instruction word as the interpreter reads it (N2-36): opcodes, the instruction
// formats and the operand descriptor (nihstro's SwizzlePattern), as plain functions on a 32-bit
// word instead of bit-field unions. It is the product's own decoder: it does not use the one of
// the shader translator (gpu/shader/pica_isa.h), so the CPU runner does not depend on that module.
#pragma once

#include <array>

#include "mh4n/types.h"

namespace mh4n::gpu::cpuvertex::isa {

// OpCode::Id. MAD / MADI take the opcodes 0x30-0x37 / 0x38-0x3F (the low 3 bits are ignored), CMP
// 0x2E-0x2F (the low bit is ignored): see Instruction::EffectiveOpCode.
enum class Opcode : u32 {
    ADD = 0x00,
    DP3 = 0x01,
    DP4 = 0x02,
    DPH = 0x03,
    DST = 0x04,
    EX2 = 0x05,
    LG2 = 0x06,
    LIT = 0x07,
    MUL = 0x08,
    SGE = 0x09,
    SLT = 0x0A,
    FLR = 0x0B,
    MAX = 0x0C,
    MIN = 0x0D,
    RCP = 0x0E,
    RSQ = 0x0F,
    MOVA = 0x12,
    MOV = 0x13,
    DPHI = 0x18,
    DSTI = 0x19,
    SGEI = 0x1A,
    SLTI = 0x1B,
    BREAK = 0x20,
    NOP = 0x21,
    END = 0x22,
    BREAKC = 0x23,
    CALL = 0x24,
    CALLC = 0x25,
    CALLU = 0x26,
    IFU = 0x27,
    IFC = 0x28,
    LOOP = 0x29,
    EMIT = 0x2A,
    SETEMIT = 0x2B,
    JMPC = 0x2C,
    JMPU = 0x2D,
    CMP = 0x2E,
    MADI = 0x30,
    MAD = 0x38,
};

// OpCode::Type (3dbrew's instruction formats).
enum class Type : u8 {
    Trivial,            // 0
    Arithmetic,         // 1
    Conditional,        // 2
    UniformFlowControl, // 3
    SetEmit,            // 4
    MultiplyAdd,        // 5
    Unknown,
};

struct OpInfo {
    Type type;
    bool src_inversed; // DPHI, DSTI, SGEI, SLTI: the second source has the wide (7-bit) field
    const char* name;
};

namespace detail {
constexpr std::array<OpInfo, 64> MakeOpTable() {
    std::array<OpInfo, 64> t{};
    for (auto& e : t) {
        e = {Type::Unknown, false, "UNK"};
    }
    auto set = [&t](Opcode op, Type type, const char* name, bool inverted = false) {
        t[static_cast<u32>(op)] = {type, inverted, name};
    };
    set(Opcode::ADD, Type::Arithmetic, "add");
    set(Opcode::DP3, Type::Arithmetic, "dp3");
    set(Opcode::DP4, Type::Arithmetic, "dp4");
    set(Opcode::DPH, Type::Arithmetic, "dph");
    set(Opcode::DST, Type::Arithmetic, "dst");
    set(Opcode::EX2, Type::Arithmetic, "exp");
    set(Opcode::LG2, Type::Arithmetic, "log");
    set(Opcode::LIT, Type::Arithmetic, "lit");
    set(Opcode::MUL, Type::Arithmetic, "mul");
    set(Opcode::SGE, Type::Arithmetic, "sge");
    set(Opcode::SLT, Type::Arithmetic, "slt");
    set(Opcode::FLR, Type::Arithmetic, "flr");
    set(Opcode::MAX, Type::Arithmetic, "max");
    set(Opcode::MIN, Type::Arithmetic, "min");
    set(Opcode::RCP, Type::Arithmetic, "rcp");
    set(Opcode::RSQ, Type::Arithmetic, "rsq");
    set(Opcode::MOVA, Type::Arithmetic, "mova");
    set(Opcode::MOV, Type::Arithmetic, "mov");
    set(Opcode::DPHI, Type::Arithmetic, "dphi", true);
    set(Opcode::DSTI, Type::Arithmetic, "dsti", true);
    set(Opcode::SGEI, Type::Arithmetic, "sgei", true);
    set(Opcode::SLTI, Type::Arithmetic, "slti", true);
    set(Opcode::BREAK, Type::Trivial, "break");
    set(Opcode::NOP, Type::Trivial, "nop");
    set(Opcode::END, Type::Trivial, "end");
    set(Opcode::BREAKC, Type::Conditional, "breakc");
    set(Opcode::CALL, Type::Conditional, "call");
    set(Opcode::CALLC, Type::Conditional, "callc");
    set(Opcode::CALLU, Type::UniformFlowControl, "callu");
    set(Opcode::IFU, Type::UniformFlowControl, "ifu");
    set(Opcode::IFC, Type::Conditional, "ifc");
    set(Opcode::LOOP, Type::UniformFlowControl, "loop");
    set(Opcode::EMIT, Type::Trivial, "emit");
    set(Opcode::SETEMIT, Type::SetEmit, "setemit");
    set(Opcode::JMPC, Type::Conditional, "jmpc");
    set(Opcode::JMPU, Type::Conditional, "jmpu");
    t[0x2E] = {Type::Arithmetic, false, "cmp"};
    t[0x2F] = {Type::Arithmetic, false, "cmp"};
    for (u32 i = 0x30; i < 0x38; ++i) {
        t[i] = {Type::MultiplyAdd, true, "madi"};
    }
    for (u32 i = 0x38; i < 0x40; ++i) {
        t[i] = {Type::MultiplyAdd, false, "mad"};
    }
    return t;
}
inline constexpr std::array<OpInfo, 64> kOpTable = MakeOpTable();
} // namespace detail

// OpCode::GetInfo of a raw 6-bit opcode.
inline const OpInfo& GetOpInfo(u32 opcode6) {
    return detail::kOpTable[opcode6 & 0x3F];
}

// RegisterType, for the three kinds a source operand can name.
enum class RegisterType : u8 { Input, Temporary, FloatUniform };

// SourceRegister: v0-v15 (value < 0x10), r0-r15 (value < 0x20), c0-c95 (the rest).
struct SourceRegister {
    u32 value = 0;

    RegisterType GetRegisterType() const {
        if (value < 0x10)
            return RegisterType::Input;
        else if (value < 0x20)
            return RegisterType::Temporary;
        else
            return RegisterType::FloatUniform;
    }
    int GetIndex() const {
        if (value < 0x10)
            return static_cast<int>(value);
        else if (value < 0x20)
            return static_cast<int>(value - 0x10);
        else
            return static_cast<int>(value - 0x20);
    }
};

// DestRegister (5 bits): o0-o15 (value < 0x10), r0-r15 (the rest).
struct DestRegister {
    u32 value = 0;

    bool IsOutput() const {
        return value < 0x10;
    }
    int GetIndex() const {
        return value < 0x10 ? static_cast<int>(value) : static_cast<int>(value - 0x10);
    }
};

// Instruction::Common::CompareOpType::Op
enum class CompareOp : u32 {
    Equal = 0,
    NotEqual = 1,
    LessThan = 2,
    LessEqual = 3,
    GreaterThan = 4,
    GreaterEqual = 5,
    Unk6 = 6,
    Unk7 = 7,
};

// Instruction::FlowControlType::Op
enum class FlowOp : u32 { Or = 0, And = 1, JustX = 2, JustY = 3 };

// nihstro::Instruction: one 32-bit word, read through the format its opcode selects.
struct Instruction {
    u32 hex = 0;

    u32 opcode() const {
        return hex >> 26;
    }
    Opcode EffectiveOpCode() const {
        const u32 op = opcode();
        if ((op & ~0x7u) == static_cast<u32>(Opcode::MAD))
            return Opcode::MAD;
        else if ((op & ~0x7u) == static_cast<u32>(Opcode::MADI))
            return Opcode::MADI;
        else if ((op & ~0x1u) == static_cast<u32>(Opcode::CMP))
            return Opcode::CMP;
        else
            return static_cast<Opcode>(op);
    }
    const OpInfo& GetInfo() const {
        return GetOpInfo(opcode());
    }

    // ---- common (arithmetic and comparison) ----
    u32 common_operand_desc_id() const {
        return hex & 0x7F;
    }
    // When two input registers are used, one has a 5-bit index and the other a 7-bit one (at most
    // one floating point uniform may be an input); the "inverted" opcodes swap which is which.
    SourceRegister common_src1(bool is_inverted) const {
        return {is_inverted ? (hex >> 14) & 0x1F : (hex >> 12) & 0x7F};
    }
    SourceRegister common_src2(bool is_inverted) const {
        return {is_inverted ? (hex >> 7) & 0x7F : (hex >> 7) & 0x1F};
    }
    // The address register used for relative addressing of src1 / src2 (inverted): 0 none,
    // 1 a0.x, 2 a0.y, 3 aL.
    u32 common_address_register_index() const {
        return (hex >> 19) & 0x3;
    }
    CompareOp common_compare_x() const {
        return static_cast<CompareOp>((hex >> 24) & 0x7);
    }
    CompareOp common_compare_y() const {
        return static_cast<CompareOp>((hex >> 21) & 0x7);
    }
    DestRegister common_dest() const {
        return {(hex >> 21) & 0x1F};
    }

    // ---- flow control ----
    u32 flow_num_instructions() const {
        return hex & 0xFF;
    }
    u32 flow_dest_offset() const {
        return (hex >> 10) & 0xFFF;
    }
    FlowOp flow_op() const {
        return static_cast<FlowOp>((hex >> 22) & 0x3);
    }
    u32 flow_bool_uniform_id() const {
        return (hex >> 22) & 0xF;
    }
    u32 flow_int_uniform_id() const {
        return (hex >> 22) & 0x3;
    }
    bool flow_refy() const {
        return ((hex >> 24) & 1) != 0;
    }
    bool flow_refx() const {
        return ((hex >> 25) & 1) != 0;
    }

    // ---- multiply-add ----
    u32 mad_operand_desc_id() const {
        return hex & 0x1F;
    }
    SourceRegister mad_src1(bool /*is_inverted*/) const {
        return {(hex >> 17) & 0x1F};
    }
    SourceRegister mad_src2(bool is_inverted) const {
        return {is_inverted ? (hex >> 12) & 0x1F : (hex >> 10) & 0x7F};
    }
    SourceRegister mad_src3(bool is_inverted) const {
        return {is_inverted ? (hex >> 5) & 0x7F : (hex >> 5) & 0x1F};
    }
    u32 mad_address_register_index() const {
        return (hex >> 22) & 0x3;
    }
    DestRegister mad_dest() const {
        return {(hex >> 24) & 0x1F};
    }

    // ---- SETEMIT ----
    bool setemit_winding() const {
        return ((hex >> 22) & 1) != 0;
    }
    bool setemit_prim_emit() const {
        return ((hex >> 23) & 1) != 0;
    }
    u32 setemit_vertex_id() const {
        return (hex >> 24) & 0x3;
    }
};

// nihstro::SwizzlePattern: the operand descriptor. Per source a negate bit and four 2-bit
// selectors; the destination write mask.
struct SwizzlePattern {
    u32 hex = 0;

    // Components of "dest" that should be written to: LSB = dest.w, MSB = dest.x
    bool DestComponentEnabled(u32 i) const {
        return (hex & (0x8u >> i)) != 0;
    }
    bool negate_src1() const {
        return ((hex >> 4) & 1) != 0;
    }
    bool negate_src2() const {
        return ((hex >> 13) & 1) != 0;
    }
    bool negate_src3() const {
        return ((hex >> 22) & 1) != 0;
    }
    // Selector `comp` (0 = x) of source 1-3: which lane (0 = x ... 3 = w) it reads.
    u32 selector_src1(u32 comp) const {
        return (hex >> (11 - 2 * comp)) & 0x3;
    }
    u32 selector_src2(u32 comp) const {
        return (hex >> (20 - 2 * comp)) & 0x3;
    }
    u32 selector_src3(u32 comp) const {
        return (hex >> (29 - 2 * comp)) & 0x3;
    }
};

} // namespace mh4n::gpu::cpuvertex::isa
