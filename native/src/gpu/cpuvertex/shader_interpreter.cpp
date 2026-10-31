// Derived from Azahar src/video_core/shader/shader_interpreter.cpp, src/video_core/pica/shader_unit.cpp
// and src/video_core/pica_types.h at 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later;
// every one of those files says "Licensed under GPLv2 or any later version"): the interpreter, one
// handler for each instruction, in the order and with the quirks of the original; ShaderUnit::LoadInput
// and WriteOutput; GeometryEmitter::Emit.
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (shader_interpreter.cpp)
// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project (shader_unit.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (pica_types.h)
//
// Each `case` below is the same case of RunInterpreter in shader_interpreter.cpp (Debug = false: the
// debug records are not kept). Where this file differs it says so in a comment starting with
// "Deviation".
#include "gpu/cpuvertex/shader_interpreter.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mh4n::gpu::cpuvertex {

using isa::CompareOp;
using isa::FlowOp;
using isa::Instruction;
using isa::Opcode;
using isa::RegisterType;
using isa::SwizzlePattern;
using isa::Type;

// ---- ShaderUnit and the emitter (shader_unit.cpp) -------------------------------------------------

void ShaderUnit::LoadInput(const ShaderInputConfig& config, const AttributeBuffer& buffer) {
    const u32 max_attribute = config.max_input_attribute_index;
    for (u32 attr = 0; attr <= max_attribute; ++attr) {
        const u32 reg = config.GetRegisterForAttribute(attr);
        input[reg] = buffer[attr];
    }
}

void ShaderUnit::WriteOutput(u32 output_mask, AttributeBuffer& buffer) const {
    u32 output_index{};
    // Common::BitSet<u32>: the set bits, lowest first.
    for (u32 mask = output_mask; mask != 0; mask &= mask - 1) {
        const u32 reg = static_cast<u32>(__builtin_ctz(mask));
        buffer[output_index++] = output[output_bank][reg];
    }
}

void GeometryEmitter::Emit(std::span<const Vec4, 16> output_regs) {
    // The interpreter has checked emit_state.vertex_id < 3 (Azahar: ASSERT).
    u32 output_index{};
    for (u32 mask = output_mask; mask != 0; mask &= mask - 1) {
        const u32 reg = static_cast<u32>(__builtin_ctz(mask));
        buffer[emit_state.vertex_id][output_index++] = output_regs[reg];
    }

    if (emit_state.prim_emit) {
        if (emit_state.winding) {
            sink->SetWinding();
        }
        for (std::size_t i = 0; i < buffer.size(); ++i) {
            sink->SubmitVertex(buffer[i]);
        }
    }
}

void GeometryShaderUnit::Reset() {
    VertexSink* keep = emitter.sink;
    const u32 mask = emitter.output_mask;
    address_registers[0] = address_registers[1] = address_registers[2] = 0;
    conditional_code[0] = conditional_code[1] = false;
    input = {};
    temporary = {};
    output = {};
    output_bank = false;
    emitter = GeometryEmitter{};
    emitter.sink = keep;
    emitter.output_mask = mask;
}

// ---- the interpreter (shader_interpreter.cpp) ------------------------------------------------------

namespace {

struct IfStackElement {
    u32 else_address;
    u32 end_address;
};

struct CallStackElement {
    u32 end_address;
    u32 return_address;
};

struct LoopStackElement {
    u32 entry_address;
    u32 end_address;
    u8 loop_downcounter;
    u8 address_increment;
    u8 previous_aL;
};

// boost::circular_buffer<T>(N) as the interpreter uses it: pushing onto a full buffer overwrites
// the oldest element.
template <typename T, std::size_t N>
class CircularStack {
public:
    bool empty() const {
        return size_ == 0;
    }
    std::size_t size() const {
        return size_;
    }
    T& back() {
        return data_[(first_ + size_ - 1) % N];
    }
    void push_back(const T& value) {
        if (size_ == N) {
            first_ = (first_ + 1) % N;
            --size_;
        }
        data_[(first_ + size_) % N] = value;
        ++size_;
    }
    void pop_back() {
        --size_;
    }

private:
    std::array<T, N> data_{};
    std::size_t first_ = 0;
    std::size_t size_ = 0;
};

// static_cast<s32>(float) as it is on x86-64 (Azahar's only configuration that matters here): a
// value that does not fit, and NaN, give the most negative integer. The C++ cast is undefined
// there. (Every such value is outside the 8-bit range LookupSourceRegister allows an address
// register, so the result only has to be deterministic.)
s32 FloatToS32(float value) {
    if (!(value > -2147483648.0f && value < 2147483648.0f)) {
        return std::numeric_limits<s32>::min();
    }
    return static_cast<s32>(value);
}

} // namespace

bool InterpreterEngine::Run(const ShaderSetup& setup, ShaderUnit& state, RunError& error) const {
    error = {};
    // SetupBatch: ASSERT(entry_point < MAX_PROGRAM_CODE_LENGTH).
    if (setup.entry_point >= kMaxProgramCodeLength) {
        error = {Unhandled::EntryPoint, setup.entry_point, 0};
        return false;
    }

    CircularStack<IfStackElement, 8> if_stack;
    CircularStack<CallStackElement, 4> call_stack;
    CircularStack<LoopStackElement, 4> loop_stack;
    u32 program_counter = setup.entry_point;

    const auto do_if = [&](Instruction instr, bool condition) {
        if (condition) {
            if_stack.push_back({
                instr.flow_dest_offset(),
                instr.flow_dest_offset() + instr.flow_num_instructions(),
            });
        } else {
            program_counter = instr.flow_dest_offset() - 1;
        }
    };

    const auto do_call = [&](Instruction instr) {
        call_stack.push_back({
            instr.flow_dest_offset() + instr.flow_num_instructions(),
            program_counter + 1,
        });
        program_counter = instr.flow_dest_offset() - 1;
    };

    const auto do_loop = [&](Instruction instr, const std::array<u8, 4>& loop_param) {
        const u8 previous_aL = static_cast<u8>(state.address_registers[2]);
        loop_stack.push_back({
            program_counter + 1,
            instr.flow_dest_offset() + 1,
            loop_param[0],
            loop_param[2],
            previous_aL,
        });
        state.address_registers[2] = loop_param[1];
    };

    auto evaluate_condition = [&state](Instruction instr) {
        bool result_x = instr.flow_refx() == state.conditional_code[0];
        bool result_y = instr.flow_refy() == state.conditional_code[1];

        switch (instr.flow_op()) {
        case FlowOp::Or:
            return result_x || result_y;
        case FlowOp::And:
            return result_x && result_y;
        case FlowOp::JustX:
            return result_x;
        case FlowOp::JustY:
            return result_y;
        }
        return false; // unreachable: the field has two bits
    };

    const Uniforms& uniforms = *setup.uniforms;
    const u32* swizzle_data = setup.swizzle_data;
    const u32* program_code = setup.program_code;

    // Constants for handling invalid inputs. In Azahar these are static arrays that a write to an
    // invalid destination would modify; the destination field has five bits, so none exists, and
    // these are constant here.
    static const f24 dummy_vec4_float24_zeros[4] = {f24::Zero(), f24::Zero(), f24::Zero(), f24::Zero()};
    static const f24 dummy_vec4_float24_ones[4] = {f24::One(), f24::One(), f24::One(), f24::One()};

    u32 iteration = 0;
    bool should_stop = false;
    while (!should_stop) {
        bool is_break = false;
        const u32 old_program_counter = program_counter;

        if (iteration >= kMaxInstructions) {
            error = {Unhandled::InstructionBudget, old_program_counter, 0};
            return false;
        }

        // Always treat the last instruction of the program code as an end instruction. This fixes
        // some games such as Thunder Blade or After Burner II which have malformed geo shaders
        // without an end instruction crashing the emulator due to the program counter growing
        // uncontrollably. (Azahar's comment; its TODO: find how real hardware reacts.)
        Instruction instr{};
        if (program_counter < kMaxProgramCodeLength - 1) {
            instr.hex = program_code[program_counter];
        } else {
            instr.hex = static_cast<u32>(Opcode::END) << 26;
        }

        const SwizzlePattern swizzle = {swizzle_data[instr.common_operand_desc_id()]};

        auto LookupSourceRegister = [&](const isa::SourceRegister& source_reg,
                                        int address_register_index) -> const f24* {
            int index = source_reg.GetIndex();
            switch (source_reg.GetRegisterType()) {
            case RegisterType::Input:
                return &state.input[static_cast<size_t>(index)].c[0];

            case RegisterType::Temporary:
                return &state.temporary[static_cast<size_t>(index)].c[0];

            case RegisterType::FloatUniform:
                if (address_register_index != 0) {
                    int offset = state.address_registers[address_register_index - 1];
                    if (offset < std::numeric_limits<s8>::min() ||
                        offset > std::numeric_limits<s8>::max()) [[unlikely]] {
                        offset = 0;
                    }
                    index = (index + offset) & 0x7F;
                    // If the index is above 96, the result is all one.
                    if (index >= 96) [[unlikely]] {
                        return dummy_vec4_float24_ones;
                    }
                }
                return &uniforms.f[static_cast<size_t>(index)].c[0];

            default:
                return dummy_vec4_float24_zeros;
            }
        };

        switch (instr.GetInfo().type) {
        case Type::Arithmetic: {
            const bool is_inverted = instr.GetInfo().src_inversed;

            const f24* src1_ = LookupSourceRegister(
                instr.common_src1(is_inverted),
                !is_inverted * static_cast<int>(instr.common_address_register_index()));
            const f24* src2_ = LookupSourceRegister(
                instr.common_src2(is_inverted),
                is_inverted * static_cast<int>(instr.common_address_register_index()));

            const bool negate_src1 = swizzle.negate_src1();
            const bool negate_src2 = swizzle.negate_src2();

            f24 src1[4] = {
                src1_[swizzle.selector_src1(0)],
                src1_[swizzle.selector_src1(1)],
                src1_[swizzle.selector_src1(2)],
                src1_[swizzle.selector_src1(3)],
            };
            if (negate_src1) {
                src1[0] = -src1[0];
                src1[1] = -src1[1];
                src1[2] = -src1[2];
                src1[3] = -src1[3];
            }
            f24 src2[4] = {
                src2_[swizzle.selector_src2(0)],
                src2_[swizzle.selector_src2(1)],
                src2_[swizzle.selector_src2(2)],
                src2_[swizzle.selector_src2(3)],
            };
            if (negate_src2) {
                src2[0] = -src2[0];
                src2[1] = -src2[1];
                src2[2] = -src2[2];
                src2[3] = -src2[3];
            }

            // Deviation: Azahar's `dest` can also point at a dummy array for a register value of
            // 0x20 and above; the field is five bits wide, so that arm cannot be taken.
            f24* dest = instr.common_dest().IsOutput()
                            ? &state.output[state.output_bank][static_cast<size_t>(instr.common_dest().GetIndex())].c[0]
                            : &state.temporary[static_cast<size_t>(instr.common_dest().GetIndex())].c[0];

            switch (instr.EffectiveOpCode()) {
            case Opcode::ADD: {
                for (int i = 0; i < 4; ++i) {
                    if (!swizzle.DestComponentEnabled(i))
                        continue;

                    dest[i] = src1[i] + src2[i];
                }
                break;
            }

            case Opcode::MUL: {
                for (int i = 0; i < 4; ++i) {
                    if (!swizzle.DestComponentEnabled(i))
                        continue;

                    dest[i] = src1[i] * src2[i];
                }
                break;
            }

            case Opcode::FLR:
                for (int i = 0; i < 4; ++i) {
                    if (!swizzle.DestComponentEnabled(i))
                        continue;

                    dest[i] = f24::FromFloat32(std::floor(src1[i].ToFloat32()));
                }
                break;

            case Opcode::MAX:
                for (int i = 0; i < 4; ++i) {
                    if (!swizzle.DestComponentEnabled(i))
                        continue;

                    // NOTE: Exact form required to match NaN semantics to hardware:
                    //   max(0, NaN) -> NaN
                    //   max(NaN, 0) -> 0
                    dest[i] = (src1[i] > src2[i]) ? src1[i] : src2[i];
                }
                break;

            case Opcode::MIN:
                for (int i = 0; i < 4; ++i) {
                    if (!swizzle.DestComponentEnabled(i))
                        continue;

                    // NOTE: Exact form required to match NaN semantics to hardware:
                    //   min(0, NaN) -> NaN
                    //   min(NaN, 0) -> 0
                    dest[i] = (src1[i] < src2[i]) ? src1[i] : src2[i];
                }
                break;

            case Opcode::DP3:
            case Opcode::DP4:
            case Opcode::DPH:
            case Opcode::DPHI: {
                const Opcode opcode = instr.EffectiveOpCode();
                if (opcode == Opcode::DPH || opcode == Opcode::DPHI)
                    src1[3] = f24::One();

                const int num_components = (opcode == Opcode::DP3) ? 3 : 4;
                // std::inner_product(src1, src1 + n, src2, f24::Zero()): init = init + a * b
                f24 dot = f24::Zero();
                for (int i = 0; i < num_components; ++i) {
                    dot = dot + src1[i] * src2[i];
                }

                for (int i = 0; i < 4; ++i) {
                    if (!swizzle.DestComponentEnabled(i))
                        continue;

                    dest[i] = dot;
                }
                break;
            }

            // Reciprocal
            case Opcode::RCP: {
                f24 rcp_res = f24::FromFloat32(1.0f / src1[0].ToFloat32());
                for (int i = 0; i < 4; ++i) {
                    if (!swizzle.DestComponentEnabled(i))
                        continue;

                    dest[i] = rcp_res;
                }
                break;
            }

            // Reciprocal Square Root
            case Opcode::RSQ: {
                f24 rsq_res = f24::FromFloat32(1.0f / std::sqrt(src1[0].ToFloat32()));
                for (int i = 0; i < 4; ++i) {
                    if (!swizzle.DestComponentEnabled(i))
                        continue;

                    dest[i] = rsq_res;
                }
                break;
            }

            case Opcode::MOVA: {
                for (int i = 0; i < 2; ++i) {
                    if (!swizzle.DestComponentEnabled(i))
                        continue;

                    // TODO (Azahar): Figure out how the rounding is done on hardware
                    state.address_registers[i] = FloatToS32(src1[i].ToFloat32());
                }
                break;
            }

            case Opcode::MOV: {
                for (int i = 0; i < 4; ++i) {
                    if (!swizzle.DestComponentEnabled(i))
                        continue;

                    dest[i] = src1[i];
                }
                break;
            }

            case Opcode::SGE:
            case Opcode::SGEI:
                for (int i = 0; i < 4; ++i) {
                    if (!swizzle.DestComponentEnabled(i))
                        continue;

                    dest[i] = (src1[i] >= src2[i]) ? f24::One() : f24::Zero();
                }
                break;

            case Opcode::SLT:
            case Opcode::SLTI:
                for (int i = 0; i < 4; ++i) {
                    if (!swizzle.DestComponentEnabled(i))
                        continue;

                    dest[i] = (src1[i] < src2[i]) ? f24::One() : f24::Zero();
                }
                break;

            case Opcode::CMP:
                for (int i = 0; i < 2; ++i) {
                    // TODO (Azahar): Can you restrict to one compare via dest masking?

                    const CompareOp op = (i == 0) ? instr.common_compare_x() : instr.common_compare_y();

                    switch (op) {
                    case CompareOp::Equal:
                        state.conditional_code[i] = (src1[i] == src2[i]);
                        break;

                    case CompareOp::NotEqual:
                        state.conditional_code[i] = (src1[i] != src2[i]);
                        break;

                    case CompareOp::LessThan:
                        state.conditional_code[i] = (src1[i] < src2[i]);
                        break;

                    case CompareOp::LessEqual:
                        state.conditional_code[i] = (src1[i] <= src2[i]);
                        break;

                    case CompareOp::GreaterThan:
                        state.conditional_code[i] = (src1[i] > src2[i]);
                        break;

                    case CompareOp::GreaterEqual:
                        state.conditional_code[i] = (src1[i] >= src2[i]);
                        break;

                    default:
                        // Deviation: Azahar logs "Unknown compare mode" and leaves the code as it
                        // is; the draw is dropped here.
                        error = {Unhandled::CompareMode, old_program_counter, instr.hex};
                        return false;
                    }
                }
                break;

            case Opcode::EX2: {
                // EX2 only takes first component exp2 and writes it to all dest components
                f24 ex2_res = f24::FromFloat32(std::exp2(src1[0].ToFloat32()));
                for (int i = 0; i < 4; ++i) {
                    if (!swizzle.DestComponentEnabled(i))
                        continue;

                    dest[i] = ex2_res;
                }
                break;
            }

            case Opcode::LG2: {
                // LG2 only takes the first component log2 and writes it to all dest components
                f24 lg2_res = f24::FromFloat32(std::log2(src1[0].ToFloat32()));
                for (int i = 0; i < 4; ++i) {
                    if (!swizzle.DestComponentEnabled(i))
                        continue;

                    dest[i] = lg2_res;
                }
                break;
            }

            default:
                // Azahar: LOG_ERROR "Unhandled arithmetic instruction" (DST, DSTI and LIT have no
                // handler in its interpreter) and goes on. Deviation: the draw is dropped.
                error = {Unhandled::Instruction, old_program_counter, instr.hex};
                return false;
            }

            break;
        }

        case Type::MultiplyAdd: {
            if ((instr.EffectiveOpCode() == Opcode::MAD) || (instr.EffectiveOpCode() == Opcode::MADI)) {
                const SwizzlePattern mad_swizzle = {swizzle_data[instr.mad_operand_desc_id()]};

                bool is_inverted = (instr.EffectiveOpCode() == Opcode::MADI);

                const f24* src1_ = LookupSourceRegister(instr.mad_src1(is_inverted), 0);
                const f24* src2_ = LookupSourceRegister(
                    instr.mad_src2(is_inverted),
                    !is_inverted * static_cast<int>(instr.mad_address_register_index()));
                const f24* src3_ = LookupSourceRegister(
                    instr.mad_src3(is_inverted),
                    is_inverted * static_cast<int>(instr.mad_address_register_index()));

                const bool negate_src1 = mad_swizzle.negate_src1();
                const bool negate_src2 = mad_swizzle.negate_src2();
                const bool negate_src3 = mad_swizzle.negate_src3();

                f24 src1[4] = {
                    src1_[mad_swizzle.selector_src1(0)],
                    src1_[mad_swizzle.selector_src1(1)],
                    src1_[mad_swizzle.selector_src1(2)],
                    src1_[mad_swizzle.selector_src1(3)],
                };
                if (negate_src1) {
                    src1[0] = -src1[0];
                    src1[1] = -src1[1];
                    src1[2] = -src1[2];
                    src1[3] = -src1[3];
                }
                f24 src2[4] = {
                    src2_[mad_swizzle.selector_src2(0)],
                    src2_[mad_swizzle.selector_src2(1)],
                    src2_[mad_swizzle.selector_src2(2)],
                    src2_[mad_swizzle.selector_src2(3)],
                };
                if (negate_src2) {
                    src2[0] = -src2[0];
                    src2[1] = -src2[1];
                    src2[2] = -src2[2];
                    src2[3] = -src2[3];
                }
                f24 src3[4] = {
                    src3_[mad_swizzle.selector_src3(0)],
                    src3_[mad_swizzle.selector_src3(1)],
                    src3_[mad_swizzle.selector_src3(2)],
                    src3_[mad_swizzle.selector_src3(3)],
                };
                if (negate_src3) {
                    src3[0] = -src3[0];
                    src3[1] = -src3[1];
                    src3[2] = -src3[2];
                    src3[3] = -src3[3];
                }

                f24* dest = instr.mad_dest().IsOutput()
                                ? &state.output[state.output_bank][static_cast<size_t>(instr.mad_dest().GetIndex())].c[0]
                                : &state.temporary[static_cast<size_t>(instr.mad_dest().GetIndex())].c[0];

                for (int i = 0; i < 4; ++i) {
                    if (!mad_swizzle.DestComponentEnabled(i))
                        continue;

                    dest[i] = src1[i] * src2[i] + src3[i];
                }
            } else {
                error = {Unhandled::Instruction, old_program_counter, instr.hex};
                return false;
            }
            break;
        }

        default: {
            // Handle each instruction on its own
            switch (static_cast<Opcode>(instr.opcode())) {
            case Opcode::END:
                should_stop = true;
                break;

            case Opcode::JMPC:
                if (evaluate_condition(instr)) {
                    program_counter = instr.flow_dest_offset() - 1;
                }
                break;

            case Opcode::JMPU:
                if (uniforms.b[instr.flow_bool_uniform_id()] == !(instr.flow_num_instructions() & 1)) {
                    program_counter = instr.flow_dest_offset() - 1;
                }
                break;

            case Opcode::CALL:
                do_call(instr);
                break;

            case Opcode::CALLU:
                if (uniforms.b[instr.flow_bool_uniform_id()]) {
                    do_call(instr);
                }
                break;

            case Opcode::CALLC:
                if (evaluate_condition(instr)) {
                    do_call(instr);
                }
                break;

            case Opcode::NOP:
                break;

            case Opcode::IFU: {
                const bool cond = uniforms.b[instr.flow_bool_uniform_id()];
                do_if(instr, cond);
                break;
            }

            case Opcode::IFC: {
                // TODO (Azahar): Do we need to consider swizzlers here?
                const bool cond = evaluate_condition(instr);
                do_if(instr, cond);
                break;
            }

            case Opcode::LOOP: {
                const std::array<u8, 4>& loop_param = uniforms.i[instr.flow_int_uniform_id()];
                state.address_registers[2] = loop_param[1];

                // Quirk kept from Azahar: aL has just been assigned, so do_loop's `previous_aL`
                // is this loop's own start value, not the value of a surrounding loop.
                do_loop(instr, loop_param);
                break;
            }

            case Opcode::BREAK: {
                is_break = true;
                break;
            }

            case Opcode::BREAKC: {
                if (evaluate_condition(instr)) {
                    is_break = true;
                }
                break;
            }

            case Opcode::EMIT: {
                auto* emitter = state.emitter_ptr;
                if (emitter == nullptr) {
                    // Azahar: ASSERT_MSG(emitter, "execute EMIT on VS")
                    error = {Unhandled::EmitOnVertexShader, old_program_counter, instr.hex};
                    return false;
                }
                if (emitter->emit_state.vertex_id >= 3) {
                    // Azahar: ASSERT(emit_state.vertex_id < 3) in GeometryEmitter::Emit
                    error = {Unhandled::EmitVertexId, old_program_counter, instr.hex};
                    return false;
                }
                emitter->Emit(state.output[state.output_bank]);
                state.output_bank = !state.output_bank;
                break;
            }

            case Opcode::SETEMIT: {
                auto* emitter = state.emitter_ptr;
                if (emitter == nullptr) {
                    // Azahar: ASSERT_MSG(emitter, "execute SETEMIT on VS")
                    error = {Unhandled::EmitOnVertexShader, old_program_counter, instr.hex};
                    return false;
                }
                emitter->emit_state.vertex_id = static_cast<u8>(instr.setemit_vertex_id());
                emitter->emit_state.prim_emit = instr.setemit_prim_emit();
                emitter->emit_state.winding = instr.setemit_winding();
                break;
            }

            default:
                // Azahar: LOG_ERROR "Unhandled instruction" and goes on. Deviation: the draw is dropped.
                error = {Unhandled::Instruction, old_program_counter, instr.hex};
                return false;
            }

            break;
        }
        }

        ++program_counter;
        ++iteration;

        // Stacks are checked in the order CALL -> IF -> LOOP. The CALL stack can be popped multiple
        // times per instruction. A JMP at the end of a scope is never taken, this is why we compare
        // against old_program_counter + 1 here.
        u32 next_program_counter = old_program_counter + 1;
        for (u32 i = 0; i < 4; i++) {
            if (call_stack.empty() || call_stack.back().end_address != next_program_counter)
                break;
            // Hardware bug: when popping four CALL scopes at once, the last one doesn't update the
            // program counter
            if (i < 3) {
                program_counter = call_stack.back().return_address;
                next_program_counter = program_counter;
            }
            call_stack.pop_back();
        }

        // The other two stacks can only pop one entry per instruction. They are checked against the
        // original program counter before any CALL scopes were closed and they overwrite any
        // previous program counter updates.
        if (!if_stack.empty() && if_stack.back().else_address == old_program_counter + 1) {
            program_counter = if_stack.back().end_address;
            if_stack.pop_back();
        }

        if (!loop_stack.empty() &&
            (loop_stack.back().end_address == old_program_counter + 1 || is_break)) {
            auto& loop = loop_stack.back();
            state.address_registers[2] += loop.address_increment;
            if (!is_break && loop.loop_downcounter--) {
                program_counter = loop.entry_address;
            } else {
                program_counter = loop.end_address;
                // Only restore previous value if there is a surrounding LOOP scope.
                if (loop_stack.size() > 1)
                    state.address_registers[2] = loop.previous_aL;
                loop_stack.pop_back();
            }
        }
    }
    return true;
}

} // namespace mh4n::gpu::cpuvertex
