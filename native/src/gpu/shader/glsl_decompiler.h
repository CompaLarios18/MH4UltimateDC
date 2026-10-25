// Derived from Azahar src/video_core/shader/generator/glsl_shader_decompiler.h at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-2.0-or-later; the file says "Licensed under GPLv2 or
// any later version").
// Copyright 2017 Citra Emulator Project (glsl_shader_decompiler.h)
//
// The PICA program to GLSL translator (N2-06): the interface of glsl_decompiler.cpp. It returns the
// text of everything between the shader's globals and its subroutines (see the .cpp for the contract
// with the code around it); the vertex stage around it is vertex_shader.cpp.
#pragma once

#include <functional>
#include <span>
#include <string>
#include <vector>

#include "mh4n/types.h"

namespace mh4n::gpu::shader {

// What a program may do: the PICA's program memory is 4096 words (Pica::MAX_PROGRAM_CODE_LENGTH).
inline constexpr u32 kMaxProgramWords = 4096;
inline constexpr u32 kMaxSwizzleWords = 4096;

// The bound on the iterations of the loop that dispatches the labels of a subroutine that contains
// a JMPC / JMPU (a backward jump can loop for ever; a GPU cannot be allowed to hang). A program
// that really needs more is not one the game ships: the hardware has the LOOP instruction.
inline constexpr u32 kMaxJumpIterations = 65536;

// The generated code never executes more than this many source instructions per subroutine
// call tree (a static bound on the size of the output, not on the work): a larger program is
// rejected.
inline constexpr u32 kMaxCompiledInstructions = 24576;

using RegGetter = std::function<std::string(u32)>;

struct DecompileInput {
    std::span<const u32> program;  // the loaded words (ShaderSetup::biggest_program_size of them)
    std::span<const u32> swizzle;  // the loaded operand descriptors (words beyond are zero)
    u32 main_offset = 0;
};

struct DecompileResult {
    bool ok = false;
    std::string source;              // GLSL, see glsl_decompiler.cpp
    std::string error;               // when !ok: why the program is not translated
    std::string error_category;      // ... and the cause in a word, with no part of the program in it
    int error_word = -1;             // ... and the offset of the word at fault, -1 when none
    std::vector<std::string> notes;  // things the translation does that the caller may want to know
    // The program contains instructions Azahar does not implement (it logs "Unhandled arithmetic
    // instruction" in its interpreter and rejects the program in its decompiler): DST, DSTI, LIT. They
    // are translated after the vs_3_0 definitions (see glsl_decompiler.cpp) and this says so.
    bool uses_unverified_instructions = false;
    // The program has a shape in which the interpreter and the structured readings disagree (a loop or
    // call body that ends with an if/else, a call or jump that is the last instruction of a scope): a
    // mismatch with the interpreter in such a program may be the interpreter's quirk.
    bool differs_from_interpreter = false;
};

// `input_reg(i)` names the GLSL expression of input register i (and marks it used), `output_reg(i)`
// the one of output register i, or "" when the register has no output attribute (writes are
// dropped).
DecompileResult DecompileProgram(const DecompileInput& input, const RegGetter& input_reg,
                                 const RegGetter& output_reg);

} // namespace mh4n::gpu::shader
