// Derived from Azahar externals/nihstro/include/nihstro/shader_bytecode.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (BSD 3-clause licence of externals/nihstro/license.txt,
// reproduced at the top of the header next to this file; not GPL): the instruction info table (OpCode::GetInfo).
// Copyright 2014 Tony Wasserka
#include "gpu/shader/pica_isa.h"

#include <array>

namespace mh4n::gpu::shader::isa {

namespace {

constexpr std::array<OpInfo, 64> MakeTable() {
    std::array<OpInfo, 64> t{};
    for (auto& e : t) {
        e = {Format::Unknown, false, "UNK"};
    }
    auto set = [&](Opcode op, Format f, const char* name, bool inverted = false) {
        t[static_cast<u32>(op)] = {f, inverted, name};
    };
    set(Opcode::ADD, Format::Arithmetic, "add");
    set(Opcode::DP3, Format::Arithmetic, "dp3");
    set(Opcode::DP4, Format::Arithmetic, "dp4");
    set(Opcode::DPH, Format::Arithmetic, "dph");
    set(Opcode::DST, Format::Arithmetic, "dst");
    set(Opcode::EX2, Format::Arithmetic, "ex2");
    set(Opcode::LG2, Format::Arithmetic, "lg2");
    set(Opcode::LIT, Format::Arithmetic, "lit");
    set(Opcode::MUL, Format::Arithmetic, "mul");
    set(Opcode::SGE, Format::Arithmetic, "sge");
    set(Opcode::SLT, Format::Arithmetic, "slt");
    set(Opcode::FLR, Format::Arithmetic, "flr");
    set(Opcode::MAX, Format::Arithmetic, "max");
    set(Opcode::MIN, Format::Arithmetic, "min");
    set(Opcode::RCP, Format::Arithmetic, "rcp");
    set(Opcode::RSQ, Format::Arithmetic, "rsq");
    set(Opcode::MOVA, Format::Arithmetic, "mova");
    set(Opcode::MOV, Format::Arithmetic, "mov");
    set(Opcode::DPHI, Format::Arithmetic, "dphi", true);
    set(Opcode::DSTI, Format::Arithmetic, "dsti", true);
    set(Opcode::SGEI, Format::Arithmetic, "sgei", true);
    set(Opcode::SLTI, Format::Arithmetic, "slti", true);
    set(Opcode::BREAK, Format::Trivial, "break");
    set(Opcode::NOP, Format::Trivial, "nop");
    set(Opcode::END, Format::Trivial, "end");
    set(Opcode::BREAKC, Format::Conditional, "breakc");
    set(Opcode::CALL, Format::Conditional, "call");
    set(Opcode::CALLC, Format::Conditional, "callc");
    set(Opcode::CALLU, Format::UniformFlowControl, "callu");
    set(Opcode::IFU, Format::UniformFlowControl, "ifu");
    set(Opcode::IFC, Format::Conditional, "ifc");
    set(Opcode::LOOP, Format::UniformFlowControl, "loop");
    set(Opcode::EMIT, Format::Trivial, "emit");
    set(Opcode::SETEMIT, Format::SetEmit, "setemit");
    set(Opcode::JMPC, Format::Conditional, "jmpc");
    set(Opcode::JMPU, Format::Conditional, "jmpu");
    t[0x2E] = {Format::Arithmetic, false, "cmp"};
    t[0x2F] = {Format::Arithmetic, false, "cmp"};
    for (u32 i = 0x30; i < 0x38; ++i) {
        t[i] = {Format::MultiplyAdd, true, "madi"};
    }
    for (u32 i = 0x38; i < 0x40; ++i) {
        t[i] = {Format::MultiplyAdd, false, "mad"};
    }
    return t;
}

constexpr std::array<OpInfo, 64> kTable = MakeTable();

} // namespace

const OpInfo& GetOpInfo(u32 opcode6) {
    return kTable[opcode6 & 0x3F];
}

} // namespace mh4n::gpu::shader::isa
