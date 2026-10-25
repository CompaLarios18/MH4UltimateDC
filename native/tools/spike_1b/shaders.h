// The skeleton of uber_color (design 8 (j)) in its two forms, and glslang (N2-35).
//
//   UberSource()        option 1b / the ubershader: written once against the configuration block cfg; every key
//                       field is read through an accessor that is a cfg read (spec_uber = 1) or a specialisation
//                       constant (spec_uber = 0).
//   LiteralSource(key)  option 1: a C++ function that emits, for one key, only the chosen case of each switch,
//                       the light loop unrolled to the key's count and the values as literals (the style of
//                       Azahar's FragmentModule: WriteTevStage, WriteLighting, WriteAlphaTestCondition).
//
// Both share the key-independent helpers of the prologue (the texture stand-ins, the quaternion, the LUT fetch),
// and nothing else: the key-dependent code of the two is written separately, and the validity check of every key
// is what shows that they agree.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "key.h"

namespace spike {

std::string VertexSource();
std::string UberSource();
std::string LiteralSource(const Key& key);
// A shader that only writes the output, for warming the driver up before anything is timed.
std::string WarmupFragmentSource();

// glslang as a library (N2-06's mh4n_glslang): the same environment as native/src/gpu/shader/spirv_compile.cpp
// (Vulkan 1.2, SPIR-V 1.5, no optimizer, no validation).
double InitGlslang(); // milliseconds the process-wide initialisation took
bool CompileGlsl(bool fragment, const std::string& source, std::vector<uint32_t>& spirv, std::string& error);

// Adds an OpSourceExtension naming `salt` to a module: another module hash for the driver's cache, the same code.
std::vector<uint32_t> SaltSpirv(const std::vector<uint32_t>& spirv, const std::string& salt);

} // namespace spike
