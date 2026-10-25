// Derived from Azahar src/video_core/shader/generator/glsl_shader_decompiler.cpp, glsl_shader_decompiler.h,
// src/video_core/shader/shader_interpreter.cpp (the reference semantics of every instruction and of the
// loop, call and if stacks) and src/video_core/pica_types.h (the multiplication rule) at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; each of those files says "Licensed under
// GPLv2 or any later version").
// Copyright 2017-2025 Citra Emulator Project / Azahar Emulator Project (glsl_shader_decompiler.cpp)
// Copyright 2017 Citra Emulator Project (glsl_shader_decompiler.h)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (shader_interpreter.cpp)
// Copyright 2015-2026 Citra Emulator Project / Azahar Emulator Project (pica_types.h)
//
// The PICA program to GLSL translator (N2-06). A port of Azahar's decompiler: the control-flow
// analysis into subroutines, and one GLSL function per subroutine; what is different, and why:
//
//  * Vulkan GLSL (`#version 450`, glslang with Vulkan rules), not GL or either: nothing here depends
//    on the dialect except the uniform block members the caller declares as `uint`.
//  * The multiplication rule of the PICA is always on (Azahar's `sanitize_mul` true): MUL, DP3, DP4,
//    DPH, MAD and the first product of DST give +0 for 0 x +-inf and keep NaN when a factor is NaN;
//    MAX and MIN order as the hardware (`src1 > src2 ? src1 : src2`, `src1 < src2 ? src1 : src2`). The
//    NaN tests and the selections are done on the bits (see the prelude), because a compiler that
//    assumes there is no NaN is allowed to fold `isnan` and to turn `a > b ? a : b` into a fmax.
//  * RCP and RSQ are rounded as the interpreter computes them (1.0f / x and 1.0f / sqrtf(x), each step to
//    nearest), whatever the device's own reciprocal and inverse square root give (see the prelude): the
//    RX 6700 XT's are off by one ulp in about one input in nine, and a program that subtracts nearly equal
//    numbers after a normalization turns that into more than the 2^-16 of the comparison.
//  * BREAK and BREAKC are translated (Azahar's decompiler rejects the program): the loop body is a
//    function, so a break sets a flag that the callers return through, and the loop checks it.
//  * LOOP follows the interpreter where Azahar's decompiler differs: a loop also adds its increment to aL
//    after a break, and a loop that ends inside another loop sets aL to its own initial value y (the
//    interpreter's previous_aL; Azahar's decompiler leaves aL as the inner loop left it).
//  * A subroutine that contains JMPC / JMPU dispatches its labels in a loop bounded by
//    kMaxJumpIterations, so a backward jump that never ends ends the shader instead of the device.
//  * Code the program does not hold is not executed: a program whose reachable code runs past the
//    words that were loaded is rejected (Azahar's interpreter would run the zero words to the last
//    word of the buffer).
//  * DST, DSTI and LIT are translated (vs_3_0 definitions, nihstro's comment on DST says it is the
//    same as vs_3_0's). Azahar implements none of them: its decompiler rejects the program and its
//    interpreter and JIT skip the instruction. Not verified on hardware: DecompileResult says so.
//  * Where the interpreter and the structured readings disagree (DecompileResult::differs_from_interpreter,
//    a HINT: it flags the shapes known to differ, with an over-approximation, so it may flag a program on
//    which the two agree; a mismatch with the interpreter in a flagged program is first read as the
//    interpreter's quirk, a mismatch in an unflagged program is a translator bug until shown otherwise).
//    The interpreter ends a scope by comparing the program counter with `old_program_counter + 1` after
//    each instruction (shader_interpreter.cpp: the call-stack check against next_program_counter, then the
//    if-stack check, which pops ONE entry and overwrites the program counter, then the loop-stack check
//    against old_program_counter + 1, again one entry). A jump over the else branch, or over the rest of a
//    scope, therefore lands where the next scope's test does not fire. The shapes: (1) a LOOP or CALL body
//    that ends with an IF/ELSE: after the then-branch the interpreter jumps to the IF's end (the scope's
//    end) without testing the loop, so the loop does not iterate again (the loop stack keeps a stale entry);
//    the else branch ends at the loop end and iterates; (2) a LOOP or CALL body that ends with an IF WITHOUT
//    else: when the condition is false the jump goes to the scope's end and the end test (against the IF's
//    own address + 1) does not fire: the loop does not iterate, the call does not return; (3) an IF, with or
//    without else, at the tail of the THEN-branch of an IF that has an else: the inner pop leaves the outer
//    entry, the program counter lands on the outer else address and the else branch runs as well; (4) two
//    loops that end on the same instruction: one loop is repeated or popped per instruction, so the outer
//    loop does not iterate; (5) a CALL or a jump that is the last instruction of an IF, CALL or LOOP scope: the
//    scope-end handling overwrites the program counter it set, so it is never taken. Azahar's x64 JIT
//    (Compile_LOOP and the structural IF/CALL) and its decompiler follow the structure and so does this
//    translation; the port of the interpreter in the tests is the same code as Azahar's here, so the oracle
//    shows the interpreter's behaviour, and gpu_shader_test.cpp pins both sides. Which one the hardware
//    does is not known. Not flagged on purpose: an IF or IF/ELSE in the middle of a body, an IF/ELSE at the
//    tail of the ELSE branch of an outer IF, nested loops whose inner loop is not the last statement.
//  * EMIT and SETEMIT generate nothing, as in Azahar's decompiler (it logs "Geometry shader operation
//    detected in vertex shader"): the vertex program of a draw with a geometry shader is an ordinary
//    vertex program; a program that does use them as a geometry shader would is not translated by this
//    module (the interpreter, run on a vertex unit with no emitter, does nothing for them either).
//  * Temporaries start as (0, 0, 0, 1), as in Azahar's generated shader (the interpreter keeps the
//    previous vertex's values).
#include "gpu/shader/glsl_decompiler.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

#include <fmt/format.h>

#include "gpu/shader/pica_isa.h"

namespace mh4n::gpu::shader {

namespace {

using isa::Instruction;
using isa::Opcode;
using isa::SourceKind;
using isa::SourceRegister;
using isa::Swizzle;

constexpr u32 PROGRAM_END = kMaxProgramWords;

// A rejection: the message for people, and a category and a word offset that name the cause without
// quoting the program (the dump check prints only those two for a real program).
class DecompileFail : public std::runtime_error {
public:
    explicit DecompileFail(const std::string& message, std::string category_ = "other", int word_ = -1)
        : std::runtime_error(message), category(std::move(category_)), word(word_) {}
    std::string category;
    int word;
};

/// Describes the behaviour of code path of a given entry point and a return point.
enum class ExitMethod {
    Undetermined, ///< Internal value. Only occur when analyzing JMP loop.
    AlwaysReturn, ///< All code paths reach the return point.
    Conditional,  ///< Code path reaches the return point or an END instruction conditionally.
    AlwaysEnd,    ///< All code paths reach a END instruction.
};

/// A subroutine is a range of code refereced by a CALL, IF or LOOP instruction.
struct Subroutine {
    Subroutine(u32 b, u32 e) : begin(b), end(e), exit_method(ExitMethod::Undetermined) {}

    /// Generates a name suitable for GLSL source code.
    std::string GetName() const {
        return "sub_" + std::to_string(begin) + "_" + std::to_string(end);
    }

    u32 begin;              ///< Entry point of the subroutine.
    u32 end;                ///< Return point of the subroutine.
    ExitMethod exit_method; ///< Exit method of the subroutine.
    std::set<u32> labels;   ///< Addresses refereced by JMP instructions.
    bool has_break = false; ///< A BREAK or BREAKC can run in it, outside any loop it contains.

    bool operator<(const Subroutine& rhs) const {
        return std::tie(begin, end) < std::tie(rhs.begin, rhs.end);
    }
};

// The program as the analysis and the generator read it. A word that was never loaded is not code.
class Program {
public:
    explicit Program(const DecompileInput& input) : in_(input) {}

    Instruction Fetch(u32 offset) const {
        if (offset >= in_.program.size()) {
            throw DecompileFail(fmt::format(
                "the code runs to word {} but only {} words are loaded (a jump, call or the end of the "
                "program leaves the loaded code)",
                offset, in_.program.size()),
                "code-outside-loaded-words", static_cast<int>(offset));
        }
        return Instruction{in_.program[offset]};
    }
    Swizzle SwizzleOf(u32 id) const {
        return Swizzle{id < in_.swizzle.size() ? in_.swizzle[id] : 0u};
    }

private:
    const DecompileInput& in_;
};

// The addresses at which a scope of the program ends: for the shapes in which the interpreter and the
// structured readings (this translation, Azahar's decompiler and x64 JIT) disagree. `any` holds the end
// of every CALL, LOOP and IF scope and the else address of every IF; `call_loop` the ends of CALL and LOOP
// scopes only.
struct ScopeEnds {
    std::set<u32> any;
    std::set<u32> call_loop;
    std::map<u32, std::set<u32>> loops_by_end; // LOOP instructions by the end address of their scope
    std::set<u32> else_addresses;              // the else address of every IF that has an else branch
};

/// Analyzes shader code and produces a set of subroutines.
class ControlFlowAnalyzer {
public:
    ControlFlowAnalyzer(const Program& program, u32 main_offset) : program_(program) {
        // Recursively finds all subroutines.
        const Subroutine& program_main = AddSubroutine(main_offset, PROGRAM_END, true);
        if (program_main.exit_method != ExitMethod::AlwaysEnd)
            throw DecompileFail("Program does not always end", "program-does-not-end");
    }

    std::set<Subroutine> MoveSubroutines() {
        return std::move(subroutines);
    }
    const ScopeEnds& Scopes() const {
        return scope_ends;
    }

private:
    const Program& program_;
    std::set<Subroutine> subroutines;
    ScopeEnds scope_ends;
    std::map<std::pair<u32, u32>, ExitMethod> exit_method_map;
    std::map<std::pair<u32, u32>, bool> break_map;
    std::set<std::pair<u32, u32>> break_visiting;

    /// Adds and analyzes a new subroutine if it is not added yet.
    const Subroutine& AddSubroutine(u32 begin, u32 end, bool is_main = false) {
        auto iter = subroutines.find(Subroutine(begin, end));
        if (iter != subroutines.end())
            return *iter;

        if (subroutines.size() >= 4096) {
            throw DecompileFail("Too many subroutines", "too-large");
        }
        Subroutine subroutine(begin, end);
        subroutine.exit_method = Scan(begin, end, subroutine.labels);
        if (subroutine.exit_method == ExitMethod::Undetermined)
            throw DecompileFail("Recursive function detected", "recursion");
        subroutine.has_break = HasBreak(begin, end);
        if (!is_main && subroutine.has_break && subroutine.exit_method == ExitMethod::AlwaysEnd) {
            // Every path ends, but one of them breaks out of the enclosing loop first (the break returns
            // from the function): the caller must go on after the loop, so the exit is conditional.
            subroutine.exit_method = ExitMethod::Conditional;
        }
        return *subroutines.insert(std::move(subroutine)).first;
    }

    /// Whether a BREAK / BREAKC can run in the instructions of [begin, end), following the structure
    /// (an IF / CALL body counts, a LOOP body does not: the loop takes the break).
    bool HasBreak(u32 begin, u32 end) {
        const std::pair<u32, u32> key{begin, end};
        if (auto it = break_map.find(key); it != break_map.end()) {
            return it->second;
        }
        if (!break_visiting.insert(key).second) {
            return false;
        }
        bool result = false;
        for (u32 offset = begin; offset != end && offset < PROGRAM_END && !result;) {
            const Instruction instr = program_.Fetch(offset);
            u32 next = offset + 1;
            switch (static_cast<Opcode>(instr.opcode())) {
            case Opcode::END:
                next = end; // the rest is unreachable
                break;
            case Opcode::BREAK:
            case Opcode::BREAKC:
                result = true;
                break;
            case Opcode::CALL:
            case Opcode::CALLC:
            case Opcode::CALLU:
                result = HasBreak(instr.dest_offset(), instr.dest_offset() + instr.num_instructions());
                break;
            case Opcode::IFU:
            case Opcode::IFC: {
                result = HasBreak(offset + 1, instr.dest_offset());
                if (!result && instr.num_instructions() != 0) {
                    result = HasBreak(instr.dest_offset(), instr.dest_offset() + instr.num_instructions());
                }
                next = instr.dest_offset() + instr.num_instructions();
                break;
            }
            case Opcode::LOOP:
                next = instr.dest_offset() + 1;
                break;
            default:
                break;
            }
            if (next <= offset) {
                break; // a malformed backward range: do not loop
            }
            offset = next;
        }
        break_visiting.erase(key);
        break_map[key] = result;
        return result;
    }

    void NoteScope(u32 end, bool call_or_loop) {
        scope_ends.any.insert(end);
        if (call_or_loop) {
            scope_ends.call_loop.insert(end);
        }
    }

    /// Merges exit method of two parallel branches.
    static ExitMethod ParallelExit(ExitMethod a, ExitMethod b) {
        if (a == ExitMethod::Undetermined) {
            return b;
        }
        if (b == ExitMethod::Undetermined) {
            return a;
        }
        if (a == b) {
            return a;
        }
        return ExitMethod::Conditional;
    }

    /// Cascades exit method of two blocks of code.
    static ExitMethod SeriesExit(ExitMethod a, ExitMethod b) {
        // This should be handled before evaluating b.
        if (a == ExitMethod::Undetermined) {
            return ExitMethod::Undetermined;
        }

        if (a == ExitMethod::AlwaysReturn) {
            return b;
        }

        if (b == ExitMethod::Undetermined || b == ExitMethod::AlwaysEnd) {
            return ExitMethod::AlwaysEnd;
        }

        return ExitMethod::Conditional;
    }

    /// Scans a range of code for labels and determines the exit method.
    ExitMethod Scan(u32 begin, u32 end, std::set<u32>& labels) {
        auto [iter, inserted] =
            exit_method_map.emplace(std::make_pair(begin, end), ExitMethod::Undetermined);
        ExitMethod& exit_method = iter->second;
        if (!inserted)
            return exit_method;

        for (u32 offset = begin; offset != end && offset != PROGRAM_END; ++offset) {
            const Instruction instr = program_.Fetch(offset);
            switch (instr.opcode()) {
            case static_cast<u32>(Opcode::END): {
                return exit_method = ExitMethod::AlwaysEnd;
            }
            case static_cast<u32>(Opcode::JMPC):
            case static_cast<u32>(Opcode::JMPU): {
                labels.insert(instr.dest_offset());
                ExitMethod no_jmp = Scan(offset + 1, end, labels);
                ExitMethod jmp = Scan(instr.dest_offset(), end, labels);
                return exit_method = ParallelExit(no_jmp, jmp);
            }
            case static_cast<u32>(Opcode::CALL): {
                NoteScope(instr.dest_offset() + instr.num_instructions(), true);
                auto& call = AddSubroutine(instr.dest_offset(),
                                           instr.dest_offset() + instr.num_instructions());
                if (call.exit_method == ExitMethod::AlwaysEnd)
                    return exit_method = ExitMethod::AlwaysEnd;
                ExitMethod after_call = Scan(offset + 1, end, labels);
                return exit_method = SeriesExit(call.exit_method, after_call);
            }
            case static_cast<u32>(Opcode::LOOP): {
                NoteScope(instr.dest_offset() + 1, true);
                scope_ends.loops_by_end[instr.dest_offset() + 1].insert(offset);
                auto& loop = AddSubroutine(offset + 1, instr.dest_offset() + 1);
                if (loop.exit_method == ExitMethod::AlwaysEnd)
                    return exit_method = ExitMethod::AlwaysEnd;
                ExitMethod after_loop = Scan(instr.dest_offset() + 1, end, labels);
                return exit_method = SeriesExit(loop.exit_method, after_loop);
            }
            case static_cast<u32>(Opcode::CALLC):
            case static_cast<u32>(Opcode::CALLU): {
                NoteScope(instr.dest_offset() + instr.num_instructions(), true);
                auto& call = AddSubroutine(instr.dest_offset(),
                                           instr.dest_offset() + instr.num_instructions());
                ExitMethod after_call = Scan(offset + 1, end, labels);
                return exit_method = SeriesExit(
                           ParallelExit(call.exit_method, ExitMethod::AlwaysReturn), after_call);
            }
            case static_cast<u32>(Opcode::IFU):
            case static_cast<u32>(Opcode::IFC): {
                NoteScope(instr.dest_offset(), false);
                NoteScope(instr.dest_offset() + instr.num_instructions(), false);
                if (instr.num_instructions() != 0) {
                    scope_ends.else_addresses.insert(instr.dest_offset());
                }
                auto& if_sub = AddSubroutine(offset + 1, instr.dest_offset());
                ExitMethod else_method;
                if (instr.num_instructions() != 0) {
                    auto& else_sub = AddSubroutine(instr.dest_offset(),
                                                   instr.dest_offset() + instr.num_instructions());
                    else_method = else_sub.exit_method;
                } else {
                    else_method = ExitMethod::AlwaysReturn;
                }

                ExitMethod both = ParallelExit(if_sub.exit_method, else_method);
                if (both == ExitMethod::AlwaysEnd)
                    return exit_method = ExitMethod::AlwaysEnd;
                ExitMethod after_call =
                    Scan(instr.dest_offset() + instr.num_instructions(), end, labels);
                return exit_method = SeriesExit(both, after_call);
            }
            default:
                break;
            }
        }
        return exit_method = ExitMethod::AlwaysReturn;
    }
};

class ShaderWriter {
public:
    // Forwards all arguments directly to libfmt.
    // Note that all formatting requirements for fmt must be
    // obeyed when using this function. (e.g. {{ must be used
    // printing the character '{' is desirable. Ditto for }} and '}',
    // etc).
    template <typename... Args>
    void AddLine(fmt::format_string<Args...> text, Args&&... args) {
        AddExpression(fmt::format(text, std::forward<Args>(args)...));
        AddNewLine();
    }

    void AddNewLine() {
        shader_source += '\n';
    }

    std::string MoveResult() {
        return std::move(shader_source);
    }

    int scope = 0;

private:
    void AddExpression(std::string_view text) {
        if (!text.empty()) {
            shader_source.append(static_cast<std::size_t>(scope) * 4, ' ');
        }
        shader_source += text;
    }

    std::string shader_source;
};

/// The four lanes of source `source` (1-3) as a swizzle string.
std::string GetSelectorSrc(const Swizzle& pattern, u32 source) {
    std::string out;
    for (u32 i = 0; i < 4; ++i) {
        out += "xyzw"[pattern.selector(source, i)];
    }
    return out;
}

class GLSLGenerator {
public:
    GLSLGenerator(const std::set<Subroutine>& subroutines, const ScopeEnds& scope_ends, const Program& program,
                  u32 main_offset, const RegGetter& inputreg_getter, const RegGetter& outputreg_getter,
                  DecompileResult& result)
        : subroutines(subroutines), scope_ends(scope_ends), program(program), main_offset(main_offset),
          inputreg_getter(inputreg_getter), outputreg_getter(outputreg_getter), result(result) {

        Generate();
    }

    std::string MoveShaderCode() {
        return shader.MoveResult();
    }

private:
    /// Gets the Subroutine object corresponding to the specified address.
    const Subroutine& GetSubroutine(u32 begin, u32 end) const {
        auto iter = subroutines.find(Subroutine(begin, end));
        if (iter == subroutines.end()) {
            throw DecompileFail(fmt::format("Internal error: no subroutine {}..{}", begin, end));
        }
        return *iter;
    }

    /// Generates condition evaluation code for the flow control instruction.
    static std::string EvaluateCondition(const Instruction& instr) {
        using Op = isa::ConditionOp;

        const std::string_view result_x = instr.refx() ? "conditional_code.x" : "!conditional_code.x";
        const std::string_view result_y = instr.refy() ? "conditional_code.y" : "!conditional_code.y";

        switch (instr.condition_op()) {
        case Op::JustX:
            return std::string(result_x);
        case Op::JustY:
            return std::string(result_y);
        case Op::Or:
        case Op::And: {
            const std::string_view and_or = instr.condition_op() == Op::Or ? "any" : "all";
            std::string bvec;
            if (instr.refx() && instr.refy()) {
                bvec = "conditional_code";
            } else if (!instr.refx() && !instr.refy()) {
                bvec = "not(conditional_code)";
            } else {
                bvec = fmt::format("bvec2({}, {})", result_x, result_y);
            }
            return fmt::format("{}({})", and_or, bvec);
        }
        }
        return "false";
    }

    /// Generates code representing a source register.
    std::string GetSourceRegister(const SourceRegister& source_reg,
                                  u32 address_register_index) const {
        const u32 index = source_reg.index();

        switch (source_reg.kind()) {
        case SourceKind::Input:
            return inputreg_getter(index);
        case SourceKind::Temporary:
            return fmt::format("reg_tmp{}", index);
        case SourceKind::FloatUniform:
            if (address_register_index != 0) {
                return fmt::format("get_offset_register({}, address_registers.{})", index,
                                   "xyz"[address_register_index - 1]);
            }
            return fmt::format("uniforms.f[{}]", index);
        }
        return "vec4(0.0)";
    }

    /// Generates code representing a destination register; "" for one that names nothing.
    std::string GetDestRegister(const isa::DestRegister& dest_reg) const {
        if (dest_reg.is_output()) {
            return outputreg_getter(dest_reg.index());
        }
        if (dest_reg.is_temporary()) {
            return fmt::format("reg_tmp{}", dest_reg.index());
        }
        return "";
    }

    /// Generates code representing a bool uniform
    std::string GetUniformBool(u32 index, bool invert_test = false) const {
        return fmt::format("(uniforms.b & {}u) {} 0u", 1 << index, invert_test ? "==" : "!=");
    }

    /**
     * Adds code that calls a subroutine.
     * @param subroutine the subroutine to call.
     */
    void CallSubroutine(const Subroutine& subroutine, bool propagate_break = true) {
        calls[current_sub].insert(subroutine.GetName());
        if (subroutine.exit_method == ExitMethod::AlwaysEnd) {
            shader.AddLine("{}();", subroutine.GetName());
            shader.AddLine("return true;");
        } else if (subroutine.exit_method == ExitMethod::Conditional) {
            shader.AddLine("if ({}()) {{ return true; }}", subroutine.GetName());
        } else {
            shader.AddLine("{}();", subroutine.GetName());
        }
        if (subroutine.has_break && propagate_break) {
            // A BREAK / BREAKC ran in it: the callers return until the loop that takes it.
            shader.AddLine("if (loop_break) {{ return false; }}");
        }
    }

    /**
     * Writes code that does an assignment operation.
     * @param swizzle the swizzle data of the current instruction.
     * @param reg the destination register code.
     * @param value the code representing the value to assign.
     * @param dest_num_components number of components of the destination register.
     * @param value_num_components number of components of the value to assign.
     */
    void SetDest(const Swizzle& swizzle, std::string_view reg, std::string_view value,
                 u32 dest_num_components, u32 value_num_components) {
        u32 dest_mask_num_components = 0;
        std::string dest_mask_swizzle = ".";

        for (u32 i = 0; i < dest_num_components; ++i) {
            if (swizzle.DestEnabled(i)) {
                dest_mask_swizzle += "xyzw"[i];
                ++dest_mask_num_components;
            }
        }

        if (reg.empty() || dest_mask_num_components == 0) {
            return;
        }

        const std::string dest =
            fmt::format("{}{}", reg, dest_num_components != 1 ? dest_mask_swizzle : "");

        std::string src{value};
        if (value_num_components == 1) {
            if (dest_mask_num_components != 1) {
                src = fmt::format("vec{}({})", dest_mask_num_components, value);
            }
        } else if (value_num_components != dest_mask_num_components) {
            src = fmt::format("({}){}", value, dest_mask_swizzle);
        }

        shader.AddLine("{} = {};", dest, src);
    }

    // A CALL or a jump that is the last instruction of a scope: the interpreter lets the end of the scope
    // override the program counter that the instruction set, so it is never taken there.
    void NoteIfLastOfScope(u32 offset) {
        if (scope_ends.any.count(offset + 1) != 0) {
            differs_from_interpreter = true;
        }
    }

    /// The comparison operator of a CMP half, or "" for the two undefined ones.
    static std::pair<std::string_view, std::string_view> CompareOperator(isa::CompareOp op) {
        switch (op) {
        case isa::CompareOp::Equal:
            return {"==", "equal"};
        case isa::CompareOp::NotEqual:
            return {"!=", "notEqual"};
        case isa::CompareOp::LessThan:
            return {"<", "lessThan"};
        case isa::CompareOp::LessEqual:
            return {"<=", "lessThanEqual"};
        case isa::CompareOp::GreaterThan:
            return {">", "greaterThan"};
        case isa::CompareOp::GreaterEqual:
            return {">=", "greaterThanEqual"};
        default:
            return {"", ""};
        }
    }

    /**
     * Compiles a single instruction from PICA to GLSL.
     * @param offset the offset of the PICA shader instruction.
     * @return the offset of the next instruction to execute. Usually it is the current offset + 1.
     * If the current instruction is IF or LOOP, the next instruction is after the IF or LOOP block.
     * If the current instruction always terminates the program, returns PROGRAM_END.
     */
    u32 CompileInstr(u32 offset) {
        if (++compiled_instructions > kMaxCompiledInstructions) {
            throw DecompileFail("The program is too large to translate", "too-large");
        }
        const Instruction instr = program.Fetch(offset);
        const isa::OpInfo& info = instr.info();

        const Swizzle swizzle = program.SwizzleOf(
            info.format == isa::Format::MultiplyAdd ? instr.mad_operand_desc_id() : instr.operand_desc_id());

        shader.AddLine("// {}: {}", offset, info.name);

        switch (info.format) {
        case isa::Format::Arithmetic: {
            const bool is_inverted = info.src_inversed;

            std::string src1 = swizzle.negate(1) ? "-" : "";
            src1 += GetSourceRegister(instr.src1(is_inverted),
                                      !is_inverted * instr.address_register_index());
            src1 += "." + GetSelectorSrc(swizzle, 1);

            // The single-source instructions do not read the second field (it holds whatever): the
            // register it names is not an input of the program.
            std::string src2;
            switch (instr.Effective()) {
            case Opcode::FLR:
            case Opcode::RCP:
            case Opcode::RSQ:
            case Opcode::EX2:
            case Opcode::LG2:
            case Opcode::LIT:
            case Opcode::MOV:
            case Opcode::MOVA:
                break;
            default:
                src2 = swizzle.negate(2) ? "-" : "";
                src2 += GetSourceRegister(instr.src2(is_inverted), is_inverted * instr.address_register_index());
                src2 += "." + GetSelectorSrc(swizzle, 2);
                break;
            }

            std::string dest_reg = GetDestRegister(instr.dest());

            switch (instr.Effective()) {
            case Opcode::ADD: {
                SetDest(swizzle, dest_reg, fmt::format("{} + {}", src1, src2), 4, 4);
                break;
            }

            case Opcode::MUL: {
                SetDest(swizzle, dest_reg, fmt::format("sanitize_mul({}, {})", src1, src2), 4, 4);
                break;
            }

            case Opcode::FLR: {
                SetDest(swizzle, dest_reg, fmt::format("floor({})", src1), 4, 4);
                break;
            }

            case Opcode::MAX: {
                SetDest(swizzle, dest_reg, fmt::format("pica_max({}, {})", src1, src2), 4, 4);
                break;
            }

            case Opcode::MIN: {
                SetDest(swizzle, dest_reg, fmt::format("pica_min({}, {})", src1, src2), 4, 4);
                break;
            }

            case Opcode::DP3:
            case Opcode::DP4:
            case Opcode::DPH:
            case Opcode::DPHI: {
                Opcode opcode = instr.Effective();
                std::string dot;
                if (opcode == Opcode::DP3) {
                    dot = fmt::format("dot(vec3(sanitize_mul({}, {})), vec3(1.0))", src1, src2);
                } else {
                    const std::string src1_ = (opcode == Opcode::DPH || opcode == Opcode::DPHI)
                                                  ? fmt::format("vec4(({}).xyz, 1.0)", src1)
                                                  : std::move(src1);

                    dot = fmt::format("dot(sanitize_mul({}, {}), vec4(1.0))", src1_, src2);
                }

                SetDest(swizzle, dest_reg, dot, 4, 1);
                break;
            }

            case Opcode::DST:
            case Opcode::DSTI: {
                result.uses_unverified_instructions = true;
                SetDest(swizzle, dest_reg,
                        fmt::format("vec4(1.0, sanitize_mul({0}, {1}).y, ({0}).z, ({1}).w)", src1, src2), 4,
                        4);
                break;
            }

            case Opcode::LIT: {
                result.uses_unverified_instructions = true;
                SetDest(swizzle, dest_reg, fmt::format("pica_lit({})", src1), 4, 4);
                break;
            }

            case Opcode::RCP: {
                SetDest(swizzle, dest_reg, fmt::format("pica_rcp({}.x)", src1), 4, 1);
                break;
            }

            case Opcode::RSQ: {
                SetDest(swizzle, dest_reg, fmt::format("pica_rsq({}.x)", src1), 4, 1);
                break;
            }

            case Opcode::MOVA: {
                SetDest(swizzle, "address_registers", fmt::format("ivec2({})", src1), 2, 2);
                break;
            }

            case Opcode::MOV: {
                SetDest(swizzle, dest_reg, src1, 4, 4);
                break;
            }

            case Opcode::SGE:
            case Opcode::SGEI: {
                SetDest(swizzle, dest_reg,
                        fmt::format("vec4(greaterThanEqual({}, {}))", src1, src2), 4, 4);
                break;
            }

            case Opcode::SLT:
            case Opcode::SLTI: {
                SetDest(swizzle, dest_reg, fmt::format("vec4(lessThan({}, {}))", src1, src2), 4, 4);
                break;
            }

            case Opcode::CMP: {
                const auto op_x = CompareOperator(instr.compare_x());
                const auto op_y = CompareOperator(instr.compare_y());

                // An undefined compare mode leaves that half of the condition code as it was (the
                // interpreter logs it and does the same).
                if (instr.compare_x() == instr.compare_y() && !op_x.first.empty()) {
                    shader.AddLine("conditional_code = {}(vec2({}), vec2({}));", op_x.second, src1, src2);
                } else {
                    if (!op_x.first.empty()) {
                        shader.AddLine("conditional_code.x = {}.x {} {}.x;", src1, op_x.first, src2);
                    }
                    if (!op_y.first.empty()) {
                        shader.AddLine("conditional_code.y = {}.y {} {}.y;", src1, op_y.first, src2);
                    }
                }
                break;
            }

            case Opcode::EX2: {
                SetDest(swizzle, dest_reg, fmt::format("exp2({}.x)", src1), 4, 1);
                break;
            }

            case Opcode::LG2: {
                SetDest(swizzle, dest_reg, fmt::format("log2({}.x)", src1), 4, 1);
                break;
            }

            default: {
                throw DecompileFail(fmt::format("Unhandled instruction at word {} (arithmetic format)", offset),
                                    "unhandled-instruction", static_cast<int>(offset));
            }
            }

            break;
        }

        case isa::Format::MultiplyAdd: {
            if ((instr.Effective() == Opcode::MAD) || (instr.Effective() == Opcode::MADI)) {
                bool is_inverted = (instr.Effective() == Opcode::MADI);

                std::string src1 = swizzle.negate(1) ? "-" : "";
                src1 += GetSourceRegister(instr.mad_src1(is_inverted), 0);
                src1 += "." + GetSelectorSrc(swizzle, 1);

                std::string src2 = swizzle.negate(2) ? "-" : "";
                src2 += GetSourceRegister(instr.mad_src2(is_inverted),
                                          !is_inverted * instr.mad_address_register_index());
                src2 += "." + GetSelectorSrc(swizzle, 2);

                std::string src3 = swizzle.negate(3) ? "-" : "";
                src3 += GetSourceRegister(instr.mad_src3(is_inverted),
                                          is_inverted * instr.mad_address_register_index());
                src3 += "." + GetSelectorSrc(swizzle, 3);

                const std::string dest_reg = GetDestRegister(instr.mad_dest());

                SetDest(swizzle, dest_reg, fmt::format("sanitize_mul({}, {}) + {}", src1, src2, src3), 4,
                        4);
            } else {
                throw DecompileFail(fmt::format("Unhandled instruction at word {} (multiply-add format)", offset),
                                    "unhandled-instruction", static_cast<int>(offset));
            }
            break;
        }

        default: {
            switch (static_cast<Opcode>(instr.opcode())) {
            case Opcode::END: {
                shader.AddLine("return true;");
                offset = PROGRAM_END - 1;
                break;
            }

            case Opcode::BREAK:
            case Opcode::BREAKC: {
                std::string condition = "loop_depth > 0";
                if (static_cast<Opcode>(instr.opcode()) == Opcode::BREAKC) {
                    condition = fmt::format("loop_depth > 0 && ({})", EvaluateCondition(instr));
                }
                shader.AddLine("if ({}) {{", condition);
                ++shader.scope;
                shader.AddLine("loop_break = true;");
                shader.AddLine("return false;");
                --shader.scope;
                shader.AddLine("}}");
                break;
            }

            case Opcode::JMPC:
            case Opcode::JMPU: {
                NoteIfLastOfScope(offset);
                std::string condition;
                if (static_cast<Opcode>(instr.opcode()) == Opcode::JMPC) {
                    condition = EvaluateCondition(instr);
                } else {
                    const bool invert_test = instr.num_instructions() & 1;
                    condition = GetUniformBool(instr.bool_uniform_id(), invert_test);
                }
                if (instr.dest_offset() <= offset) {
                    backward_jump_offset = offset;
                }

                shader.AddLine("if ({}) {{", condition);
                ++shader.scope;
                shader.AddLine("{{ jmp_to = {}u; break; }}", instr.dest_offset());

                --shader.scope;
                shader.AddLine("}}");
                break;
            }

            case Opcode::CALL:
            case Opcode::CALLC:
            case Opcode::CALLU: {
                NoteIfLastOfScope(offset);
                const Opcode opcode = static_cast<Opcode>(instr.opcode());
                std::string condition;
                if (opcode == Opcode::CALLC) {
                    condition = EvaluateCondition(instr);
                } else if (opcode == Opcode::CALLU) {
                    condition = GetUniformBool(instr.bool_uniform_id());
                }

                if (condition.empty()) {
                    shader.AddLine("{{");
                } else {
                    shader.AddLine("if ({}) {{", condition);
                }
                ++shader.scope;

                auto& call_sub =
                    GetSubroutine(instr.dest_offset(), instr.dest_offset() + instr.num_instructions());

                CallSubroutine(call_sub);
                if (opcode == Opcode::CALL && call_sub.exit_method == ExitMethod::AlwaysEnd) {
                    offset = PROGRAM_END - 1;
                }

                --shader.scope;
                shader.AddLine("}}");
                break;
            }

            case Opcode::NOP: {
                break;
            }

            case Opcode::IFC:
            case Opcode::IFU: {
                std::string condition;
                if (static_cast<Opcode>(instr.opcode()) == Opcode::IFC) {
                    condition = EvaluateCondition(instr);
                } else {
                    condition = GetUniformBool(instr.bool_uniform_id());
                }

                const u32 if_offset = offset + 1;
                const u32 else_offset = instr.dest_offset();
                const u32 endif_offset = instr.dest_offset() + instr.num_instructions();
                // An IF (with or without else) that is the last statement of a loop or call body, or of the
                // THEN-branch of an IF that has an else: its end is the end of that scope. The interpreter
                // tests one scope end per instruction (see the header comment), and the jump of the IF
                // lands where the other scope's test does not fire.
                if (scope_ends.call_loop.count(endif_offset) != 0 || scope_ends.else_addresses.count(endif_offset) != 0) {
                    differs_from_interpreter = true;
                }

                shader.AddLine("if ({}) {{", condition);
                ++shader.scope;

                auto& if_sub = GetSubroutine(if_offset, else_offset);
                CallSubroutine(if_sub);
                offset = else_offset - 1;

                if (instr.num_instructions() != 0) {
                    --shader.scope;
                    shader.AddLine("}} else {{");
                    ++shader.scope;

                    auto& else_sub = GetSubroutine(else_offset, endif_offset);
                    CallSubroutine(else_sub);
                    offset = endif_offset - 1;

                    if (if_sub.exit_method == ExitMethod::AlwaysEnd &&
                        else_sub.exit_method == ExitMethod::AlwaysEnd) {
                        offset = PROGRAM_END - 1;
                    }
                }

                --shader.scope;
                shader.AddLine("}}");
                break;
            }

            case Opcode::LOOP: {
                const std::string int_uniform =
                    fmt::format("uniforms.i[{}]", instr.int_uniform_id());
                const std::string loop_var = fmt::format("loop{}", offset);
                auto& loop_sub = GetSubroutine(offset + 1, instr.dest_offset() + 1);

                // As the interpreter: aL is set to y, the increment is added after the last iteration and
                // after a break, and a loop entered inside another loop sets aL back to y when it ends
                // (shader_interpreter.cpp keeps previous_aL after the LOOP case has assigned y to aL, so
                // that is the value it gives back; the intention may have been the outer loop's aL, but
                // the interpreter is the reference. Azahar's decompiler leaves aL as the loop left it).
                shader.AddLine("{{");
                ++shader.scope;
                shader.AddLine("address_registers.z = int({}.y);", int_uniform);
                shader.AddLine("++loop_depth;");
                shader.AddLine("for (uint {} = 0u; {} <= {}.x; ++{}) {{", loop_var, loop_var, int_uniform,
                               loop_var);
                ++shader.scope;
                CallSubroutine(loop_sub, false); // the loop takes the break itself
                shader.AddLine("address_registers.z += int({}.z);", int_uniform);
                if (loop_sub.has_break) {
                    shader.AddLine("if (loop_break) {{ loop_break = false; break; }}");
                }
                --shader.scope;
                shader.AddLine("}}");
                shader.AddLine("--loop_depth;");
                shader.AddLine("if (loop_depth > 0) {{ address_registers.z = int({}.y); }}", int_uniform);
                --shader.scope;
                shader.AddLine("}}");

                offset = instr.dest_offset();

                if (loop_sub.exit_method == ExitMethod::AlwaysEnd) {
                    offset = PROGRAM_END - 1;
                }

                break;
            }

            case Opcode::EMIT:
            case Opcode::SETEMIT:
                // As Azahar's decompiler: nothing (it logs "Geometry shader operation detected in
                // vertex shader"); the interpreter, with no emitter on a vertex unit, does nothing too.
                has_emit = true;
                break;

            default: {
                throw DecompileFail(fmt::format("Unhandled instruction at word {}", offset), "unhandled-instruction",
                                    static_cast<int>(offset));
            }
            }

            break;
        }
        }
        return offset + 1;
    }

    /**
     * Compiles a range of instructions from PICA to GLSL.
     * @param begin the offset of the starting instruction.
     * @param end the offset where the compilation should stop (exclusive).
     * @return the offset of the next instruction to compile. PROGRAM_END if the program terminates.
     */
    u32 CompileRange(u32 begin, u32 end) {
        u32 program_counter;
        for (program_counter = begin; program_counter < (begin > end ? PROGRAM_END : end);) {
            program_counter = CompileInstr(program_counter);
        }
        return program_counter;
    }

    // The analysis follows the program's structure, the generator also compiles code a jump leads into
    // from another range: GLSL has no recursion, so a cycle in the calls that were generated is refused.
    void CheckNoRecursion() const {
        std::map<std::string, int> state; // 1 on the path, 2 done
        std::vector<std::string> stack;
        const auto visit = [&](const auto& self, const std::string& name) -> void {
            int& st = state[name];
            if (st == 2) {
                return;
            }
            if (st == 1) {
                throw DecompileFail("Recursive function detected", "recursion");
            }
            st = 1;
            if (auto it = calls.find(name); it != calls.end()) {
                for (const std::string& callee : it->second) {
                    self(self, callee);
                }
            }
            state[name] = 2;
        };
        visit(visit, "exec_shader");
    }

    void Generate() {
        // The PICA's multiplication: 0 x inf is +0, a NaN factor gives NaN. The tests are on the bits so
        // that no compiler can fold them away.
        shader.AddLine("bvec4 pica_isnan(vec4 v) {{");
        ++shader.scope;
        shader.AddLine("return greaterThan(floatBitsToUint(v) & uvec4(0x7fffffffu), uvec4(0x7f800000u));");
        --shader.scope;
        shader.AddLine("}}\n");

        shader.AddLine("vec4 sanitize_mul(vec4 lhs, vec4 rhs) {{");
        ++shader.scope;
        shader.AddLine("vec4 product = lhs * rhs;");
        shader.AddLine("return mix(product, mix(mix(vec4(0.0), product, pica_isnan(rhs)), product, "
                       "pica_isnan(lhs)), pica_isnan(product));");
        --shader.scope;
        shader.AddLine("}}\n");

        // MAX is `src1 > src2 ? src1 : src2` and MIN `src1 < src2 ? src1 : src2` (a NaN src1 gives
        // src2, a NaN src2 gives NaN), selected on the bits so that it cannot become fmax / fmin.
        shader.AddLine("vec4 pica_max(vec4 a, vec4 b) {{");
        ++shader.scope;
        shader.AddLine("return uintBitsToFloat(mix(floatBitsToUint(b), floatBitsToUint(a), greaterThan(a, b)));");
        --shader.scope;
        shader.AddLine("}}\n");
        shader.AddLine("vec4 pica_min(vec4 a, vec4 b) {{");
        ++shader.scope;
        shader.AddLine("return uintBitsToFloat(mix(floatBitsToUint(b), floatBitsToUint(a), lessThan(a, b)));");
        --shader.scope;
        shader.AddLine("}}\n");

        // RCP and RSQ as the interpreter computes them (1.0f / x and 1.0f / sqrtf(x), every step rounded to
        // nearest), on any device. A device's reciprocal, square root and inverse square root are within a
        // unit in the last place of the true value but not always the rounded one (the RX 6700 XT's differ
        // from the CPU's in a sizeable share of the inputs), and one ulp is more than the comparison of the
        // dump check allows once a program subtracts nearly equal numbers afterwards (one plus the z of a
        // unit vector, z near -1). The start value is the device's own; the loops move it to the rounded
        // value by an exact test with fma: q is the nearest float to 1/s when 1 - q*s lies within half an
        // ulp of q times s, and c is the nearest float to sqrt(x) when x - c*c lies in (-c*ulp_below(c),
        // c*ulp_above(c)] (c is a multiple of its ulp, so those bounds are exact products and the strict
        // side of the lower bound is the half-ulp-squared term). The range is the one where those
        // residuals are exact floats and nothing underflows (2^-80 .. 2^80); outside it, and for zero,
        // infinity and NaN, the plain device operations are used (the interpreter's results for those are
        // the IEEE ones, which the device gives: 1/0 is inf, 1/inf is 0, a negative root is NaN).
        shader.AddLine("float pica_next(float v) {{ return uintBitsToFloat(floatBitsToUint(v) + 1u); }}");
        shader.AddLine("float pica_prev(float v) {{ return uintBitsToFloat(floatBitsToUint(v) - 1u); }}\n");
        shader.AddLine("float pica_round_rcp(float s, float q) {{");
        ++shader.scope;
        shader.AddLine("for (int i = 0; i < 4; ++i) {{");
        ++shader.scope;
        shader.AddLine("float above = pica_next(q) - q;");
        shader.AddLine("float below = q - pica_prev(q);");
        shader.AddLine("float residual = fma(-q, s, 1.0);");
        shader.AddLine("if (residual > above * s * 0.5) {{ q = pica_next(q); }}");
        shader.AddLine("else if (residual < -(below * s * 0.5)) {{ q = pica_prev(q); }}");
        --shader.scope;
        shader.AddLine("}}");
        shader.AddLine("return q;");
        --shader.scope;
        shader.AddLine("}}\n");
        shader.AddLine("float pica_round_sqrt(float x, float c) {{");
        ++shader.scope;
        shader.AddLine("for (int i = 0; i < 4; ++i) {{");
        ++shader.scope;
        shader.AddLine("float above = pica_next(c) - c;");
        shader.AddLine("float below = c - pica_prev(c);");
        shader.AddLine("float residual = fma(-c, c, x);");
        shader.AddLine("if (residual > c * above) {{ c = pica_next(c); }}");
        shader.AddLine("else if (residual <= -(c * below)) {{ c = pica_prev(c); }}");
        --shader.scope;
        shader.AddLine("}}");
        shader.AddLine("return c;");
        --shader.scope;
        shader.AddLine("}}\n");
        shader.AddLine("float pica_rcp(float x) {{");
        ++shader.scope;
        shader.AddLine("float magnitude = abs(x);");
        shader.AddLine("if (magnitude >= 8.27e-25 && magnitude <= 1.2e24) {{");
        ++shader.scope;
        shader.AddLine("float q = pica_round_rcp(magnitude, 1.0 / magnitude);");
        shader.AddLine("return x < 0.0 ? -q : q;");
        --shader.scope;
        shader.AddLine("}}");
        shader.AddLine("return 1.0 / x;");
        --shader.scope;
        shader.AddLine("}}\n");
        shader.AddLine("float pica_rsq(float x) {{");
        ++shader.scope;
        shader.AddLine("if (x >= 8.27e-25 && x <= 1.2e24) {{");
        ++shader.scope;
        shader.AddLine("float root = pica_round_sqrt(x, sqrt(x));");
        shader.AddLine("return pica_round_rcp(root, 1.0 / root);");
        --shader.scope;
        shader.AddLine("}}");
        shader.AddLine("return 1.0 / sqrt(x);");
        --shader.scope;
        shader.AddLine("}}\n");

        // LIT as vs_3_0 (and ARB_vertex_program) define it; not verified on hardware.
        shader.AddLine("vec4 pica_lit(vec4 s) {{");
        ++shader.scope;
        shader.AddLine("float power = clamp(s.w, -127.9961, 127.9961);");
        shader.AddLine("float spec = (s.x > 0.0 && s.y > 0.0) ? pow(s.y, power) : 0.0;");
        shader.AddLine("return vec4(1.0, max(s.x, 0.0), spec, 1.0);");
        --shader.scope;
        shader.AddLine("}}\n");

        shader.AddLine("vec4 get_offset_register(int base_index, int offset) {{");
        ++shader.scope;
        shader.AddLine("int fixed_offset = offset >= -128 && offset <= 127 ? offset : 0;");
        shader.AddLine("uint index = uint((base_index + fixed_offset) & 0x7F);");
        shader.AddLine("return index < 96u ? uniforms.f[index] : vec4(1.0);");
        --shader.scope;
        shader.AddLine("}}\n");

        // Add declarations for registers
        shader.AddLine("bvec2 conditional_code = bvec2(false);");
        shader.AddLine("ivec3 address_registers = ivec3(0);");
        shader.AddLine("int loop_depth = 0;");
        shader.AddLine("bool loop_break = false;");
        for (int i = 0; i < 16; ++i) {
            shader.AddLine("vec4 reg_tmp{} = vec4(0.0, 0.0, 0.0, 1.0);", i);
        }
        shader.AddNewLine();

        // Add declarations for all subroutines
        for (const auto& subroutine : subroutines) {
            shader.AddLine("bool {}();", subroutine.GetName());
        }
        shader.AddNewLine();

        // Add the main entry point
        current_sub = "exec_shader";
        shader.AddLine("bool exec_shader() {{");
        ++shader.scope;
        CallSubroutine(GetSubroutine(main_offset, PROGRAM_END));
        --shader.scope;
        shader.AddLine("}}\n");

        // Add definitions for all subroutines
        for (const auto& subroutine : subroutines) {
            std::set<u32> labels = subroutine.labels;
            current_sub = subroutine.GetName();

            shader.AddLine("bool {}() {{", subroutine.GetName());
            ++shader.scope;

            if (labels.empty()) {
                if (CompileRange(subroutine.begin, subroutine.end) != PROGRAM_END) {
                    shader.AddLine("return false;");
                }
            } else {
                labels.insert(subroutine.begin);
                shader.AddLine("uint jmp_to = {}u;", subroutine.begin);
                shader.AddLine("for (uint jmp_guard = 0u; jmp_guard < {}u; ++jmp_guard) {{",
                               kMaxJumpIterations);
                ++shader.scope;

                shader.AddLine("switch (jmp_to) {{");

                for (auto label : labels) {
                    shader.AddLine("case {}u: {{", label);
                    ++shader.scope;

                    auto next_it = labels.lower_bound(label + 1);
                    u32 next_label = next_it == labels.end() ? subroutine.end : *next_it;

                    u32 compile_end = CompileRange(label, next_label);
                    if (compile_end > next_label && compile_end != PROGRAM_END) {
                        // This happens only when there is a label inside a IF/LOOP block
                        shader.AddLine("{{ jmp_to = {}u; break; }}", compile_end);
                        labels.emplace(compile_end);
                    }

                    --shader.scope;
                    shader.AddLine("}}");
                }

                shader.AddLine("default: return false;");
                shader.AddLine("}}");

                --shader.scope;
                shader.AddLine("}}");

                shader.AddLine("return false;");
            }

            --shader.scope;
            shader.AddLine("}}\n");
        }

        CheckNoRecursion();
        if (backward_jump_offset) {
            result.notes.push_back(fmt::format(
                "the jump at word {} goes backwards: the translated shader dispatches the labels of a "
                "subroutine at most {} times",
                *backward_jump_offset, kMaxJumpIterations));
        }
        for (const auto& [end, loops] : scope_ends.loops_by_end) {
            // two loops that end on the same instruction: the interpreter repeats or pops one loop per instruction
            differs_from_interpreter = differs_from_interpreter || loops.size() > 1;
        }
        if (differs_from_interpreter) {
            result.differs_from_interpreter = true;
            result.notes.push_back(
                "a loop or call body ends with an if (with or without else), an if is the last statement of an "
                "outer then-branch, two loops end together, or a call or jump is the last instruction of a "
                "scope: the interpreter and the structured readings (this translation, Azahar's decompiler and "
                "JIT) may disagree on this shape (a hint; see the header comment of glsl_decompiler.cpp)");
        }
        if (has_emit) {
            result.notes.push_back(
                "EMIT / SETEMIT are ignored (the program is translated as a vertex program, as Azahar's "
                "decompiler does)");
        }
    }

private:
    const std::set<Subroutine>& subroutines;
    const ScopeEnds& scope_ends;
    const Program& program;
    const u32 main_offset;
    const RegGetter& inputreg_getter;
    const RegGetter& outputreg_getter;
    DecompileResult& result;
    u32 compiled_instructions = 0;
    std::string current_sub;
    std::map<std::string, std::set<std::string>> calls;
    std::optional<u32> backward_jump_offset;
    bool has_emit = false;
    bool differs_from_interpreter = false;

    ShaderWriter shader;
};

} // namespace

DecompileResult DecompileProgram(const DecompileInput& input, const RegGetter& input_reg,
                                 const RegGetter& output_reg) {
    DecompileResult result;
    try {
        if (input.main_offset >= kMaxProgramWords) {
            throw DecompileFail("The entry point is outside the program memory", "entry-point");
        }
        const Program program(input);
        ControlFlowAnalyzer analyzer(program, input.main_offset);
        const ScopeEnds scope_ends = analyzer.Scopes();
        auto subroutines = analyzer.MoveSubroutines();
        GLSLGenerator generator(subroutines, scope_ends, program, input.main_offset, input_reg, output_reg, result);
        result.source = generator.MoveShaderCode();
        result.ok = true;
    } catch (const DecompileFail& exception) {
        result.ok = false;
        result.source.clear();
        result.error = exception.what();
        result.error_category = exception.category;
        result.error_word = exception.word;
    }
    return result;
}

} // namespace mh4n::gpu::shader
