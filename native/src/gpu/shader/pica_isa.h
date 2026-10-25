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
// The PICA vertex-shader instruction encoding (N2-06): opcodes, the instruction formats and the
// swizzle pattern word (the "operand descriptor"), as plain functions on a 32-bit word instead of
// nihstro's bit-field unions. Nothing here knows about GLSL or about a shader unit.
#pragma once

#include "mh4n/types.h"

namespace mh4n::gpu::shader::isa {

// Opcode ids (the top 6 bits of an instruction). MAD / MADI take the opcodes 0x30-0x37 / 0x38-0x3F
// (the low 3 bits are ignored), CMP 0x2E-0x2F (the low bit is ignored): use Instruction::Effective().
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

// 3dbrew's formats.
enum class Format : u8 {
    Trivial,            // 0
    Arithmetic,         // 1
    Conditional,        // 2
    UniformFlowControl, // 3
    SetEmit,            // 4
    MultiplyAdd,        // 5
    Unknown,
};

struct OpInfo {
    Format format;
    bool src_inversed; // DPHI, DSTI, SGEI, SLTI: the second source has the wide (7-bit) field
    const char* name;
};

// The table entry of a raw 6-bit opcode.
const OpInfo& GetOpInfo(u32 opcode6);

// The registers a source operand names: v0-v15, r0-r15, and c0-c95 (the uniform may be offset by an
// address register).
enum class SourceKind : u8 { Input, Temporary, FloatUniform };
struct SourceRegister {
    u32 value = 0;
    SourceKind kind() const {
        return value < 0x10 ? SourceKind::Input : value < 0x20 ? SourceKind::Temporary : SourceKind::FloatUniform;
    }
    u32 index() const {
        return value < 0x10 ? value : value < 0x20 ? value - 0x10 : value - 0x20;
    }
};

// A destination: o0-o15 (value < 0x10), r0-r15 (value < 0x20); larger values name nothing (a write
// to them is dropped; the interpreter writes into a dummy).
struct DestRegister {
    u32 value = 0;
    bool is_output() const { return value < 0x10; }
    bool is_temporary() const { return value >= 0x10 && value < 0x20; }
    bool is_valid() const { return value < 0x20; }
    u32 index() const { return value < 0x10 ? value : value - 0x10; }
};

enum class CompareOp : u32 { Equal = 0, NotEqual = 1, LessThan = 2, LessEqual = 3, GreaterThan = 4, GreaterEqual = 5, Unk6 = 6, Unk7 = 7 };
enum class ConditionOp : u32 { Or = 0, And = 1, JustX = 2, JustY = 3 };

struct Instruction {
    u32 hex = 0;

    u32 opcode() const { return hex >> 26; }
    Opcode Effective() const {
        const u32 op = opcode();
        if ((op & ~0x7u) == static_cast<u32>(Opcode::MAD)) {
            return Opcode::MAD;
        }
        if ((op & ~0x7u) == static_cast<u32>(Opcode::MADI)) {
            return Opcode::MADI;
        }
        if ((op & ~0x1u) == static_cast<u32>(Opcode::CMP)) {
            return Opcode::CMP;
        }
        return static_cast<Opcode>(op);
    }
    const OpInfo& info() const { return GetOpInfo(opcode()); }

    // ---- arithmetic (common) ----
    u32 operand_desc_id() const { return hex & 0x7F; }
    // The first source is the 7-bit field and the second the 5-bit one, or the other way round for
    // the "inverted" opcodes.
    SourceRegister src1(bool inverted) const {
        return {inverted ? (hex >> 14) & 0x1F : (hex >> 12) & 0x7F};
    }
    SourceRegister src2(bool inverted) const {
        return {inverted ? (hex >> 7) & 0x7F : (hex >> 7) & 0x1F};
    }
    u32 address_register_index() const { return (hex >> 19) & 0x3; } // 0 none, 1 a0.x, 2 a0.y, 3 aL
    DestRegister dest() const { return {(hex >> 21) & 0x1F}; }
    CompareOp compare_x() const { return static_cast<CompareOp>((hex >> 24) & 0x7); }
    CompareOp compare_y() const { return static_cast<CompareOp>((hex >> 21) & 0x7); }

    // ---- flow control ----
    u32 num_instructions() const { return hex & 0xFF; }
    u32 dest_offset() const { return (hex >> 10) & 0xFFF; }
    ConditionOp condition_op() const { return static_cast<ConditionOp>((hex >> 22) & 0x3); }
    u32 bool_uniform_id() const { return (hex >> 22) & 0xF; }
    u32 int_uniform_id() const { return (hex >> 22) & 0x3; }
    bool refy() const { return ((hex >> 24) & 1) != 0; }
    bool refx() const { return ((hex >> 25) & 1) != 0; }

    // ---- multiply-add ----
    u32 mad_operand_desc_id() const { return hex & 0x1F; }
    SourceRegister mad_src1(bool /*inverted*/) const { return {(hex >> 17) & 0x1F}; }
    SourceRegister mad_src2(bool inverted) const {
        return {inverted ? (hex >> 12) & 0x1F : (hex >> 10) & 0x7F};
    }
    SourceRegister mad_src3(bool inverted) const {
        return {inverted ? (hex >> 5) & 0x7F : (hex >> 5) & 0x1F};
    }
    u32 mad_address_register_index() const { return (hex >> 22) & 0x3; }
    DestRegister mad_dest() const { return {(hex >> 24) & 0x1F}; }
};

// The operand descriptor (nihstro's SwizzlePattern): per source a 4x2-bit selector and a negate bit,
// and the destination write mask.
struct Swizzle {
    u32 hex = 0;

    // Bit 3 of the mask is x, bit 0 is w.
    bool DestEnabled(u32 component) const { return (hex & (0x8u >> component)) != 0; }
    bool negate(u32 source) const { // source 1-3
        static constexpr u32 kBits[3] = {4, 13, 22};
        return ((hex >> kBits[source - 1]) & 1) != 0;
    }
    // Component `component` (0 = x) of source `source` (1-3) selects this lane (0 = x ... 3 = w).
    u32 selector(u32 source, u32 component) const {
        static constexpr u32 kFirstBit[3] = {5, 14, 23}; // the lowest bit of the 8-bit field
        return (hex >> (kFirstBit[source - 1] + 2 * (3 - component))) & 0x3;
    }
};

} // namespace mh4n::gpu::shader::isa
